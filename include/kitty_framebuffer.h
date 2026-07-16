#ifndef KITTY_FRAMEBUFFER_H
#define KITTY_FRAMEBUFFER_H

/*
 * A C11/POSIX terminal framebuffer presenter for the Kitty graphics
 * protocol.  Applications hand it RGBA frames; the library strips them to
 * RGB, zlib-compresses, base64-encodes and pushes them down the terminal
 * connection inside DEC 2026 synchronized updates, alternating between two
 * image ids so the screen never shows a half-decoded or blank frame.
 *
 * Encoding runs on a presenter thread with a newest-frame-wins pending
 * slot: a slow terminal costs dropped frames, never a stalled caller.
 *
 * This header is presentation only.  Keyboard input is a separate concern;
 * compose this library with an input library or plain read() calls.
 */

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <termios.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KITTYFB_VERSION_MAJOR 0
#define KITTYFB_VERSION_MINOR 1
#define KITTYFB_VERSION_PATCH 0

typedef struct kittyfb_options {
    /* Save termios and switch the input descriptor's terminal to raw
     * mode; restored on stop.  Disable when the application owns raw
     * mode.  Default true. */
    bool manage_raw_mode;

    /* Enter the alternate screen on start and leave it on stop.
     * Default true. */
    bool manage_alt_screen;

    /* Hide the cursor on start and show it again on stop.  Default
     * true. */
    bool hide_cursor;

    /* Send a Kitty graphics query paired with a primary device
     * attributes request before drawing anything.  Terminals without
     * graphics support still answer the DA1, which bounds the wait.
     * When the terminal does not answer the graphics query, start fails
     * with errno = ENOTSUP.  Default true.  The environment variable
     * KITTYFB_SKIP_PROBE=1 also skips the probe. */
    bool probe_graphics;

    /* Install a SIGWINCH handler that flags a pending resize check.
     * kittyfb_check_resize() also re-reads the window size directly, so
     * resizes are detected even without the handler; applications that
     * own SIGWINCH should disable this and call kittyfb_notify_resize()
     * from their handler.  Default true. */
    bool install_winch_handler;

    /* Wait for the first probe response byte at most this long, in
     * milliseconds; high-latency connections to capable terminals need
     * a generous window.  Default 1000. */
    int probe_timeout_ms;

    /* Framebuffer pixel dimension bounds.  The chosen size is derived
     * from the terminal's reported pixel geometry, clamped into these
     * bounds, then snapped to whole cells and even pixel counts.
     * Defaults 640x400 .. 1600x1000. */
    int min_width;
    int min_height;
    int max_width;
    int max_height;

    /* The pair of Kitty image ids alternated between frames.  Must be
     * positive and distinct.  Change them when the application also
     * places its own images.  Defaults 1 and 2. */
    int image_id_a;
    int image_id_b;

    /* zlib compression level, -1 (zlib default) or 0..9.  Level 1 is
     * fast enough to encode full frames at game frame rates and is the
     * default. */
    int zlib_level;
} kittyfb_options;

typedef struct kittyfb_stats {
    uint64_t frames_presented;  /* frames accepted by kittyfb_present() */
    uint64_t frames_encoded;    /* frames fully encoded and written */
    uint64_t frames_dropped;    /* pending frames replaced before encoding */
    uint64_t encode_failures;   /* compression, allocation, write failures */
} kittyfb_stats;

/* Public so callers can allocate it without malloc; fields are internal. */
typedef struct kittyfb_session {
    kittyfb_options options;
    int input_fd;
    int output_fd;

    /* chosen geometry; guarded by frame_lock once the session is active */
    int width;
    int height;
    int cell_width;
    int cell_height;
    char origin_sequence[32];
    bool clear_pending;

    /* saved terminal state */
    struct termios saved_termios;
    struct sigaction saved_winch_action;
    int saved_output_flags;
    bool termios_saved;
    bool winch_handler_installed;
    bool output_flags_saved;

    /* lifecycle guards shared with async-signal paths */
    volatile sig_atomic_t active;
    volatile sig_atomic_t shutdown_claimed;
    volatile sig_atomic_t presenter_disabled;
    volatile sig_atomic_t write_cancel;

    /* presenter thread state; guarded by frame_lock */
    pthread_t presenter_thread;
    pthread_mutex_t frame_lock;
    pthread_cond_t frame_cond;
    uint8_t *pending_buffer;
    uint8_t *encode_buffer;
    size_t pending_capacity;
    size_t encode_capacity;
    int frame_width;
    int frame_height;
    bool frame_pending;
    bool presenter_running;
    bool presenter_started;
    bool presenter_failed;
    kittyfb_stats stats;

    /* encoder scratch; presenter thread (or synchronous fallback) only */
    uint8_t *rgb_buffer;
    uint8_t *z_buffer;
    char *b64_buffer;
    char *packet_buffer;
    size_t rgb_capacity;
    size_t z_capacity;
    size_t b64_capacity;
    size_t packet_capacity;
    int shown_image_id;

    /* prebuilt restore sequence for the async-signal path */
    char emergency[160];
    size_t emergency_length;
} kittyfb_session;

void kittyfb_options_init(kittyfb_options *options);

/* Initialize once before the first start; sessions may then cycle through
 * any number of start/stop pairs. */
void kittyfb_session_init(kittyfb_session *session);

/*
 * Measure the terminal, switch it to raw mode, probe for graphics
 * support, enter the alternate screen and hide the cursor (each step as
 * configured).  Returns 0, or -1 with errno set: ENOTSUP when the
 * terminal answered the device-attributes query but not the graphics
 * query, ENOTTY when a required descriptor is not a terminal, EBUSY when
 * the session is already active.
 *
 * The output descriptor must be a terminal.  The input descriptor must
 * be a terminal when manage_raw_mode or probe_graphics is set.  The
 * output descriptor is made non-blocking for the session's lifetime so
 * no write - including the async-signal restore - can hang on a stalled
 * terminal connection.
 */
int kittyfb_start(
    kittyfb_session *session,
    int input_fd,
    int output_fd,
    const kittyfb_options *options);

/* Chosen framebuffer pixel size and terminal cell pixel size.  Valid
 * after a successful start; call from the thread that presents. */
int kittyfb_width(const kittyfb_session *session);
int kittyfb_height(const kittyfb_session *session);
int kittyfb_cell_width(const kittyfb_session *session);
int kittyfb_cell_height(const kittyfb_session *session);

/*
 * Queue one RGBA frame (width * height * 4 bytes, alpha ignored) for
 * presentation.  The frame is copied out before returning; encoding and
 * the terminal write happen on the presenter thread.  If the encoder is
 * still busy when the next frame arrives, the newest frame replaces the
 * pending one.  Falls back to synchronous encoding when the presenter
 * thread cannot be created.  Returns false on invalid arguments, an
 * inactive session, allocation failure, or a latched presenter failure.
 */
bool kittyfb_present(
    kittyfb_session *session,
    const uint8_t *rgba,
    int width,
    int height);

/*
 * Re-read the terminal size and re-derive the framebuffer geometry.
 * Cheap; call once per frame.  Returns true - with the new size stored
 * through width/height when non-NULL - only when the framebuffer pixel
 * size changed; the caller must then present frames at the new size.
 * Any geometry change (including centering-only changes) schedules a
 * screen clear with the next presented frame.
 */
bool kittyfb_check_resize(kittyfb_session *session, int *width, int *height);

/* Flag a pending resize from an application-owned SIGWINCH handler.
 * Async-signal-safe. */
void kittyfb_notify_resize(void);

/*
 * Join the presenter thread, free its buffers and restore the terminal:
 * close any half-written graphics escape, end a pending synchronized
 * update, delete this session's two image ids (and no other images),
 * show the cursor, leave the alternate screen and restore termios and
 * descriptor flags.  Safe to call twice; the session may be started
 * again afterwards.
 */
void kittyfb_stop(kittyfb_session *session);

/*
 * Async-signal-safe restore for fatal-signal handlers: no locks, no
 * thread join.  Fences the presenter thread so it emits no further
 * bytes, then writes a prebuilt restore sequence - ending the
 * synchronized update first, since a truncated update would freeze the
 * terminal - and restores termios and descriptor flags.  A later
 * kittyfb_stop() still reclaims the presenter thread and its memory.
 */
void kittyfb_emergency_restore(kittyfb_session *session);

/* True once the presenter has latched a failure (the terminal stopped
 * consuming output, compression failed, or memory ran out); subsequent
 * kittyfb_present() calls return false.  Cleared by the next start. */
bool kittyfb_failed(const kittyfb_session *session);

/* Snapshot the frame counters.  Callable while the session is active. */
void kittyfb_get_stats(kittyfb_session *session, kittyfb_stats *out);

#ifdef __cplusplus
}
#endif

#endif
