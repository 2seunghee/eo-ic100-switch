#include "gfx.h"

#include <ft2build.h>
#include <math.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#define CACHE_SIZE 1024   // power of two

typedef struct {
    u32  key;              // codepoint << 9 | size << 1 | bold; 0 = empty
    s16  left, top, adv;
    u16  w, h;
    u8  *bmp;
} glyph;

static struct {
#ifndef EOFM_HOST
    Framebuffer fb;
#endif
    bool        fb_ok;
    u32        *px;
    u32         stride;    // in pixels
    FT_Library  ft;
    FT_Face     face[2];   // [0] Korean, [1] standard (Latin, symbols)
    int         nface;
    glyph       cache[CACHE_SIZE];
} G;

// ------------------------------------------------------------------ setup

#ifdef EOFM_HOST
// Screen preview on a PC (tests/preview.c): fonts from files, the frame written as a PPM image.
const char *g_host_fonts[2];
const char *g_host_out;
static u32 g_host_px[SCREEN_W * SCREEN_H];

bool gfx_init(char *err, size_t err_size) {
    if (FT_Init_FreeType(&G.ft)) return false;
    for (int i = 0; i < 2; i++)
        if (g_host_fonts[i] && !FT_New_Face(G.ft, g_host_fonts[i], 0, &G.face[G.nface])) G.nface++;
    snprintf(err, err_size, "no font");
    return G.nface > 0;
}

void gfx_exit(void) {
    for (int i = 0; i < G.nface; i++) FT_Done_Face(G.face[i]);
    G.nface = 0;
    FT_Done_FreeType(G.ft);
}

void gfx_begin(u32 background) {
    G.px = g_host_px;
    G.stride = SCREEN_W;
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) G.px[i] = background;
}

void gfx_end(void) {
    FILE *f = fopen(g_host_out, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        u8 rgb[3] = {(u8)G.px[i], (u8)(G.px[i] >> 8), (u8)(G.px[i] >> 16)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
#else
bool gfx_init(char *err, size_t err_size) {
    Result rc = plInitialize(PlServiceType_User);
    if (R_FAILED(rc)) {
        snprintf(err, err_size, "pl (shared fonts): 0x%X", rc);
        return false;
    }
    if (FT_Init_FreeType(&G.ft)) {
        snprintf(err, err_size, "FreeType init failed");
        return false;
    }
    PlSharedFontType types[2] = {PlSharedFontType_KO, PlSharedFontType_Standard};
    for (int i = 0; i < 2; i++) {
        PlFontData fd;
        if (R_FAILED(plGetSharedFontByType(&fd, types[i]))) continue;
        if (!FT_New_Memory_Face(G.ft, fd.address, fd.size, 0, &G.face[G.nface])) G.nface++;
    }
    if (!G.nface) {
        snprintf(err, err_size, "no shared font could be loaded");
        return false;
    }
    if (R_FAILED(framebufferCreate(&G.fb, nwindowGetDefault(), SCREEN_W, SCREEN_H, PIXEL_FORMAT_RGBA_8888, 2))) {
        snprintf(err, err_size, "framebuffer");
        return false;
    }
    framebufferMakeLinear(&G.fb);
    G.fb_ok = true;
    return true;
}

void gfx_exit(void) {
    for (int i = 0; i < CACHE_SIZE; i++) free(G.cache[i].bmp);
    memset(G.cache, 0, sizeof(G.cache));
    for (int i = 0; i < G.nface; i++) FT_Done_Face(G.face[i]);
    G.nface = 0;
    if (G.ft) FT_Done_FreeType(G.ft);
    G.ft = NULL;
    if (G.fb_ok) framebufferClose(&G.fb);
    G.fb_ok = false;
    plExit();
}

void gfx_begin(u32 background) {
    u32 stride_bytes;
    G.px = framebufferBegin(&G.fb, &stride_bytes);
    G.stride = stride_bytes / 4;
    for (int y = 0; y < SCREEN_H; y++) {
        u32 *row = G.px + (size_t)y * G.stride;
        for (int x = 0; x < SCREEN_W; x++) row[x] = background;
    }
}

void gfx_end(void) { framebufferEnd(&G.fb); }
#endif

// ------------------------------------------------------------------ pixels

static inline void blend(int x, int y, u32 c, float cover) {
    if ((unsigned)x >= SCREEN_W || (unsigned)y >= SCREEN_H || cover <= 0.0f) return;
    u32 a = (u32)((c >> 24) * (cover > 1.0f ? 1.0f : cover) + 0.5f);
    if (!a) return;
    u32 *p = G.px + (size_t)y * G.stride + x;
    if (a >= 255) {
        *p = c | 0xFF000000u;
        return;
    }
    u32 d = *p, ia = 255 - a;
    u32 r = ((c & 0xFF) * a + (d & 0xFF) * ia) / 255;
    u32 g = (((c >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * ia) / 255;
    u32 b = (((c >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * ia) / 255;
    *p = r | g << 8 | b << 16 | 0xFF000000u;
}

void gfx_rect(int x, int y, int w, int h, u32 c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) blend(i, j, c, 1.0f);
}

// Coverage of a pixel from a signed distance (negative = inside), 1 px soft edge.
static inline float cover_of(float d) { return d <= -0.5f ? 1.0f : d >= 0.5f ? 0.0f : 0.5f - d; }

// Signed distance to a rounded rectangle.
static float sd_rrect(float px, float py, float x, float y, float w, float h, float r) {
    float cx = x + w / 2, cy = y + h / 2;
    float qx = fabsf(px - cx) - (w / 2 - r), qy = fabsf(py - cy) - (h / 2 - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

void gfx_rrect(int x, int y, int w, int h, int r, u32 c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    gfx_rect(x, y + r, w, h - 2 * r, c);   // middle band: no edge maths
    for (int j = y; j < y + r; j++)
        for (int i = x; i < x + w; i++) {
            blend(i, j, c, cover_of(sd_rrect(i + 0.5f, j + 0.5f, x, y, w, h, r)));
            int jj = y + h - 1 - (j - y);
            blend(i, jj, c, cover_of(sd_rrect(i + 0.5f, jj + 0.5f, x, y, w, h, r)));
        }
}

void gfx_rrect_outline(int x, int y, int w, int h, int r, float t, u32 c) {
    if (r * 2 > h) r = h / 2;
    for (int j = y - 1; j <= y + h; j++)
        for (int i = x - 1; i <= x + w; i++) {
            bool near_edge = j < y + r + 2 || j > y + h - r - 3 || i < x + t + 2 || i > x + w - t - 3;
            if (!near_edge) continue;
            float d = sd_rrect(i + 0.5f, j + 0.5f, x, y, w, h, r);
            blend(i, j, c, cover_of(fabsf(d + t / 2) - t / 2));
        }
}

void gfx_circle(float cx, float cy, float r, u32 c) {
    for (int j = (int)(cy - r - 1); j <= (int)(cy + r + 1); j++)
        for (int i = (int)(cx - r - 1); i <= (int)(cx + r + 1); i++) {
            float dx = i + 0.5f - cx, dy = j + 0.5f - cy;
            blend(i, j, c, cover_of(sqrtf(dx * dx + dy * dy) - r));
        }
}

void gfx_ring(float cx, float cy, float r, float t, u32 c) { gfx_arc(cx, cy, r, t, 0, 360, c); }

void gfx_arc(float cx, float cy, float r, float t, float a0, float a1, u32 c) {
    float R = r + t / 2 + 1;
    for (int j = (int)(cy - R); j <= (int)(cy + R); j++)
        for (int i = (int)(cx - R); i <= (int)(cx + R); i++) {
            float dx = i + 0.5f - cx, dy = j + 0.5f - cy;
            float ang = atan2f(dx, -dy) * 180.0f / (float)M_PI;   // 0 = up, clockwise
            if (ang < 0) ang += 360;
            bool in = a0 <= a1 ? (ang >= a0 && ang <= a1) : (ang >= a0 || ang <= a1);
            if (!in) continue;
            blend(i, j, c, cover_of(fabsf(sqrtf(dx * dx + dy * dy) - r) - t / 2));
        }
}

void gfx_line(float x0, float y0, float x1, float y1, float t, u32 c) {
    float minx = fminf(x0, x1) - t, maxx = fmaxf(x0, x1) + t, miny = fminf(y0, y1) - t, maxy = fmaxf(y0, y1) + t;
    float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
    for (int j = (int)miny; j <= (int)maxy; j++)
        for (int i = (int)minx; i <= (int)maxx; i++) {
            float px = i + 0.5f - x0, py = j + 0.5f - y0;
            float k = len2 > 0 ? (px * vx + py * vy) / len2 : 0;
            k = k < 0 ? 0 : k > 1 ? 1 : k;
            float dx = px - k * vx, dy = py - k * vy;
            blend(i, j, c, cover_of(sqrtf(dx * dx + dy * dy) - t / 2));
        }
}

// ------------------------------------------------------------------ text

static u32 utf8_next(const char **s) {
    const u8 *p = (const u8 *)*s;
    u32 c = *p++;
    int extra = 0;
    if (c >= 0xF0) c &= 0x07, extra = 3;
    else if (c >= 0xE0) c &= 0x0F, extra = 2;
    else if (c >= 0xC0) c &= 0x1F, extra = 1;
    for (int k = 0; k < extra && (*p & 0xC0) == 0x80; k++) c = c << 6 | (*p++ & 0x3F);
    *s = (const char *)p;
    return c;
}

static glyph *get_glyph(u32 cp, int size, bool bold) {
    u32 key = cp << 9 | (u32)(size & 0xFF) << 1 | (bold ? 1 : 0);
    u32 h = (key * 2654435761u) >> 22;   // top 10 bits (CACHE_SIZE 1024): the low bits barely depend on the codepoint
    for (int probe = 0; probe < 16; probe++, h = (h + 1) & (CACHE_SIZE - 1)) {
        glyph *g = &G.cache[h];
        if (g->key == key) return g;
        if (g->key) continue;
        FT_Face face = G.face[0];
        FT_UInt idx = FT_Get_Char_Index(face, cp);
        for (int f = 1; !idx && f < G.nface; f++) {
            face = G.face[f];
            idx = FT_Get_Char_Index(face, cp);
        }
        FT_Set_Pixel_Sizes(face, 0, (FT_UInt)size);
        if (FT_Load_Glyph(face, idx, FT_LOAD_DEFAULT)) return NULL;
        if (bold) FT_GlyphSlot_Embolden(face->glyph);
        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) return NULL;
        FT_Bitmap *b = &face->glyph->bitmap;
        g->key = key;
        g->left = (s16)face->glyph->bitmap_left;
        g->top = (s16)face->glyph->bitmap_top;
        g->adv = (s16)(face->glyph->advance.x >> 6);
        g->w = (u16)b->width;
        g->h = (u16)b->rows;
        g->bmp = g->w && g->h ? malloc((size_t)g->w * g->h) : NULL;
        if (g->bmp)
            for (u32 r = 0; r < b->rows; r++) memcpy(g->bmp + r * g->w, b->buffer + r * b->pitch, g->w);
        return g;
    }
    return NULL;   // cache region full: skip this character
}

int gfx_text_width(int size, bool bold, const char *s) {
    int w = 0;
    while (*s) {
        glyph *g = get_glyph(utf8_next(&s), size, bold);
        if (g) w += g->adv;
    }
    return w;
}

int gfx_text(int x, int y, int size, u32 c, int align, bool bold, const char *s) {
    int w = gfx_text_width(size, bold, s);
    if (align == ALIGN_CENTER) x -= w / 2;
    else if (align == ALIGN_RIGHT) x -= w;
    int base = y + (int)(size * 0.86f);
    while (*s) {
        glyph *g = get_glyph(utf8_next(&s), size, bold);
        if (!g) continue;
        for (int r = 0; r < g->h; r++)
            for (int k = 0; k < g->w; k++) {
                u8 a = g->bmp[r * g->w + k];
                if (a) blend(x + g->left + k, base - g->top + r, c, a / 255.0f);
            }
        x += g->adv;
    }
    return w;
}
