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

#ifdef __cplusplus
}
#endif

#endif
