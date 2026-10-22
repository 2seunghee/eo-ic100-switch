#pragma once
#include "common.h"

// Small software renderer on the libnx framebuffer (1280x720 RGBA8888) with anti-aliased shapes
// and FreeType text from the console's shared fonts (Korean + standard).

#define SCREEN_W 1280
#define SCREEN_H 720

// Colours are 0xAABBGGRR in memory order R, G, B, A.
#define RGBA(r, g, b, a) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16 | (u32)(a) << 24)
#define RGB(r, g, b)     RGBA(r, g, b, 255)

enum { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

bool gfx_init(char *err, size_t err_size);
void gfx_exit(void);
void gfx_begin(u32 background);
void gfx_end(void);

void gfx_rect(int x, int y, int w, int h, u32 c);
void gfx_rrect(int x, int y, int w, int h, int r, u32 c);                  // filled, rounded
void gfx_rrect_outline(int x, int y, int w, int h, int r, float t, u32 c);  // border of thickness t
void gfx_circle(float cx, float cy, float r, u32 c);
void gfx_ring(float cx, float cy, float r, float t, u32 c);
// Ring between angles a0..a1 (degrees, 0 = up, clockwise) - for the power icon.
void gfx_arc(float cx, float cy, float r, float t, float a0, float a1, u32 c);
void gfx_line(float x0, float y0, float x1, float y1, float t, u32 c);

// Text. `y` is the top of the line box (size pixels high). Returns the advance width.
int gfx_text(int x, int y, int size, u32 c, int align, bool bold, const char *utf8);
int gfx_text_width(int size, bool bold, const char *utf8);
