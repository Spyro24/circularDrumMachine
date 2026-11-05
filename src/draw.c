#include "draw.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define TAU 6.28318530718f

struct font_s {
    FT_Face face;
};

static FT_Library g_lib;
static int g_lib_ready;

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------------ canvas */

canvas_t *canvas_new(int w, int h)
{
    if (w <= 0 || h <= 0)
        return NULL;
    canvas_t *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->px = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!c->px) {
        free(c);
        return NULL;
    }
    c->w = w;
    c->h = h;
    return c;
}

void canvas_free(canvas_t *c)
{
    if (!c)
        return;
    free(c->px);
    free(c);
}

void canvas_clear(canvas_t *c, uint32_t color)
{
    size_t n = (size_t)c->w * (size_t)c->h;
    for (size_t i = 0; i < n; i++)
        c->px[i] = color;
}

static inline void blend(canvas_t *c, int x, int y, uint32_t col, float cov)
{
    if ((unsigned)x >= (unsigned)c->w || (unsigned)y >= (unsigned)c->h)
        return;
    float a = (float)((col >> 24) & 0xff) / 255.0f * cov;
    if (a <= 0.0f)
        return;
    if (a > 1.0f)
        a = 1.0f;

    uint32_t *p = &c->px[(size_t)y * (size_t)c->w + x];
    uint32_t d = *p;
    float ia = 1.0f - a;
    float r = (float)((col >> 16) & 0xff) * a + (float)((d >> 16) & 0xff) * ia;
    float g = (float)((col >> 8) & 0xff) * a + (float)((d >> 8) & 0xff) * ia;
    float b = (float)(col & 0xff) * a + (float)(d & 0xff) * ia;
    *p = 0xff000000u | ((uint32_t)(r + 0.5f) << 16) | ((uint32_t)(g + 0.5f) << 8) |
         (uint32_t)(b + 0.5f);
}

/* ------------------------------------------------------------------ shapes */

static float sd_rbox(float px, float py, float hw, float hh, float r)
{
    float qx = fabsf(px) - (hw - r);
    float qy = fabsf(py) - (hh - r);
    float ox = qx > 0.0f ? qx : 0.0f;
    float oy = qy > 0.0f ? qy : 0.0f;
    float m = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (m < 0.0f ? m : 0.0f) - r;
}

static void bbox(canvas_t *c, float x, float y, float w, float h,
                 int *x0, int *y0, int *x1, int *y1)
{
    *x0 = (int)floorf(x) - 1;
    *y0 = (int)floorf(y) - 1;
    *x1 = (int)ceilf(x + w) + 1;
    *y1 = (int)ceilf(y + h) + 1;
    if (*x0 < 0)
        *x0 = 0;
    if (*y0 < 0)
        *y0 = 0;
    if (*x1 > c->w)
        *x1 = c->w;
    if (*y1 > c->h)
        *y1 = c->h;
}

void d_rect(canvas_t *c, float x, float y, float w, float h, uint32_t color)
{
    d_rrect(c, x, y, w, h, 0.0f, color);
}

void d_rrect(canvas_t *c, float x, float y, float w, float h, float r, uint32_t color)
{
    if (w <= 0.0f || h <= 0.0f)
        return;
    float hw = w * 0.5f, hh = h * 0.5f;
    float cx = x + hw, cy = y + hh;
    r = 0.0f; /* square corners: buttons and panels stay rectangular */

    int x0, y0, x1, y1;
    bbox(c, x, y, w, h, &x0, &y0, &x1, &y1);
    for (int py = y0; py < y1; py++) {
        float fy = (float)py + 0.5f - cy;
        for (int px = x0; px < x1; px++) {
            float fx = (float)px + 0.5f - cx;
            float cov = clampf(0.5f - sd_rbox(fx, fy, hw, hh, r), 0.0f, 1.0f);
            if (cov > 0.0f)
                blend(c, px, py, color, cov);
        }
    }
}

void d_rrect_border(canvas_t *c, float x, float y, float w, float h, float r, float t,
                    uint32_t color)
{
    if (w <= 0.0f || h <= 0.0f || t <= 0.0f)
        return;
    float hw = w * 0.5f, hh = h * 0.5f;
    float cx = x + hw, cy = y + hh;
    r = 0.0f; /* square corners: buttons and panels stay rectangular */
    float half = t * 0.5f;

    int x0, y0, x1, y1;
    bbox(c, x, y, w, h, &x0, &y0, &x1, &y1);
    for (int py = y0; py < y1; py++) {
        float fy = (float)py + 0.5f - cy;
        for (int px = x0; px < x1; px++) {
            float fx = (float)px + 0.5f - cx;
            float d = fabsf(sd_rbox(fx, fy, hw, hh, r));
            float cov = clampf(0.5f + half - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                blend(c, px, py, color, cov);
        }
    }
}

void d_circle(canvas_t *c, float cx, float cy, float r, uint32_t color)
{
    if (r <= 0.0f)
        return;
    int x0, y0, x1, y1;
    bbox(c, cx - r, cy - r, r * 2.0f, r * 2.0f, &x0, &y0, &x1, &y1);
    for (int py = y0; py < y1; py++) {
        float fy = (float)py + 0.5f - cy;
        for (int px = x0; px < x1; px++) {
            float fx = (float)px + 0.5f - cx;
            float cov = clampf(0.5f - (sqrtf(fx * fx + fy * fy) - r), 0.0f, 1.0f);
            if (cov > 0.0f)
                blend(c, px, py, color, cov);
        }
    }
}

void d_circle_border(canvas_t *c, float cx, float cy, float r, float t, uint32_t color)
{
    if (r <= 0.0f || t <= 0.0f)
        return;
    float half = t * 0.5f;
    int x0, y0, x1, y1;
    bbox(c, cx - r - t, cy - r - t, (r + t) * 2.0f, (r + t) * 2.0f, &x0, &y0, &x1, &y1);
    for (int py = y0; py < y1; py++) {
        float fy = (float)py + 0.5f - cy;
        for (int px = x0; px < x1; px++) {
            float fx = (float)px + 0.5f - cx;
            float d = fabsf(sqrtf(fx * fx + fy * fy) - r);
            float cov = clampf(0.5f + half - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                blend(c, px, py, color, cov);
        }
    }
}

void d_arc(canvas_t *c, float cx, float cy, float r0, float r1, float a0, float a1,
           uint32_t color)
{
    if (r1 <= r0 || r0 < 0.0f)
        return;
    float span = a1 - a0;
    if (span >= TAU)
        span = TAU;

    int x0, y0, x1, y1;
    bbox(c, cx - r1, cy - r1, r1 * 2.0f, r1 * 2.0f, &x0, &y0, &x1, &y1);
    for (int py = y0; py < y1; py++) {
        float fy = (float)py + 0.5f - cy;
        for (int px = x0; px < x1; px++) {
            float fx = (float)px + 0.5f - cx;
            float len = sqrtf(fx * fx + fy * fy);
            if (len > r1 + 1.0f || len < r0 - 1.0f)
                continue;
            float d = fmaxf(r0 - len, len - r1);
            if (span < TAU) {
                float ang = atan2f(fx, -fy); /* 0 at 12 o'clock, clockwise */
                float rel = fmodf(ang - a0, TAU);
                if (rel < 0.0f)
                    rel += TAU;
                /* Angular distance in pixels: negative inside the wedge, so it
                 * combines with the radial distance and anti-aliases like it. */
                float ad;
                if (rel <= span) {
                    float edge = rel < span - rel ? rel : span - rel;
                    ad = -edge * len;
                } else {
                    float back = rel - span;
                    float fwd = TAU - rel;
                    ad = (back < fwd ? back : fwd) * len;
                }
                if (ad > d)
                    d = ad;
            }
            float cov = clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                blend(c, px, py, color, cov);
        }
    }
}

void d_line(canvas_t *c, float x0, float y0, float x1, float y1, float t, uint32_t color)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    float half = t * 0.5f;

    int bx0, by0, bx1, by1;
    float minx = x0 < x1 ? x0 : x1, maxx = x0 < x1 ? x1 : x0;
    float miny = y0 < y1 ? y0 : y1, maxy = y0 < y1 ? y1 : y0;
    bbox(c, minx - half, miny - half, (maxx - minx) + t, (maxy - miny) + t,
         &bx0, &by0, &bx1, &by1);
    for (int py = by0; py < by1; py++) {
        float fy = (float)py + 0.5f;
        for (int px = bx0; px < bx1; px++) {
            float fx = (float)px + 0.5f;
            float u = len2 > 0.0f ? ((fx - x0) * dx + (fy - y0) * dy) / len2 : 0.0f;
            u = clampf(u, 0.0f, 1.0f);
            float ex = x0 + dx * u - fx;
            float ey = y0 + dy * u - fy;
            float cov = clampf(0.5f + half - sqrtf(ex * ex + ey * ey), 0.0f, 1.0f);
            if (cov > 0.0f)
                blend(c, px, py, color, cov);
        }
    }
}

void d_triangle(canvas_t *c, float ax, float ay, float bx, float by, float cx, float cy,
                uint32_t color)
{
    float minx = fminf(ax, fminf(bx, cx)), maxx = fmaxf(ax, fmaxf(bx, cx));
    float miny = fminf(ay, fminf(by, cy)), maxy = fmaxf(ay, fmaxf(by, cy));
    if (maxx - minx < 0.5f || maxy - miny < 0.5f)
        return;

    float vx[3] = { ax, bx, cx };
    float vy[3] = { ay, by, cy };
    float nx[3], ny[3];
    float gx = (ax + bx + cx) / 3.0f, gy = (ay + by + cy) / 3.0f;

    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        float ex = vx[j] - vx[i], ey = vy[j] - vy[i];
        float len = sqrtf(ex * ex + ey * ey);
        if (len < 0.0001f)
            return;
        nx[i] = -ey / len;
        ny[i] = ex / len;
        /* Flip so the normal always points away from the centroid: positive
         * distance then means outside, whatever the winding. */
        if ((gx - vx[i]) * nx[i] + (gy - vy[i]) * ny[i] > 0.0f) {
            nx[i] = -nx[i];
            ny[i] = -ny[i];
        }
    }

    int x0, y0, x1, y1;
    bbox(c, minx, miny, maxx - minx, maxy - miny, &x0, &y0, &x1, &y1);
    for (int py = y0; py < y1; py++) {
        float fy = (float)py + 0.5f;
        for (int px = x0; px < x1; px++) {
            float fx = (float)px + 0.5f;
            float d = -1e9f;
            for (int i = 0; i < 3; i++) {
                float di = (fx - vx[i]) * nx[i] + (fy - vy[i]) * ny[i];
                if (di > d)
                    d = di;
            }
            float cov = clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f)
                blend(c, px, py, color, cov);
        }
    }
}

/* -------------------------------------------------------------------- text */
font_t *font_load(const char *path)
{
    if (!g_lib_ready) {
        if (FT_Init_FreeType(&g_lib) != 0)
            return NULL;
        g_lib_ready = 1;
    }
    font_t *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    if (FT_New_Face(g_lib, path, 0, &f->face) != 0) {
        free(f);
        return NULL;
    }
    return f;
}

void font_free(font_t *f)
{
    if (!f)
        return;
    FT_Done_Face(f->face);
    free(f);
}

static void set_px(font_t *f, float px)
{
    unsigned size = (unsigned)(px + 0.5f);
    if (size < 1)
        size = 1;
    FT_Set_Pixel_Sizes(f->face, 0, size);
}

float text_width(font_t *f, const char *s, float px, float tracking)
{
    if (!f || !s || !*s)
        return 0.0f;
    set_px(f, px);
    float w = 0.0f;
    int n = 0;
    for (const char *p = s; *p; p++) {
        if (FT_Load_Char(f->face, (FT_ULong)(unsigned char)*p, FT_LOAD_DEFAULT) != 0)
            continue;
        w += (float)f->face->glyph->advance.x / 64.0f;
        n++;
    }
    if (n > 1)
        w += tracking * (float)(n - 1);
    return w;
}

static void glyph_blend(canvas_t *c, int x0, int y0, const FT_Bitmap *b, uint32_t color)
{
    for (unsigned row = 0; row < b->rows; row++) {
        const unsigned char *src = b->buffer + row * b->pitch;
        for (unsigned col = 0; col < b->width; col++) {
            if (!src[col])
                continue;
            blend(c, x0 + (int)col, y0 + (int)row, color, (float)src[col] / 255.0f);
        }
    }
}

float text_draw(canvas_t *c, font_t *f, const char *s, float x, float y, float px,
                float tracking, uint32_t color, int align)
{
    if (!f || !s || !*s)
        return 0.0f;

    float total = text_width(f, s, px, tracking);
    set_px(f, px);

    float pen = x;
    if (align == ALIGN_CENTER)
        pen -= total * 0.5f;
    else if (align == ALIGN_RIGHT)
        pen -= total;

    for (const char *p = s; *p; p++) {
        if (FT_Load_Char(f->face, (FT_ULong)(unsigned char)*p, FT_LOAD_RENDER) != 0)
            continue;
        FT_GlyphSlot slot = f->face->glyph;
        int gx = (int)floorf(pen + (float)slot->bitmap_left + 0.5f);
        int gy = (int)floorf(y - (float)slot->bitmap_top + 0.5f);
        glyph_blend(c, gx, gy, &slot->bitmap, color);
        pen += (float)slot->advance.x / 64.0f + tracking;
    }
    return total;
}

void text_draw_vertical(canvas_t *c, font_t *f, const char *s, float cx, float cy,
                        float px, float tracking, uint32_t color)
{
    if (!f || !s || !*s)
        return;

    float total = text_width(f, s, px, tracking);
    set_px(f, px);

    /* Rotating 90 deg clockwise maps the pen's +x onto +y: the string descends
     * from the starting point, so centre it vertically on cy. */
    float pen_x = cx + px * 0.35f;
    float pen_y = cy - total * 0.5f;

    for (const char *p = s; *p; p++) {
        if (FT_Load_Char(f->face, (FT_ULong)(unsigned char)*p, FT_LOAD_RENDER) != 0)
            continue;
        FT_GlyphSlot slot = f->face->glyph;
        const FT_Bitmap *b = &slot->bitmap;
        int left = slot->bitmap_left;
        int top = slot->bitmap_top;
        for (unsigned row = 0; row < b->rows; row++) {
            const unsigned char *src = b->buffer + row * b->pitch;
            for (unsigned col = 0; col < b->width; col++) {
                if (!src[col])
                    continue;
                int dx = (int)floorf(pen_x + (float)top - (float)row + 0.5f);
                int dy = (int)floorf(pen_y + (float)left + (float)col + 0.5f);
                blend(c, dx, dy, color, (float)src[col] / 255.0f);
            }
        }
        pen_y += (float)slot->advance.x / 64.0f + tracking;
    }
}
