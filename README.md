# kitty-framebuffer

`kitty-framebuffer` is a small C11 library that presents RGBA framebuffers
in a terminal through the [Kitty graphics
protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/). Hand it a
frame; it gets to the terminal, wrapped in a DEC 2026 synchronized update.

Encoding and the terminal write run on a presenter thread with a
newest-frame-wins pending slot, so a slow terminal connection costs
dropped frames, never a stalled render loop. Two image ids alternate
between frames - the new frame is transmitted under the id not on screen,
then the old id is deleted - so the screen never shows a blank or
half-decoded state.

## Transports

How the pixels travel is a choice, and it matters more than it sounds.

- **Shared memory (`t=s`)** writes the frame into a POSIX shared-memory
  object and sends only its *name*. The terminal unlinks the object once
  it has read it, and that unlink is the acknowledgement a bounded ring
  of slots is built on. A frame costs a `memcpy` and a few dozen bytes of
  escape stream. Requires a local terminal sharing `/dev/shm`.
- **Inline (`t=d,o=z`)** zlib-compresses the frame, strips the alpha
  channel and base64-encodes the result into 4 KB graphics escapes. It
  works anywhere - over ssh, through tmux - and it is the only choice
  when the terminal does not share this process's memory.

Inline costs a compressed copy of every frame in terminal input, and
photographic content barely compresses: a 640x360 camera frame measured
66% of its raw size after zlib, or ~600 KB of escape stream per frame
however fast the encoder runs. For a game canvas the waste is tolerable;
for video it is the whole budget.

`options.transport` selects between them and defaults to
`KITTYFB_TRANSPORT_AUTO`, which picks shared memory when `shm_open()`
works and the session is not under tmux, and inline otherwise. The
decision is made once per start, in `kittyfb_start()`;
`kittyfb_active_transport()` reports what it chose. tmux disqualifies
shared memory even when `shm_open()` succeeds, because tmux forwards the
escape to a terminal that need not share this process's `/dev/shm` - and
a name the terminal cannot open is a blank screen, not a degraded one.

`options.shm_slots` (default 3) bounds how many frames may be in flight.
A frame arriving when every slot is still unread is dropped and counted
in `stats.frames_dropped`, exactly as a frame arriving while the encoder
is busy is dropped. Dropping is not failing: the session continues.

A process killed without unwinding leaves its objects behind. Names embed
the owning pid, so `kittyfb_reap_orphans()` can remove the ones whose
owner is gone without touching a running session's frames. A normal
`kittyfb_stop()` already releases this session's objects.

The library is presentation only. Keyboard input is a separate concern;
compose it with [`kitty-input`](https://github.com/itsmygithubacct/kitty-input)
or plain `read()` calls. See the composition note below.


## Damage presents

`kittyfb_present()` retransmits every pixel, which is right for video and wrong
for an interactive editor: moving a pointer changes a few hundred pixels, and a
full 1080p frame is ~1.5 MB of escape stream per motion event.

`kittyfb_present_damage()` edits the image already on screen in place, using
kitty's `a=f` frame edits, so the wire cost follows what actually changed.

```c
kittyfb_rect damage[2] = { {x0, y0, x1, y1}, ... };
kittyfb_present_damage(&session, rgba, width, height, damage, 2);
```

It takes the **whole** frame plus the rects that changed, not loose tiles — so
the caller's composition is unchanged, and the library can decide from the
damaged area whether patching is even the cheaper option.

It falls back to a full present automatically when nothing has been presented
yet, when a full frame is pending or in flight, when a resize clear is pending,
when the displayed image has different dimensions, or when the damage is too
large or scattered for patches to pay. A frame initially delivered through
shared memory can still receive inline frame edits, so local sessions retain
the cheap full-frame transport without giving up small updates. Overlapping and
adjacent rectangles are coalesced when their bounding rectangle sends no extra
pixels. That fallback is the point: a caller can use the function
unconditionally instead of reasoning about when it helps.

**It is synchronous, unlike `kittyfb_present()`.** The presenter thread keeps
only the newest pending frame and drops the rest — correct for video, where a
dropped frame is one nobody needed, and corrupting for patches, where each
carries only its own rectangles and a dropped one leaves that region wrong until
something else redraws it. Damage and full-frame writes share one serializer, so
the final on-screen result follows API call order and their protocol bytes cannot
interleave.

That shared serializer has a real worst case: it can be held across a complete
full-frame encode plus terminal write, and each write burst tolerates up to 40
consecutive stalled 50 ms polls — about two seconds, and longer on a connection
that trickles just enough to reset the stall counter. A caller on a
latency-sensitive thread uses the bounded form instead:
`kittyfb_try_present_damage()` returns `KITTYFB_PRESENT_BUSY` immediately when
the serializer is held, writes nothing, and latches nothing; the caller keeps
its damage, coalesces it with the next update, and retries. Everything else,
including the automatic full-frame fallback, matches the blocking form.

## Scroll composition

`kittyfb_present_scroll_region()` handles a common retained-image update without
retransmitting pixels that merely moved. The caller supplies the complete new
frame, a viewport, and the `(dx, dy)` shift from the previous frame. The library
asks the Kilix Kitty fork to compose only that viewport's overlapping interior
in place with `a=c,C=1,N=2` and patches its newly exposed edge strips from the
new frame. Pixels outside the viewport stay put, so fixed chrome is neither
shifted nor retransmitted. Extra damage rectangles can cover independent changes.

```c
kittyfb_rect viewport = {0, toolbar_height, width, height};
kittyfb_present_scroll_region(&session, rgba, width, height, &viewport,
                              0, -scroll_pixels, NULL, 0);
```

`kittyfb_present_scroll()` remains the whole-frame convenience form for scenes
where every retained pixel moves together.

The `KITTY_KILIX_RENDERING=1` marker is the capability negotiation for the
fork extension. Without it, before an initial frame, across a resize or queued
frame, for a large shift, or when patches stop being economical, the operation
falls back to a normal full presentation. On the fork, the terminal performs
the compose on a hardware GPU when available and uses its existing CPU upload
path otherwise; either route has the same snapshot/memmove result. Compose and
patch packets are synchronous and share the full-frame serializer.

Framebuffer placements use a z-index below `INT32_MIN / 2`: the image remains
above the default canvas but below non-default cell backgrounds and terminal
foreground UI. That keeps Kilix's software mouse cursor visible over full-window
graphics while retaining the native OS-pointer fallback on terminals that do
not provide one.

## Build and test

```sh
make
make test
make sanitize
make benchmark
./build/bounce
```

`make benchmark` reports base64 and packet throughput plus full-frame, enqueue,
and damage-present timings against a drained PTY. The final command runs an
animated example (a bouncing ball over a
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

Applications that need an additional terminal mode, such as mouse tracking,
can set `options.enter_sequence` and `options.leave_sequence`. The enter
sequence uses the same bounded nonblocking writer as framebuffer setup. The
leave sequence is copied into both normal and emergency restore paths, before
the cursor and alternate screen are restored; each sequence is limited to
`KITTYFB_CONTROL_SEQUENCE_MAX` bytes.

## Presenting frames

`kittyfb_present()` copies the frame out and returns immediately;
compression, encoding and the write happen on the presenter thread. If a
new frame arrives while the previous one is still encoding, the pending
frame is replaced (counted in `frames_dropped`). If the presenter thread
cannot be created, the frame is encoded synchronously on the caller.

Frames may be any size representable as one C object; most applications present
at the size the library chose. All size arithmetic is overflow-checked, objects
larger than `PTRDIFF_MAX` are rejected, and every buffer growth is verified, so
oversized or hostile dimensions fail cleanly with `false` rather than attempting
an impossible allocation or corrupting memory.

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

`kittyfb_suspend()` joins the presenter and restores the terminal but retains
the frame and encoder buffers at their high-water capacities. Call
`kittyfb_start()` after continuation; repeated job-control cycles then reuse
the large heap allocations. Shared-memory slots and their tmpfs objects are
released during suspension and recreated on the next start, which also
re-resolves changed transport options and environment. A final `kittyfb_stop()`
releases retained storage whether or not the session was restarted.

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
`KITTYFB_SKIP_PROBE` (conventionally `KITTYFB_SKIP_PROBE=1`); frames are then
written blind. Probe reads use one monotonic deadline, so repeated signals do
not extend the configured timeout indefinitely.

## Diagnostics

`kittyfb_get_stats()` snapshots frames presented, encoded, and dropped; encode
failures; and damage/scroll presents, fallbacks, and bytes. When compression,
allocation, or the terminal write fails, the failure latches:
`kittyfb_present()` returns false from then on and `kittyfb_failed()` reports it,
so the application can exit its render loop instead of animating into a void.
The latch clears on the next start.

## Composing with kitty-input

This library pairs with
[`kitty-input`](https://github.com/itsmygithubacct/kitty-input) for input; its
low-level decoder lives in
[`third_party/kitty_keyboard`](https://github.com/itsmygithubacct/kitty-input/tree/main/third_party/kitty_keyboard).
The libraries share no state. Start `kitty-framebuffer` first: its probe reads
the graphics/DA1 responses from the input descriptor, and a keyboard decoder
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
