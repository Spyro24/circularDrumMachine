#ifndef CDM_DRAW_H
#define CDM_DRAW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Small software rasteriser: everything is drawn into an RGBA32 buffer with
 * analytic anti-aliasing, then handed to SDL2/OpenGL as a single texture.
 * Colours are 0xAARRGGBB with straight alpha.
 */

typedef struct {
    uint32_t *px;
    int w, h;
} canvas_t;

typedef struct font_s font_t;

#define CDM_RGBA(r, g, b, a) \
    (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

enum { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

canvas_t *canvas_new(int w, int h);
void canvas_free(canvas_t *c);
void canvas_clear(canvas_t *c, uint32_t color);

font_t *font_load(const char *path);
void font_free(font_t *f);

float text_width(font_t *f, const char *s, float px, float tracking);
float text_draw(canvas_t *c, font_t *f, const char *s, float x, float y, float px,
                float tracking, uint32_t color, int align);
/* Rotated 90 deg clockwise, running downwards, centred on (cx, cy). */
void text_draw_vertical(canvas_t *c, font_t *f, const char *s, float cx, float cy,
                        float px, float tracking, uint32_t color);

void d_rect(canvas_t *c, float x, float y, float w, float h, uint32_t color);
void d_rrect(canvas_t *c, float x, float y, float w, float h, float r, uint32_t color);
void d_rrect_border(canvas_t *c, float x, float y, float w, float h, float r, float t,
                    uint32_t color);
void d_circle(canvas_t *c, float cx, float cy, float r, uint32_t color);
void d_circle_border(canvas_t *c, float cx, float cy, float r, float t, uint32_t color);
/* Annulus sector. Angles in radians, 0 = 12 o'clock, growing clockwise. */
void d_arc(canvas_t *c, float cx, float cy, float r0, float r1, float a0, float a1,
           uint32_t color);
void d_line(canvas_t *c, float x0, float y0, float x1, float y1, float t, uint32_t color);
/* Filled convex triangle (used for the transport glyph). */
void d_triangle(canvas_t *c, float ax, float ay, float bx, float by, float cx, float cy,
                uint32_t color);

#ifdef __cplusplus
}
#endif

#endif
