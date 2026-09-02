#include "fsociety.h"
#include "gfx.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "fsociety";

// Face geometry, in pixels relative to the centre of the mask. Mirrors
// tools/fsociety.py so the on-device render and the .anim export agree.
#define CX      80.0f
#define CY      30.0f
#define A       19.5f
#define B_TOP   26.0f
#define B_BOT   29.0f

#define FRAME_COUNT 48
#define FRAME_MS    65

#define C_FACE    RGB(232, 220, 198)
#define C_DARK    RGB(16,  13,  11)
#define C_OUTLINE RGB(60,  52,  44)

static uint16_t *s_base = NULL;      // the clean mask, rendered once
static uint16_t  s_frame = 0;
static uint8_t   dim5[32], dim6[64]; // scanline dimming LUTs

// Small xorshift so the glitch pattern is cheap and repeatable across boots.
static uint32_t s_rng = 0xF50C1E7Fu;
static inline uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static inline uint16_t sw(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static inline bool ellipse(float dx, float dy, float ex, float ey, float rx, float ry)
{
    float u = (dx - ex) / rx;
    float v = (dy - ey) / ry;
    return u * u + v * v <= 1.0f;
}

static bool in_face(float dx, float dy)
{
    float b = (dy < 0) ? B_TOP : B_BOT;
    float t = dy / b;
    if (t < -1.0f || t > 1.0f) return false;
    float taper = t > 0 ? t : 0.0f;
    float a_eff = A * (1.0f - 0.26f * taper * taper);   // jaw narrows toward the chin
    float u = dx / a_eff;
    return u * u + t * t <= 1.0f;
}

static bool in_brow(float dx, float dy)
{
    // Band between two concentric ellipses, split into a left and right arch and
    // kept clear of the eyes so the two never merge into one dark mass.
    bool inner = ellipse(dx, dy, 0, -3.0f, 17.0f, 12.6f);
    bool outer = ellipse(dx, dy, 0, -3.0f, 17.0f, 15.8f);
    float adx = dx < 0 ? -dx : dx;
    return outer && !inner && dy < -12.6f && adx >= 3.0f && adx <= 15.0f;
}

static bool in_eye(float dx, float dy)
{
    return ellipse(dx, dy, -8.6f, -5.0f, 5.6f, 6.6f)
        || ellipse(dx, dy,  8.6f, -5.0f, 5.6f, 6.6f);
}

static bool in_mustache(float dx, float dy)
{
    // Two wings joined by a thin bridge so a dip shows under the nose, plus the
    // upturned curls at the outer ends.
    return ellipse(dx, dy, -8.5f, 9.5f, 8.2f, 3.9f)
        || ellipse(dx, dy,  8.5f, 9.5f, 8.2f, 3.9f)
        || ellipse(dx, dy,  0.0f, 8.4f, 3.8f, 2.0f)
        || ellipse(dx, dy, -15.8f, 5.8f, 3.2f, 3.6f)
        || ellipse(dx, dy,  15.8f, 5.8f, 3.2f, 3.6f);
}

static bool in_goatee(float dx, float dy)
{
    return ellipse(dx, dy, 0, 20.5f, 4.0f, 4.6f)
        || ellipse(dx, dy, 0, 24.5f, 2.3f, 4.2f);
}

esp_err_t fsociety_init(void)
{
    if (s_base) return ESP_OK;

    s_base = heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_base) s_base = malloc(LCD_W * LCD_H * 2);
    if (!s_base) return ESP_ERR_NO_MEM;

    for (int i = 0; i < 32; i++) dim5[i] = (uint8_t)((i * 78) / 100);
    for (int i = 0; i < 64; i++) dim6[i] = (uint8_t)((i * 78) / 100);

    // Soft float on the C5 is slow, but this runs exactly once.
    for (int y = 0; y < LCD_H; y++) {
        float dy = (float)y - CY;
        for (int x = 0; x < LCD_W; x++) {
            float dx = (float)x - CX;
            uint16_t c = C_BLACK;
            if (in_face(dx, dy)) {
                c = (in_eye(dx, dy) || in_brow(dx, dy) || in_mustache(dx, dy)
                     || in_goatee(dx, dy)) ? C_DARK : C_FACE;
            }
            s_base[y * LCD_W + x] = c;
        }
    }

    // 1px outline so the pale face reads against black.
    for (int y = 0; y < LCD_H; y++) {
        for (int x = 0; x < LCD_W; x++) {
            if (s_base[y * LCD_W + x] != C_BLACK) continue;
            bool touches = false;
            if (y > 0          && s_base[(y - 1) * LCD_W + x] == C_FACE) touches = true;
            if (y < LCD_H - 1  && s_base[(y + 1) * LCD_W + x] == C_FACE) touches = true;
            if (x > 0          && s_base[y * LCD_W + x - 1]   == C_FACE) touches = true;
            if (x < LCD_W - 1  && s_base[y * LCD_W + x + 1]   == C_FACE) touches = true;
            if (touches) s_base[y * LCD_W + x] = C_OUTLINE;
        }
    }

    ESP_LOGI(TAG, "mask rasterised");
    return ESP_OK;
}

// ------------------------------------------------------------- effects ----
static void slice_glitch(int count)
{
    static uint16_t row[LCD_W];
    for (int i = 0; i < count; i++) {
        int y0 = rnd() % (LCD_H - 2);
        int hh = 2 + rnd() % 8;
        int shift = (int)(rnd() % 29) - 14;
        if (!shift) continue;
        for (int y = y0; y < y0 + hh && y < LCD_H; y++) {
            uint16_t *src = &g_fb[y * LCD_W];
            for (int x = 0; x < LCD_W; x++) {
                int sx = x - shift;
                while (sx < 0) sx += LCD_W;
                while (sx >= LCD_W) sx -= LCD_W;
                row[x] = src[sx];
            }
            memcpy(src, row, sizeof(row));
        }
    }
}

static void channel_split(int off)
{
    static uint16_t row[LCD_W];
    for (int y = 0; y < LCD_H; y++) {
        uint16_t *line = &g_fb[y * LCD_W];
        memcpy(row, line, sizeof(row));
        for (int x = 0; x < LCD_W; x++) {
            int xr = x - off; if (xr < 0) xr = 0;
            int xb = x + off; if (xb >= LCD_W) xb = LCD_W - 1;
            uint16_t r = sw(row[xr]) & 0xF800;
            uint16_t g = sw(row[x])  & 0x07E0;
            uint16_t b = sw(row[xb]) & 0x001F;
            line[x] = sw(r | g | b);
        }
    }
}

static void noise(int per_thousand)
{
    int n = (LCD_W * LCD_H * per_thousand) / 1000;
    for (int i = 0; i < n; i++) {
        uint32_t r = rnd();
        g_fb[r % (LCD_W * LCD_H)] = ((r >> 16) & 0xFF) < 90 ? C_LIME : RGB(200, 200, 200);
    }
}

static void scanlines(void)
{
    for (int y = 1; y < LCD_H; y += 2) {
        uint16_t *line = &g_fb[y * LCD_W];
        for (int x = 0; x < LCD_W; x++) {
            uint16_t n = sw(line[x]);
            if (!n) continue;
            uint16_t v = ((uint16_t)dim5[(n >> 11) & 0x1F] << 11)
                       | ((uint16_t)dim6[(n >> 5)  & 0x3F] << 5)
                       |  (uint16_t)dim5[n & 0x1F];
            line[x] = sw(v);
        }
    }
}

// -------------------------------------------------------------- render ----
uint16_t fsociety_draw(void)
{
    if (!s_base && fsociety_init() != ESP_OK) {
        gfx_clear(C_BLACK);
        gfx_text(20, 36, "mask alloc failed", C_RED, 1);
        return 1000;
    }

    const uint16_t f = s_frame;
    memcpy(g_fb, s_base, LCD_W * LCD_H * 2);

    // The wordmark types itself in, with a blinking caret.
    static const char WORD[] = "fsociety";
    int shown = (f >= 16) ? 8 : (int)((f > 2 ? f - 2 : 0) / 2);
    if (shown > 8) shown = 8;
    if (shown > 0) {
        char buf[9];
        memcpy(buf, WORD, shown);
        buf[shown] = '\0';
        gfx_text(32, 63, buf, C_LIME, 2);
    }
    if (f >= 6 && f < 16 && (f & 1) == 0 && shown < 8) {
        gfx_text(32 + shown * 12, 63, "_", C_LIME, 2);
    }

    if (f < 8) {                            // resolve out of static
        int cutoff = (LCD_H * (f + 1)) / 8;
        for (int y = cutoff; y < LCD_H; y++)
            memset(&g_fb[y * LCD_W], 0, LCD_W * 2);
        noise(55);
        slice_glitch(3);
    } else if (f >= FRAME_COUNT - 4) {      // break apart before looping
        slice_glitch(5);
        channel_split(3);
        noise(28);
    } else if (f % 11 <= 1) {
        slice_glitch(2);
        channel_split(2);
    } else if (f % 17 == 5) {
        noise(8);
    }

    scanlines();

    s_frame = (uint16_t)((f + 1) % FRAME_COUNT);
    return FRAME_MS;
}
