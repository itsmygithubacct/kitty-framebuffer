# Changelog

## 0.5.0 — Unreleased

- Add `kittyfb_present_scroll()` for retained-image shifts. Kilix terminals use
  overlapping same-frame replacement composition and patch the exposed strips;
  standard terminals and unsafe or uneconomical states retain the full-frame
  fallback.
- Permit inline `a=f` damage patches after a full image was delivered through
  shared memory, retaining the cheapest transport for both update sizes.
- Add scroll presentation, fallback, byte, mixed-transport, and wire-protocol
  coverage.

## 0.4.0 — Unreleased

- Add `kittyfb_present_damage()`, `kittyfb_rect`, and damage statistics for
  synchronous in-place edits of an existing Kitty image. This changes the
  public session/stat structure layout; rebuild dependent objects and libraries.
- Serialize damage and asynchronous full-frame encoding so scratch buffers,
  image state, and terminal protocol bytes cannot race or interleave.
- Emit protocol-complete animation continuation chunks and use root-frame
  replacement semantics for patches.
- Validate frame sizes, dimensions, rectangle arrays, damage totals, geometry
  arithmetic, and packet-growth calculations without overflow or impossible
  allocations.
- Fall back to a newest full frame when an edit cannot safely apply: a full frame
  is pending or in flight, a clear is pending, dimensions differ, damage is too
  large or scattered, or the active transport is shared memory.
- Coalesce cost-free overlapping/adjacent damage and assemble each rectangle's
  chunks into one write. Speed up RGB packing and the overflow-safe base64 loop.
- Roll back failed shared-memory publications, preserve pending resize clears
  across dropped frames, release shared-memory mappings during suspension, and
  re-resolve the transport on restart.
- Bound graphics probing with a monotonic deadline, preserve real probe I/O
  errors, and make stop/suspend buffer ownership race-free.
- Complete ordinary signal-handler and descriptor bookkeeping after an
  emergency restore even when no presenter thread or frame buffer was created.
- Add concurrency, protocol, overflow, fault-injection, lifecycle, and benchmark
  coverage.

## 0.3.0

- Add the bounded POSIX shared-memory frame transport and automatic local-inline
  transport selection.

## 0.2.0

- Add paired terminal control sequences, race-safe normal/emergency shutdown,
  restartable suspension with retained heap buffers, and minimum-preserving
  geometry.

## 0.1.0

- Initial C11 Kitty graphics framebuffer presenter with asynchronous
  newest-frame-wins encoding.
