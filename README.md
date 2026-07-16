# kitty-framebuffer

`kitty-framebuffer` is a small C11 library that presents RGBA framebuffers
in a terminal through the [Kitty graphics
protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/). Hand it a
frame; it strips the alpha channel, zlib-compresses the pixels, base64
encodes them into 4 KB graphics escapes and writes the whole frame in a
single burst, wrapped in a DEC 2026 synchronized update.

Encoding and the terminal write run on a presenter thread with a
newest-frame-wins pending slot, so a slow terminal connection costs
dropped frames, never a stalled render loop. Two image ids alternate
between frames - the new frame is transmitted under the id not on screen,
then the old id is deleted - so the screen never shows a blank or
half-decoded state.

The library is presentation only. Keyboard input is a separate concern;
compose it with an input library (see the note on `kitty-keyboard` below)
or plain `read()` calls.

## Build and test

```sh
make
make test
make sanitize
./build/bounce
```

The final command runs an animated example (a bouncing ball over a
scrolling gradient at ~30 fps; `q` or Ctrl-C quits). It needs a terminal
that implements the Kitty graphics protocol: kitty, ghostty, wezterm, or
a recent konsole. The test suite runs anywhere; it covers the base64,
geometry and chunking math and drives the full lifecycle - probe,
presentation, resize, restore - against fake terminals on a PTY.

Dependencies: a C11 compiler, POSIX, pthreads and zlib (`-lz`). The PTY
tests use `libutil`; applications do not.

## Quick start

```c
#include "kitty_framebuffer.h"

kittyfb_session session;
kittyfb_options options;

kittyfb_session_init(&session);
kittyfb_options_init(&options);

if (kittyfb_start(&session, STDIN_FILENO, STDOUT_FILENO, &options) != 0) {
    /* errno describes the failure; ENOTSUP means the terminal
     * answered the probe but does not speak the graphics protocol. */
}

int width = kittyfb_width(&session);
int height = kittyfb_height(&session);
uint8_t *frame = malloc((size_t)width * height * 4);

for (;;) {
    int new_width, new_height;
    if (kittyfb_check_resize(&session, &new_width, &new_height)) {
        /* reallocate the frame at the new size */
    }
    /* ... fill frame with RGBA pixels ... */
    if (!kittyfb_present(&session, frame, width, height)) {
        break;  /* invalid frame, or the presenter latched a failure */
    }
}

kittyfb_stop(&session);
```

`kittyfb_start()` measures the terminal, switches it to raw mode, probes
for graphics support, enters the alternate screen and hides the cursor.
Each step can be disabled through the options. The chosen framebuffer
size is derived from the terminal's reported pixel geometry, clamped into
the configured bounds (640x400 .. 1600x1000 by default), snapped to whole
cells and centered, with one cell row left free for the shell prompt
after exit.

## Presenting frames

`kittyfb_present()` copies the frame out and returns immediately;
compression, encoding and the write happen on the presenter thread. If a
new frame arrives while the previous one is still encoding, the pending
frame is replaced (counted in `frames_dropped`). If the presenter thread
cannot be created, the frame is encoded synchronously on the caller.

Frames may be any size; most applications present at the size the
library chose. All size arithmetic is overflow-checked and every buffer
growth is verified, so oversized or hostile dimensions fail cleanly with
`false` rather than corrupting memory.

## Resize handling

`kittyfb_check_resize()` re-reads the window size (a cheap ioctl; call it
once per frame) and re-derives the geometry. It returns true only when
the framebuffer pixel size changed - present at the new size from then
on. Any geometry change, including a centering-only change, schedules a
screen clear inside the next frame's synchronized update, so stale pixels
are wiped without an interleaved write.

A SIGWINCH handler that flags resizes is installed by default. It is
only a hint - detection works by polling - so applications that own
SIGWINCH can set `install_winch_handler = false` and optionally call
`kittyfb_notify_resize()` (async-signal-safe) from their handler.

## Shutdown and emergency restore

`kittyfb_stop()` joins the presenter thread, frees its buffers, and
restores the terminal: it ends any pending synchronized update *first*
(a truncated update would freeze the terminal), closes any half-written
graphics escape with an ST, deletes the session's two image ids - and
only those; deleting all images (`d=A`) would wipe images other programs
placed - then shows the cursor, leaves the alternate screen, and restores
termios and descriptor flags. Stop is safe to call twice, and the
session can be started again afterwards.

`kittyfb_emergency_restore()` is for fatal-signal handlers. It is
async-signal-safe: no locks, no join. It fences the presenter thread
through a `sig_atomic_t` flag so no further frame bytes interleave with
the restore, then writes one prebuilt restore sequence and restores
termios. Because the output descriptor is non-blocking for the whole
session, the signal path can never hang on a stalled connection. If the
process survives, a later `kittyfb_stop()` still reclaims the presenter
thread and its memory.

```c
static kittyfb_session session;

static void on_fatal_signal(int signal_number)
{
    kittyfb_emergency_restore(&session);
    _exit(128 + signal_number);
}
```

## Graphics probe

By default, start sends a 1x1 Kitty graphics query paired with a primary
device-attributes request. Graphics terminals answer the query; every
terminal answers the DA1, which bounds the wait on terminals that
silently ignore graphics escapes. When the terminal answers only the
DA1, `kittyfb_start()` fails with `errno = ENOTSUP` and restores
everything it changed - the deliberate behavior for unsupported
terminals, matching the game family this library was extracted from.
To run anyway (for example through a passthrough multiplexer), set
`options.probe_graphics = false` or the environment variable
`KITTYFB_SKIP_PROBE=1`; frames are then written blind.

## Diagnostics

`kittyfb_get_stats()` snapshots frames presented, encoded, dropped, and
encode failures. When compression, allocation, or the terminal write
fails on the presenter thread, the failure latches: `kittyfb_present()`
returns false from then on and `kittyfb_failed()` reports it, so the
application can exit its render loop instead of animating into a void.
The latch clears on the next start.

## Composing with kitty-keyboard

This library pairs with
[`kitty-keyboard`](https://github.com/itsmygithubacct) for input; the two
share no state. Start `kitty-framebuffer` first: its probe reads the
graphics/DA1 responses from the input descriptor, and a keyboard decoder
reading concurrently would swallow them. (Alternatively set
`probe_graphics = false`.) Then start the keyboard layer with its
`make_raw` disabled, since this library already owns raw mode - and stop
it before `kittyfb_stop()`, so its mode pop happens inside the alternate
screen it was pushed on.

## Behavior contract

- One session owns the terminal; call the API from one thread (the
  presenter thread is internal). `kittyfb_emergency_restore()` is the
  only call safe from a signal handler.
- The output descriptor is `O_NONBLOCK` for the session's lifetime and
  every write is poll-based with a stall limit; original flags are
  restored on stop. Do not write to the descriptor yourself while a
  session is active.
- Raw mode is applied with `TCSAFLUSH`, discarding input typed before
  start.
- Image ids default to 1 and 2; configure `image_id_a`/`image_id_b` if
  the application places its own images.
- The API is pre-1.0 and may change between minor releases.

## Install

```sh
make install PREFIX=/usr/local
```

The build produces static and shared libraries. Applications may instead
compile `src/kitty_framebuffer.c` directly.

## Lineage

Extracted from the terminal presenter shared by the chess-bash /
terminal-lander family of games (github.com/itsmygithubacct), which
copy-pasted and independently hardened the same `term.c`. This library
consolidates that lineage and bakes in the fixes the forks accumulated:
the pending-buffer-only growth rule, the presenter fence with a
synchronized-update-first emergency restore, targeted image deletes, and
restartability after stop.

## License

MIT. See [LICENSE](LICENSE) for the complete notices.
