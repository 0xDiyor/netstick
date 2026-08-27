#include "gfx.h"
#include "fonts/font_5x8.h"
#include "fonts/font_6x12.h"
#include "fonts/font_8x16.h"
#include "fonts/font_12x24.h"
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

static void glyph(const gfx_font_t *f, int x, int y, char ch, uint16_t fg, bool opaque, uint16_t bg)
{
    if ((unsigned char)ch < 32 || (unsigned char)ch > 126) ch = '?';
    const uint8_t *g = f->data + ((unsigned char)ch - 32) * f->h * f->bpr;
    for (int row = 0; row < f->h; row++) {
        int dy = y + row;
        if ((unsigned)dy >= LCD_H) continue;
        const uint8_t *r = g + row * f->bpr;
        for (int col = 0; col < f->w; col++) {
            int dx = x + col;
            if ((unsigned)dx >= LCD_W) continue;
            bool on = r[col >> 3] & (0x80 >> (col & 7));
            if (on) g_fb[dy * LCD_W + dx] = fg;
            else if (opaque) g_fb[dy * LCD_W + dx] = bg;
        }
    }
}

int gfx_text_w(const gfx_font_t *f, const char *s) { return (int)strlen(s) * f->w; }

int gfx_text_f(const gfx_font_t *f, int x, int y, const char *s, uint16_t fg)
{
    int x0 = x;
    for (; *s; s++, x += f->w) glyph(f, x, y, *s, fg, false, 0);
    return x - x0;
}

int gfx_text_fbg(const gfx_font_t *f, int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    int x0 = x;
    for (; *s; s++, x += f->w) glyph(f, x, y, *s, fg, true, bg);
    return x - x0;
}

int gfx_text(int x, int y, const char *s, uint16_t fg) { return gfx_text_f(&font_6x12, x, y, s, fg); }
int gfx_text_bg(int x, int y, const char *s, uint16_t fg, uint16_t bg) { return gfx_text_fbg(&font_6x12, x, y, s, fg, bg); }
int gfx_text_right(int right_x, int y, const char *s, uint16_t fg)
{
    return gfx_text(right_x - gfx_text_w(&font_6x12, s), y, s, fg);
}
int gfx_text_center(int y, const char *s, uint16_t fg)
{
    return gfx_text((LCD_W - gfx_text_w(&font_6x12, s)) / 2, y, s, fg);
}

int gfx_printf(int x, int y, uint16_t fg, const char *fmt, ...)
{
    char buf[96];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    return gfx_text(x, y, buf, fg);
}

int gfx_printf_f(const gfx_font_t *f, int x, int y, uint16_t fg, const char *fmt, ...)
{
    char buf[96];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    return gfx_text_f(f, x, y, buf, fg);
}
