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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

int kittyfb_snap_axis(int value, int cell, int minimum, int maximum)
{
    int64_t step;
    int64_t snapped;

    if (cell <= 0 || minimum <= 0 || maximum < minimum) {
        return 0;
    }
    if (value < minimum) {
        value = minimum;
    } else if (value > maximum) {
        value = maximum;
    }

    /* An odd cell dimension needs a two-cell step for an even pixel size. */
    step = (cell % 2 == 0) ? cell : (int64_t)cell * 2;
    snapped = ((int64_t)value / step) * step;
    if (snapped < minimum) {
        int64_t rounded_minimum =
            (((int64_t)minimum + step - 1) / step) * step;

        snapped = rounded_minimum <= maximum
            ? rounded_minimum
            : ((int64_t)maximum / step) * step;
    }
    if (snapped <= 0) {
        /* No cell-aligned value fits. Preserve the hard maximum bound. */
        snapped = (int64_t)maximum & ~INT64_C(1);
    }
    return (int)snapped;
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
    /* Snap to whole, even cell groups without crossing back below the
     * requested minimum. A minimum can encode a required integer scale,
     * so rounding below it is more harmful than covering one extra cell. */
    width = kittyfb_snap_axis(
        width, cell_width, options->min_width, options->max_width);
    height = kittyfb_snap_axis(
        height, cell_height, options->min_height, options->max_height);
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

size_t kittyfb_build_shm_packet(
    char *output,
    size_t capacity,
    const char *shm_name,
    int new_id,
    int old_id,
    int width,
    int height,
    const char *origin,
    bool clear_first)
{
    char encoded[KITTYFB_SHM_NAME_MAX * 4 / 3 + 8];
    char *at = output;
    size_t remaining = capacity;
    size_t name_length;
    size_t encoded_length;
    int printed;

    if (output == NULL || shm_name == NULL || origin == NULL ||
        new_id <= 0 || old_id <= 0 || width <= 0 || height <= 0) {
        return 0;
    }
    name_length = strlen(shm_name);
    if (name_length == 0 || name_length >= KITTYFB_SHM_NAME_MAX) {
        return 0;
    }
    encoded_length = kittyfb_base64_encode(
        (const uint8_t *)shm_name, name_length, encoded);

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

    /* f=32: the object holds RGBA.  t=s: the payload is a name, not
     * pixels, so there is nothing to chunk and nothing to compress. */
    printed = snprintf(
        at,
        remaining,
        "\x1b_Ga=T,f=32,i=%d,q=2,t=s,s=%d,v=%d;%.*s\x1b\\",
        new_id,
        width,
        height,
        (int)encoded_length,
        encoded);
    if (printed < 0 || (size_t)printed >= remaining) {
        return 0;
    }
    at += printed;
    remaining -= (size_t)printed;

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
        if (__atomic_load_n(&session->presenter_disabled, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&session->write_cancel, __ATOMIC_ACQUIRE)) {
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
    options->enter_sequence = NULL;
    options->leave_sequence = NULL;
    options->min_width = 640;
    options->min_height = 400;
    options->max_width = 1600;
    options->max_height = 1000;
    options->image_id_a = 1;
    options->image_id_b = 2;
    options->zlib_level = 1;
    options->transport = KITTYFB_TRANSPORT_AUTO;
    options->shm_slots = 3;
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
    bool failed;
    kittyfb_session *mutable_session;

    if (session == NULL) return false;
    /* Locking is logically const: it protects the snapshot without changing
     * the session's observable state. */
    mutable_session = (kittyfb_session *)session;
    pthread_mutex_lock(&mutable_session->frame_lock);
    failed = mutable_session->presenter_failed;
    pthread_mutex_unlock(&mutable_session->frame_lock);
    return failed;
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

/* -------------------------- shared-memory ring -------------------------- */

/*
 * Slot names must be unique across concurrent sessions in this process
 * and across processes.  The pid separates processes and this counter
 * separates sessions within one; a pid recycled from a dead process can
 * still collide with its leaked objects, which shm_slot_publish() handles
 * by unlinking the stale name and retrying once.
 */
static unsigned shm_session_serial;

static void shm_slot_release(struct kittyfb_shm_slot *slot)
{
    if (slot->mapping != NULL) {
        (void)munmap(slot->mapping, slot->mapping_size);
        slot->mapping = NULL;
    }
    if (slot->fd >= 0) {
        (void)close(slot->fd);
        slot->fd = -1;
    }
    slot->mapping_size = 0;
    slot->busy = false;
}

/*
 * Free every slot the terminal has consumed.  Kitty unlinks a t=s object
 * as soon as it has read it, so a name that no longer resolves is an
 * acknowledgement.  Returns the number of free slots.
 */
static int shm_ring_reap(kittyfb_session *session)
{
    int free_count = 0;

    for (int index = 0; index < session->shm_slot_count; index++) {
        struct kittyfb_shm_slot *slot = &session->shm_slots[index];
        if (!slot->busy) {
            free_count++;
            continue;
        }
        int probe = shm_open(slot->name, O_RDONLY, 0);
        if (probe >= 0) {
            (void)close(probe);
            continue;   /* still unread */
        }
        if (errno == ENOENT) {
            shm_slot_release(slot);
            free_count++;
        }
        /* Any other errno leaves the slot busy: it will be retried on the
         * next frame, and the ring degrades to fewer slots rather than
         * handing the terminal an object it may still be reading. */
    }
    return free_count;
}

/*
 * Copy `size` bytes into a free slot and return its name, or NULL when
 * every slot is still in flight (the caller drops the frame) or the
 * object could not be created.  *saturated distinguishes the two.
 */
static const char *shm_ring_publish(
    kittyfb_session *session,
    const uint8_t *data,
    size_t size,
    bool *saturated)
{
    struct kittyfb_shm_slot *slot = NULL;

    *saturated = false;
    if (shm_ring_reap(session) == 0) {
        *saturated = true;
        return NULL;
    }
    for (int index = 0; index < session->shm_slot_count; index++) {
        if (!session->shm_slots[index].busy) {
            slot = &session->shm_slots[index];
            break;
        }
    }
    if (slot == NULL) {
        *saturated = true;
        return NULL;
    }

    int fd = shm_open(slot->name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 && errno == EEXIST) {
        /* A leaked object from a dead process whose pid we now carry.
         * Nothing live can own it: the name embeds our own pid. */
        (void)shm_unlink(slot->name);
        fd = shm_open(slot->name, O_RDWR | O_CREAT | O_EXCL, 0600);
    }
    if (fd < 0) {
        return NULL;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        (void)close(fd);
        (void)shm_unlink(slot->name);
        return NULL;
    }
    void *mapping = mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        (void)close(fd);
        (void)shm_unlink(slot->name);
        return NULL;
    }
    memcpy(mapping, data, size);

    slot->fd = fd;
    slot->mapping = mapping;
    slot->mapping_size = size;
    slot->busy = true;
    return slot->name;
}

static void shm_ring_destroy(kittyfb_session *session)
{
    if (session->shm_slots == NULL) {
        return;
    }
    for (int index = 0; index < session->shm_slot_count; index++) {
        struct kittyfb_shm_slot *slot = &session->shm_slots[index];
        /* Unlink unconditionally: a slot still in flight at teardown was
         * never read, and leaving it behind leaks a frame of tmpfs until
         * the next reboot. */
        (void)shm_unlink(slot->name);
        shm_slot_release(slot);
    }
    free(session->shm_slots);
    session->shm_slots = NULL;
    session->shm_slot_count = 0;
    session->shm_active = false;
}

/*
 * Allocate the ring and prove shared memory actually works by creating
 * and removing one object.  Returns false when the transport is
 * unavailable, which for AUTO means falling back to INLINE.
 */
static bool shm_ring_create(kittyfb_session *session, int slot_count)
{
    unsigned serial = shm_session_serial++;
    struct kittyfb_shm_slot *slots =
        calloc((size_t)slot_count, sizeof(*slots));

    if (slots == NULL) {
        return false;
    }
    for (int index = 0; index < slot_count; index++) {
        slots[index].fd = -1;
        int printed = snprintf(
            slots[index].name,
            sizeof(slots[index].name),
            "/kilix-fb-%ld-%u-%d",
            (long)getpid(),
            serial,
            index);
        if (printed < 0 || (size_t)printed >= sizeof(slots[index].name)) {
            free(slots);
            return false;
        }
    }

    /* A probe is the only way to know shm_open() is usable here: it fails
     * on systems without /dev/shm mounted, and inside containers that
     * deny it, and neither is visible any other way. */
    int probe = shm_open(slots[0].name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (probe < 0 && errno == EEXIST) {
        (void)shm_unlink(slots[0].name);
        probe = shm_open(slots[0].name, O_RDWR | O_CREAT | O_EXCL, 0600);
    }
    if (probe < 0) {
        free(slots);
        return false;
    }
    (void)close(probe);
    (void)shm_unlink(slots[0].name);

    session->shm_slots = slots;
    session->shm_slot_count = slot_count;
    return true;
}

/*
 * Best-effort cleanup of objects left behind by a process that died
 * without unwinding - a SIGKILL, or a crash before kittyfb_stop().  The
 * emergency restore deliberately does not do this: it is async-signal
 * safe and unlinking a slot array in a signal handler is not worth the
 * risk, so the names carry the owning pid instead and a later run
 * reclaims them.
 *
 * Only objects whose pid no longer exists are removed, so a running
 * session's frames are never pulled out from under it.  Linux exposes
 * shared memory as a directory; elsewhere this does nothing, which is
 * correct rather than merely tolerable - the leak is bounded by
 * shm_slots frames per crashed process and tmpfs is cleared on reboot.
 */
int kittyfb_reap_orphans(void)
{
    static const char prefix[] = "kilix-fb-";
    DIR *directory = opendir("/dev/shm");
    struct dirent *entry;
    int reaped = 0;

    if (directory == NULL) {
        return 0;
    }
    while ((entry = readdir(directory)) != NULL) {
        char name[KITTYFB_SHM_NAME_MAX];
        long owner;
        char *end;
        int printed;

        if (strncmp(entry->d_name, prefix, sizeof(prefix) - 1u) != 0) {
            continue;
        }
        errno = 0;
        owner = strtol(entry->d_name + sizeof(prefix) - 1u, &end, 10);
        if (errno != 0 || end == entry->d_name + sizeof(prefix) - 1u ||
            *end != '-' || owner <= 0) {
            continue;
        }
        /* ESRCH is the only answer that proves the owner is gone.  EPERM
         * means a live process this user may not signal. */
        if (kill((pid_t)owner, 0) == 0 || errno != ESRCH) {
            continue;
        }
        printed = snprintf(name, sizeof(name), "/%s", entry->d_name);
        if (printed < 0 || (size_t)printed >= sizeof(name)) {
            continue;
        }
        if (shm_unlink(name) == 0) {
            reaped++;
        }
    }
    (void)closedir(directory);
    return reaped;
}

/* ------------------------------- encoding ------------------------------- */

/* Runs on the presenter thread, or on the caller when thread creation
 * failed; either way it is the only user of the encoder scratch. */
/*
 * The shared-memory path: no alpha strip and no compression, because
 * neither byte saving buys anything once the pixels stop travelling down
 * the terminal connection.  The frame is copied once, into the slot.
 *
 * Returns true when the packet was written.  On a saturated ring it
 * returns false with *dropped set: that is a dropped frame, matching the
 * newest-frame-wins policy, and must not be mistaken for a failure.
 */
static bool publish_shm(
    kittyfb_session *session,
    const uint8_t *rgba,
    int width,
    int height,
    const char *origin,
    bool clear_first,
    bool *dropped)
{
    size_t size = (size_t)width * (size_t)height * 4u;
    bool saturated = false;
    const char *name = shm_ring_publish(session, rgba, size, &saturated);

    if (name == NULL) {
        *dropped = saturated;
        return false;
    }

    int new_id = session->shown_image_id == session->options.image_id_a
                     ? session->options.image_id_b
                     : session->options.image_id_a;

    size_t packet_needed = KITTYFB_SHM_NAME_MAX * 2u + 512u;
    if (!grow_chars(&session->packet_buffer, &session->packet_capacity,
                    packet_needed)) {
        return false;
    }
    size_t packet_length = kittyfb_build_shm_packet(
        session->packet_buffer,
        session->packet_capacity,
        name,
        new_id,
        session->shown_image_id,
        width,
        height,
        origin,
        clear_first);
    if (packet_length == 0) {
        return false;
    }

    if (__atomic_load_n(&session->presenter_disabled, __ATOMIC_ACQUIRE)) {
        return false;
    }
    if (!write_all(session, session->packet_buffer, packet_length)) {
        return false;
    }
    session->shown_image_id = new_id;
    return true;
}

static bool encode_and_write(
    kittyfb_session *session,
    const uint8_t *rgba,
    int width,
    int height,
    const char *origin,
    bool clear_first,
    bool *dropped)
{
    *dropped = false;

    /* A signal-time restore has fenced the presenter: emit nothing. */
    if (__atomic_load_n(&session->presenter_disabled, __ATOMIC_ACQUIRE)) {
        return false;
    }
    if (rgba == NULL || width <= 0 || height <= 0) {
        return false;
    }
    if ((size_t)width > SIZE_MAX / 4u / (size_t)height) {
        return false;
    }

    if (session->shm_active) {
        return publish_shm(session, rgba, width, height, origin, clear_first,
                           dropped);
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
    if (__atomic_load_n(&session->presenter_disabled, __ATOMIC_ACQUIRE)) {
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

        bool dropped = false;
        bool encoded = encode_and_write(
            session,
            session->encode_buffer,
            width,
            height,
            origin,
            clear_first,
            &dropped);

        pthread_mutex_lock(&session->frame_lock);
        if (encoded) {
            session->stats.frames_encoded++;
        } else if (dropped) {
            /* Every shared-memory slot is still unread.  Dropping the
             * newest frame is the same bargain the pending slot already
             * makes: a slow terminal costs frames, never a stall. */
            session->stats.frames_dropped++;
        } else if (!__atomic_load_n(&session->write_cancel, __ATOMIC_ACQUIRE) &&
                   !__atomic_load_n(&session->presenter_disabled,
                                    __ATOMIC_ACQUIRE)) {
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
            bool dropped = false;
            bool encoded = encode_and_write(
                session, rgba, width, height, origin, clear_first, &dropped);
            pthread_mutex_lock(&session->frame_lock);
            if (encoded) {
                session->stats.frames_encoded++;
            } else if (dropped) {
                session->stats.frames_dropped++;
            } else {
                session->stats.encode_failures++;
            }
            pthread_mutex_unlock(&session->frame_lock);
            /* A dropped frame is not a presentation failure: the caller
             * should keep sending frames, exactly as it does when the
             * pending slot is overwritten. */
            return encoded || dropped;
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

/* Join the presenter.  Suspension retains high-water buffers so a
 * stop/start job-control cycle does not churn multi-megabyte allocations;
 * final shutdown releases them.  Safe when the thread was never started. */
static void presenter_shutdown(
    kittyfb_session *session,
    bool release_buffers)
{
    __atomic_store_n(&session->write_cancel, 1, __ATOMIC_RELEASE);
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
    if (release_buffers) {
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
        /* The presenter is joined, so nothing else can touch the ring.
         * Suspension deliberately keeps it, matching the frame buffers:
         * a resumed session reuses its slot names. */
        shm_ring_destroy(session);
    }
    pthread_mutex_unlock(&session->frame_lock);
    __atomic_store_n(&session->write_cancel, 0, __ATOMIC_RELEASE);
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
    bool enter_valid = options->enter_sequence == NULL ||
        memchr(options->enter_sequence, '\0',
               KITTYFB_CONTROL_SEQUENCE_MAX + 1u) != NULL;
    bool leave_valid = options->leave_sequence == NULL ||
        memchr(options->leave_sequence, '\0',
               KITTYFB_CONTROL_SEQUENCE_MAX + 1u) != NULL;
    return options->min_width > 0 && options->min_height > 0 &&
           options->min_width <= options->max_width &&
           options->min_height <= options->max_height &&
           options->image_id_a > 0 && options->image_id_b > 0 &&
           options->image_id_a != options->image_id_b &&
           options->zlib_level >= -1 && options->zlib_level <= 9 &&
           options->probe_timeout_ms >= 0 &&
           options->transport >= KITTYFB_TRANSPORT_AUTO &&
           options->transport <= KITTYFB_TRANSPORT_SHM &&
           options->shm_slots >= 1 &&
           options->shm_slots <= KITTYFB_SHM_SLOTS_MAX &&
           enter_valid && leave_valid;
}

/*
 * Decide the transport once, here, rather than per frame: a per-frame
 * decision is a per-frame syscall and a session whose behavior changes
 * under the caller.
 *
 * tmux disqualifies shared memory even when shm_open() works, because
 * tmux forwards the escape to a terminal that need not share this
 * process's /dev/shm - and a name it cannot open is a blank screen, not
 * a degraded one.  An explicit KITTYFB_TRANSPORT_SHM still honors the
 * caller's choice; only AUTO declines.
 */
static void resolve_transport(kittyfb_session *session)
{
    kittyfb_transport requested = session->options.transport;
    const char *override = getenv("KITTYFB_TRANSPORT");

    /* An environment override makes both paths reachable in a real
     * terminal without rebuilding the application, which is the only way
     * to check the one the application did not choose. */
    if (override != NULL) {
        if (strcmp(override, "inline") == 0) {
            requested = KITTYFB_TRANSPORT_INLINE;
        } else if (strcmp(override, "shm") == 0) {
            requested = KITTYFB_TRANSPORT_SHM;
        } else if (strcmp(override, "auto") == 0) {
            requested = KITTYFB_TRANSPORT_AUTO;
        }
    }

    session->shm_active = false;
    if (requested == KITTYFB_TRANSPORT_INLINE) {
        return;
    }
    if (requested == KITTYFB_TRANSPORT_AUTO) {
        const char *tmux = getenv("TMUX");
        if (tmux != NULL && tmux[0] != '\0') {
            return;
        }
    }
    if (shm_ring_create(session, session->options.shm_slots)) {
        session->shm_active = true;
    }
    /* An explicit SHM request that cannot be honored falls back to
     * inline rather than failing the start: a working picture beats a
     * refused session, and kittyfb_active_transport() reports it. */
}

kittyfb_transport kittyfb_active_transport(const kittyfb_session *session)
{
    if (session == NULL || !session->shm_active) {
        return KITTYFB_TRANSPORT_INLINE;
    }
    return KITTYFB_TRANSPORT_SHM;
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
        "\x1b[?2026l%s%s%s",
        session->options.image_id_a,
        session->options.image_id_b,
        session->options.leave_sequence != NULL
            ? session->options.leave_sequence : "",
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
    bool setup_started = false;

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
    __atomic_store_n(&session->presenter_disabled, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&session->write_cancel, 0, __ATOMIC_RELEASE);
    session->presenter_started = false;
    session->presenter_running = false;
    session->presenter_failed = false;
    session->frame_pending = false;
    session->clear_pending = false;
    session->shown_image_id = session->options.image_id_b;
    (void)memset(&session->stats, 0, sizeof(session->stats));
    winch_flag = 0;

    /* A suspended session keeps its ring; a fresh one builds a new one
     * under a new serial so two sessions never share slot names. */
    if (session->shm_slots == NULL) {
        resolve_transport(session);
    }

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
    setup_started = true;
    if (printed < 0 || (size_t)printed >= sizeof(setup) ||
        !write_all(session, setup, (size_t)printed)) {
        goto fail;
    }
    if (session->options.enter_sequence != NULL &&
        !write_all(session, session->options.enter_sequence,
                   strlen(session->options.enter_sequence))) {
        goto fail;
    }

    session->active = 1;
    return 0;

fail:
    failure = errno;
    if (setup_started && session->emergency_length > 0) {
        (void)write_all(session, session->emergency,
                        session->emergency_length);
    }
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

static void restore_process_state(kittyfb_session *session)
{
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

static void restore_terminal(kittyfb_session *session)
{
    if (session->emergency_length > 0) {
        (void)write_all(session, session->emergency,
                        session->emergency_length);
    }
    restore_process_state(session);
}

void kittyfb_stop(kittyfb_session *session)
{
    bool restore;
    bool retained;

    if (session == NULL) {
        return;
    }
    restore = session->active || session->presenter_started;
    retained =
        session->pending_buffer != NULL ||
        session->encode_buffer != NULL ||
        session->rgb_buffer != NULL ||
        session->z_buffer != NULL ||
        session->b64_buffer != NULL ||
        session->packet_buffer != NULL;
    if (!restore && !retained) return;
    /* Stop the presenter first so no frame write interleaves with the
     * restore sequence.  This also reclaims the thread and buffers after
     * an emergency restore already released the terminal. */
    presenter_shutdown(session, true);
    if (restore) {
        if (claim_shutdown(session)) {
            restore_terminal(session);
        } else {
            /* The signal-safe path cannot clear ordinary bool bookkeeping or
             * restore the process's previous SIGWINCH disposition.  Retry the
             * idempotent OS-state restoration here without emitting the
             * terminal escape sequence a second time. */
            restore_process_state(session);
        }
    }
    session->active = 0;
}

void kittyfb_suspend(kittyfb_session *session)
{
    if (session == NULL ||
        (!session->active && !session->presenter_started)) {
        return;
    }
    presenter_shutdown(session, false);
    if (claim_shutdown(session)) {
        restore_terminal(session);
    } else {
        restore_process_state(session);
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
    __atomic_store_n(&session->presenter_disabled, 1, __ATOMIC_RELEASE);
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
