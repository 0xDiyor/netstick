#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "display.h"

// Pixels are stored byte-swapped (big-endian RGB565) so the framebuffer can be
// DMA'd to the ST7735 untouched. Always build colours through RGB()/rgb565().
#define RGB565_RAW(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define RGB(r, g, b) \
    ((uint16_t)((RGB565_RAW(r, g, b) >> 8) | (RGB565_RAW(r, g, b) << 8)))

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return RGB(r, g, b); }

// Terminal palette. Phosphor green on black, with a handful of status colours.
#define C_BG      RGB(0,   0,   0)
#define C_FG      RGB(140, 255, 160)   // body text
#define C_BRIGHT  RGB(235, 255, 235)   // emphasised text
#define C_DIM     RGB(70,  120, 85)    // secondary text, separators
#define C_FAINT   RGB(28,  50,  36)    // hold-progress, subtle fills
#define C_OK      RGB(60,  255, 100)
#define C_WARN    RGB(255, 190, 40)
#define C_FAIL    RGB(255, 70,  70)
#define C_INFO    RGB(80,  200, 255)   // 5 GHz / links / values
#define C_ALT     RGB(255, 160, 60)    // 2.4 GHz
#define C_INV_BG  RGB(140, 255, 160)   // inverse video (selected menu row)
#define C_INV_FG  RGB(0,   0,   0)

typedef struct {
    uint8_t w, h, bpr;          // glyph width/height in px, bytes per row
    const uint8_t *data;        // 95 glyphs (' '..'~'), row-major, MSB = left
} gfx_font_t;

extern const gfx_font_t font_5x8, font_6x12, font_8x16, font_12x24;

// Layout helpers for the 6x12 body font: 26 columns x 6 rows.
#define F_W   6
#define F_H   12
#define COLS  (LCD_W / F_W)
#define CX(col) ((col) * F_W)

void gfx_clear(uint16_t c);
void gfx_px(int x, int y, uint16_t c);
void gfx_hline(int x, int y, int w, uint16_t c);
void gfx_vline(int x, int y, int h, uint16_t c);
void gfx_fill_rect(int x, int y, int w, int h, uint16_t c);
void gfx_rect(int x, int y, int w, int h, uint16_t c);

// Text. All return the width drawn in pixels. bg == C_BG with opaque=false
// leaves the background untouched.
int gfx_text_f(const gfx_font_t *f, int x, int y, const char *s, uint16_t fg);
int gfx_text_fbg(const gfx_font_t *f, int x, int y, const char *s, uint16_t fg, uint16_t bg);
int gfx_text_w(const gfx_font_t *f, const char *s);

// Body-font conveniences (6x12).
int gfx_text(int x, int y, const char *s, uint16_t fg);
int gfx_text_bg(int x, int y, const char *s, uint16_t fg, uint16_t bg);
int gfx_text_right(int right_x, int y, const char *s, uint16_t fg);
int gfx_text_center(int y, const char *s, uint16_t fg);
int gfx_printf(int x, int y, uint16_t fg, const char *fmt, ...);
int gfx_printf_f(const gfx_font_t *f, int x, int y, uint16_t fg, const char *fmt, ...);
