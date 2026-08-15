#include "kitty_framebuffer.h"
#include "kitty_framebuffer_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(                                                    \
                stderr,                                                       \
                "%s:%d: check failed: %s\n",                               \
                __FILE__,                                                     \
                __LINE__,                                                     \
                #condition);                                                  \
            return false;                                                     \
        }                                                                     \
    } while (false)

/* The reply a graphics-capable terminal sends to the paired probe: the
 * APC query answer followed by a primary device-attributes response. */
static const char graphics_reply[] = "\x1b_Gi=31;OK\x1b\\\x1b[?62;4c";
static const char da1_only_reply[] = "\x1b[?6c";

static void test_winch_handler(int signal_number)
{
    (void)signal_number;
}

static int64_t
monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000 + (int64_t)now.tv_nsec / 1000000;
}

static void
sleep_milliseconds(int milliseconds)
{
    struct timespec pause;

    pause.tv_sec = milliseconds / 1000;
    pause.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    (void)nanosleep(&pause, NULL);
}

static size_t
find_bytes(
    const char *haystack,
    size_t haystack_size,
    const char *needle,
    size_t needle_size)
{
    size_t index;

    if (needle_size > haystack_size) {
        return SIZE_MAX;
    }
    for (index = 0u; index + needle_size <= haystack_size; ++index) {
        if (memcmp(haystack + index, needle, needle_size) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

static bool
contains_bytes(
    const char *haystack,
    size_t haystack_size,
    const char *needle,
    size_t needle_size)
{
    return find_bytes(haystack, haystack_size, needle, needle_size) !=
           SIZE_MAX;
}

static bool
contains_str(const char *haystack, size_t haystack_size, const char *needle)
{
    return contains_bytes(haystack, haystack_size, needle, strlen(needle));
}

static bool
starts_with(const char *haystack, size_t haystack_size, const char *needle)
{
    size_t needle_size = strlen(needle);

    return needle_size <= haystack_size &&
           memcmp(haystack, needle, needle_size) == 0;
}

static bool
ends_with(const char *haystack, size_t haystack_size, const char *needle)
{
    size_t needle_size = strlen(needle);

    return needle_size <= haystack_size &&
           memcmp(haystack + haystack_size - needle_size, needle,
                  needle_size) == 0;
}

static size_t
count_bytes(
    const char *haystack,
    size_t haystack_size,
    const char *needle,
    size_t needle_size)
{
    size_t count = 0u;
    size_t offset = 0u;

    while (offset + needle_size <= haystack_size) {
        size_t found = find_bytes(
            haystack + offset,
            haystack_size - offset,
            needle,
            needle_size);
        if (found == SIZE_MAX) {
            break;
        }
        ++count;
        offset += found + needle_size;
    }
    return count;
}

static size_t
read_available(int fd, char *output, size_t capacity)
{
    size_t total = 0u;

    while (total < capacity) {
        const ssize_t count = read(fd, output + total, capacity - total);

        if (count > 0) {
            total += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    return total;
}

/* Accumulate bytes from fd into buffer until needle appears or the
 * timeout expires; *used carries the running fill across calls. */
static bool
wait_for_bytes(
    int fd,
    char *buffer,
    size_t capacity,
    size_t *used,
    const char *needle,
    size_t needle_size)
{
    const int64_t deadline = monotonic_milliseconds() + 3000;

    for (;;) {
        struct pollfd descriptor;
        int ready;

        if (contains_bytes(buffer, *used, needle, needle_size)) {
            return true;
        }
        if (monotonic_milliseconds() >= deadline || *used >= capacity) {
            return false;
        }
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        do {
            ready = poll(&descriptor, 1u, 100);
        } while (ready < 0 && errno == EINTR);
        if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
            *used += read_available(fd, buffer + *used, capacity - *used);
        }
    }
}

static void
drain_descriptor(int fd)
{
    char scratch[8192];

    sleep_milliseconds(20);
    (void)read_available(fd, scratch, sizeof(scratch));
}

typedef struct wire_drainer {
    int fd;
    int stop;
    uint64_t bytes;
} wire_drainer;

static void *
wire_drainer_main(void *opaque)
{
    wire_drainer *drainer = opaque;
    char scratch[16384];

    while (!__atomic_load_n(&drainer->stop, __ATOMIC_ACQUIRE)) {
        struct pollfd descriptor = {drainer->fd, POLLIN, 0};
        int ready = poll(&descriptor, 1u, 20);

        if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
            const ssize_t count = read(drainer->fd, scratch, sizeof(scratch));
            if (count > 0) {
                drainer->bytes += (uint64_t)count;
            }
        } else if (ready < 0 && errno != EINTR) {
            break;
        }
    }
    return NULL;
}

static int
base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static size_t
base64_decode(const char *input, size_t length, uint8_t *output)
{
    uint32_t accumulator = 0u;
    int bits = 0;
    size_t out = 0u;
    size_t index;

    for (index = 0u; index < length; ++index) {
        int value;

        if (input[index] == '=') {
            break;
        }
        value = base64_value(input[index]);
        if (value < 0) {
            return 0u;
        }
        accumulator = (accumulator << 6) | (uint32_t)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output[out++] = (uint8_t)((accumulator >> bits) & 0xffu);
        }
    }
    return out;
}

/* Open a pty pair with the given cell and pixel geometry, put the slave
 * in raw mode (so pre-written probe replies are neither echoed nor held
 * in a canonical line), and make the master non-blocking. */
static bool
open_test_pty(
    int *master,
    int *slave,
    int columns,
    int rows,
    int xpixel,
    int ypixel,
    struct termios *original)
{
    struct winsize window;
    struct termios raw;
    int flags;

    (void)memset(&window, 0, sizeof(window));
    window.ws_col = (unsigned short)columns;
    window.ws_row = (unsigned short)rows;
    window.ws_xpixel = (unsigned short)xpixel;
    window.ws_ypixel = (unsigned short)ypixel;
    if (openpty(master, slave, NULL, NULL, &window) != 0) {
        return false;
    }
    if (tcgetattr(*slave, &raw) != 0) {
        return false;
    }
    cfmakeraw(&raw);
    if (tcsetattr(*slave, TCSANOW, &raw) != 0) {
        return false;
    }
    if (original != NULL) {
        *original = raw;
    }
    flags = fcntl(*master, F_GETFL);
    if (flags < 0 || fcntl(*master, F_SETFL, flags | O_NONBLOCK) != 0) {
        return false;
    }
    return true;
}

static bool
same_termios(const struct termios *a, const struct termios *b)
{
    return a->c_iflag == b->c_iflag && a->c_oflag == b->c_oflag &&
           a->c_cflag == b->c_cflag && a->c_lflag == b->c_lflag;
}

static void
fill_test_frame(uint8_t *rgba, int width, int height, uint8_t salt)
{
    int x;
    int y;

    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
            size_t at = ((size_t)y * (size_t)width + (size_t)x) * 4u;

            rgba[at] = (uint8_t)(x * 7 + salt);
            rgba[at + 1] = (uint8_t)(y * 13 + salt);
            rgba[at + 2] = (uint8_t)(x ^ y);
            rgba[at + 3] = 255u;
        }
    }
}

static void
fill_noise_frame(uint8_t *rgba, int width, int height, uint32_t seed)
{
    size_t pixels = (size_t)width * (size_t)height;

    for (size_t index = 0u; index < pixels; ++index) {
        for (size_t channel = 0u; channel < 3u; ++channel) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            rgba[index * 4u + channel] = (uint8_t)seed;
        }
        rgba[index * 4u + 3u] = 255u;
    }
}

/* ------------------------------ pure tests ------------------------------ */

static bool
test_base64_encoding(void)
{
    static const struct {
        const char *input;
        const char *expected;
    } cases[] = {
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"}
    };
    static const uint8_t zeroes[3] = {0u, 0u, 0u};
    static const uint8_t high[3] = {0xffu, 0xffu, 0xfeu};
    char output[64];
    size_t index;

    CHECK(kittyfb_base64_encode((const uint8_t *)"", 0u, output) == 0u);
    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        size_t length = kittyfb_base64_encode(
            (const uint8_t *)cases[index].input,
            strlen(cases[index].input),
            output);
        CHECK(length == strlen(cases[index].expected));
        CHECK(memcmp(output, cases[index].expected, length) == 0);
    }
    CHECK(kittyfb_base64_encode(zeroes, 3u, output) == 4u);
    CHECK(memcmp(output, "AAAA", 4u) == 0);
    CHECK(kittyfb_base64_encode(high, 3u, output) == 4u);
    CHECK(memcmp(output, "///+", 4u) == 0);
    return true;
}

static bool
test_geometry_derivation(void)
{
    kittyfb_options options;
    kittyfb_geometry geometry;

    kittyfb_options_init(&options);

    /* Typical reported geometry: fills the grid minus the prompt row. */
    CHECK(kittyfb_derive_geometry(100, 30, 900, 540, &options, &geometry));
    CHECK(geometry.cell_width == 9 && geometry.cell_height == 18);
    CHECK(geometry.width == 900 && geometry.height == 522);
    CHECK(geometry.origin_row == 1 && geometry.origin_column == 1);

    /* Huge terminal: clamped to the max bounds and centered. */
    CHECK(kittyfb_derive_geometry(300, 80, 3000, 1600, &options, &geometry));
    CHECK(geometry.cell_width == 10 && geometry.cell_height == 20);
    CHECK(geometry.width == 1600 && geometry.height == 1000);
    CHECK(geometry.origin_row == 15 && geometry.origin_column == 71);

    /* No pixel report: 9x18 cells are assumed. */
    CHECK(kittyfb_derive_geometry(80, 24, 0, 0, &options, &geometry));
    CHECK(geometry.cell_width == 9 && geometry.cell_height == 18);
    CHECK(geometry.width == 720 && geometry.height == 414);
    CHECK(geometry.origin_row == 1 && geometry.origin_column == 1);

    /* Zero cells fall back to an 80x24 grid. */
    CHECK(kittyfb_derive_geometry(0, 0, 0, 0, &options, &geometry));
    CHECK(geometry.width == 720 && geometry.height == 414);

    /* The pure helper accepts int inputs, not only unsigned-short ioctl
     * fields.  Extreme synthetic geometry must clamp without signed
     * multiplication or ceil-division overflow. */
    CHECK(kittyfb_derive_geometry(INT_MAX, INT_MAX, 0, 0,
                                  &options, &geometry));
    CHECK(geometry.width == 1584);
    CHECK(geometry.height == 990);
    CHECK(geometry.origin_row > 0 && geometry.origin_column > 0);

    CHECK(!kittyfb_derive_geometry(80, 24, 0, 0, NULL, &geometry));
    CHECK(!kittyfb_derive_geometry(80, 24, 0, 0, &options, NULL));
    return true;
}

static bool
test_geometry_small_terminal_clamps(void)
{
    kittyfb_options options;
    kittyfb_geometry geometry;

    kittyfb_options_init(&options);

    /* A tiny terminal upscales to the first complete, even cell group
     * at or above the minimum; the origin clamps to the top-left. */
    CHECK(kittyfb_derive_geometry(40, 12, 0, 0, &options, &geometry));
    CHECK(geometry.width == 648 && geometry.height == 414);
    CHECK(geometry.width % 2 == 0 && geometry.height % 2 == 0);
    CHECK(geometry.origin_row == 1 && geometry.origin_column == 1);

    /* The maximum remains hard. Odd-width cells use two-cell groups so
     * the result stays both cell-aligned and even. */
    options.max_width = 700;
    options.max_height = 500;
    CHECK(kittyfb_derive_geometry(100, 30, 900, 540, &options, &geometry));
    CHECK(geometry.width == 684 && geometry.height == 486);

    /* A 480-pixel minimum must not snap down to 476 with 34-pixel cells:
     * callers use the minimum to protect an integer render scale. */
    kittyfb_options_init(&options);
    options.min_height = 480;
    CHECK(kittyfb_derive_geometry(80, 15, 720, 510, &options, &geometry));
    CHECK(geometry.height == 510);

    CHECK(kittyfb_snap_axis(640, 9, 640, 1600) == 648);
    CHECK(kittyfb_snap_axis(480, 34, 480, 1000) == 510);
    CHECK(kittyfb_snap_axis(700, 9, 640, 700) == 684);
    CHECK(kittyfb_snap_axis(500, 18, 400, 500) == 486);
    return true;
}

static bool
test_packet_chunk_boundaries(void)
{
    enum { PAYLOAD_MAX = 10000 };
    static char payload[PAYLOAD_MAX];
    static char packet[PAYLOAD_MAX + 2048];
    size_t length;
    size_t header_end;
    static const char header_4096[] =
        "\x1b_Ga=T,f=24,i=1,q=2,o=z,s=640,v=400,z=-1073741825,m=0;";
    static const char header_more[] =
        "\x1b_Ga=T,f=24,i=1,q=2,o=z,s=640,v=400,z=-1073741825,m=1;";

    (void)memset(payload, 'A', sizeof(payload));

    /* Exactly one chunk's worth stays a single m=0 escape. */
    length = kittyfb_build_packet(
        packet, sizeof(packet), payload, 4096u,
        1, 2, 640, 400, "\x1b[1;1H", false);
    CHECK(length > 0u);
    CHECK(contains_str(packet, length, header_4096));
    CHECK(!contains_str(packet, length, "\x1b_Gm="));

    /* One byte over the boundary splits into m=1 then a 1-byte m=0. */
    length = kittyfb_build_packet(
        packet, sizeof(packet), payload, 4097u,
        1, 2, 640, 400, "\x1b[1;1H", false);
    CHECK(length > 0u);
    header_end = find_bytes(packet, length, header_more,
                            strlen(header_more));
    CHECK(header_end != SIZE_MAX);
    header_end += strlen(header_more);
    /* the first chunk carries exactly 4096 payload bytes before its ST */
    CHECK(packet[header_end + 4096u] == '\x1b');
    CHECK(packet[header_end + 4097u] == '\\');
    CHECK(contains_str(packet, length, "\x1b_Gm=0;A\x1b\\"));

    /* 10000 bytes make chunks of 4096 + 4096 + 1808. */
    length = kittyfb_build_packet(
        packet, sizeof(packet), payload, 10000u,
        1, 2, 640, 400, "\x1b[1;1H", false);
    CHECK(length > 0u);
    CHECK(count_bytes(packet, length, "\x1b_Gm=1;", 7u) == 1u);
    CHECK(count_bytes(packet, length, "\x1b_Gm=0;", 7u) == 1u);

    /* Insufficient capacity is reported, never overrun. */
    CHECK(kittyfb_build_packet(
              packet, 512u, payload, 4096u,
              1, 2, 640, 400, "\x1b[1;1H", false) == 0u);
    CHECK(kittyfb_build_packet(
              packet, sizeof(packet), payload, 0u,
              1, 2, 640, 400, "\x1b[1;1H", false) == 0u);
    return true;
}

static bool
test_packet_wrapper_and_delete(void)
{
    static const char payload[] = "AAAA";
    char packet[512];
    size_t length;
    static const char cleared_prefix[] = "\x1b[?2026h\x1b[2J\x1b[3;5H";
    static const char plain_prefix[] = "\x1b[?2026h\x1b[3;5H";
    static const char trailer[] = "\x1b_Ga=d,d=I,i=9,q=2\x1b\\\x1b[?2026l";

    length = kittyfb_build_packet(
        packet, sizeof(packet), payload, sizeof(payload) - 1u,
        7, 9, 320, 200, "\x1b[3;5H", true);
    CHECK(length > 0u);
    CHECK(starts_with(packet, length, cleared_prefix));
    CHECK(contains_str(
        packet, length,
        "\x1b_Ga=T,f=24,i=7,q=2,o=z,s=320,v=200,z=-1073741825,m=0;AAAA\x1b\\"));
    CHECK(ends_with(packet, length, trailer));

    length = kittyfb_build_packet(
        packet, sizeof(packet), payload, sizeof(payload) - 1u,
        7, 9, 320, 200, "\x1b[3;5H", false);
    CHECK(length > 0u);
    CHECK(starts_with(packet, length, plain_prefix));
    CHECK(!contains_str(packet, length, "\x1b[2J"));

    CHECK(kittyfb_build_packet(
              packet, sizeof(packet), payload, sizeof(payload) - 1u,
              0, 9, 320, 200, "\x1b[3;5H", false) == 0u);
    CHECK(kittyfb_build_packet(
              packet, sizeof(packet), payload, sizeof(payload) - 1u,
              7, 9, 0, 200, "\x1b[3;5H", false) == 0u);
    return true;
}

static bool
test_options_defaults(void)
{
    kittyfb_options options;

    kittyfb_options_init(&options);
    CHECK(KITTYFB_VERSION_MAJOR == 0);
    CHECK(KITTYFB_VERSION_MINOR == 5);
    CHECK(KITTYFB_VERSION_PATCH == 0);
    CHECK(options.manage_raw_mode);
    CHECK(options.manage_alt_screen);
    CHECK(options.hide_cursor);
    CHECK(options.probe_graphics);
    CHECK(options.install_winch_handler);
    CHECK(options.probe_timeout_ms == 1000);
    CHECK(options.enter_sequence == NULL && options.leave_sequence == NULL);
    CHECK(options.min_width == 640 && options.min_height == 400);
    CHECK(options.max_width == 1600 && options.max_height == 1000);
    CHECK(options.image_id_a == 1 && options.image_id_b == 2);
    CHECK(options.zlib_level == 1);
    CHECK(options.transport == KITTYFB_TRANSPORT_AUTO);
    CHECK(options.shm_slots == 3);
    return true;
}

static bool
test_shm_packet_shape(void)
{
    static char packet[1024];
    static const char name[] = "/kilix-fb-1234-0-2";
    static char encoded[64];
    static char expected[256];
    size_t encoded_length;
    size_t length;

    length = kittyfb_build_shm_packet(
        packet, sizeof(packet), name, 1, 2, 320, 180, "\x1b[3;5H", false);
    CHECK(length > 0u);

    /* The payload is the base64 of the object NAME, not of any pixels:
     * that is the entire point of this transport. */
    encoded_length = kittyfb_base64_encode(
        (const uint8_t *)name, strlen(name), encoded);
    CHECK(encoded_length > 0u);
    CHECK((size_t)snprintf(expected, sizeof(expected),
                           "\x1b_Ga=T,f=32,i=1,q=2,t=s,s=320,v=180,z=-1073741825;%.*s\x1b\\",
                           (int)encoded_length, encoded) < sizeof(expected));
    CHECK(contains_str(packet, length, expected));

    /* Same wrapper contract as the inline form: synchronized update,
     * cursor origin, targeted delete of the other id, update end. */
    CHECK(starts_with(packet, length, "\x1b[?2026h\x1b[3;5H"));
    CHECK(contains_str(packet, length, "\x1b_Ga=d,d=I,i=2,q=2\x1b\\"));
    CHECK(ends_with(packet, length, "\x1b[?2026l"));

    /* No pixels travel: the whole packet is far below one inline chunk. */
    CHECK(length < KITTYFB_CHUNK_SIZE);

    /* Nothing is compressed, so o=z must not appear. */
    CHECK(!contains_str(packet, length, "o=z"));

    /* clear_first inserts the erase before the cursor move. */
    length = kittyfb_build_shm_packet(
        packet, sizeof(packet), name, 2, 1, 320, 180, "\x1b[1;1H", true);
    CHECK(length > 0u);
    CHECK(starts_with(packet, length, "\x1b[?2026h\x1b[2J\x1b[1;1H"));

    /* Rejections: bad ids, bad geometry, an empty or oversized name, and
     * a capacity that cannot hold the result. */
    CHECK(kittyfb_build_shm_packet(packet, sizeof(packet), name, 0, 2, 320,
                                   180, "\x1b[1;1H", false) == 0u);
    CHECK(kittyfb_build_shm_packet(packet, sizeof(packet), name, 1, 2, 0, 180,
                                   "\x1b[1;1H", false) == 0u);
    CHECK(kittyfb_build_shm_packet(packet, sizeof(packet), "", 1, 2, 320, 180,
                                   "\x1b[1;1H", false) == 0u);
    CHECK(kittyfb_build_shm_packet(packet, 16u, name, 1, 2, 320, 180,
                                   "\x1b[1;1H", false) == 0u);
    CHECK(kittyfb_build_shm_packet(packet, sizeof(packet), NULL, 1, 2, 320,
                                   180, "\x1b[1;1H", false) == 0u);
    return true;
}

static bool
test_inactive_session_is_safe(void)
{
    kittyfb_session session;
    kittyfb_stats stats;
    uint8_t pixel[4] = {0u, 0u, 0u, 255u};
    int width = -1;
    int height = -1;

    kittyfb_session_init(&session);
    CHECK(!kittyfb_present(&session, pixel, 1, 1));
    CHECK(!kittyfb_check_resize(&session, &width, &height));
    CHECK(!kittyfb_failed(&session));
    CHECK(kittyfb_width(&session) == 0 && kittyfb_height(&session) == 0);
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.frames_presented == 0u && stats.encode_failures == 0u);
    kittyfb_stop(&session);
    kittyfb_emergency_restore(&session);
    kittyfb_stop(&session);
    CHECK(!kittyfb_present(NULL, pixel, 1, 1));
    kittyfb_stop(NULL);
    kittyfb_emergency_restore(NULL);
    return true;
}

typedef struct failure_writer {
    kittyfb_session *session;
    size_t iterations;
} failure_writer;

static void *toggle_presenter_failure(void *argument)
{
    failure_writer *writer = argument;
    size_t index;

    for (index = 0u; index < writer->iterations; ++index) {
        pthread_mutex_lock(&writer->session->frame_lock);
        writer->session->presenter_failed = (index & 1u) != 0u;
        pthread_mutex_unlock(&writer->session->frame_lock);
    }
    return NULL;
}

static bool
test_failure_snapshot_is_synchronized(void)
{
    kittyfb_session session;
    failure_writer writer;
    pthread_t thread;
    size_t index;

    kittyfb_session_init(&session);
    writer.session = &session;
    writer.iterations = 10000u;
    CHECK(pthread_create(&thread, NULL, toggle_presenter_failure, &writer) == 0);
    for (index = 0u; index < writer.iterations; ++index)
        (void)kittyfb_failed(&session);
    CHECK(pthread_join(thread, NULL) == 0);
    return true;
}

/* ------------------------------- PTY tests ------------------------------ */

/*
 * A minimal fake terminal: wait (without consuming, so the test can still
 * assert on the query bytes) until the library writes its probe, then
 * answer on the master side.  Replying up front would not work - the
 * library re-applies raw mode with TCSAFLUSH, which discards input that
 * arrived before the probe was sent.
 */
typedef struct probe_replier {
    int master_fd;
    const char *reply;
    int reply_delay_ms;
} probe_replier;

static void *
probe_replier_main(void *opaque)
{
    probe_replier *replier = opaque;
    struct pollfd descriptor;
    int ready;

    descriptor.fd = replier->master_fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    do {
        ready = poll(&descriptor, 1u, 3000);
    } while (ready < 0 && errno == EINTR);
    if (ready > 0) {
        sleep_milliseconds(replier->reply_delay_ms);
        (void)write(replier->master_fd, replier->reply,
                    strlen(replier->reply));
    }
    return NULL;
}

/* Run kittyfb_start against a fake terminal that answers the probe with
 * the given reply.  Returns start's result; *saved_errno holds errno. */
static int
start_with_fake_terminal(
    kittyfb_session *session,
    int master,
    int slave,
    const kittyfb_options *options,
    const char *reply,
    int *saved_errno)
{
    probe_replier replier;
    pthread_t thread;
    kittyfb_options inline_defaults;
    int result;

    /* Tests that do not care about the transport still need a
     * deterministic one: AUTO would pick shared memory on a machine
     * outside tmux, and every wire-format assertion here describes the
     * inline encoding.  Shared memory has its own tests. */
    if (options == NULL) {
        kittyfb_options_init(&inline_defaults);
        inline_defaults.transport = KITTYFB_TRANSPORT_INLINE;
        options = &inline_defaults;
    }

    replier.master_fd = master;
    replier.reply = reply;
    replier.reply_delay_ms = 10;
    if (pthread_create(&thread, NULL, probe_replier_main, &replier) != 0) {
        return -1;
    }
    errno = 0;
    result = kittyfb_start(session, slave, slave, options);
    if (saved_errno != NULL) {
        *saved_errno = errno;
    }
    (void)pthread_join(thread, NULL);
    return result;
}

typedef struct signal_storm {
    pthread_t target;
    int started;
} signal_storm;

static void *
signal_storm_main(void *opaque)
{
    signal_storm *storm = opaque;

    __atomic_store_n(&storm->started, 1, __ATOMIC_RELEASE);
    for (int sent = 0; sent < 220; ++sent) {
        (void)pthread_kill(storm->target, SIGUSR1);
        sleep_milliseconds(1);
    }
    return NULL;
}

static bool
present_and_capture(
    kittyfb_session *session,
    int master,
    uint8_t *frame,
    int width,
    int height,
    uint8_t salt,
    char *buffer,
    size_t capacity,
    size_t *used)
{
    fill_test_frame(frame, width, height, salt);
    CHECK(kittyfb_present(session, frame, width, height));
    *used = 0u;
    CHECK(wait_for_bytes(master, buffer, capacity, used, "\x1b[?2026l", 8u));
    return true;
}

static bool
decoded_payload_matches(
    const char *buffer,
    size_t used,
    const char *header,
    const uint8_t *rgba,
    int width,
    int height)
{
    static uint8_t compressed[65536];
    static uint8_t raw[65536];
    size_t header_at;
    size_t payload_start;
    size_t payload_size;
    size_t compressed_length;
    uLongf raw_length;
    size_t pixels = (size_t)width * (size_t)height;
    size_t index;

    header_at = find_bytes(buffer, used, header, strlen(header));
    CHECK(header_at != SIZE_MAX);
    payload_start = header_at + strlen(header);
    payload_size = find_bytes(buffer + payload_start, used - payload_start,
                              "\x1b\\", 2u);
    CHECK(payload_size != SIZE_MAX);
    CHECK(payload_size > 0u);

    compressed_length = base64_decode(
        buffer + payload_start,
        payload_size,
        compressed);
    CHECK(compressed_length > 0u);
    raw_length = (uLongf)sizeof(raw);
    CHECK(uncompress(raw, &raw_length, compressed,
                     (uLong)compressed_length) == Z_OK);
    CHECK((size_t)raw_length == pixels * 3u);
    for (index = 0u; index < pixels; ++index) {
        CHECK(raw[index * 3u] == rgba[index * 4u]);
        CHECK(raw[index * 3u + 1u] == rgba[index * 4u + 1u]);
        CHECK(raw[index * 3u + 2u] == rgba[index * 4u + 2u]);
    }
    return true;
}

/*
 * Extract the shared-memory object name from a t=s packet: the payload
 * between the header's ';' and the APC terminator is the base64 of the
 * name.
 */
static bool
extract_shm_name(
    const char *buffer,
    size_t used,
    char *name,
    size_t name_capacity)
{
    static const char header[] = "\x1b_Ga=T,f=32,";
    size_t header_at;
    size_t semicolon;
    size_t payload_start;
    size_t payload_size;
    size_t decoded;

    header_at = find_bytes(buffer, used, header, strlen(header));
    CHECK(header_at != SIZE_MAX);
    semicolon = find_bytes(buffer + header_at, used - header_at, ";", 1u);
    CHECK(semicolon != SIZE_MAX);
    payload_start = header_at + semicolon + 1u;
    payload_size = find_bytes(buffer + payload_start, used - payload_start,
                              "\x1b\\", 2u);
    CHECK(payload_size != SIZE_MAX);
    CHECK(payload_size > 0u);

    decoded = base64_decode(buffer + payload_start, payload_size,
                            (uint8_t *)name);
    CHECK(decoded > 0u && decoded < name_capacity);
    name[decoded] = '\0';
    /* POSIX shared-memory names are absolute. */
    CHECK(name[0] == '/');
    return true;
}

/*
 * Do exactly what Kitty does with a t=s transmission: open the named
 * object, map it, and unlink it.  The unlink is the acknowledgement the
 * ring polls for, so a test that skips it deliberately saturates the
 * ring.  The mapped pixels are compared against what was presented.
 */
static bool
consume_shm_frame(
    const char *name,
    const uint8_t *rgba,
    int width,
    int height)
{
    size_t size = (size_t)width * (size_t)height * 4u;
    int fd = shm_open(name, O_RDONLY, 0);
    void *mapping;
    struct stat info;

    CHECK(fd >= 0);
    CHECK(fstat(fd, &info) == 0);
    /* f=32 means the object carries RGBA, alpha included - the inline
     * path's alpha strip buys nothing once no bytes cross the wire. */
    CHECK((size_t)info.st_size == size);
    mapping = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(mapping != MAP_FAILED);
    CHECK(memcmp(mapping, rgba, size) == 0);
    CHECK(munmap(mapping, size) == 0);
    CHECK(close(fd) == 0);
    CHECK(shm_unlink(name) == 0);
    return true;
}

static bool
wait_for_dropped_frames(kittyfb_session *session, uint64_t minimum)
{
    const int64_t deadline = monotonic_milliseconds() + 3000;
    kittyfb_stats stats;

    for (;;) {
        kittyfb_get_stats(session, &stats);
        if (stats.frames_dropped >= minimum) {
            return true;
        }
        if (monotonic_milliseconds() >= deadline) {
            return false;
        }
        sleep_milliseconds(10);
    }
}

static bool
wait_for_encoded_frames(kittyfb_session *session, uint64_t minimum)
{
    const int64_t deadline = monotonic_milliseconds() + 3000;
    kittyfb_stats stats;

    for (;;) {
        kittyfb_get_stats(session, &stats);
        if (stats.frames_encoded >= minimum) {
            return true;
        }
        if (monotonic_milliseconds() >= deadline) {
            return false;
        }
        sleep_milliseconds(10);
    }
}

static bool
wait_for_failure(kittyfb_session *session)
{
    const int64_t deadline = monotonic_milliseconds() + 3000;

    while (!kittyfb_failed(session)) {
        if (monotonic_milliseconds() >= deadline) {
            return false;
        }
        sleep_milliseconds(10);
    }
    return true;
}

static bool
test_pty_lifecycle(void)
{
    enum { FRAME_W = 32, FRAME_H = 16 };
    int master = -1;
    int slave = -1;
    struct termios original;
    struct termios restored;
    kittyfb_session session;
    kittyfb_options options;
    kittyfb_stats stats;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[65536];
    size_t used;
    static const char restore_sequence[] =
        "\x1b[?2026l\x1b\\"
        "\x1b_Ga=d,d=i,i=1,q=2\x1b\\"
        "\x1b_Ga=d,d=i,i=2,q=2\x1b\\"
        "\x1b[?2026l\x1b[?1003l\x1b[?25h\x1b[?1049l";

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, &original));

    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_INLINE;
    options.enter_sequence = "\x1b[?1003h";
    options.leave_sequence = "\x1b[?1003l";
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);
    CHECK(kittyfb_width(&session) == 900);
    CHECK(kittyfb_height(&session) == 522);
    CHECK(kittyfb_cell_width(&session) == 9);
    CHECK(kittyfb_cell_height(&session) == 18);

    /* Start emitted the paired probe, the alt screen and cursor hide. */
    used = read_available(master, buffer, sizeof(buffer));
    CHECK(contains_str(buffer, used,
                       "\x1b_Gi=31,a=q,t=d,f=24,s=1,v=1;AAAA\x1b\\\x1b[c"));
    CHECK(contains_str(buffer, used, "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H"));
    CHECK(contains_str(buffer, used, "\x1b[?1003h"));

    /* First frame goes out under id 1 inside a synchronized update and
     * deletes id 2; the pixel data round-trips through zlib + base64. */
    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 0u,
                              buffer, sizeof(buffer), &used));
    CHECK(starts_with(buffer, used, "\x1b[?2026h\x1b[1;1H"));
    CHECK(decoded_payload_matches(
        buffer, used, "\x1b_Ga=T,f=24,i=1,q=2,o=z,s=32,v=16,z=-1073741825,m=0;",
        frame, FRAME_W, FRAME_H));
    CHECK(contains_str(buffer, used, "\x1b_Ga=d,d=I,i=2,q=2\x1b\\"));

    /* The ids ping-pong: the second frame is id 2 and deletes id 1. */
    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 3u,
                              buffer, sizeof(buffer), &used));
    CHECK(contains_str(buffer, used,
                       "\x1b_Ga=T,f=24,i=2,q=2,o=z,s=32,v=16,z=-1073741825,m=0;"));
    CHECK(contains_str(buffer, used, "\x1b_Ga=d,d=I,i=1,q=2\x1b\\"));

    /* Absurd dimensions are rejected without disturbing the session. */
    CHECK(!kittyfb_present(&session, frame, INT_MAX, INT_MAX));
    CHECK(!kittyfb_failed(&session));

    CHECK(wait_for_encoded_frames(&session, 2u));
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.frames_presented == 2u);
    CHECK(stats.frames_encoded == 2u);
    CHECK(stats.frames_dropped == 0u);
    CHECK(stats.encode_failures == 0u);

    /* Stop restores the terminal: synchronized update ended first, only
     * this session's two image ids deleted, cursor and screen back. */
    kittyfb_stop(&session);
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         restore_sequence, strlen(restore_sequence)));
    CHECK(tcgetattr(slave, &restored) == 0);
    CHECK(same_termios(&original, &restored));

    /* A second stop and a present after stop are safe no-ops. */
    kittyfb_stop(&session);
    CHECK(!kittyfb_present(&session, frame, FRAME_W, FRAME_H));
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.frames_presented == 2u);

    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

static bool
test_pty_start_after_stop(void)
{
    enum { FRAME_W = 16, FRAME_H = 8 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[65536];
    size_t used;

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));

    kittyfb_session_init(&session);
    CHECK(start_with_fake_terminal(&session, master, slave, NULL,
                                   graphics_reply, NULL) == 0);
    kittyfb_stop(&session);
    drain_descriptor(master);

    /* The shutdown guard must reset: a second start after stop works
     * and the image id ping-pong starts over. */
    CHECK(start_with_fake_terminal(&session, master, slave, NULL,
                                   graphics_reply, NULL) == 0);
    CHECK(kittyfb_width(&session) == 900);
    drain_descriptor(master);

    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 0u,
                              buffer, sizeof(buffer), &used));
    CHECK(contains_str(buffer, used,
                       "\x1b_Ga=T,f=24,i=1,q=2,o=z,s=16,v=8,z=-1073741825,m=0;"));
    CHECK(contains_str(buffer, used, "\x1b_Ga=d,d=I,i=2,q=2\x1b\\"));

    kittyfb_stop(&session);
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?1049l", 8u));

    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

static bool
test_pty_suspend_retains_buffers(void)
{
    enum { FRAME_W = 16, FRAME_H = 8 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[65536];
    uint8_t *pending;
    uint8_t *encoding;
    uint8_t *rgb;
    uint8_t *compressed;
    char *base64;
    char *packet;
    size_t pending_capacity;
    size_t encode_capacity;
    size_t used;

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));
    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_INLINE;
    options.probe_graphics = false;
    CHECK(kittyfb_start(&session, slave, slave, &options) == 0);
    drain_descriptor(master);
    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 0u,
                              buffer, sizeof(buffer), &used));
    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 1u,
                              buffer, sizeof(buffer), &used));
    CHECK(wait_for_encoded_frames(&session, 2u));
    kittyfb_suspend(&session);
    CHECK(!session.active);
    CHECK(!session.presenter_started);

    /* Sampled only now that suspend has joined the presenter.  It swaps
     * pending_buffer and encode_buffer as it runs, so reading them from
     * this thread beforehand is a data race - and one that could make
     * the comparison below fail for no reason, if a swap happened to
     * land between the sample and the join. */
    CHECK(session.pending_buffer != NULL);
    CHECK(session.encode_buffer != NULL);
    CHECK(session.rgb_buffer != NULL);
    CHECK(session.z_buffer != NULL);
    CHECK(session.b64_buffer != NULL);
    CHECK(session.packet_buffer != NULL);
    pending = session.pending_buffer;
    encoding = session.encode_buffer;
    rgb = session.rgb_buffer;
    compressed = session.z_buffer;
    base64 = session.b64_buffer;
    packet = session.packet_buffer;
    pending_capacity = session.pending_capacity;
    encode_capacity = session.encode_capacity;
    /* A second suspend changes nothing. */
    kittyfb_suspend(&session);
    CHECK(session.pending_buffer == pending);
    CHECK(session.encode_buffer == encoding);
    CHECK(session.rgb_buffer == rgb);
    CHECK(session.z_buffer == compressed);
    CHECK(session.b64_buffer == base64);
    CHECK(session.packet_buffer == packet);
    CHECK(session.pending_capacity == pending_capacity);
    CHECK(session.encode_capacity == encode_capacity);

    drain_descriptor(master);
    CHECK(kittyfb_start(&session, slave, slave, &options) == 0);
    CHECK(session.pending_buffer == pending);
    CHECK(session.encode_buffer == encoding);
    CHECK(session.rgb_buffer == rgb);
    CHECK(session.z_buffer == compressed);
    CHECK(session.b64_buffer == base64);
    CHECK(session.packet_buffer == packet);
    kittyfb_suspend(&session);

    /* Final stop releases retained storage even while suspended. */
    kittyfb_stop(&session);
    CHECK(session.pending_buffer == NULL);
    CHECK(session.encode_buffer == NULL);
    CHECK(session.rgb_buffer == NULL);
    CHECK(session.z_buffer == NULL);
    CHECK(session.b64_buffer == NULL);
    CHECK(session.packet_buffer == NULL);
    CHECK(session.pending_capacity == 0u);
    CHECK(session.encode_capacity == 0u);

    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

static bool
test_pty_probe_rejects_da1_only_terminal(void)
{
    int master = -1;
    int slave = -1;
    struct termios original;
    struct termios after;
    kittyfb_session session;
    kittyfb_options options;
    int saved_errno = 0;
    static char buffer[8192];
    size_t used;

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, &original));

    /* This terminal answers device attributes but not the graphics
    * query: start must fail cleanly with ENOTSUP, restore termios, and
    * never touch the alternate screen. */
    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_SHM;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   da1_only_reply, &saved_errno) == -1);
    CHECK(saved_errno == ENOTSUP);
    CHECK(session.shm_slots == NULL);
    CHECK(session.shm_slot_count == 0);
    CHECK(tcgetattr(slave, &after) == 0);
    CHECK(same_termios(&original, &after));

    used = read_available(master, buffer, sizeof(buffer));
    CHECK(contains_str(buffer, used, "\x1b_Gi=31,a=q"));
    CHECK(!contains_str(buffer, used, "\x1b[?1049h"));

    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

/* Signal interruptions may retry the probe read, but they must not restart
 * its timeout window.  A deliberately late capable reply distinguishes a
 * real monotonic deadline from one extended indefinitely by EINTR. */
static bool
test_pty_probe_timeout_survives_signal_storm(void)
{
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    probe_replier replier;
    signal_storm storm = {0};
    pthread_t replier_thread;
    pthread_t storm_thread;
    struct sigaction action;
    struct sigaction previous;
    int result;
    int saved_errno;

    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = test_winch_handler;
    CHECK(sigemptyset(&action.sa_mask) == 0);
    CHECK(sigaction(SIGUSR1, &action, &previous) == 0);
    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));

    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_INLINE;
    options.probe_timeout_ms = 30;
    replier.master_fd = master;
    replier.reply = graphics_reply;
    replier.reply_delay_ms = 150;
    storm.target = pthread_self();
    CHECK(pthread_create(&storm_thread, NULL, signal_storm_main, &storm) == 0);
    CHECK(pthread_create(
              &replier_thread, NULL, probe_replier_main, &replier) == 0);
    while (!__atomic_load_n(&storm.started, __ATOMIC_ACQUIRE)) {
        sleep_milliseconds(1);
    }

    errno = 0;
    result = kittyfb_start(&session, slave, slave, &options);
    saved_errno = errno;
    if (result == 0) {
        kittyfb_stop(&session);
    }
    CHECK(pthread_join(replier_thread, NULL) == 0);
    CHECK(pthread_join(storm_thread, NULL) == 0);
    CHECK(sigaction(SIGUSR1, &previous, NULL) == 0);

    CHECK(result == -1);
    CHECK(saved_errno == ENOTSUP);
    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

static bool
test_pty_probe_disabled_starts_blind(void)
{
    enum { FRAME_W = 16, FRAME_H = 8 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[65536];
    char overlong[KITTYFB_CONTROL_SEQUENCE_MAX + 2u];
    size_t used;

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));

    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_INLINE;
    options.probe_graphics = false;
    (void)memset(overlong, 'x', sizeof(overlong));
    overlong[sizeof(overlong) - 1u] = '\0';
    options.enter_sequence = overlong;
    errno = 0;
    CHECK(kittyfb_start(&session, slave, slave, &options) == -1);
    CHECK(errno == EINVAL);
    options.enter_sequence = NULL;
    options.max_width = INT_MAX;
    options.max_height = INT_MAX;
    errno = 0;
    CHECK(kittyfb_start(&session, slave, slave, &options) == -1);
    CHECK(errno == EINVAL);
    options.max_width = 1600;
    options.max_height = 1000;
    CHECK(kittyfb_start(&session, slave, slave, &options) == 0);

    used = read_available(master, buffer, sizeof(buffer));
    CHECK(!contains_str(buffer, used, "\x1b_Gi=31"));
    CHECK(contains_str(buffer, used, "\x1b[?1049h"));

    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 0u,
                              buffer, sizeof(buffer), &used));
    CHECK(contains_str(buffer, used, "\x1b_Ga=T,f=24,i=1,"));

    kittyfb_stop(&session);
    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

static bool
test_pty_emergency_restore(void)
{
    enum { FRAME_W = 16, FRAME_H = 8 };
    int master = -1;
    int slave = -1;
    struct termios original;
    struct termios after;
    struct sigaction previous_winch;
    struct sigaction expected_winch;
    struct sigaction restored_winch;
    kittyfb_session session;
    kittyfb_options options;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[65536];
    size_t used;
    static const char restore_sequence[] =
        "\x1b[?2026l\x1b\\"
        "\x1b_Ga=d,d=i,i=1,q=2\x1b\\"
        "\x1b_Ga=d,d=i,i=2,q=2\x1b\\"
        "\x1b[?2026l\x1b[?1006l\x1b[?25h\x1b[?1049l";

    (void)memset(&expected_winch, 0, sizeof expected_winch);
    expected_winch.sa_handler = test_winch_handler;
    CHECK(sigemptyset(&expected_winch.sa_mask) == 0);
    CHECK(sigaction(SIGWINCH, &expected_winch, &previous_winch) == 0);

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, &original));

    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_INLINE;
    options.enter_sequence = "\x1b[?1006h";
    options.leave_sequence = "\x1b[?1006l";
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);
    drain_descriptor(master);
    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 0u,
                              buffer, sizeof(buffer), &used));

    /* The async-signal path: the prebuilt sequence ends the
     * synchronized update first, then deletes only this session's ids. */
    kittyfb_emergency_restore(&session);
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         restore_sequence, strlen(restore_sequence)));
    CHECK(starts_with(buffer, used, restore_sequence));
    CHECK(tcgetattr(slave, &after) == 0);
    CHECK(same_termios(&original, &after));

    /* The session is inactive, a second emergency call is a no-op, and
     * stop still reclaims the presenter thread and its buffers. */
    CHECK(!kittyfb_present(&session, frame, FRAME_W, FRAME_H));
    errno = 0;
    CHECK(kittyfb_start(&session, slave, slave, &options) == -1);
    CHECK(errno == EBUSY);
    kittyfb_emergency_restore(&session);
    kittyfb_stop(&session);
    CHECK(sigaction(SIGWINCH, NULL, &restored_winch) == 0);
    CHECK(restored_winch.sa_handler == test_winch_handler);
    CHECK(!session.winch_handler_installed);
    CHECK(sigaction(SIGWINCH, &previous_winch, NULL) == 0);

    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

static bool
test_pty_emergency_restore_before_present(void)
{
    int master = -1;
    int slave = -1;
    int original_flags;
    struct sigaction previous_winch;
    struct sigaction expected_winch;
    struct sigaction restored_winch;
    kittyfb_session session;
    kittyfb_options options;

    (void)memset(&expected_winch, 0, sizeof expected_winch);
    expected_winch.sa_handler = test_winch_handler;
    CHECK(sigemptyset(&expected_winch.sa_mask) == 0);
    CHECK(sigaction(SIGWINCH, &expected_winch, &previous_winch) == 0);
    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));
    original_flags = fcntl(slave, F_GETFL);
    CHECK(original_flags >= 0);

    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_INLINE;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);
    drain_descriptor(master);

    /* With no presenter thread or retained frame buffer, emergency cleanup
     * still leaves signal-safe bookkeeping for the later ordinary stop. */
    kittyfb_emergency_restore(&session);
    CHECK(fcntl(slave, F_GETFL) == original_flags);
    kittyfb_stop(&session);
    CHECK(sigaction(SIGWINCH, NULL, &restored_winch) == 0);
    CHECK(restored_winch.sa_handler == test_winch_handler);
    CHECK(!session.winch_handler_installed);
    CHECK(!session.output_flags_saved);
    CHECK(!session.termios_saved);
    CHECK(sigaction(SIGWINCH, &previous_winch, NULL) == 0);

    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

static bool
test_pty_resize(void)
{
    enum { FRAME_W = 16, FRAME_H = 8 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    struct winsize window;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[65536];
    size_t used;
    int width = 0;
    int height = 0;

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));

    kittyfb_session_init(&session);
    CHECK(start_with_fake_terminal(&session, master, slave, NULL,
                                   graphics_reply, NULL) == 0);
    drain_descriptor(master);
    CHECK(!kittyfb_check_resize(&session, &width, &height));

    (void)memset(&window, 0, sizeof(window));
    window.ws_col = 120;
    window.ws_row = 40;
    window.ws_xpixel = 1080;
    window.ws_ypixel = 720;
    CHECK(ioctl(master, TIOCSWINSZ, &window) == 0);
    kittyfb_notify_resize();

    CHECK(kittyfb_check_resize(&session, &width, &height));
    CHECK(width == 1080 && height == 702);
    CHECK(kittyfb_width(&session) == 1080);
    CHECK(kittyfb_height(&session) == 702);
    CHECK(!kittyfb_check_resize(&session, &width, &height));

    /* The frame after a resize clears stale cells inside its own
     * synchronized update. */
    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 0u,
                              buffer, sizeof(buffer), &used));
    CHECK(starts_with(buffer, used, "\x1b[?2026h\x1b[2J\x1b[1;1H"));

    kittyfb_stop(&session);
    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}


/*
 * The frame is centered rather than pinned to a corner, so a pointer
 * report - which the terminal gives relative to itself - is offset from
 * the frame by however much of the terminal the frame does not fill.
 * Without this exposed, a caller reading a mouse would have to
 * re-implement the centering or parse it back out of an escape string.
 */
static bool
test_pty_origin_is_the_centering_offset(void)
{
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    struct winsize window;
    int width = 0;
    int height = 0;

    /* Larger than the default maximum, so the frame is clamped and there
     * is real terminal left over around it. */
    CHECK(open_test_pty(&master, &slave, 300, 80, 3000, 1600, NULL));

    kittyfb_session_init(&session);
    CHECK(start_with_fake_terminal(&session, master, slave, NULL,
                                   graphics_reply, NULL) == 0);
    drain_descriptor(master);
    CHECK(kittyfb_width(&session) == 1600);
    CHECK(kittyfb_height(&session) == 1000);
    CHECK(kittyfb_cell_width(&session) == 10);
    CHECK(kittyfb_cell_height(&session) == 20);

    /* 71st column and 15th row, one-based, converted to pixels. */
    CHECK(kittyfb_origin_x(&session) == 700);
    CHECK(kittyfb_origin_y(&session) == 280);
    /* A graphics placement starts on a cell boundary, so these always
     * land on one too. */
    CHECK(kittyfb_origin_x(&session) % kittyfb_cell_width(&session) == 0);
    CHECK(kittyfb_origin_y(&session) % kittyfb_cell_height(&session) == 0);

    /* A resize moves them, or the offset would be stale for the rest of
     * the session - the case a caller cannot detect for itself. */
    (void)memset(&window, 0, sizeof(window));
    window.ws_col = 100;
    window.ws_row = 30;
    window.ws_xpixel = 900;
    window.ws_ypixel = 540;
    CHECK(ioctl(master, TIOCSWINSZ, &window) == 0);
    kittyfb_notify_resize();
    CHECK(kittyfb_check_resize(&session, &width, &height));
    CHECK(width == 900 && height == 522);
    CHECK(kittyfb_origin_x(&session) == 0);
    CHECK(kittyfb_origin_y(&session) == 0);

    CHECK(kittyfb_origin_x(NULL) == 0);
    CHECK(kittyfb_origin_y(NULL) == 0);

    kittyfb_stop(&session);
    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

/*
 * Frames and patches come from different threads, and both write escape
 * sequences to one terminal.
 *
 * The presenter owns a thread; damage patches are written synchronously
 * by whoever called.  Nothing stopped the two from splicing their output
 * together mid-sequence, and nothing stopped a patch from being written
 * ahead of an already-queued frame that would then paint over it with
 * older pixels.  Neither is visible in a single-threaded test.
 *
 * Run this under `make race`; without a thread sanitizer it still
 * exercises the path, but the interleaving is what it is really for.
 */
typedef struct concurrency_state {
    kittyfb_session *session;
    const uint8_t *frame;
    int width;
    int height;
    int rounds;
    int stop;
    int master;
} concurrency_state;

static void *concurrency_presenter(void *opaque)
{
    concurrency_state *state = opaque;

    for (int i = 0; i < state->rounds; i++) {
        (void)kittyfb_present(state->session, state->frame, state->width,
                              state->height);
    }
    return NULL;
}

static void *concurrency_reader(void *opaque)
{
    concurrency_state *state = opaque;
    static char sink[16384];

    /* Keep the pty drained, or the writers stall against a full buffer
     * and the test measures poll timeouts instead of interleaving. */
    while (__atomic_load_n(&state->stop, __ATOMIC_ACQUIRE) == 0) {
        if (read(state->master, sink, sizeof(sink)) <= 0) {
            struct timespec pause = {0, 1000000};

            (void)nanosleep(&pause, NULL);
        }
    }
    return NULL;
}

static bool
test_pty_frames_and_patches_from_two_threads(void)
{
    /* An upper bound, not the size: the framebuffer snaps to whole cells,
     * so the geometry it settles on is what has to be presented. */
    enum { FRAME_MAX_W = 128, FRAME_MAX_H = 64, ROUNDS = 200 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    kittyfb_stats stats;
    concurrency_state state;
    pthread_t writer;
    pthread_t reader;
    static uint8_t frame[(size_t)FRAME_MAX_W * FRAME_MAX_H * 4u];
    const kittyfb_rect rect = {0, 0, 16, 8};

    for (size_t i = 0u; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 7u);
    }
    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));
    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    /* Patching has no per-rect form over shared memory, so pin the
     * transport that does. */
    options.transport = KITTYFB_TRANSPORT_INLINE;
    options.min_width = 1;
    options.min_height = 1;
    options.max_width = FRAME_MAX_W;
    options.max_height = FRAME_MAX_H;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);

    state.session = &session;
    state.frame = frame;
    state.width = kittyfb_width(&session);
    state.height = kittyfb_height(&session);
    state.rounds = ROUNDS;
    state.stop = 0;
    state.master = master;
    CHECK(state.width > 0 && state.width <= FRAME_MAX_W);
    CHECK(state.height > 0 && state.height <= FRAME_MAX_H);

    CHECK(pthread_create(&reader, NULL, concurrency_reader, &state) == 0);
    CHECK(pthread_create(&writer, NULL, concurrency_presenter, &state) == 0);
    for (int i = 0; i < ROUNDS; i++) {
        (void)kittyfb_present_damage(&session, frame, state.width,
                                     state.height, &rect, 1u);
    }
    CHECK(pthread_join(writer, NULL) == 0);
    __atomic_store_n(&state.stop, 1, __ATOMIC_RELEASE);
    CHECK(pthread_join(reader, NULL) == 0);

    /* Every patch either went out or turned into a frame; none was lost
     * and nothing latched a failure. */
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.damage_presents + stats.damage_fallbacks == (uint64_t)ROUNDS);
    CHECK(!kittyfb_failed(&session));
    CHECK(stats.encode_failures == 0u);

    kittyfb_stop(&session);
    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

/* Does the captured escape stream contain this literal? */
static bool wire_has(const char *buffer, size_t used, const char *needle)
{
    size_t n = strlen(needle);

    if (n == 0u || used < n) {
        return false;
    }
    for (size_t i = 0u; i + n <= used; ++i) {
        if (memcmp(buffer + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * Damage presents patch the image already on screen instead of
 * retransmitting it.  These check the wire, because what goes over it is
 * the entire point.
 */
static bool
test_pty_damage_patches_in_place(void)
{
    enum { FRAME_W = 64, FRAME_H = 48 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    kittyfb_stats stats;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[262144];
    size_t full_used = 0u;
    size_t patch_used = 0u;
    kittyfb_rect rect;

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));
    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_INLINE;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);
    drain_descriptor(master);

    /* A full frame first: there has to be an image to edit. */
    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 1u,
                              buffer, sizeof(buffer), &full_used));
    CHECK(wire_has(buffer, full_used, "a=T"));

    /* Now change the frame and send only a small rectangle of it. */
    fill_test_frame(frame, FRAME_W, FRAME_H, 2u);
    rect.x0 = 8;
    rect.y0 = 4;
    rect.x1 = 20;
    rect.y1 = 12;
    CHECK(kittyfb_present_damage(&session, frame, FRAME_W, FRAME_H, &rect, 1u));
    patch_used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &patch_used,
                         "\x1b[?2026l", 8u));

    /* The edit form, where it lands, and its own dimensions. */
    CHECK(wire_has(buffer, patch_used, "a=f"));
    CHECK(wire_has(buffer, patch_used, "r=1"));
    CHECK(wire_has(buffer, patch_used, "x=8"));
    CHECK(wire_has(buffer, patch_used, "y=4"));
    CHECK(wire_has(buffer, patch_used, "s=12"));
    CHECK(wire_has(buffer, patch_used, "v=8"));
    /* Wrapped, so the screen never shows a half-applied edit. */
    CHECK(wire_has(buffer, patch_used, "\x1b[?2026h"));
    /* And nothing was transmitted as a new image. */
    CHECK(!wire_has(buffer, patch_used, "a=T"));

    /* The reason the function exists: a small patch must cost far less
     * than the frame it patches. */
    CHECK(patch_used * 4u < full_used);

    kittyfb_get_stats(&session, &stats);
    CHECK(stats.damage_presents == 1u);
    CHECK(stats.damage_fallbacks == 0u);
    CHECK(stats.damage_bytes > 0u);

    kittyfb_stop(&session);
    (void)close(master);
    (void)close(slave);
    return true;
}

/* Kitty requires every continuation chunk of animation frame data to repeat
 * a=f, while allowing only a=f, m, and optional q. A one-chunk edit cannot
 * catch either omission of a=f or an illegal repeated image id. */
static bool
test_pty_damage_multichunk_protocol(void)
{
    enum { FRAME_W = 128, FRAME_H = 128 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[262144];
    size_t used = 0u;
    kittyfb_rect rect = {0, 0, 40, 32};

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));
    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_INLINE;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);
    drain_descriptor(master);
    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 1u,
                              buffer, sizeof(buffer), &used));

    fill_noise_frame(frame, FRAME_W, FRAME_H, UINT32_C(0x4b465231));
    CHECK(kittyfb_present_damage(
        &session, frame, FRAME_W, FRAME_H, &rect, 1u));
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?2026l", 8u));

    CHECK(wire_has(buffer, used, "a=f,i=1,r=1,X=1"));
    CHECK(wire_has(buffer, used, "s=40,v=32,m=1;"));
    CHECK(count_bytes(buffer, used, "\x1b_Ga=f,q=2,m=",
                      strlen("\x1b_Ga=f,q=2,m=")) >= 1u);
    CHECK(wire_has(buffer, used, "\x1b_Ga=f,q=2,m=0;"));
    CHECK(!wire_has(buffer, used, "\x1b_Ga=f,i=1,q=2,m="));
    CHECK(!wire_has(buffer, used, "\x1b_Gm="));

    kittyfb_stop(&session);
    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

/*
 * Consume one complete single-chunk a=f patch packet at *offset: the exact
 * header for this rectangle, a base64 payload that decodes to the rect's
 * pixels, and the APC terminator.  Advances *offset past the packet, so a
 * caller walking a burst pins byte-level contiguity, not just presence.
 */
static bool
expect_patch_packet(
    const char *buffer,
    size_t used,
    size_t *offset,
    int image_id,
    const kittyfb_rect *rect,
    const uint8_t *rgba,
    int width)
{
    static uint8_t compressed[65536];
    static uint8_t raw[65536];
    char header[128];
    int printed;
    size_t payload_size;
    size_t compressed_length;
    uLongf raw_length;
    const int rect_width = rect->x1 - rect->x0;
    const int rect_height = rect->y1 - rect->y0;

    printed = snprintf(
        header, sizeof(header),
        "\x1b_Ga=f,i=%d,r=1,X=1,q=2,f=24,o=z,x=%d,y=%d,s=%d,v=%d,m=0;",
        image_id, rect->x0, rect->y0, rect_width, rect_height);
    CHECK(printed > 0 && (size_t)printed < sizeof(header));
    CHECK(*offset + (size_t)printed <= used);
    CHECK(memcmp(buffer + *offset, header, (size_t)printed) == 0);
    *offset += (size_t)printed;

    payload_size = find_bytes(buffer + *offset, used - *offset, "\x1b\\", 2u);
    CHECK(payload_size != SIZE_MAX && payload_size > 0u);
    compressed_length = base64_decode(buffer + *offset, payload_size,
                                      compressed);
    CHECK(compressed_length > 0u);
    raw_length = (uLongf)sizeof(raw);
    CHECK(uncompress(raw, &raw_length, compressed,
                     (uLong)compressed_length) == Z_OK);
    CHECK((size_t)raw_length ==
          (size_t)rect_width * (size_t)rect_height * 3u);
    for (int y = 0; y < rect_height; ++y) {
        for (int x = 0; x < rect_width; ++x) {
            const uint8_t *expected = rgba +
                (((size_t)(rect->y0 + y) * (size_t)width +
                  (size_t)(rect->x0 + x)) * 4u);
            const uint8_t *got = raw +
                ((size_t)y * (size_t)rect_width + (size_t)x) * 3u;

            CHECK(got[0] == expected[0]);
            CHECK(got[1] == expected[1]);
            CHECK(got[2] == expected[2]);
        }
    }
    *offset += payload_size + 2u;
    return true;
}

/*
 * Pin the complete escape stream of multi-rect damage and scroll bursts,
 * packet by packet in submission order with nothing in between: begin
 * update, per-rect patches (compose first for a scroll), end update, and
 * the byte counters describing exactly what was captured.  Any change to
 * how the burst is assembled or written must reproduce this stream
 * byte for byte.
 */
static bool
test_pty_patch_burst_wire_sequence(void)
{
    enum { FRAME_W = 64, FRAME_H = 48 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    kittyfb_stats stats;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[262144];
    char compose[224];
    int compose_length;
    size_t used = 0u;
    size_t offset;
    const kittyfb_rect scattered[3] = {
        {2, 2, 10, 8}, {30, 20, 40, 28}, {50, 40, 60, 46}
    };
    const kittyfb_rect viewport = {0, 2, FRAME_W, FRAME_H};
    const kittyfb_rect toolbar = {0, 0, FRAME_W, 2};
    const kittyfb_rect exposed = {0, 44, FRAME_W, FRAME_H};

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));
    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_INLINE;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);
    drain_descriptor(master);
    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 1u,
                              buffer, sizeof(buffer), &used));
    CHECK(wait_for_encoded_frames(&session, 1u));
    drain_descriptor(master);

    /* Three rects too far apart to coalesce: the burst must carry each in
     * the order given, wrapped in one synchronized update. */
    fill_test_frame(frame, FRAME_W, FRAME_H, 2u);
    CHECK(kittyfb_present_damage(
        &session, frame, FRAME_W, FRAME_H, scattered, 3u));
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?2026l", 8u));

    CHECK(starts_with(buffer, used, "\x1b[?2026h"));
    offset = 8u;
    for (size_t i = 0u; i < 3u; ++i) {
        CHECK(expect_patch_packet(buffer, used, &offset, 1, &scattered[i],
                                  frame, FRAME_W));
    }
    CHECK(used == offset + 8u);
    CHECK(ends_with(buffer, used, "\x1b[?2026l"));
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.damage_presents == 1u);
    CHECK(stats.damage_bytes == (uint64_t)used);

    /* A scroll burst is the same stream with the compose packet leading:
     * begin, compose, exposed strip, extra rects, end. */
    CHECK(setenv("KITTY_KILIX_RENDERING", "1", 1) == 0);
    fill_test_frame(frame, FRAME_W, FRAME_H, 3u);
    CHECK(kittyfb_present_scroll_region(
        &session, frame, FRAME_W, FRAME_H, &viewport, 0, -4, &toolbar, 1u));
    CHECK(unsetenv("KITTY_KILIX_RENDERING") == 0);
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?2026l", 8u));

    compose_length = snprintf(
        compose, sizeof(compose),
        "\x1b_Ga=c,i=%d,r=1,c=1,x=%d,y=%d,X=%d,Y=%d,w=%d,h=%d,C=1,N=2,q=2;\x1b\\",
        1, 0, 2, 0, 6, FRAME_W, 42);
    CHECK(compose_length > 0 && (size_t)compose_length < sizeof(compose));
    CHECK(starts_with(buffer, used, "\x1b[?2026h"));
    offset = 8u;
    CHECK(offset + (size_t)compose_length <= used);
    CHECK(memcmp(buffer + offset, compose, (size_t)compose_length) == 0);
    offset += (size_t)compose_length;
    CHECK(expect_patch_packet(buffer, used, &offset, 1, &exposed,
                              frame, FRAME_W));
    CHECK(expect_patch_packet(buffer, used, &offset, 1, &toolbar,
                              frame, FRAME_W));
    CHECK(used == offset + 8u);
    CHECK(ends_with(buffer, used, "\x1b[?2026l"));
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.scroll_presents == 1u);
    CHECK(stats.scroll_bytes == (uint64_t)used);

    kittyfb_stop(&session);
    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

static bool
test_pty_shm_frame_accepts_inline_damage(void)
{
    enum { FRAME_W = 64, FRAME_H = 48 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    kittyfb_stats stats;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[262144];
    char shm_name[128];
    size_t used = 0u;
    kittyfb_rect rect = {8, 4, 20, 12};

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));
    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_SHM;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);
    if (kittyfb_active_transport(&session) != KITTYFB_TRANSPORT_SHM) {
        (void)fprintf(stderr,
                      "  SKIPPED: shared memory unavailable in this "
                      "environment\n");
        kittyfb_stop(&session);
        (void)close(master);
        (void)close(slave);
        return true;
    }
    drain_descriptor(master);

    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 1u,
                              buffer, sizeof(buffer), &used));
    CHECK(wire_has(buffer, used, "t=s"));
    CHECK(extract_shm_name(buffer, used, shm_name, sizeof(shm_name)));
    CHECK(consume_shm_frame(shm_name, frame, FRAME_W, FRAME_H));

    fill_test_frame(frame, FRAME_W, FRAME_H, 2u);
    CHECK(kittyfb_present_damage(
        &session, frame, FRAME_W, FRAME_H, &rect, 1u));
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?2026l", 8u));
    CHECK(wire_has(buffer, used, "a=f"));
    CHECK(!wire_has(buffer, used, "a=T"));
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.damage_presents == 1u);
    CHECK(stats.damage_fallbacks == 0u);

    kittyfb_stop(&session);
    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

static bool
test_pty_scroll_compose_and_fallback(void)
{
    enum { FRAME_W = 64, FRAME_H = 48 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    kittyfb_stats stats;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[262144];
    size_t used = 0u;
    kittyfb_rect viewport = {0, 2, FRAME_W, FRAME_H};
    kittyfb_rect toolbar = {0, 0, FRAME_W, 2};

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));
    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_INLINE;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);
    drain_descriptor(master);
    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 1u,
                              buffer, sizeof(buffer), &used));
    CHECK(wire_has(buffer, used, "z=-1073741825"));

    CHECK(setenv("KITTY_KILIX_RENDERING", "1", 1) == 0);
    fill_test_frame(frame, FRAME_W, FRAME_H, 2u);
    CHECK(kittyfb_present_scroll_region(
        &session, frame, FRAME_W, FRAME_H, &viewport,
        0, -4, NULL, 0u));
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?2026l", 8u));
    CHECK(wire_has(buffer, used, "a=c,i=1,r=1,c=1"));
    CHECK(wire_has(buffer, used, "x=0,y=2,X=0,Y=6,w=64,h=42"));
    CHECK(wire_has(buffer, used, "C=1,N=2,q=2"));
    CHECK(wire_has(buffer, used, "a=f"));
    CHECK(wire_has(buffer, used, "x=0,y=44,s=64,v=4"));
    CHECK(!wire_has(buffer, used, "x=0,y=0,s=64,v=2"));
    CHECK(!wire_has(buffer, used, "a=T"));
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.scroll_presents == 1u);
    CHECK(stats.scroll_fallbacks == 0u);
    CHECK(stats.scroll_bytes > 0u);

    /* The original API remains a whole-frame convenience wrapper. */
    fill_test_frame(frame, FRAME_W, FRAME_H, 3u);
    CHECK(kittyfb_present_scroll(
        &session, frame, FRAME_W, FRAME_H, 0, 4, &toolbar, 1u));
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?2026l", 8u));
    CHECK(wire_has(buffer, used, "x=0,y=4,X=0,Y=0,w=64,h=44"));
    CHECK(wire_has(buffer, used, "x=0,y=0,s=64,v=4"));
    CHECK(!wire_has(buffer, used, "a=T"));

    /* The environment marker is capability negotiation.  Without it a
     * standard Kitty-compatible terminal receives a complete frame and can
     * never be left with a rejected fork-only compose command. */
    CHECK(unsetenv("KITTY_KILIX_RENDERING") == 0);
    fill_test_frame(frame, FRAME_W, FRAME_H, 4u);
    CHECK(kittyfb_present_scroll(
        &session, frame, FRAME_W, FRAME_H, 0, -4, NULL, 0u));
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?2026l", 8u));
    CHECK(wire_has(buffer, used, "a=T"));
    CHECK(!wire_has(buffer, used, "a=c"));
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.scroll_presents == 2u);
    CHECK(stats.scroll_fallbacks == 1u);

    CHECK(!kittyfb_present_scroll(
        &session, NULL, FRAME_W, FRAME_H, 0, -4, NULL, 0u));
    CHECK(!kittyfb_present_scroll(
        &session, frame, FRAME_W, FRAME_H, 0, -4, NULL, 1u));
    CHECK(!kittyfb_present_scroll_region(
        &session, frame, FRAME_W, FRAME_H, NULL, 0, -4, NULL, 0u));

    kittyfb_stop(&session);
    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

/* Falling back rather than refusing is what lets a caller use this
 * unconditionally instead of reasoning about when it pays. */
static bool
test_pty_damage_falls_back(void)
{
    enum { FRAME_W = 64, FRAME_H = 48, MANY_RECTS = 65 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    kittyfb_stats stats;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[262144];
    size_t used = 0u;
    kittyfb_rect rect;
    kittyfb_rect many[MANY_RECTS];

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));
    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_INLINE;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);
    drain_descriptor(master);
    fill_test_frame(frame, FRAME_W, FRAME_H, 3u);

    /* Nothing on screen yet, so there is no image to edit: transmit. */
    rect.x0 = 0;
    rect.y0 = 0;
    rect.x1 = 8;
    rect.y1 = 8;
    CHECK(kittyfb_present_damage(&session, frame, FRAME_W, FRAME_H, &rect, 1u));
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?2026l", 8u));
    CHECK(wire_has(buffer, used, "a=T"));
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.damage_fallbacks == 1u);
    CHECK(stats.damage_presents == 0u);

    /* Damage covering the whole frame is cheaper sent whole. */
    rect.x1 = FRAME_W;
    rect.y1 = FRAME_H;
    CHECK(kittyfb_present_damage(&session, frame, FRAME_W, FRAME_H, &rect, 1u));
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?2026l", 8u));
    CHECK(wire_has(buffer, used, "a=T"));
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.damage_fallbacks == 2u);

    /* An edit cannot be applied to a root frame of different dimensions. */
    rect.x1 = 8;
    rect.y1 = 8;
    CHECK(kittyfb_present_damage(&session, frame, FRAME_W / 2, FRAME_H / 2,
                                 &rect, 1u));
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?2026l", 8u));
    CHECK(wire_has(buffer, used, "a=T"));

    /* More rects than the bounded edit set must retransmit the whole newest
     * frame, never silently ignore entries after the first 64. */
    for (size_t i = 0u; i < sizeof(many) / sizeof(many[0]); ++i) {
        many[i] = (kittyfb_rect){0, 0, 1, 1};
    }
    CHECK(kittyfb_present_damage(
        &session, frame, FRAME_W, FRAME_H, many,
        sizeof(many) / sizeof(many[0])));
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?2026l", 8u));
    CHECK(wire_has(buffer, used, "a=T"));

    /* A centering-only resize sets clear_pending even when dimensions are
     * unchanged. Damage must preserve that clear by falling back. */
    pthread_mutex_lock(&session.frame_lock);
    session.clear_pending = true;
    pthread_mutex_unlock(&session.frame_lock);
    CHECK(kittyfb_present_damage(
        &session, frame, FRAME_W, FRAME_H, &rect, 1u));
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?2026l", 8u));
    CHECK(wire_has(buffer, used, "\x1b[2J"));

    kittyfb_get_stats(&session, &stats);
    CHECK(stats.damage_fallbacks == 5u);

    kittyfb_stop(&session);
    (void)close(master);
    (void)close(slave);
    return true;
}

static bool
test_pty_damage_rect_handling(void)
{
    enum { FRAME_W = 64, FRAME_H = 48 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    kittyfb_stats stats;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[262144];
    size_t used = 0u;
    kittyfb_rect rects[4];

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));
    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_INLINE;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);
    drain_descriptor(master);
    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 4u,
                              buffer, sizeof(buffer), &used));

    /* No rects is not an error: a frame where nothing changed needs no
     * write, and making every caller check would spread that everywhere. */
    CHECK(kittyfb_present_damage(&session, frame, FRAME_W, FRAME_H, NULL, 0u));
    CHECK(kittyfb_present_damage(&session, frame, FRAME_W, FRAME_H, rects, 0u));

    /* Empty and inverted rects are skipped rather than rejected: a caller
     * deriving damage from geometry should not special-case frame edges. */
    rects[0].x0 = 5; rects[0].y0 = 5; rects[0].x1 = 5; rects[0].y1 = 9;
    rects[1].x0 = 9; rects[1].y0 = 9; rects[1].x1 = 4; rects[1].y1 = 12;
    CHECK(kittyfb_present_damage(&session, frame, FRAME_W, FRAME_H, rects, 2u));
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.damage_presents == 0u);
    CHECK(stats.damage_fallbacks == 0u);

    /* Out of bounds is clamped, not refused. */
    rects[0].x0 = -10; rects[0].y0 = -10; rects[0].x1 = 6; rects[0].y1 = 6;
    CHECK(kittyfb_present_damage(&session, frame, FRAME_W, FRAME_H, rects, 1u));
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?2026l", 8u));
    CHECK(wire_has(buffer, used, "x=0"));
    CHECK(wire_has(buffer, used, "y=0"));
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.damage_presents == 1u);

    /* Adjacent rectangles with no gap carry the same pixels as their union.
     * Coalesce them so one animation-frame edit replaces two headers and
     * writes, while retaining the exact destination and extent. */
    rects[0] = (kittyfb_rect){2, 3, 6, 7};
    rects[1] = (kittyfb_rect){6, 3, 10, 7};
    CHECK(kittyfb_present_damage(
        &session, frame, FRAME_W, FRAME_H, rects, 2u));
    used = 0u;
    CHECK(wait_for_bytes(master, buffer, sizeof(buffer), &used,
                         "\x1b[?2026l", 8u));
    CHECK(wire_has(buffer, used, "x=2,y=3,s=8,v=4"));
    CHECK(count_bytes(buffer, used, "r=1", 3u) == 1u);
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.damage_presents == 2u);

    /* Bad arguments still fail. */
    CHECK(!kittyfb_present_damage(NULL, frame, FRAME_W, FRAME_H, rects, 1u));
    CHECK(!kittyfb_present_damage(&session, NULL, FRAME_W, FRAME_H, rects, 1u));
    CHECK(!kittyfb_present_damage(&session, frame, 0, FRAME_H, rects, 1u));
    CHECK(!kittyfb_present_damage(&session, frame, FRAME_W, FRAME_H, NULL, 1u));
    CHECK(!kittyfb_present_damage(
        &session, frame, INT_MAX, INT_MAX, rects, 1u));

    kittyfb_stop(&session);
    (void)close(master);
    (void)close(slave);
    return true;
}

/* A damage call immediately after queueing a full frame is the ordinary
 * editor workload and used to race the presenter's scratch buffers/output.
 * Keep a terminal reader active so both paths can run under ThreadSanitizer. */
static bool
test_pty_damage_serializes_with_presenter(void)
{
    enum { FRAME_W = 320, FRAME_H = 180, ITERATIONS = 24 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    kittyfb_stats stats;
    wire_drainer drainer = {0};
    pthread_t reader;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[1048576];
    size_t used = 0u;
    kittyfb_rect rect = {0, 0, 32, 32};

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));
    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_INLINE;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);
    drain_descriptor(master);
    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 1u,
                              buffer, sizeof(buffer), &used));

    drainer.fd = master;
    CHECK(pthread_create(&reader, NULL, wire_drainer_main, &drainer) == 0);
    for (uint32_t iteration = 0u; iteration < ITERATIONS; ++iteration) {
        fill_noise_frame(frame, FRAME_W, FRAME_H,
                         UINT32_C(0x9e3779b9) ^ iteration);
        CHECK(kittyfb_present(&session, frame, FRAME_W, FRAME_H));
        CHECK(kittyfb_present_damage(
            &session, frame, FRAME_W, FRAME_H, &rect, 1u));
    }
    kittyfb_suspend(&session);
    __atomic_store_n(&drainer.stop, 1, __ATOMIC_RELEASE);
    CHECK(pthread_join(reader, NULL) == 0);
    CHECK(drainer.bytes > 0u);
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.frames_encoded > 0u);
    CHECK(stats.encode_failures == 0u);
    CHECK(!kittyfb_failed(&session));

    kittyfb_stop(&session);
    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

static bool
test_pty_shm_transport(void)
{
    enum { FRAME_W = 32, FRAME_H = 16 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    kittyfb_stats stats;
    static uint8_t frame_a[(size_t)FRAME_W * FRAME_H * 4u];
    static uint8_t frame_b[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[65536];
    char name_a[128];
    char name_b[128];
    char name_c[128];
    int probe_fd;
    size_t used;

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));

    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_SHM;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);

    /* Where shared memory is unavailable (no /dev/shm, or a sandbox that
     * denies it) the library falls back rather than failing the start,
     * and there is nothing for this test to assert. */
    if (kittyfb_active_transport(&session) != KITTYFB_TRANSPORT_SHM) {
        /* Say so loudly: a silently skipped test that prints "ok" is
         * indistinguishable from one that verified something. */
        (void)fprintf(stderr,
                      "  SKIPPED: shared memory unavailable in this "
                      "environment\n");
        kittyfb_stop(&session);
        (void)close(master);
        (void)close(slave);
        return true;
    }

    drain_descriptor(master);

    /* First frame: id 1, deletes id 2, and the pixels arrive through
     * shared memory rather than the escape stream. */
    CHECK(present_and_capture(&session, master, frame_a, FRAME_W, FRAME_H, 0u,
                              buffer, sizeof(buffer), &used));
    CHECK(starts_with(buffer, used, "\x1b[?2026h\x1b[1;1H"));
    CHECK(contains_str(buffer, used, "\x1b_Ga=T,f=32,i=1,q=2,t=s,s=32,v=16,z=-1073741825;"));
    CHECK(contains_str(buffer, used, "\x1b_Ga=d,d=I,i=2,q=2\x1b\\"));
    CHECK(!contains_str(buffer, used, "o=z"));

    /* The whole frame costs a few hundred bytes of escape stream instead
     * of a compressed copy of every pixel.  This is the reason the
     * transport exists, so assert it rather than trusting it. */
    CHECK(used < (size_t)FRAME_W * FRAME_H * 3u);
    CHECK(extract_shm_name(buffer, used, name_a, sizeof(name_a)));

    /* Second frame while the first is still unread: it must land in a
     * different slot, or the terminal would be handed an object whose
     * contents changed under it. */
    CHECK(present_and_capture(&session, master, frame_b, FRAME_W, FRAME_H, 3u,
                              buffer, sizeof(buffer), &used));
    CHECK(contains_str(buffer, used, "\x1b_Ga=T,f=32,i=2,q=2,t=s,s=32,v=16,z=-1073741825;"));
    CHECK(contains_str(buffer, used, "\x1b_Ga=d,d=I,i=1,q=2\x1b\\"));
    CHECK(extract_shm_name(buffer, used, name_b, sizeof(name_b)));
    CHECK(strcmp(name_b, name_a) != 0);

    /* Both objects are still live and hold their own frame's pixels. */
    CHECK(consume_shm_frame(name_a, frame_a, FRAME_W, FRAME_H));
    CHECK(consume_shm_frame(name_b, frame_b, FRAME_W, FRAME_H));

    /* Consumption is an unlink, so the slot is reaped and its name comes
     * back round.  Recycling a name is safe: it creates a NEW object,
     * and the terminal's mapping of the old one stays valid. */
    CHECK(present_and_capture(&session, master, frame_a, FRAME_W, FRAME_H, 7u,
                              buffer, sizeof(buffer), &used));
    CHECK(extract_shm_name(buffer, used, name_c, sizeof(name_c)));
    CHECK(strcmp(name_c, name_a) == 0);
    CHECK(consume_shm_frame(name_c, frame_a, FRAME_W, FRAME_H));

    CHECK(wait_for_encoded_frames(&session, 3u));
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.frames_encoded == 3u);
    CHECK(stats.encode_failures == 0u);
    CHECK(!kittyfb_failed(&session));

    /* A frame presented but never consumed leaves a live object behind,
     * which teardown must clean up. */
    CHECK(present_and_capture(&session, master, frame_b, FRAME_W, FRAME_H, 9u,
                              buffer, sizeof(buffer), &used));
    CHECK(extract_shm_name(buffer, used, name_b, sizeof(name_b)));
    probe_fd = shm_open(name_b, O_RDONLY, 0);
    CHECK(probe_fd >= 0);
    CHECK(close(probe_fd) == 0);

    kittyfb_suspend(&session);
    /* Suspension retains heap high-water buffers but releases every tmpfs
     * object; a stopped job may remain suspended indefinitely. */
    CHECK(session.shm_slots == NULL);
    CHECK(session.shm_slot_count == 0);
    /* Teardown unlinks every slot, consumed or not: a slot left behind
     * leaks a frame of tmpfs until the next reboot. */
    CHECK(shm_open(name_b, O_RDONLY, 0) < 0);
    kittyfb_stop(&session);

    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

static bool
test_pty_shm_saturation_drops(void)
{
    enum { FRAME_W = 16, FRAME_H = 8, SLOTS = 2 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    kittyfb_stats stats;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[65536];
    size_t used;
    int attempt;

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));

    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_SHM;
    options.shm_slots = SLOTS;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);
    if (kittyfb_active_transport(&session) != KITTYFB_TRANSPORT_SHM) {
        /* Say so loudly: a silently skipped test that prints "ok" is
         * indistinguishable from one that verified something. */
        (void)fprintf(stderr,
                      "  SKIPPED: shared memory unavailable in this "
                      "environment\n");
        kittyfb_stop(&session);
        (void)close(master);
        (void)close(slave);
        return true;
    }
    drain_descriptor(master);

    /* Never unlink: this terminal accepts frames and never reads them,
     * so the ring fills and stays full. */
    for (attempt = 0; attempt < SLOTS + 4; ++attempt) {
        used = 0u;
        CHECK(kittyfb_present(&session, frame, FRAME_W, FRAME_H));
        (void)wait_for_bytes(master, buffer, sizeof(buffer), &used,
                             "\x1b[?2026l", 8u);
        sleep_milliseconds(20);
    }

    /* A saturated ring drops the newest frame - the same bargain the
     * pending slot already makes - and must NOT latch a failure, which
     * would stop the session for what is a transient condition. */
    CHECK(wait_for_dropped_frames(&session, 1u));
    CHECK(!kittyfb_failed(&session));
    kittyfb_get_stats(&session, &stats);
    CHECK(stats.encode_failures == 0u);
    CHECK(stats.frames_encoded <= (uint64_t)SLOTS);

    /* A resize clear belongs to the next frame that actually reaches the
     * terminal.  A saturated ring must not consume it with a dropped frame. */
    pthread_mutex_lock(&session.frame_lock);
    session.clear_pending = true;
    pthread_mutex_unlock(&session.frame_lock);
    kittyfb_get_stats(&session, &stats);
    uint64_t dropped_before_clear = stats.frames_dropped;
    CHECK(kittyfb_present(&session, frame, FRAME_W, FRAME_H));
    CHECK(wait_for_dropped_frames(&session, dropped_before_clear + 1u));
    pthread_mutex_lock(&session.frame_lock);
    CHECK(session.clear_pending);
    pthread_mutex_unlock(&session.frame_lock);

    kittyfb_stop(&session);
    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

/* Once a slot is populated, any later packet/build/write failure must unlink
 * and release it. Otherwise no terminal received the name and the ring loses
 * that slot forever. */
static bool
test_pty_shm_publish_failure_rolls_back(void)
{
    enum { FRAME_W = 16, FRAME_H = 8 };
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    kittyfb_stats stats;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));
    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_SHM;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);
    if (kittyfb_active_transport(&session) != KITTYFB_TRANSPORT_SHM) {
        (void)fprintf(stderr,
                      "  SKIPPED: shared memory unavailable in this "
                      "environment\n");
        kittyfb_stop(&session);
        CHECK(close(master) == 0);
        CHECK(close(slave) == 0);
        return true;
    }
    drain_descriptor(master);

    /* Fault-inject an inconsistent packet allocation: growth believes the
     * capacity exists, then the builder safely rejects the NULL output. */
    CHECK(session.packet_buffer == NULL);
    session.packet_capacity = 1024u;
    CHECK(kittyfb_present(&session, frame, FRAME_W, FRAME_H));
    CHECK(wait_for_failure(&session));

    pthread_mutex_lock(&session.frame_lock);
    for (int index = 0; index < session.shm_slot_count; ++index) {
        int fd;

        CHECK(!session.shm_slots[index].busy);
        CHECK(session.shm_slots[index].fd == -1);
        fd = shm_open(session.shm_slots[index].name, O_RDONLY, 0);
        CHECK(fd < 0 && errno == ENOENT);
    }
    session.packet_capacity = 0u;
    pthread_mutex_unlock(&session.frame_lock);

    kittyfb_get_stats(&session, &stats);
    CHECK(stats.encode_failures == 1u);
    kittyfb_stop(&session);
    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);
    return true;
}

static bool
test_shm_auto_declines_under_tmux(void)
{
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    char *saved = getenv("TMUX");
    char preserved[256];
    bool had_tmux = saved != NULL;

    if (had_tmux) {
        (void)snprintf(preserved, sizeof(preserved), "%s", saved);
    }
    CHECK(setenv("TMUX", "/tmp/tmux-1000/default,1234,0", 1) == 0);

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));
    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.transport = KITTYFB_TRANSPORT_AUTO;
    CHECK(start_with_fake_terminal(&session, master, slave, &options,
                                   graphics_reply, NULL) == 0);

    /* tmux forwards the escape to a terminal that need not share this
     * process's /dev/shm, and a name it cannot open is a blank screen
     * rather than a degraded one.  AUTO must decline. */
    CHECK(kittyfb_active_transport(&session) == KITTYFB_TRANSPORT_INLINE);

    kittyfb_stop(&session);
    CHECK(close(master) == 0);
    CHECK(close(slave) == 0);

    if (had_tmux) {
        CHECK(setenv("TMUX", preserved, 1) == 0);
    } else {
        CHECK(unsetenv("TMUX") == 0);
    }
    return true;
}

static bool
test_shm_reap_orphans(void)
{
    /* A process that dies without unwinding leaves its frames behind.
     * Reaping must remove exactly those, and must not touch an object
     * whose owner is still running - this process. */
    char dead_name[64];
    char live_name[64];
    long dead_pid = 0;
    int fd;
    int reaped;

    /* Find a pid that does not exist, so the object looks abandoned. */
    for (long candidate = 4194303L; candidate > 1L; --candidate) {
        if (kill((pid_t)candidate, 0) != 0 && errno == ESRCH) {
            dead_pid = candidate;
            break;
        }
    }
    if (dead_pid == 0) {
        (void)fprintf(stderr, "  SKIPPED: no free pid to impersonate\n");
        return true;
    }

    (void)snprintf(dead_name, sizeof(dead_name), "/kilix-fb-%ld-0-0",
                   dead_pid);
    (void)snprintf(live_name, sizeof(live_name), "/kilix-fb-%ld-0-0",
                   (long)getpid());
    (void)shm_unlink(dead_name);
    (void)shm_unlink(live_name);

    fd = shm_open(dead_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        (void)fprintf(stderr,
                      "  SKIPPED: shared memory unavailable in this "
                      "environment\n");
        return true;
    }
    CHECK(close(fd) == 0);
    fd = shm_open(live_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);

    reaped = kittyfb_reap_orphans();
    CHECK(reaped >= 1);

    /* The abandoned one is gone; the live owner's survives. */
    CHECK(shm_open(dead_name, O_RDONLY, 0) < 0);
    fd = shm_open(live_name, O_RDONLY, 0);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);
    CHECK(shm_unlink(live_name) == 0);
    return true;
}

typedef bool (*test_function)(void);

typedef struct test_case {
    const char *name;
    test_function function;
} test_case;

int
main(void)
{
    static const test_case tests[] = {
        {"base64 encoding", test_base64_encoding},
        {"geometry derivation", test_geometry_derivation},
        {"geometry small-terminal clamps", test_geometry_small_terminal_clamps},
        {"packet chunk boundaries", test_packet_chunk_boundaries},
        {"packet wrapper and delete", test_packet_wrapper_and_delete},
        {"shm packet shape", test_shm_packet_shape},
        {"options defaults", test_options_defaults},
        {"inactive session is safe", test_inactive_session_is_safe},
        {"failure snapshot is synchronized",
         test_failure_snapshot_is_synchronized},
        {"PTY lifecycle", test_pty_lifecycle},
        {"PTY start after stop", test_pty_start_after_stop},
        {"PTY suspend retains buffers", test_pty_suspend_retains_buffers},
        {"PTY probe rejects DA1-only terminal",
         test_pty_probe_rejects_da1_only_terminal},
        {"PTY probe timeout survives signal storm",
         test_pty_probe_timeout_survives_signal_storm},
        {"PTY probe disabled starts blind",
         test_pty_probe_disabled_starts_blind},
        {"PTY emergency restore", test_pty_emergency_restore},
        {"PTY emergency before present",
         test_pty_emergency_restore_before_present},
        {"PTY resize", test_pty_resize},
        {"PTY origin is the centering offset",
         test_pty_origin_is_the_centering_offset},
        {"PTY frames and patches from two threads",
         test_pty_frames_and_patches_from_two_threads},
        {"PTY damage patches in place", test_pty_damage_patches_in_place},
        {"PTY damage multichunk protocol",
         test_pty_damage_multichunk_protocol},
        {"PTY patch burst wire sequence",
         test_pty_patch_burst_wire_sequence},
        {"PTY shm frame accepts inline damage",
         test_pty_shm_frame_accepts_inline_damage},
        {"PTY scroll compose and fallback",
         test_pty_scroll_compose_and_fallback},
        {"PTY damage falls back", test_pty_damage_falls_back},
        {"PTY damage rect handling", test_pty_damage_rect_handling},
        {"PTY damage serializes with presenter",
         test_pty_damage_serializes_with_presenter},
        {"PTY shm transport", test_pty_shm_transport},
        {"PTY shm saturation drops", test_pty_shm_saturation_drops},
        {"PTY shm publish failure rolls back",
         test_pty_shm_publish_failure_rolls_back},
        {"shm AUTO declines under tmux", test_shm_auto_declines_under_tmux},
        {"shm reap orphans", test_shm_reap_orphans}
    };
    size_t passed = 0u;
    size_t index;

    /* The public overrides are useful interactively but must not silently
     * select different transports or bypass probes in a test process. */
    if (unsetenv("KITTYFB_TRANSPORT") != 0 ||
        unsetenv("KITTYFB_SKIP_PROBE") != 0 ||
        unsetenv("KITTY_KILIX_RENDERING") != 0 || unsetenv("TMUX") != 0) {
        (void)perror("unsetenv");
        return 1;
    }

    for (index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        const bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        ++passed;
    }
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
