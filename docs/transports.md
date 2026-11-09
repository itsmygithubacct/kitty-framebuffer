# kitty-framebuffer transports, damage and scroll presents

How pixels reach the terminal (shared memory or inline), and the partial-update paths that avoid retransmitting a whole frame, moved here from the README.

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
