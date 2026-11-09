# Building, testing and installing kitty-framebuffer

Build, test, benchmark and install instructions, moved here from the README.

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

## Install

```sh
make install PREFIX=/usr/local
```

The build produces static and shared libraries. Applications may instead
compile `src/kitty_framebuffer.c` directly.
