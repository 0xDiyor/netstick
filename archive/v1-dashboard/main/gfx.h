#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "display.h"

// Pixels are stored byte-swapped (big-endian RGB565) so the framebuffer can be
// DMA'd to the ST7735 untouched. Always build colours through RGB()/rgb565().
#define RGB565_RAW(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// Macro form so colours can initialise static const tables.
#define RGB(r, g, b) \
    ((uint16_t)((RGB565_RAW(r, g, b) >> 8) | (RGB565_RAW(r, g, b) << 8)))

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return RGB(r, g, b); }

#define C_BLACK   RGB(0,   0,   0)
#define C_WHITE   RGB(255, 255, 255)
#define C_GREY    RGB(120, 128, 136)
#define C_DIM     RGB(58,  64,  72)
#define C_RED     RGB(255, 64,  64)
#define C_GREEN   RGB(0,   230, 90)
#define C_LIME    RGB(0,   255, 65)     // fsociety phosphor green
#define C_AMBER   RGB(255, 176, 0)
#define C_BLUE    RGB(70,  150, 255)
#define C_CYAN    RGB(0,   220, 220)
#define C_MAGENTA RGB(230, 80,  200)

void gfx_clear(uint16_t c);
void gfx_px(int x, int y, uint16_t c);
void gfx_hline(int x, int y, int w, uint16_t c);
void gfx_vline(int x, int y, int h, uint16_t c);
void gfx_fill_rect(int x, int y, int w, int h, uint16_t c);
void gfx_rect(int x, int y, int w, int h, uint16_t c);
void gfx_blit(int x, int y, int w, int h, const uint16_t *src);

// 5x8 font. scale 1 => 6px advance, scale 2 => 12px advance.
int  gfx_text(int x, int y, const char *s, uint16_t fg, int scale);
int  gfx_text_bg(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);
int  gfx_printf(int x, int y, uint16_t fg, int scale, const char *fmt, ...);
int  gfx_text_w(const char *s, int scale);
int  gfx_text_right(int right_x, int y, const char *s, uint16_t fg, int scale);
