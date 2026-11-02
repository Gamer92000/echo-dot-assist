/* Software drawing for the screen UI (src/ui/): straight into the gralloc buffer ANativeWindow_lock hands out, in its
 * layout, RGBX_8888 (bytes R, G, B, X: 0xXXBBGGRR as a little-endian word).  Everything is clipped to g->clip, the
 * frame's dirty rectangle, so a frame costs what changed and not the screen (src/ui/scene.c).  Shapes are anti-aliased
 * by their distance to the edge, computed only where a pixel can be partly covered; insides are plain fills. */
#ifndef UI_GFX_H
#define UI_GFX_H
#include <stdint.h>

struct rect { int x0, y0, x1, y1; };            /* half-open: x0 <= x < x1 */
struct gfx { uint32_t *px; int w, h, stride; struct rect clip; };

/* colours as written in code, 0xRRGGBB; alpha 0..255 over what is there */
static inline uint32_t gfx_px(uint32_t rgb) { return 0xFF000000u | (rgb & 0xFF) << 16 | (rgb & 0xFF00) | (rgb >> 16 & 0xFF); }
uint32_t gfx_mix(uint32_t a, uint32_t b, float k);         /* 0xRRGGBB: a..b */
uint32_t gfx_scale(uint32_t rgb, float k);                 /* 0xRRGGBB times k (0..1) */

struct rect rect_and(struct rect a, struct rect b);
struct rect rect_or(struct rect a, struct rect b);         /* the box around both; an empty one counts as nothing */
static inline int rect_empty(struct rect r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }

void gfx_fill(struct gfx *g, struct rect r, uint32_t rgb);
void gfx_rrect(struct gfx *g, float x0, float y0, float x1, float y1, float rad, uint32_t rgb, int alpha);
void gfx_poly(struct gfx *g, const float *xy, int n, uint32_t rgb, int alpha);   /* convex, either winding */
/* coverage mask (text): 0..255 per pixel */
void gfx_mask(struct gfx *g, int x, int y, const uint8_t *m, int w, int h, int stride, uint32_t rgb, int alpha);
/* an image in the buffer's own layout; premul: its alpha byte (premultiplied, Android's RGBA_8888) counts, else opaque */
void gfx_image(struct gfx *g, int x, int y, const uint32_t *src, int w, int h, int stride, int premul, int alpha);
/* the image scaled to dw x dh (bilinear), opaque, with rounded corners of radius rad (0: square); at its own size a
 * copy (thumbnails come at the size they are shown) */
void gfx_image_scaled(struct gfx *g, int x, int y, int dw, int dh, const uint32_t *src, int w, int h, int stride, float rad);
/* a premultiplied image (an app's icon) scaled to dw x dh, bilinear, over what is there */
void gfx_icon_scaled(struct gfx *g, int x, int y, int dw, int dh, const uint32_t *src, int w, int h, int stride);
/* top to bottom from one colour to the other over r (a backdrop: in 8 steps of the 255 there are, no dithering) */
void gfx_vgrad(struct gfx *g, struct rect r, uint32_t top, uint32_t bottom);
/* the outline of a rounded box, t thick, inside its edge */
void gfx_rrect_stroke(struct gfx *g, float x0, float y0, float x1, float y1, float rad, float t, uint32_t rgb, int alpha);
/* a ring's arc of radius r (to the middle of the stroke), t thick, from angle a0 to a1 (radians, 0 = right, clockwise
 * on the screen), with round ends; a1 - a0 >= 2 pi is the whole ring */
void gfx_arc(struct gfx *g, float cx, float cy, float r, float t, float a0, float a1, uint32_t rgb, int alpha);
#endif
