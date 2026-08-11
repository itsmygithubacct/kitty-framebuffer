#include "kitty_framebuffer.h"
#include "kitty_framebuffer_internal.h"

#include <errno.h>
#include <fcntl.h>
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

typedef struct drain_context {
    int fd;
    int stop;
    uint64_t bytes;
} drain_context;

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0u;
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static void pause_briefly(void)
{
    const struct timespec pause = {0, 100000L};

    (void)nanosleep(&pause, NULL);
}

static void *drain_main(void *opaque)
{
    drain_context *context = opaque;
    char buffer[32768];

    while (!__atomic_load_n(&context->stop, __ATOMIC_ACQUIRE)) {
        struct pollfd descriptor = {context->fd, POLLIN, 0};
        int ready = poll(&descriptor, 1u, 20);

        if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
            ssize_t count = read(context->fd, buffer, sizeof(buffer));
            if (count > 0) {
                context->bytes += (uint64_t)count;
            }
        } else if (ready < 0 && errno != EINTR) {
            break;
        }
    }
    return NULL;
}

static void fill_noise(uint8_t *rgba, size_t pixels, uint32_t seed)
{
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

static void fill_smooth(uint8_t *rgba, int width, int height)
{
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t at = ((size_t)y * (size_t)width + (size_t)x) * 4u;
            rgba[at] = (uint8_t)(x / 4);
            rgba[at + 1u] = (uint8_t)(y / 3);
            rgba[at + 2u] = (uint8_t)((x + y) / 8);
            rgba[at + 3u] = 255u;
        }
    }
}

static bool wait_for_encoded(kittyfb_session *session, uint64_t target)
{
    uint64_t deadline = monotonic_ns() + UINT64_C(10000000000);

    for (;;) {
        kittyfb_stats stats;

        kittyfb_get_stats(session, &stats);
        if (stats.frames_encoded >= target) {
            return true;
        }
        if (stats.encode_failures != 0u || monotonic_ns() >= deadline) {
            return false;
        }
        pause_briefly();
    }
}

static bool wait_for_idle(kittyfb_session *session)
{
    uint64_t deadline = monotonic_ns() + UINT64_C(10000000000);

    for (;;) {
        kittyfb_stats stats;

        kittyfb_get_stats(session, &stats);
        if (stats.frames_encoded + stats.frames_dropped >=
            stats.frames_presented) {
            return stats.encode_failures == 0u;
        }
        if (stats.encode_failures != 0u || monotonic_ns() >= deadline) {
            return false;
        }
        pause_briefly();
    }
}

static bool open_benchmark_pty(int *master, int *slave)
{
    struct winsize window = {0};
    struct termios raw;
    int flags;

    window.ws_col = 100;
    window.ws_row = 30;
    window.ws_xpixel = 900;
    window.ws_ypixel = 540;
    if (openpty(master, slave, NULL, NULL, &window) != 0 ||
        tcgetattr(*slave, &raw) != 0) {
        return false;
    }
    cfmakeraw(&raw);
    if (tcsetattr(*slave, TCSANOW, &raw) != 0) {
        return false;
    }
    flags = fcntl(*master, F_GETFL);
    return flags >= 0 &&
           fcntl(*master, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool benchmark_base64(void)
{
    const size_t input_size = 4u * 1024u * 1024u;
    const size_t output_size = ((input_size + 2u) / 3u) * 4u;
    const int repetitions = 24;
    uint8_t *input = malloc(input_size);
    char *output = malloc(output_size);
    uint64_t checksum = 0u;
    uint64_t start;
    uint64_t elapsed;

    if (input == NULL || output == NULL) {
        free(input);
        free(output);
        return false;
    }
    for (size_t index = 0u; index < input_size; ++index) {
        input[index] = (uint8_t)(index * 131u + index / 17u);
    }
    start = monotonic_ns();
    for (int run = 0; run < repetitions; ++run) {
        size_t length = kittyfb_base64_encode(input, input_size, output);
        checksum += (uint8_t)output[(size_t)run % length];
    }
    elapsed = monotonic_ns() - start;
    (void)printf("base64_4mib_mib_s %.3f checksum=%llu\n",
                 ((double)input_size * repetitions / (1024.0 * 1024.0)) /
                     ((double)elapsed / 1.0e9),
                 (unsigned long long)checksum);
    free(input);
    free(output);
    return true;
}

static bool benchmark_packet_build(void)
{
    const size_t payload_size = 1024u * 1024u;
    const int repetitions = 400;
    char *payload = malloc(payload_size);
    char *packet = malloc(payload_size + payload_size / 32u + 4096u);
    uint64_t checksum = 0u;
    uint64_t start;
    uint64_t elapsed;

    if (payload == NULL || packet == NULL) {
        free(payload);
        free(packet);
        return false;
    }
    (void)memset(payload, 'A', payload_size);
    start = monotonic_ns();
    for (int run = 0; run < repetitions; ++run) {
        size_t length = kittyfb_build_packet(
            packet, payload_size + payload_size / 32u + 4096u,
            payload, payload_size, 1, 2, 1280, 720, "\x1b[1;1H", false);
        if (length == 0u) {
            free(payload);
            free(packet);
            return false;
        }
        checksum += (uint8_t)packet[length - 1u];
    }
    elapsed = monotonic_ns() - start;
    (void)printf("packet_1mib_mib_s %.3f checksum=%llu\n",
                 ((double)payload_size * repetitions / (1024.0 * 1024.0)) /
                     ((double)elapsed / 1.0e9),
                 (unsigned long long)checksum);
    free(payload);
    free(packet);
    return true;
}

static bool run_sequential_frames(
    kittyfb_session *session,
    const uint8_t *frame,
    int width,
    int height,
    int repetitions,
    const char *label)
{
    kittyfb_stats before;
    uint64_t start;
    uint64_t elapsed;

    kittyfb_get_stats(session, &before);
    start = monotonic_ns();
    for (int run = 0; run < repetitions; ++run) {
        if (!kittyfb_present(session, frame, width, height) ||
            !wait_for_encoded(session, before.frames_encoded +
                                       (uint64_t)run + 1u)) {
            return false;
        }
    }
    elapsed = monotonic_ns() - start;
    (void)printf("%s_ms %.3f\n", label,
                 (double)elapsed / 1.0e6 / repetitions);
    return true;
}

static bool benchmark_presenter(void)
{
    enum { WIDTH = 640, HEIGHT = 360 };
    const size_t pixels = (size_t)WIDTH * HEIGHT;
    const size_t bytes = pixels * 4u;
    int master = -1;
    int slave = -1;
    kittyfb_session session;
    kittyfb_options options;
    drain_context drain = {0};
    pthread_t reader;
    uint8_t *frame = malloc(bytes);
    uint64_t start;
    uint64_t elapsed;
    bool ok = false;

    if (frame == NULL || !open_benchmark_pty(&master, &slave)) {
        free(frame);
        return false;
    }
    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    options.manage_raw_mode = false;
    options.manage_alt_screen = false;
    options.hide_cursor = false;
    options.probe_graphics = false;
    options.install_winch_handler = false;
    options.transport = KITTYFB_TRANSPORT_INLINE;
    if (kittyfb_start(&session, slave, slave, &options) != 0) {
        goto done;
    }
    drain.fd = master;
    if (pthread_create(&reader, NULL, drain_main, &drain) != 0) {
        kittyfb_stop(&session);
        goto done;
    }

    fill_smooth(frame, WIDTH, HEIGHT);
    if (!run_sequential_frames(
            &session, frame, WIDTH, HEIGHT, 20, "inline_smooth_640x360")) {
        goto stop_reader;
    }
    fill_noise(frame, pixels, UINT32_C(0x4b464231));
    if (!run_sequential_frames(
            &session, frame, WIDTH, HEIGHT, 10, "inline_noise_640x360")) {
        goto stop_reader;
    }

    start = monotonic_ns();
    for (int run = 0; run < 120; ++run) {
        if (!kittyfb_present(&session, frame, WIDTH, HEIGHT)) {
            goto stop_reader;
        }
    }
    elapsed = monotonic_ns() - start;
    (void)printf("enqueue_copy_640x360_ms %.3f\n",
                 (double)elapsed / 1.0e6 / 120.0);
    if (!wait_for_idle(&session)) {
        goto stop_reader;
    }

    {
        const kittyfb_rect rect = {40, 40, 104, 104};
        const int repetitions = 100;

        start = monotonic_ns();
        for (int run = 0; run < repetitions; ++run) {
            frame[((size_t)40 * WIDTH + 40u) * 4u] ^= (uint8_t)run;
            if (!kittyfb_present_damage(
                    &session, frame, WIDTH, HEIGHT, &rect, 1u)) {
                goto stop_reader;
            }
        }
        elapsed = monotonic_ns() - start;
        (void)printf("damage_64x64_ms %.3f\n",
                     (double)elapsed / 1.0e6 / repetitions);
    }
    {
        kittyfb_rect rects[8];
        const int repetitions = 100;

        for (int index = 0; index < 8; ++index) {
            rects[index].x0 = 40 + index * 8;
            rects[index].y0 = 40;
            rects[index].x1 = rects[index].x0 + 8;
            rects[index].y1 = 104;
        }
        start = monotonic_ns();
        for (int run = 0; run < repetitions; ++run) {
            frame[((size_t)40 * WIDTH + 40u) * 4u] ^= (uint8_t)run;
            if (!kittyfb_present_damage(
                    &session, frame, WIDTH, HEIGHT, rects, 8u)) {
                goto stop_reader;
            }
        }
        elapsed = monotonic_ns() - start;
        (void)printf("damage_8_adjacent_64x64_ms %.3f\n",
                     (double)elapsed / 1.0e6 / repetitions);
    }
    {
        const kittyfb_rect viewport = {0, 32, WIDTH, HEIGHT};
        const int repetitions = 100;

        if (setenv("KITTY_KILIX_RENDERING", "1", 1) != 0) {
            goto stop_reader;
        }
        start = monotonic_ns();
        for (int run = 0; run < repetitions; ++run) {
            int dy = (run & 1) == 0 ? -24 : 24;

            if (!kittyfb_present_scroll_region(
                    &session, frame, WIDTH, HEIGHT, &viewport,
                    0, dy, NULL, 0u)) {
                goto stop_reader;
            }
        }
        elapsed = monotonic_ns() - start;
        (void)printf("scroll_24px_toolbar_ms %.3f\n",
                     (double)elapsed / 1.0e6 / repetitions);
        if (unsetenv("KITTY_KILIX_RENDERING") != 0) {
            goto stop_reader;
        }
    }
    ok = true;

stop_reader:
    kittyfb_suspend(&session);
    __atomic_store_n(&drain.stop, 1, __ATOMIC_RELEASE);
    (void)pthread_join(reader, NULL);
    kittyfb_stop(&session);
    (void)printf("wire_bytes %llu\n", (unsigned long long)drain.bytes);
done:
    (void)close(master);
    (void)close(slave);
    free(frame);
    return ok;
}

int main(void)
{
    if (unsetenv("KITTYFB_TRANSPORT") != 0 ||
        unsetenv("KITTY_KILIX_RENDERING") != 0) {
        (void)perror("unsetenv");
        return 1;
    }
    if (!benchmark_base64() || !benchmark_packet_build() ||
        !benchmark_presenter()) {
        (void)fprintf(stderr, "benchmark failed\n");
        return 1;
    }
    return 0;
}
