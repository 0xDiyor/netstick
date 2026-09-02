#include "gfx.h"
#include "font5x8.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void gfx_clear(uint16_t c)
{
    uint16_t *p = g_fb;
    for (int i = 0; i < LCD_W * LCD_H; i++) *p++ = c;
}

void gfx_px(int x, int y, uint16_t c)
{
    if ((unsigned)x >= LCD_W || (unsigned)y >= LCD_H) return;
    g_fb[y * LCD_W + x] = c;
}

void gfx_hline(int x, int y, int w, uint16_t c)
{
    if ((unsigned)y >= LCD_H) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    uint16_t *p = &g_fb[y * LCD_W + x];
    while (w-- > 0) *p++ = c;
}

void gfx_vline(int x, int y, int h, uint16_t c)
{
    if ((unsigned)x >= LCD_W) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > LCD_H) h = LCD_H - y;
    uint16_t *p = &g_fb[y * LCD_W + x];
    while (h-- > 0) { *p = c; p += LCD_W; }
}

void gfx_fill_rect(int x, int y, int w, int h, uint16_t c)
{
    for (int i = 0; i < h; i++) gfx_hline(x, y + i, w, c);
}

void gfx_rect(int x, int y, int w, int h, uint16_t c)
{
    gfx_hline(x, y, w, c);
    gfx_hline(x, y + h - 1, w, c);
    gfx_vline(x, y, h, c);
    gfx_vline(x + w - 1, y, h, c);
}

void gfx_blit(int x, int y, int w, int h, const uint16_t *src)
{
    for (int row = 0; row < h; row++) {
        int dy = y + row;
        if ((unsigned)dy >= LCD_H) continue;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if ((unsigned)dx >= LCD_W) continue;
            g_fb[dy * LCD_W + dx] = src[row * w + col];
        }
    }
}

static void draw_glyph(int x, int y, char ch, uint16_t fg, bool use_bg, uint16_t bg, int scale)
{
    if ((unsigned char)ch < FONT_FIRST || (unsigned char)ch > FONT_LAST) ch = '?';
    const uint8_t *g = &font5x8[((unsigned char)ch - FONT_FIRST) * FONT_W];

    for (int col = 0; col < FONT_W + 1; col++) {
        uint8_t bits = (col < FONT_W) ? g[col] : 0x00;   // trailing spacer column
        for (int row = 0; row < FONT_H; row++) {
            bool on = bits & (1 << row);
            if (!on && !use_bg) continue;
            uint16_t c = on ? fg : bg;
            if (scale == 1) {
                gfx_px(x + col, y + row, c);
            } else {
                gfx_fill_rect(x + col * scale, y + row * scale, scale, scale, c);
            }
        }
    }
}

int gfx_text_w(const char *s, int scale) { return (int)strlen(s) * (FONT_W + 1) * scale; }

int gfx_text(int x, int y, const char *s, uint16_t fg, int scale)
{
    int x0 = x;
    for (; *s; s++) {
        draw_glyph(x, y, *s, fg, false, 0, scale);
        x += (FONT_W + 1) * scale;
    }
    return x - x0;
}

int gfx_text_bg(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    int x0 = x;
    for (; *s; s++) {
        draw_glyph(x, y, *s, fg, true, bg, scale);
        x += (FONT_W + 1) * scale;
    }
    return x - x0;
}

int gfx_text_right(int right_x, int y, const char *s, uint16_t fg, int scale)
{
    return gfx_text(right_x - gfx_text_w(s, scale), y, s, fg, scale);
}

int gfx_printf(int x, int y, uint16_t fg, int scale, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return gfx_text(x, y, buf, fg, scale);
}
