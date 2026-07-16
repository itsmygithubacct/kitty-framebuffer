/*
 * bounce: a ball on a scrolling gradient, presented straight from an RGBA
 * buffer at ~30 fps.  Needs a terminal that speaks the Kitty graphics
 * protocol (kitty, ghostty, wezterm, recent konsole).  Press q or Ctrl-C
 * to quit; the terminal is restored on exit.
 *
 * Input is deliberately plain read() so the example depends on nothing
 * but this library; real applications would compose a keyboard library.
 */
#include "kitty_framebuffer.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FRAME_MILLISECONDS 33

/* File scope so the fatal-signal handler can reach it. */
static kittyfb_session session;

static void
on_fatal_signal(int signal_number)
{
    kittyfb_emergency_restore(&session);
    _exit(128 + signal_number);
}

static void
install_fatal_handlers(void)
{
    struct sigaction action;
    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = on_fatal_signal;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGHUP, &action, NULL);
}

static void
advance_deadline(struct timespec *deadline)
{
    deadline->tv_nsec += FRAME_MILLISECONDS * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec += 1;
        deadline->tv_nsec -= 1000000000L;
    }
}

static void
render(uint8_t *rgba, int width, int height, int tick, int ball_x, int ball_y)
{
    const int radius = height / 12 > 8 ? height / 12 : 8;
    const int radius_squared = radius * radius;
    int x;
    int y;

    for (y = 0; y < height; ++y) {
        uint8_t *row = rgba + (size_t)y * (size_t)width * 4u;
        for (x = 0; x < width; ++x) {
            int dx = x - ball_x;
            int dy = y - ball_y;
            uint8_t *pixel = row + (size_t)x * 4u;

            if (dx * dx + dy * dy <= radius_squared) {
                pixel[0] = 255u;
                pixel[1] = 208u;
                pixel[2] = 64u;
            } else {
                pixel[0] = (uint8_t)((x * 255 / width + tick) & 0xff);
                pixel[1] = (uint8_t)((y * 255 / height) & 0xff);
                pixel[2] = (uint8_t)(((x + y) / 4 + tick * 2) & 0xff);
            }
            pixel[3] = 255u;
        }
    }
}

static bool
quit_requested(void)
{
    unsigned char byte;

    /* raw mode has VMIN=0/VTIME=0: read returns immediately */
    while (read(STDIN_FILENO, &byte, 1u) == 1) {
        if (byte == 'q' || byte == 'Q' || byte == 3u) {
            return true;
        }
    }
    return false;
}

int
main(void)
{
    kittyfb_options options;
    kittyfb_stats stats;
    struct timespec deadline;
    uint8_t *frame;
    int width;
    int height;
    int ball_x;
    int ball_y;
    int velocity_x = 4;
    int velocity_y = 3;
    int tick = 0;

    kittyfb_session_init(&session);
    kittyfb_options_init(&options);
    if (kittyfb_start(&session, STDIN_FILENO, STDOUT_FILENO, &options) != 0) {
        if (errno == ENOTSUP) {
            (void)fprintf(
                stderr,
                "bounce: this terminal did not answer the Kitty graphics "
                "query.\nTry kitty, ghostty, wezterm, or a recent konsole "
                "(KITTYFB_SKIP_PROBE=1 overrides).\n");
        } else {
            (void)fprintf(stderr, "bounce: %s\n", strerror(errno));
        }
        return 1;
    }
    install_fatal_handlers();

    width = kittyfb_width(&session);
    height = kittyfb_height(&session);
    frame = malloc((size_t)width * (size_t)height * 4u);
    if (frame == NULL) {
        kittyfb_stop(&session);
        (void)fprintf(stderr, "bounce: out of memory\n");
        return 1;
    }
    ball_x = width / 2;
    ball_y = height / 3;

    (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
    for (;;) {
        int new_width;
        int new_height;

        if (quit_requested()) {
            break;
        }
        if (kittyfb_check_resize(&session, &new_width, &new_height)) {
            uint8_t *grown = realloc(
                frame,
                (size_t)new_width * (size_t)new_height * 4u);
            if (grown == NULL) {
                break;
            }
            frame = grown;
            width = new_width;
            height = new_height;
            if (ball_x >= width) {
                ball_x = width / 2;
            }
            if (ball_y >= height) {
                ball_y = height / 2;
            }
        }

        ball_x += velocity_x;
        ball_y += velocity_y;
        if (ball_x < 0 || ball_x >= width) {
            velocity_x = -velocity_x;
            ball_x += 2 * velocity_x;
        }
        if (ball_y < 0 || ball_y >= height) {
            velocity_y = -velocity_y;
            ball_y += 2 * velocity_y;
        }

        render(frame, width, height, tick++, ball_x, ball_y);
        if (!kittyfb_present(&session, frame, width, height)) {
            break;
        }

        /* ~30 fps: absolute-deadline pacing is immune to render jitter */
        advance_deadline(&deadline);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline,
                               NULL) == EINTR) {
            continue;
        }
    }

    kittyfb_get_stats(&session, &stats);
    kittyfb_stop(&session);
    free(frame);
    (void)printf(
        "presented %llu frames, dropped %llu, failures %llu\n",
        (unsigned long long)stats.frames_presented,
        (unsigned long long)stats.frames_dropped,
        (unsigned long long)stats.encode_failures);
    return kittyfb_failed(&session) ? 1 : 0;
}
