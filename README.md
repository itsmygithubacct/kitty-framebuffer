# kitty-framebuffer

`kitty-framebuffer` is a small C11 library that presents RGBA framebuffers in a
terminal through the
[Kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/),
with a threaded presenter, synchronized updates, resize handling and a
crash-safe terminal restore.

It is not installed on its own. It reaches programs through
[kitty-terminal-session](https://github.com/itsmygithubacct/kitty-terminal-session#readme),
which pins it as a submodule; Kilix games and apps compile it into their
binaries, and those install on demand from the Kilix content catalog.

## What it does

- Measures the terminal, enters raw mode and the alternate screen, probes for
  graphics support, and sizes a framebuffer to whole cells.
- Encodes and writes frames on a presenter thread with a newest-frame-wins slot,
  so a slow terminal drops frames instead of stalling the render loop.
- Sends pixels over POSIX shared memory when the terminal shares `/dev/shm`, or
  inline as zlib and base64 escapes over ssh or tmux, choosing automatically.
- Patches only changed rectangles (`a=f` frame edits) and, on the Kilix kitty
  fork, composes scrolled viewports in place, falling back to a full present
  whenever that is cheaper or required.
- Wraps every frame in a DEC 2026 synchronized update and alternates two image
  ids, so the screen never shows a blank or half-decoded state.
- Restores the terminal on stop, on suspend and, async-signal-safely, from a
  fatal-signal handler.
- Needs a C11 compiler, POSIX, pthreads and zlib.

## Quick start

```sh
git clone https://github.com/itsmygithubacct/kitty-framebuffer.git
cd kitty-framebuffer
make             # build/libkitty-framebuffer.a, .so and build/bounce
make test        # lifecycle tests against fake terminals on a PTY
./build/bounce   # animated example in a Kitty-protocol terminal; q quits
```

```c
#include "kitty_framebuffer.h"

kittyfb_session session;
kittyfb_options options;
kittyfb_session_init(&session);
kittyfb_options_init(&options);
if (kittyfb_start(&session, STDIN_FILENO, STDOUT_FILENO, &options) != 0) {
    /* errno; ENOTSUP means no graphics protocol */
}
/* each frame: kittyfb_check_resize(), fill RGBA, kittyfb_present() */
kittyfb_stop(&session);
```

The full loop, with resize and failure handling, is in [docs/api.md](docs/api.md).

## Usage

| Call | Purpose |
|---|---|
| `kittyfb_session_init`, `kittyfb_options_init` | prepare a session and its options |
| `kittyfb_start`, `kittyfb_stop`, `kittyfb_suspend` | take over, release, or pause the terminal |
| `kittyfb_width`, `kittyfb_height`, `kittyfb_check_resize`, `kittyfb_notify_resize` | framebuffer size and resize detection |
| `kittyfb_present` | queue a whole frame for the presenter thread |
| `kittyfb_present_damage`, `kittyfb_try_present_damage` | patch changed rectangles (blocking or non-blocking) |
| `kittyfb_present_scroll_region`, `kittyfb_present_scroll` | shift retained pixels and patch the exposed edge |
| `kittyfb_active_transport`, `kittyfb_reap_orphans` | report the chosen transport; clean up a dead session's shared-memory objects |
| `kittyfb_get_stats`, `kittyfb_failed` | counters and the latched failure flag |
| `kittyfb_emergency_restore` | async-signal-safe restore for fatal-signal handlers |

| Setting | Effect |
|---|---|
| `options.transport` | `KITTYFB_TRANSPORT_AUTO` (default), shared memory or inline |
| `options.probe_graphics = false`, or `KITTYFB_SKIP_PROBE=1` | skip the graphics probe and write frames blind |
| `options.enter_sequence`, `options.leave_sequence` | extra terminal modes, such as mouse tracking |
| `install_winch_handler = false` | leave SIGWINCH to the application |
| `KITTY_KILIX_RENDERING=1` in the environment | marks the Kilix kitty fork and enables its in-place scroll compose |

## How it works

```
 render loop ──kittyfb_present──▶ pending slot (newest wins) ──▶ presenter thread
                                                                   │ shm slot, or zlib+base64
                                                                   ▼
              terminal ◀── DEC 2026 synchronized update: new image id, delete old id
```

`kittyfb_start()` measures the window, applies raw mode, probes with a 1x1
graphics query paired with a device-attributes request (so terminals that ignore
graphics still answer), enters the alternate screen and derives the framebuffer
size from the reported pixel geometry. Each present copies the frame out and
returns; the presenter thread transmits it under whichever of the two image ids
is not on screen and then deletes the other. The output descriptor stays
non-blocking with poll-based, stall-limited writes, so neither the render loop
nor a signal handler can hang on a stalled connection. A failed write or
allocation latches, and `kittyfb_present()` returns false until the next start.

Damage and scroll presents are synchronous and share one serializer with full
frames, because a dropped patch would leave part of the screen wrong. The
library was extracted from the terminal presenter that the chess-bash and
terminal-lander family of games had each copied and hardened, and keeps their
accumulated fixes.

Depth: [docs/transports.md](docs/transports.md) for shared memory, inline,
damage and scroll presents; [docs/api.md](docs/api.md) for the lifecycle,
restore paths, probe, diagnostics and behaviour contract.

## Documentation

- [docs/api.md](docs/api.md): quick start, presenting, resize, shutdown and
  emergency restore, the graphics probe, diagnostics, composing with
  kitty-input, the behaviour contract, and lineage.
- [docs/transports.md](docs/transports.md): shared-memory and inline transports,
  damage presents and scroll composition.
- [docs/building.md](docs/building.md): build, test, sanitizer, benchmark and
  install targets (`make race` runs the tests under ThreadSanitizer).
- [CHANGELOG.md](CHANGELOG.md): release history.

## Related repositories

- [kitty-terminal-session](https://github.com/itsmygithubacct/kitty-terminal-session#readme)
  pins this library at `third_party/kitty-framebuffer` next to kitty-input and
  wraps both into one terminal lifecycle. Everything that pins that session
  compiles kitty-framebuffer in:
  [kilix-game-sdk](https://github.com/itsmygithubacct/kilix-game-sdk#readme) and
  the games built on it
  ([kilix-games](https://github.com/itsmygithubacct/kilix-games#readme),
  [pleb-tower](https://github.com/itsmygithubacct/pleb-tower#readme),
  [super-kilix](https://github.com/itsmygithubacct/super-kilix#readme),
  [pleb-plant-grower](https://github.com/itsmygithubacct/pleb-plant-grower#readme),
  [kilix-land](https://github.com/itsmygithubacct/kilix-land#readme)), and
  [kilix-techno](https://github.com/itsmygithubacct/kilix-techno#readme),
  [kilix-pdf](https://github.com/itsmygithubacct/kilix-pdf#readme),
  [kilix-playalong](https://github.com/itsmygithubacct/kilix-playalong#readme),
  [kilix-rtsp](https://github.com/itsmygithubacct/kilix-rtsp#readme),
  [kilix-sound-detect](https://github.com/itsmygithubacct/kilix-sound-detect#readme)
  and [kilix-mask](https://github.com/itsmygithubacct/kilix-mask#readme).
- [kitty-input](https://github.com/itsmygithubacct/kitty-input#readme) is the
  matching input library.
- [soft-raster](https://github.com/itsmygithubacct/soft-raster#readme) is the
  software rasterizer most of those programs draw their frames with.
- [kitty](https://github.com/itsmygithubacct/kitty#readme), the Kilix kitty
  fork, implements the scroll-compose extension.

## Part of Plebian-OS

kitty-framebuffer is one of the repositories behind
[Plebian-OS](https://github.com/itsmygithubacct/plebian-os#readme), a Debian
install whose desktop is [Kilix](https://github.com/itsmygithubacct/kilix#readme)
running the [Kilix 95](https://github.com/itsmygithubacct/kilix-95#readme)
desktop through [Pleb](https://github.com/itsmygithubacct/pleb#readme).
It reaches a user compiled, through kitty-terminal-session, into the Kilix games
and apps that install on demand from the Kilix content catalog. The map of every
repository and how it ships is on the
[components page](https://github.com/itsmygithubacct/plebian-os/blob/main/docs/components.md).

## License

MIT. See [LICENSE](LICENSE) for the complete notices.
