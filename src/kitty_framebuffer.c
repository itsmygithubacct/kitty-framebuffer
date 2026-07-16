/*
 * Terminal framebuffer presenter for the Kitty graphics protocol.
 *
 * Frames are packed to RGB, zlib-compressed (o=z) and pushed with a=T
 * (transmit + display), chunked into 4 KB base64 payloads.  Two image ids
 * are used alternately: the new frame is transmitted and placed under one
 * id, then the previous frame's id is deleted.  Retransmitting a single
 * id would flicker - the terminal drops the old placement (blank screen)
 * before the replacement finishes decoding.  Each frame is additionally
 * wrapped in a DEC 2026 synchronized update so it applies atomically.
 *
 * Compression + base64 + the terminal write run on a presenter thread so
 * a slow-to-encode frame overlaps the caller's next frame instead of
 * stalling it; if the encoder is still busy when the next frame arrives,
 * the newest frame simply replaces the pending one (a dropped frame,
 * never a stall).
 *
 * Hardening notes carried over from the presenter's game lineage:
 *
 * - Only the pending buffer is ever grown by the caller; the presenter
 *   swaps buffers together with their capacities.  Growing the encode
 *   buffer from the caller would let realloc free the block the encoder
 *   is reading mid-frame.
 * - Every buffer growth goes through a temporary pointer so a failed
 *   realloc keeps the old block and the old capacity instead of leaking
 *   the block and freezing the picture.
 * - The emergency restore is async-signal-safe: it fences the presenter
 *   with a sig_atomic flag (a signal handler cannot join the thread),
 *   then writes one prebuilt sequence that ends the synchronized update
 *   FIRST - a truncated update would otherwise freeze the terminal - and
 *   deletes only this session's image ids.  A d=A delete would wipe
 *   every Kitty image in the terminal, including other programs'.
 * - The output descriptor is non-blocking with a poll-based write loop,
 *   so neither the presenter nor the signal path can hang on a stalled
 *   terminal connection.
 */

#include "kitty_framebuffer_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <zlib.h>

/* The probe never places an image (a=q is query-only), so this id cannot
 * collide with anything the application shows. */
#define KITTYFB_PROBE_IMAGE_ID 31

/* A stalled non-blocking write polls in 50 ms slices; give up after this
 * many consecutive slices without progress. */
#define KITTYFB_WRITE_STALL_LIMIT 40

static const char BASE64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* One flag for the whole process: a signal handler cannot carry a session
 * pointer, and a session owns the terminal exclusively anyway. */
static volatile sig_atomic_t winch_flag;

static void handle_winch(int signal_number)
{
    (void)signal_number;
    winch_flag = 1;
}

void kittyfb_notify_resize(void)
{
    winch_flag = 1;
}

/* ------------------------------ pure parts ------------------------------ */

size_t kittyfb_base64_encode(const uint8_t *input, size_t length, char *output)
{
    size_t in = 0;
    size_t out = 0;

    while (in + 2 < length) {
        uint32_t value = ((uint32_t)input[in] << 16) |
                         ((uint32_t)input[in + 1] << 8) |
                         (uint32_t)input[in + 2];
        output[out++] = BASE64_TABLE[(value >> 18) & 63u];
        output[out++] = BASE64_TABLE[(value >> 12) & 63u];
        output[out++] = BASE64_TABLE[(value >> 6) & 63u];
        output[out++] = BASE64_TABLE[value & 63u];
        in += 3;
    }
    if (in + 1 == length) {
        uint32_t value = (uint32_t)input[in] << 16;
        output[out++] = BASE64_TABLE[(value >> 18) & 63u];
        output[out++] = BASE64_TABLE[(value >> 12) & 63u];
        output[out++] = '=';
        output[out++] = '=';
    } else if (in + 2 == length) {
        uint32_t value = ((uint32_t)input[in] << 16) |
                         ((uint32_t)input[in + 1] << 8);
        output[out++] = BASE64_TABLE[(value >> 18) & 63u];
        output[out++] = BASE64_TABLE[(value >> 12) & 63u];
        output[out++] = BASE64_TABLE[(value >> 6) & 63u];
        output[out++] = '=';
    }
    return out;
}

bool kittyfb_derive_geometry(
    int columns,
    int rows,
    int xpixel,
    int ypixel,
    const kittyfb_options *options,
    kittyfb_geometry *out)
{
    if (options == NULL || out == NULL) {
        return false;
    }
    if (columns <= 0) {
        columns = 80;
    }
    if (rows <= 0) {
        rows = 24;
    }

    /* cell size in pixels; assume 9x18 if the terminal doesn't report it */
    int cell_width = xpixel > 0 ? xpixel / columns : 9;
    int cell_height = ypixel > 0 ? ypixel / rows : 18;
    if (cell_width <= 0) {
        cell_width = 9;
    }
    if (cell_height <= 0) {
        cell_height = 18;
    }

    /* leave one cell row free at the bottom so the shell prompt after
     * exit doesn't scroll the image */
    int grid_rows = rows > 1 ? rows - 1 : 1;
    int width = columns * cell_width;
    int height = grid_rows * cell_height;
    if (width < options->min_width) {
        width = options->min_width;
    }
    if (height < options->min_height) {
        height = options->min_height;
    }
    if (width > options->max_width) {
        width = options->max_width;
    }
    if (height > options->max_height) {
        height = options->max_height;
    }
    /* snap to whole cells so the image doesn't end in a ragged
     * partially-covered cell column/row, then force even dimensions */
    width -= width % cell_width;
    height -= height % cell_height;
    width &= ~1;
    height &= ~1;
    if (width <= 0 || height <= 0) {
        return false;
    }

    /* center the image instead of pinning it top-left */
    int image_columns = (width + cell_width - 1) / cell_width;
    int image_rows = (height + cell_height - 1) / cell_height;
    int origin_column = 1 + (columns - image_columns) / 2;
    int origin_row = 1 + (grid_rows - image_rows) / 2;
    if (origin_column < 1) {
        origin_column = 1;
    }
    if (origin_row < 1) {
        origin_row = 1;
    }

    out->width = width;
    out->height = height;
    out->cell_width = cell_width;
    out->cell_height = cell_height;
    out->origin_row = origin_row;
    out->origin_column = origin_column;
    return true;
}

size_t kittyfb_build_packet(
    char *output,
    size_t capacity,
    const char *payload,
    size_t payload_length,
    int new_id,
    int old_id,
    int width,
    int height,
    const char *origin,
    bool clear_first)
{
    char *at = output;
    size_t remaining = capacity;
    int printed;

    if (output == NULL || payload == NULL || origin == NULL ||
        new_id <= 0 || old_id <= 0 || width <= 0 || height <= 0) {
        return 0;
    }

    printed = snprintf(
        at,
        remaining,
        "\x1b[?2026h%s%s",
        clear_first ? "\x1b[2J" : "",
        origin);
    if (printed < 0 || (size_t)printed >= remaining) {
        return 0;
    }
    at += printed;
    remaining -= (size_t)printed;

    size_t offset = 0;
    bool first = true;
    while (offset < payload_length) {
        size_t count = payload_length - offset;
        if (count > KITTYFB_CHUNK_SIZE) {
            count = KITTYFB_CHUNK_SIZE;
        }
        int more = offset + count < payload_length ? 1 : 0;
        if (first) {
            printed = snprintf(
                at,
                remaining,
                "\x1b_Ga=T,f=24,i=%d,q=2,o=z,s=%d,v=%d,m=%d;",
                new_id,
                width,
                height,
                more);
            first = false;
        } else {
            printed = snprintf(at, remaining, "\x1b_Gm=%d;", more);
        }
        if (printed < 0 || (size_t)printed >= remaining) {
            return 0;
        }
        at += printed;
        remaining -= (size_t)printed;
        if (count + 2 > remaining) {
            return 0;
        }
        memcpy(at, payload + offset, count);
        at += count;
        *at++ = '\x1b';
        *at++ = '\\';
        remaining -= count + 2;
        offset += count;
    }

    /* d=I frees the old frame's pixel data in the terminal, not just the
     * placement, so terminal memory stays at two frames */
    printed = snprintf(
        at,
        remaining,
        "\x1b_Ga=d,d=I,i=%d,q=2\x1b\\\x1b[?2026l",
        old_id);
    if (printed < 0 || (size_t)printed >= remaining) {
        return 0;
    }
    at += printed;
    return (size_t)(at - output);
}

/* ------------------------------ small utils ----------------------------- */

/* Growth always goes through a temporary so a failed realloc keeps the
 * old buffer and the old capacity: with "cap = n; p = realloc(p, cap)" a
 * single OOM left p == NULL while cap claimed the space existed, so every
 * later frame skipped the realloc and bailed - the picture froze forever
 * and the old block leaked. */
static bool grow_bytes(uint8_t **buffer, size_t *capacity, size_t needed)
{
    if (needed <= *capacity) {
        return true;
    }
    uint8_t *grown = realloc(*buffer, needed);
    if (grown == NULL) {
        return false;
    }
    *buffer = grown;
    *capacity = needed;
    return true;
}

static bool grow_chars(char **buffer, size_t *capacity, size_t needed)
{
    if (needed <= *capacity) {
        return true;
    }
    char *grown = realloc(*buffer, needed);
    if (grown == NULL) {
        return false;
    }
    *buffer = grown;
    *capacity = needed;
    return true;
}

/* Cancellable, poll-based write loop for the non-blocking output fd.
 * Returns false when fenced, cancelled, stalled out, or on error. */
static bool write_all(kittyfb_session *session, const char *data, size_t size)
{
    size_t offset = 0;
    int stalled_polls = 0;

    while (offset < size) {
        if (session->presenter_disabled || session->write_cancel) {
            return false;
        }
        ssize_t count = write(session->output_fd, data + offset, size - offset);
        if (count > 0) {
            offset += (size_t)count;
            stalled_polls = 0;
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++stalled_polls > KITTYFB_WRITE_STALL_LIMIT) {
                errno = ETIMEDOUT;
                return false;
            }
            struct pollfd descriptor = { session->output_fd, POLLOUT, 0 };
            int ready;
            do {
                ready = poll(&descriptor, 1u, 50);
            } while (ready < 0 && errno == EINTR);
            if (ready < 0) {
                return false;
            }
            if (ready > 0 &&
                (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                errno = EIO;
                return false;
            }
            continue;
        }
        if (count == 0) {
            errno = EIO;
        }
        return false;
    }
    return true;
}

static bool read_byte_timeout(int fd, unsigned char *byte, int timeout_ms)
{
    /* Retry across signal interruptions (SIGWINCH arrives continuously
     * while a window is dragged); an EINTR misread as a timeout would
     * truncate the probe response mid-parse. */
    for (;;) {
        struct pollfd descriptor = { fd, POLLIN, 0 };
        int ready = poll(&descriptor, 1u, timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (ready == 0) {
            return false;
        }
        return read(fd, byte, 1u) == 1;
    }
}

/* ---------------------------- options/session --------------------------- */

void kittyfb_options_init(kittyfb_options *options)
{
    if (options == NULL) {
        return;
    }
    options->manage_raw_mode = true;
    options->manage_alt_screen = true;
    options->hide_cursor = true;
    options->probe_graphics = true;
    options->install_winch_handler = true;
    options->probe_timeout_ms = 1000;
    options->min_width = 640;
    options->min_height = 400;
    options->max_width = 1600;
    options->max_height = 1000;
    options->image_id_a = 1;
    options->image_id_b = 2;
    options->zlib_level = 1;
}

void kittyfb_session_init(kittyfb_session *session)
{
    if (session == NULL) {
        return;
    }
    (void)memset(session, 0, sizeof(*session));
    session->input_fd = -1;
    session->output_fd = -1;
    session->saved_output_flags = -1;
    (void)pthread_mutex_init(&session->frame_lock, NULL);
    (void)pthread_cond_init(&session->frame_cond, NULL);
}

int kittyfb_width(const kittyfb_session *session)
{
    return session != NULL ? session->width : 0;
}

int kittyfb_height(const kittyfb_session *session)
{
    return session != NULL ? session->height : 0;
}

int kittyfb_cell_width(const kittyfb_session *session)
{
    return session != NULL ? session->cell_width : 0;
}

int kittyfb_cell_height(const kittyfb_session *session)
{
    return session != NULL ? session->cell_height : 0;
}

bool kittyfb_failed(const kittyfb_session *session)
{
    return session != NULL && session->presenter_failed;
}

void kittyfb_get_stats(kittyfb_session *session, kittyfb_stats *out)
{
    if (session == NULL || out == NULL) {
        return;
    }
    pthread_mutex_lock(&session->frame_lock);
    *out = session->stats;
    pthread_mutex_unlock(&session->frame_lock);
}

/* ------------------------------- encoding ------------------------------- */

/* Runs on the presenter thread, or on the caller when thread creation
 * failed; either way it is the only user of the encoder scratch. */
static bool encode_and_write(
    kittyfb_session *session,
    const uint8_t *rgba,
    int width,
    int height,
    const char *origin,
    bool clear_first)
{
    /* A signal-time restore has fenced the presenter: emit nothing. */
    if (session->presenter_disabled) {
        return false;
    }
    if (rgba == NULL || width <= 0 || height <= 0) {
        return false;
    }
    if ((size_t)width > SIZE_MAX / 4u / (size_t)height) {
        return false;
    }

    /* strip the (ignored) alpha channel: 25% less data to compress,
     * encode and push down the terminal connection every frame */
    size_t pixels = (size_t)width * (size_t)height;
    size_t raw_length = pixels * 3u;
    if (!grow_bytes(&session->rgb_buffer, &session->rgb_capacity, raw_length)) {
        return false;
    }
    const uint8_t *source = rgba;
    uint8_t *destination = session->rgb_buffer;
    for (size_t index = 0; index < pixels; index++) {
        destination[0] = source[0];
        destination[1] = source[1];
        destination[2] = source[2];
        source += 4;
        destination += 3;
    }

    size_t z_needed = (size_t)compressBound((uLong)raw_length);
    if (z_needed > SIZE_MAX - 2u) {
        return false;
    }
    size_t base64_groups = (z_needed + 2u) / 3u;
    if (base64_groups > (SIZE_MAX - 1u) / 4u) {
        return false;
    }
    size_t base64_needed = base64_groups * 4u + 1u;
    if (base64_needed > SIZE_MAX - 512u) {
        return false;
    }
    size_t chunk_count = base64_needed / KITTYFB_CHUNK_SIZE + 1u;
    if (chunk_count > (SIZE_MAX - base64_needed - 512u) / 96u) {
        return false;
    }
    size_t packet_needed = base64_needed + chunk_count * 96u + 512u;
    if (!grow_bytes(&session->z_buffer, &session->z_capacity, z_needed) ||
        !grow_chars(&session->b64_buffer, &session->b64_capacity,
                    base64_needed) ||
        !grow_chars(&session->packet_buffer, &session->packet_capacity,
                    packet_needed)) {
        return false;
    }

    uLongf z_length = (uLongf)z_needed;
    if (compress2(session->z_buffer, &z_length, session->rgb_buffer,
                  (uLong)raw_length, session->options.zlib_level) != Z_OK) {
        return false;
    }

    size_t base64_length = kittyfb_base64_encode(
        session->z_buffer,
        (size_t)z_length,
        session->b64_buffer);

    /* Double buffer: transmit the new frame under the id NOT currently
     * on screen, then delete the old id.  Inside a synchronized update
     * the swap is atomic, so the screen never shows a half-drawn or
     * blank state. */
    int new_id = session->shown_image_id == session->options.image_id_a
                     ? session->options.image_id_b
                     : session->options.image_id_a;

    size_t packet_length = kittyfb_build_packet(
        session->packet_buffer,
        session->packet_capacity,
        session->b64_buffer,
        base64_length,
        new_id,
        session->shown_image_id,
        width,
        height,
        origin,
        clear_first);
    if (packet_length == 0) {
        return false;
    }

    /* Re-check the fence right before the write: if a restore raced in
     * after the top check, this frame's packet must not go out at all. */
    if (session->presenter_disabled) {
        return false;
    }
    if (!write_all(session, session->packet_buffer, packet_length)) {
        return false;
    }
    session->shown_image_id = new_id;
    return true;
}

/* --------------------------- presenter thread --------------------------- */

static void *presenter_main(void *opaque)
{
    kittyfb_session *session = opaque;

    for (;;) {
        pthread_mutex_lock(&session->frame_lock);
        while (!session->frame_pending && session->presenter_running) {
            pthread_cond_wait(&session->frame_cond, &session->frame_lock);
        }
        if (!session->presenter_running) {
            pthread_mutex_unlock(&session->frame_lock);
            break;
        }
        /* Swap buffers together with their capacities: the caller keeps
         * writing new frames into pending_buffer while this one encodes
         * from encode_buffer.  The caller only ever grows the pending
         * buffer, so the block being encoded can never be reallocated
         * out from under the encoder. */
        uint8_t *swap_buffer = session->pending_buffer;
        session->pending_buffer = session->encode_buffer;
        session->encode_buffer = swap_buffer;
        size_t swap_capacity = session->pending_capacity;
        session->pending_capacity = session->encode_capacity;
        session->encode_capacity = swap_capacity;
        int width = session->frame_width;
        int height = session->frame_height;
        char origin[sizeof(session->origin_sequence)];
        memcpy(origin, session->origin_sequence, sizeof(origin));
        bool clear_first = session->clear_pending;
        session->clear_pending = false;
        session->frame_pending = false;
        pthread_mutex_unlock(&session->frame_lock);

        bool encoded = encode_and_write(
            session,
            session->encode_buffer,
            width,
            height,
            origin,
            clear_first);

        pthread_mutex_lock(&session->frame_lock);
        if (encoded) {
            session->stats.frames_encoded++;
        } else if (!session->write_cancel && !session->presenter_disabled) {
            /* A cancelled or fenced write is shutdown noise, not a
             * failure; anything else latches so the caller can stop. */
            session->stats.encode_failures++;
            session->presenter_failed = true;
            session->presenter_running = false;
        }
        bool keep_running = session->presenter_running;
        pthread_mutex_unlock(&session->frame_lock);
        if (!keep_running) {
            break;
        }
    }
    return NULL;
}

bool kittyfb_present(
    kittyfb_session *session,
    const uint8_t *rgba,
    int width,
    int height)
{
    if (session == NULL || !session->active || rgba == NULL ||
        width <= 0 || height <= 0) {
        return false;
    }
    if ((size_t)width > SIZE_MAX / 4u / (size_t)height) {
        return false;
    }
    size_t needed = (size_t)width * (size_t)height * 4u;

    pthread_mutex_lock(&session->frame_lock);
    if (session->presenter_failed ||
        (session->presenter_started && !session->presenter_running)) {
        pthread_mutex_unlock(&session->frame_lock);
        return false;
    }
    /* Only the pending buffer may be grown here: the presenter could be
     * mid-encode on encode_buffer and realloc would free it under its
     * feet.  The capacities travel with the buffers through the swap. */
    if (!grow_bytes(&session->pending_buffer, &session->pending_capacity,
                    needed)) {
        pthread_mutex_unlock(&session->frame_lock);
        return false;
    }
    if (!session->presenter_started) {
        session->presenter_running = true;
        if (pthread_create(&session->presenter_thread, NULL, presenter_main,
                           session) != 0) {
            /* Synchronous fallback; thread creation is retried on the
             * next present. */
            session->presenter_running = false;
            char origin[sizeof(session->origin_sequence)];
            memcpy(origin, session->origin_sequence, sizeof(origin));
            bool clear_first = session->clear_pending;
            session->clear_pending = false;
            session->stats.frames_presented++;
            pthread_mutex_unlock(&session->frame_lock);
            bool encoded = encode_and_write(
                session, rgba, width, height, origin, clear_first);
            pthread_mutex_lock(&session->frame_lock);
            if (encoded) {
                session->stats.frames_encoded++;
            } else {
                session->stats.encode_failures++;
            }
            pthread_mutex_unlock(&session->frame_lock);
            return encoded;
        }
        session->presenter_started = true;
    }
    /* overwriting an undelivered frame = dropping it in favor of this one */
    if (session->frame_pending) {
        session->stats.frames_dropped++;
    }
    memcpy(session->pending_buffer, rgba, needed);
    session->frame_width = width;
    session->frame_height = height;
    session->frame_pending = true;
    session->stats.frames_presented++;
    pthread_cond_signal(&session->frame_cond);
    pthread_mutex_unlock(&session->frame_lock);
    return true;
}

/* Join the presenter and release its memory.  Safe when never started. */
static void presenter_shutdown(kittyfb_session *session)
{
    session->write_cancel = 1;
    pthread_mutex_lock(&session->frame_lock);
    bool must_join = session->presenter_started;
    session->presenter_running = false;
    session->frame_pending = false;
    pthread_cond_broadcast(&session->frame_cond);
    pthread_mutex_unlock(&session->frame_lock);
    if (must_join) {
        pthread_join(session->presenter_thread, NULL);
    }

    pthread_mutex_lock(&session->frame_lock);
    session->presenter_started = false;
    free(session->pending_buffer);
    free(session->encode_buffer);
    session->pending_buffer = NULL;
    session->encode_buffer = NULL;
    session->pending_capacity = 0;
    session->encode_capacity = 0;
    free(session->rgb_buffer);
    free(session->z_buffer);
    free(session->b64_buffer);
    free(session->packet_buffer);
    session->rgb_buffer = NULL;
    session->z_buffer = NULL;
    session->b64_buffer = NULL;
    session->packet_buffer = NULL;
    session->rgb_capacity = 0;
    session->z_capacity = 0;
    session->b64_capacity = 0;
    session->packet_capacity = 0;
    pthread_mutex_unlock(&session->frame_lock);
    session->write_cancel = 0;
}

/* ------------------------------- lifecycle ------------------------------ */

/* Ask the terminal whether it speaks the Kitty graphics protocol: a 1x1
 * query image followed by a primary device-attributes request.  Graphics
 * terminals answer the APC query; every terminal answers the DA1, which
 * bounds the wait on terminals that silently ignore APCs. */
static bool probe_for_graphics(kittyfb_session *session)
{
    char query[80];
    char expected[24];
    char response[512];
    size_t length = 0;
    bool graphics = false;

    int printed = snprintf(
        query,
        sizeof(query),
        "\x1b_Gi=%d,a=q,t=d,f=24,s=1,v=1;AAAA\x1b\\\x1b[c",
        KITTYFB_PROBE_IMAGE_ID);
    if (printed < 0 || (size_t)printed >= sizeof(query)) {
        return false;
    }
    if (!write_all(session, query, (size_t)printed)) {
        return false;
    }
    printed = snprintf(
        expected,
        sizeof(expected),
        "\x1b_Gi=%d",
        KITTYFB_PROBE_IMAGE_ID);
    if (printed < 0 || (size_t)printed >= sizeof(expected)) {
        return false;
    }

    while (length + 1 < sizeof(response)) {
        unsigned char byte;
        /* Give the first byte a generous window so a high-latency
         * connection to a fully capable terminal is not misjudged as
         * lacking graphics support; once bytes flow they arrive fast. */
        int wait_ms = length == 0 ? session->options.probe_timeout_ms : 250;
        if (!read_byte_timeout(session->input_fd, &byte, wait_ms)) {
            break;
        }
        response[length++] = (char)byte;
        response[length] = '\0';
        if (strstr(response, expected) != NULL) {
            graphics = true;
        }
        /* primary DA reply terminator */
        if (byte == 'c' && strstr(response, "\x1b[?") != NULL) {
            break;
        }
    }
    return graphics;
}

static bool validate_options(const kittyfb_options *options)
{
    return options->min_width > 0 && options->min_height > 0 &&
           options->min_width <= options->max_width &&
           options->min_height <= options->max_height &&
           options->image_id_a > 0 && options->image_id_b > 0 &&
           options->image_id_a != options->image_id_b &&
           options->zlib_level >= -1 && options->zlib_level <= 9 &&
           options->probe_timeout_ms >= 0;
}

static bool build_emergency_sequence(kittyfb_session *session)
{
    /* End the synchronized update FIRST: the presenter's packet begins
     * with ?2026h, and a signal that truncated the packet before its
     * closing ?2026l would leave the terminal frozen, swallowing the
     * rest of this restore.  The ST then closes any half-written APC,
     * and the second ?2026l covers the case where the first one was
     * consumed as APC payload.  Deletes target only this session's two
     * image ids. */
    int printed = snprintf(
        session->emergency,
        sizeof(session->emergency),
        "\x1b[?2026l\x1b\\"
        "\x1b_Ga=d,d=i,i=%d,q=2\x1b\\"
        "\x1b_Ga=d,d=i,i=%d,q=2\x1b\\"
        "\x1b[?2026l%s%s",
        session->options.image_id_a,
        session->options.image_id_b,
        session->options.hide_cursor ? "\x1b[?25h" : "",
        session->options.manage_alt_screen ? "\x1b[?1049l" : "");
    if (printed < 0 || (size_t)printed >= sizeof(session->emergency)) {
        return false;
    }
    session->emergency_length = (size_t)printed;
    return true;
}

static void set_geometry(kittyfb_session *session, const kittyfb_geometry *g)
{
    session->width = g->width;
    session->height = g->height;
    session->cell_width = g->cell_width;
    session->cell_height = g->cell_height;
    (void)snprintf(
        session->origin_sequence,
        sizeof(session->origin_sequence),
        "\x1b[%d;%dH",
        g->origin_row,
        g->origin_column);
}

int kittyfb_start(
    kittyfb_session *session,
    int input_fd,
    int output_fd,
    const kittyfb_options *options)
{
    kittyfb_options defaults;
    struct winsize window;
    kittyfb_geometry geometry;
    char setup[32];
    int printed;
    int failure;

    if (session == NULL || input_fd < 0 || output_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (session->active) {
        errno = EBUSY;
        return -1;
    }
    if (options == NULL) {
        kittyfb_options_init(&defaults);
        options = &defaults;
    }
    if (!validate_options(options)) {
        errno = EINVAL;
        return -1;
    }
    if (!isatty(output_fd)) {
        errno = ENOTTY;
        return -1;
    }
    if ((options->manage_raw_mode || options->probe_graphics) &&
        !isatty(input_fd)) {
        errno = ENOTTY;
        return -1;
    }

    session->options = *options;
    session->input_fd = input_fd;
    session->output_fd = output_fd;

    if (ioctl(output_fd, TIOCGWINSZ, &window) != 0) {
        return -1;
    }
    if (!kittyfb_derive_geometry(window.ws_col, window.ws_row,
                                 window.ws_xpixel, window.ws_ypixel,
                                 &session->options, &geometry)) {
        errno = ERANGE;
        return -1;
    }
    set_geometry(session, &geometry);
    if (!build_emergency_sequence(session)) {
        errno = EOVERFLOW;
        return -1;
    }

    /* Reset every one-shot guard: a second start after stop must work,
     * and a latched claim or fence from the previous run must not
     * swallow this run's shutdown. */
    session->shutdown_claimed = 0;
    session->presenter_disabled = 0;
    session->write_cancel = 0;
    session->presenter_started = false;
    session->presenter_running = false;
    session->presenter_failed = false;
    session->frame_pending = false;
    session->clear_pending = false;
    session->shown_image_id = session->options.image_id_b;
    (void)memset(&session->stats, 0, sizeof(session->stats));
    winch_flag = 0;

    /* Non-blocking output: neither the presenter nor the async-signal
     * restore may ever hang on a stalled terminal connection. */
    int flags = fcntl(output_fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    session->saved_output_flags = flags;
    session->output_flags_saved = true;
    if ((flags & O_NONBLOCK) == 0 &&
        fcntl(output_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        session->output_flags_saved = false;
        return -1;
    }

    session->termios_saved = false;
    if (session->options.manage_raw_mode) {
        if (tcgetattr(input_fd, &session->saved_termios) != 0) {
            goto fail;
        }
        session->termios_saved = true;
        struct termios raw = session->saved_termios;
        raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG | IEXTEN);
        raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_oflag &= (tcflag_t)~OPOST;
        raw.c_cflag |= CS8;
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(input_fd, TCSAFLUSH, &raw) != 0) {
            session->termios_saved = false;
            goto fail;
        }
    }

    if (session->options.probe_graphics &&
        getenv("KITTYFB_SKIP_PROBE") == NULL &&
        !probe_for_graphics(session)) {
        errno = ENOTSUP;
        goto fail;
    }

    session->winch_handler_installed = false;
    if (session->options.install_winch_handler) {
        struct sigaction action;
        (void)memset(&action, 0, sizeof(action));
        action.sa_handler = handle_winch;
        (void)sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        if (sigaction(SIGWINCH, &action, &session->saved_winch_action) == 0) {
            session->winch_handler_installed = true;
        }
    }

    /* alt screen, hide cursor, clear */
    printed = snprintf(
        setup,
        sizeof(setup),
        "%s%s\x1b[2J\x1b[H",
        session->options.manage_alt_screen ? "\x1b[?1049h" : "",
        session->options.hide_cursor ? "\x1b[?25l" : "");
    if (printed < 0 || (size_t)printed >= sizeof(setup) ||
        !write_all(session, setup, (size_t)printed)) {
        goto fail;
    }

    session->active = 1;
    return 0;

fail:
    failure = errno;
    if (session->winch_handler_installed) {
        (void)sigaction(SIGWINCH, &session->saved_winch_action, NULL);
        session->winch_handler_installed = false;
    }
    if (session->termios_saved) {
        (void)tcsetattr(input_fd, TCSAFLUSH, &session->saved_termios);
        session->termios_saved = false;
    }
    if (session->output_flags_saved) {
        (void)fcntl(output_fd, F_SETFL, session->saved_output_flags);
        session->output_flags_saved = false;
    }
    errno = failure;
    return -1;
}

bool kittyfb_check_resize(kittyfb_session *session, int *width, int *height)
{
    struct winsize window;
    kittyfb_geometry geometry;
    char origin[sizeof(session->origin_sequence)];

    if (session == NULL || !session->active) {
        return false;
    }
    /* Consume the SIGWINCH hint, but measure regardless: the polled
     * TIOCGWINSZ catches resizes even when the signal was missed or the
     * handler was never installed. */
    winch_flag = 0;
    if (ioctl(session->output_fd, TIOCGWINSZ, &window) != 0) {
        return false;
    }
    if (!kittyfb_derive_geometry(window.ws_col, window.ws_row,
                                 window.ws_xpixel, window.ws_ypixel,
                                 &session->options, &geometry)) {
        return false;
    }
    int printed = snprintf(
        origin,
        sizeof(origin),
        "\x1b[%d;%dH",
        geometry.origin_row,
        geometry.origin_column);
    if (printed < 0 || (size_t)printed >= sizeof(origin)) {
        return false;
    }

    pthread_mutex_lock(&session->frame_lock);
    bool size_changed = geometry.width != session->width ||
                        geometry.height != session->height;
    bool anything_changed =
        size_changed ||
        geometry.cell_width != session->cell_width ||
        geometry.cell_height != session->cell_height ||
        strcmp(origin, session->origin_sequence) != 0;
    if (anything_changed) {
        session->width = geometry.width;
        session->height = geometry.height;
        session->cell_width = geometry.cell_width;
        session->cell_height = geometry.cell_height;
        memcpy(session->origin_sequence, origin,
               sizeof(session->origin_sequence));
        /* The presenter wipes stale cells inside its next synchronized
         * update; clearing here would interleave with an in-flight
         * frame write. */
        session->clear_pending = true;
    }
    pthread_mutex_unlock(&session->frame_lock);

    if (size_changed) {
        if (width != NULL) {
            *width = geometry.width;
        }
        if (height != NULL) {
            *height = geometry.height;
        }
    }
    return size_changed;
}

/* One-shot guard shared by the normal and signal-handler restore paths. */
static bool claim_shutdown(kittyfb_session *session)
{
    return !__sync_lock_test_and_set(&session->shutdown_claimed, 1);
}

static void restore_terminal(kittyfb_session *session)
{
    if (session->emergency_length > 0) {
        (void)write_all(session, session->emergency,
                        session->emergency_length);
    }
    if (session->termios_saved) {
        (void)tcsetattr(session->input_fd, TCSAFLUSH,
                        &session->saved_termios);
        session->termios_saved = false;
    }
    if (session->output_flags_saved) {
        (void)fcntl(session->output_fd, F_SETFL,
                    session->saved_output_flags);
        session->output_flags_saved = false;
    }
    if (session->winch_handler_installed) {
        (void)sigaction(SIGWINCH, &session->saved_winch_action, NULL);
        session->winch_handler_installed = false;
    }
}

void kittyfb_stop(kittyfb_session *session)
{
    if (session == NULL || (!session->active && !session->presenter_started)) {
        return;
    }
    /* Stop the presenter first so no frame write interleaves with the
     * restore sequence.  This also reclaims the thread and buffers after
     * an emergency restore already released the terminal. */
    presenter_shutdown(session);
    if (claim_shutdown(session)) {
        restore_terminal(session);
    }
    session->active = 0;
}

void kittyfb_emergency_restore(kittyfb_session *session)
{
    if (session == NULL) {
        return;
    }
    /* Fence the presenter first (an async-signal-safe flag write): the
     * thread cannot be joined from a signal handler, so this stops it
     * emitting bytes that would interleave with the restore below. */
    session->presenter_disabled = 1;
    if (!claim_shutdown(session)) {
        return;
    }
    /* write, tcsetattr, fcntl and sigaction are async-signal-safe; the
     * output descriptor is non-blocking, so nothing here can hang. */
    if (session->emergency_length > 0) {
        (void)write(session->output_fd, session->emergency,
                    session->emergency_length);
    }
    if (session->termios_saved) {
        (void)tcsetattr(session->input_fd, TCSAFLUSH,
                        &session->saved_termios);
    }
    if (session->output_flags_saved) {
        (void)fcntl(session->output_fd, F_SETFL,
                    session->saved_output_flags);
    }
    session->active = 0;
}
