#include "kitty_framebuffer.h"
#include "kitty_framebuffer_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

    /* A tiny terminal upscales to the minimum, then snaps to whole
     * cells and even pixels; the origin clamps to the top-left. */
    CHECK(kittyfb_derive_geometry(40, 12, 0, 0, &options, &geometry));
    CHECK(geometry.width == 638 && geometry.height == 396);
    CHECK(geometry.width % 2 == 0 && geometry.height % 2 == 0);
    CHECK(geometry.origin_row == 1 && geometry.origin_column == 1);

    /* Clamping to the maximum snaps down to whole cells (700 -> 693 for
     * 9-pixel cells), then forces even pixel counts (693 -> 692). */
    options.max_width = 700;
    options.max_height = 500;
    CHECK(kittyfb_derive_geometry(100, 30, 900, 540, &options, &geometry));
    CHECK(geometry.width == 692 && geometry.height == 486);
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
        "\x1b_Ga=T,f=24,i=1,q=2,o=z,s=640,v=400,m=0;";
    static const char header_more[] =
        "\x1b_Ga=T,f=24,i=1,q=2,o=z,s=640,v=400,m=1;";

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
        "\x1b_Ga=T,f=24,i=7,q=2,o=z,s=320,v=200,m=0;AAAA\x1b\\"));
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
    CHECK(options.manage_raw_mode);
    CHECK(options.manage_alt_screen);
    CHECK(options.hide_cursor);
    CHECK(options.probe_graphics);
    CHECK(options.install_winch_handler);
    CHECK(options.probe_timeout_ms == 1000);
    CHECK(options.min_width == 640 && options.min_height == 400);
    CHECK(options.max_width == 1600 && options.max_height == 1000);
    CHECK(options.image_id_a == 1 && options.image_id_b == 2);
    CHECK(options.zlib_level == 1);
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
        sleep_milliseconds(10);
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
    int result;

    replier.master_fd = master;
    replier.reply = reply;
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
test_pty_lifecycle(void)
{
    enum { FRAME_W = 32, FRAME_H = 16 };
    int master = -1;
    int slave = -1;
    struct termios original;
    struct termios restored;
    kittyfb_session session;
    kittyfb_stats stats;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[65536];
    size_t used;
    static const char restore_sequence[] =
        "\x1b[?2026l\x1b\\"
        "\x1b_Ga=d,d=i,i=1,q=2\x1b\\"
        "\x1b_Ga=d,d=i,i=2,q=2\x1b\\"
        "\x1b[?2026l\x1b[?25h\x1b[?1049l";

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, &original));

    kittyfb_session_init(&session);
    CHECK(start_with_fake_terminal(&session, master, slave, NULL,
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

    /* First frame goes out under id 1 inside a synchronized update and
     * deletes id 2; the pixel data round-trips through zlib + base64. */
    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 0u,
                              buffer, sizeof(buffer), &used));
    CHECK(starts_with(buffer, used, "\x1b[?2026h\x1b[1;1H"));
    CHECK(decoded_payload_matches(
        buffer, used, "\x1b_Ga=T,f=24,i=1,q=2,o=z,s=32,v=16,m=0;",
        frame, FRAME_W, FRAME_H));
    CHECK(contains_str(buffer, used, "\x1b_Ga=d,d=I,i=2,q=2\x1b\\"));

    /* The ids ping-pong: the second frame is id 2 and deletes id 1. */
    CHECK(present_and_capture(&session, master, frame, FRAME_W, FRAME_H, 3u,
                              buffer, sizeof(buffer), &used));
    CHECK(contains_str(buffer, used,
                       "\x1b_Ga=T,f=24,i=2,q=2,o=z,s=32,v=16,m=0;"));
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
                       "\x1b_Ga=T,f=24,i=1,q=2,o=z,s=16,v=8,m=0;"));
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
test_pty_probe_rejects_da1_only_terminal(void)
{
    int master = -1;
    int slave = -1;
    struct termios original;
    struct termios after;
    kittyfb_session session;
    int saved_errno = 0;
    static char buffer[8192];
    size_t used;

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, &original));

    /* This terminal answers device attributes but not the graphics
     * query: start must fail cleanly with ENOTSUP, restore termios, and
     * never touch the alternate screen. */
    kittyfb_session_init(&session);
    CHECK(start_with_fake_terminal(&session, master, slave, NULL,
                                   da1_only_reply, &saved_errno) == -1);
    CHECK(saved_errno == ENOTSUP);
    CHECK(tcgetattr(slave, &after) == 0);
    CHECK(same_termios(&original, &after));

    used = read_available(master, buffer, sizeof(buffer));
    CHECK(contains_str(buffer, used, "\x1b_Gi=31,a=q"));
    CHECK(!contains_str(buffer, used, "\x1b[?1049h"));

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
    size_t used;

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, NULL));

    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.probe_graphics = false;
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
    kittyfb_session session;
    static uint8_t frame[(size_t)FRAME_W * FRAME_H * 4u];
    static char buffer[65536];
    size_t used;
    static const char restore_sequence[] =
        "\x1b[?2026l\x1b\\"
        "\x1b_Ga=d,d=i,i=1,q=2\x1b\\"
        "\x1b_Ga=d,d=i,i=2,q=2\x1b\\"
        "\x1b[?2026l\x1b[?25h\x1b[?1049l";

    CHECK(open_test_pty(&master, &slave, 100, 30, 900, 540, &original));

    kittyfb_session_init(&session);
    CHECK(start_with_fake_terminal(&session, master, slave, NULL,
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
    kittyfb_emergency_restore(&session);
    kittyfb_stop(&session);

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
        {"options defaults", test_options_defaults},
        {"inactive session is safe", test_inactive_session_is_safe},
        {"PTY lifecycle", test_pty_lifecycle},
        {"PTY start after stop", test_pty_start_after_stop},
        {"PTY probe rejects DA1-only terminal",
         test_pty_probe_rejects_da1_only_terminal},
        {"PTY probe disabled starts blind",
         test_pty_probe_disabled_starts_blind},
        {"PTY emergency restore", test_pty_emergency_restore},
        {"PTY resize", test_pty_resize}
    };
    size_t passed = 0u;
    size_t index;

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
