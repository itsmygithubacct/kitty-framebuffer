#ifndef KITTY_FRAMEBUFFER_INTERNAL_H
#define KITTY_FRAMEBUFFER_INTERNAL_H

/*
 * Internal building blocks, exposed with external linkage so the test
 * suite can exercise the pure math directly.  Not installed and not part
 * of the public API.
 */

#include "kitty_framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kitty graphics APC payloads are chunked at this many base64 chars. */
#define KITTYFB_CHUNK_SIZE 4096u

/* Shared-memory object name capacity, including the terminator.  POSIX
 * only guarantees NAME_MAX bytes for a portable shm name and Kitty caps
 * the payload at 2048; this is far below both and long enough for the
 * "/kilix-fb-<pid>-<serial>-<slot>" scheme. */
#define KITTYFB_SHM_NAME_MAX 64u

/* Upper bound on shm slots, matching the option's documented range. */
#define KITTYFB_SHM_SLOTS_MAX 16

/*
 * One shared-memory slot.  A slot is "busy" from the moment its object
 * is created until the terminal unlinks it, which is how consumption is
 * detected: shm_open() on a busy slot's name failing with ENOENT means
 * the terminal has read and released it.
 *
 * The fd and mapping are held for the whole busy period rather than
 * closed after writing, because the mapping keeps the unlinked object
 * alive until the terminal has finished with it.
 */
struct kittyfb_shm_slot {
    char name[KITTYFB_SHM_NAME_MAX];
    void *mapping;
    size_t mapping_size;
    int fd;
    bool busy;
};

typedef struct kittyfb_geometry {
    int width;          /* framebuffer pixels, cell-snapped and even */
    int height;
    int cell_width;     /* pixels per terminal cell */
    int cell_height;
    int origin_row;     /* 1-based cursor origin centering the image */
    int origin_column;
} kittyfb_geometry;

/* Plain base64 without line breaks.  The output buffer needs
 * ((length + 2) / 3) * 4 bytes; the encoded length is returned. */
size_t kittyfb_base64_encode(
    const uint8_t *input,
    size_t length,
    char *output);

/*
 * Snap one dimension to complete cells and an even pixel size. Rounding
 * prefers the value below the target unless that would violate minimum;
 * maximum remains a hard bound.
 */
int kittyfb_snap_axis(int value, int cell, int minimum, int maximum);

/*
 * Derive the framebuffer geometry from a terminal report of columns x
 * rows cells and xpixel x ypixel total pixels (either pixel value may be
 * zero; 9x18 cells are assumed).  One cell row is reserved at the bottom
 * so the shell prompt after exit does not scroll the image.  The pixel
 * size is clamped into the options' min/max bounds, snapped to whole
 * cells, and forced even; the origin centers the image.
 */
bool kittyfb_derive_geometry(
    int columns,
    int rows,
    int xpixel,
    int ypixel,
    const kittyfb_options *options,
    kittyfb_geometry *out);

/*
 * Assemble one complete presentation packet: synchronized-update begin,
 * optional clear, cursor move, the payload as chunked a=T,f=24,o=z
 * graphics escapes under new_id, a targeted delete of old_id, and the
 * synchronized-update end.  Returns the packet length, or 0 when the
 * capacity is insufficient or an argument is invalid.
 */
size_t kittyfb_build_packet(
    char *output,
    size_t capacity,
    const char *payload,
    size_t payload_length,
    int new_id,
    int old_id,
    int width,
    int height,
    const char *origin,
    bool clear_first);

/*
 * The shared-memory form of the same packet: synchronized-update begin,
 * optional clear, cursor move, a single unchunked a=T,f=32,t=s escape
 * whose payload is the base64-encoded shared-memory object name, a
 * targeted delete of old_id, and the synchronized-update end.
 *
 * Unlike the inline form this carries no pixels, so it is never chunked -
 * a shared-memory name is bounded well below the 4 KB chunk size.  The
 * pixels in the object are RGBA (f=32), not the RGB the inline path
 * packs, because the byte saving that justifies stripping alpha only
 * applies to bytes that travel down the terminal connection.
 *
 * Returns the packet length, or 0 when the capacity is insufficient or an
 * argument is invalid.
 */
size_t kittyfb_build_shm_packet(
    char *output,
    size_t capacity,
    const char *shm_name,
    int new_id,
    int old_id,
    int width,
    int height,
    const char *origin,
    bool clear_first);

#ifdef __cplusplus
}
#endif

#endif
