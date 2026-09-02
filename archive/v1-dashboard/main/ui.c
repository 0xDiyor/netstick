#include "ui.h"
#include "gfx.h"
#include "board.h"
#include "led.h"
#include "netmgr.h"
#include "scanner.h"
#include "github.h"
#include "storage.h"
#include "anim.h"
#include "fsociety.h"
#include "secrets.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef CONFIG_FB_DUMP
#include "mbedtls/base64.h"
// Debug aid: base64 the framebuffer to the console so the host can render the
// exact panel output as a PNG. Enabled with: FB_DUMP=1 idf.py build
static void fb_dump(const char *tag)
{
    const size_t raw = (size_t)LCD_W * LCD_H * 2;
    size_t cap = ((raw + 2) / 3) * 4 + 8, olen = 0;
    unsigned char *b64 = malloc(cap);
    if (!b64) return;
    // Other tasks logging mid-dump splice text into the base64 payload, so mute
    // the log for the duration.
    esp_log_level_set("*", ESP_LOG_NONE);
    if (mbedtls_base64_encode(b64, cap, &olen, (const unsigned char *)g_fb, raw) == 0) {
        printf("\n<<<FB %s %d %d\n", tag, LCD_W, LCD_H);
        fflush(stdout);
        // Push it out in small pieces with a yield between: one 34 kB blocking
        // write deadlocks the console the moment the host stops draining it.
        const size_t chunk = 512;
        for (size_t i = 0; i < olen; i += chunk) {
            size_t n = (olen - i) < chunk ? (olen - i) : chunk;
            fwrite(b64 + i, 1, n, stdout);
            fflush(stdout);
            vTaskDelay(1);
        }
        printf("\n>>>FB\n");
        fflush(stdout);
    }
    esp_log_level_set("*", ESP_LOG_INFO);
    free(b64);
}
static const char *SCR_NAME[] = { "wifi", "github", "fsociety", "clock", "anim" };
#endif

static const char *TAG = "ui";

typedef enum {
    SCR_WIFI,
    SCR_GITHUB,
    SCR_FSOCIETY,   // built into the firmware, always available
    SCR_CLOCK,
    SCR_ANIM,       // SD .anim player, skipped when the card has none
    SCR_COUNT
} screen_t;

static int s_anim_count = 0;

// The SD player is only worth stopping on if there is something to play.
static bool screen_available(screen_t s)
{
    if (s == SCR_ANIM) return storage_mounted() && s_anim_count > 0;
    return true;
}

static screen_t next_screen(screen_t from)
{
    for (int i = 1; i <= SCR_COUNT; i++) {
        screen_t cand = (screen_t)((from + i) % SCR_COUNT);
        if (screen_available(cand)) return cand;
    }
    return from;
}

static volatile screen_t s_screen = SCR_WIFI;
static volatile bool     s_force_refresh = false;
static volatile bool     s_screen_changed = true;

#define SCAN_INTERVAL_MS   20000
#define GH_INTERVAL_MS     300000

// --------------------------------------------------------------- button ---
// GPIO28, active low with an internal pull-up.
//   tap   -> next screen
//   hold  -> dim / undim the backlight
//   long  -> force a data refresh on the current screen
static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    bool     was_down = false;
    int64_t  down_at = 0;
    bool     handled_hold = false;

    while (1) {
        bool down = gpio_get_level(PIN_BTN) == 0;
        int64_t now = esp_timer_get_time();

        if (down && !was_down) {
            down_at = now;
            handled_hold = false;
        } else if (down && !handled_hold && (now - down_at) > 2500000) {
            s_force_refresh = true;
            handled_hold = true;
            led_set(255, 255, 255, 6);
        } else if (!down && was_down) {
            int64_t held_ms = (now - down_at) / 1000;
            if (handled_hold) {
                // already acted on
            } else if (held_ms > 700) {
                display_backlight(display_backlight_get() > 40 ? 15 : 100);
            } else if (held_ms > 30) {
                s_screen = next_screen(s_screen);
                s_screen_changed = true;
            }
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ------------------------------------------------------- wifi analyzer ----
static uint16_t rssi_colour(int8_t rssi)
{
    if (rssi >= -55) return C_GREEN;
    if (rssi >= -70) return C_AMBER;
    return C_RED;
}

static void draw_wifi(void)
{
    scan_summary_t r;
    net_status_t   n;
    scanner_get_summary(&r);
    netmgr_get(&n);

    gfx_clear(C_BLACK);

    char b[40];
    if (scanner_busy()) gfx_text(0, 0, "SCAN..", C_AMBER, 1);
    else                gfx_text(0, 0, "SURVEY", C_LIME, 1);

    if (r.valid) {
        snprintf(b, sizeof(b), "2.4:%u", r.n_24); gfx_text(54,  0, b, C_AMBER, 1);
        snprintf(b, sizeof(b), "5:%u",   r.n_5);  gfx_text(96,  0, b, C_CYAN, 1);
        snprintf(b, sizeof(b), "ax:%u",  r.n_ax); gfx_text(126, 0, b, C_MAGENTA, 1);
    }
    gfx_hline(0, 9, LCD_W, C_DIM);

    if (!r.valid) {
        gfx_text(28, 36, "sweeping both", C_GREY, 1);
        gfx_text(46, 46, "bands...", C_GREY, 1);
        return;
    }

    // ---- channel histogram: 2.4 GHz on the left, 5 GHz on the right --------
    const int base_y = 40, max_h = 28;
    uint8_t peak = 1;
    for (int i = 0; i < CH24_COUNT; i++) if (r.hist24[i] > peak) peak = r.hist24[i];
    for (int i = 0; i < CH5_COUNT; i++)  if (r.hist5[i]  > peak) peak = r.hist5[i];

    for (int i = 0; i < CH24_COUNT; i++) {
        int h = (r.hist24[i] * max_h) / peak;
        if (r.hist24[i] && h < 1) h = 1;
        bool mine = n.connected && n.band == 2 && n.channel == i + 1;
        if (h) gfx_fill_rect(i * 4, base_y - h, 3, h, mine ? C_GREEN : C_AMBER);
    }
    // 28 five-GHz channels at a 3px pitch: 58..141, leaving the 2.4 GHz block
    // its own 4px pitch on the left and a little breathing room on the right.
    for (int i = 0; i < CH5_COUNT; i++) {
        int h = (r.hist5[i] * max_h) / peak;
        if (r.hist5[i] && h < 1) h = 1;
        bool mine = n.connected && n.band == 5 && n.channel == ch5_list[i];
        if (h) gfx_fill_rect(58 + i * 3, base_y - h, 2, h, mine ? C_GREEN : C_CYAN);
    }
    gfx_hline(0, base_y, 56, C_DIM);
    gfx_hline(58, base_y, CH5_COUNT * 3, C_DIM);

    gfx_text(0,   42, "1",   C_DIM, 1);
    gfx_text(20,  42, "6",   C_DIM, 1);
    gfx_text(40,  42, "11",  C_DIM, 1);
    gfx_text(58,  42, "36",  C_DIM, 1);
    gfx_text(82,  42, "100", C_DIM, 1);
    gfx_text(118, 42, "149", C_DIM, 1);
    gfx_hline(0, 51, LCD_W, C_DIM);

    // ---- three strongest APs ----------------------------------------------
    scan_ap_t top[3];
    int ntop = scanner_get_top(top, 3);
    for (int i = 0; i < ntop; i++) {
        const scan_ap_t *ap = &top[i];
        int y = 53 + i * 9;
        bool mine = n.connected && strcmp(ap->ssid, n.ssid) == 0 && ap->channel == n.channel;

        char name[14];
        snprintf(name, sizeof(name), "%.13s", ap->ssid[0] ? ap->ssid : "(hidden)");
        gfx_text(0, y, mine ? ">" : " ", C_GREEN, 1);
        gfx_text(6, y, name, ap->band == 5 ? C_CYAN : C_AMBER, 1);

        snprintf(b, sizeof(b), "%d", ap->rssi);
        gfx_text_right(110, y, b, rssi_colour(ap->rssi), 1);
        snprintf(b, sizeof(b), "%u", ap->channel);
        gfx_text_right(134, y, b, C_GREY, 1);
        gfx_text(138, y, ap->phy + 2, C_DIM, 1);   // "11ax" -> "ax"
    }

    // ---- a 1px bar showing how stale this scan is -------------------------
    uint32_t age = scanner_age_ms();
    int w = (int)((age > SCAN_INTERVAL_MS ? SCAN_INTERVAL_MS : age) * LCD_W / SCAN_INTERVAL_MS);
    gfx_hline(0, 79, w, r.logged ? C_LIME : C_DIM);
}

// ------------------------------------------------------------- github ----
static const uint16_t GH_LEVEL[5] = {
    RGB(0x16, 0x1b, 0x22), RGB(0x0e, 0x44, 0x29), RGB(0x00, 0x6d, 0x32),
    RGB(0x26, 0xa6, 0x41), RGB(0x39, 0xd3, 0x53),
};

static void stat_pair(int x, int y, int right, const char *label, int value, uint16_t vc)
{
    char b[16];
    gfx_text(x, y, label, C_DIM, 1);
    if (value < 0) snprintf(b, sizeof(b), "--");
    else if (value >= 10000) snprintf(b, sizeof(b), "%dk", value / 1000);
    else snprintf(b, sizeof(b), "%d", value);
    gfx_text_right(right, y, b, vc, 1);
}

static void draw_github(void)
{
    gh_stats_t g;
    github_get(&g);
    gfx_clear(C_BLACK);

    char b[48];
    snprintf(b, sizeof(b), "@%s", g.user[0] ? g.user : GITHUB_USER);
    b[13] = '\0';
    gfx_text(0, 0, b, C_LIME, 1);

    if (g.valid && g.contrib_total >= 0) {
        snprintf(b, sizeof(b), "%d/yr", g.contrib_total);
        gfx_text_right(LCD_W, 0, b, C_WHITE, 1);
    }
    gfx_hline(0, 9, LCD_W, C_DIM);

    if (!g.valid) {
        gfx_text(10, 34, g.err[0] ? g.err : "fetching...", g.err[0] ? C_RED : C_GREY, 1);
        return;
    }

    int stats_y = 36;
    if (g.cal_weeks > 0) {
        // 53 weeks x 7 days at a 3px pitch lands in exactly 159x21 px.
        for (int w = 0; w < g.cal_weeks && w < GH_CAL_WEEKS; w++) {
            for (int d = 0; d < 7; d++) {
                uint8_t lv = g.cal[w][d];
                if (lv > 4) lv = 4;
                gfx_fill_rect(w * 3, 12 + d * 3, 2, 2, GH_LEVEL[lv]);
            }
        }
    } else {
        gfx_text(0, 14, "no heatmap: add a", C_DIM, 1);
        gfx_text(0, 24, "token to secrets.h", C_DIM, 1);
    }
    gfx_hline(0, 34, LCD_W, C_DIM);

    stat_pair(0,  stats_y,      76,    "STARS",  g.stars,      C_AMBER);
    stat_pair(0,  stats_y + 11, 76,    "FOLLOW", g.followers,  C_WHITE);
    stat_pair(0,  stats_y + 22, 76,    "STREAK", g.streak_cur, g.streak_cur ? C_LIME : C_GREY);
    stat_pair(86, stats_y,      LCD_W, "PRS",    g.open_prs,   C_MAGENTA);
    stat_pair(86, stats_y + 11, LCD_W, "REPOS",  g.repos,      C_WHITE);
    stat_pair(86, stats_y + 22, LCD_W, "TODAY",  g.contrib_today,
              g.contrib_today > 0 ? C_LIME : C_GREY);

    if (g.err[0]) {
        gfx_text(0, 71, g.err, C_RED, 1);
    } else if (g.fetched_at) {
        int mins = (int)((time(NULL) - g.fetched_at) / 60);
        snprintf(b, sizeof(b), "%s  %dm ago", g.authed ? "graphql" : "rest", mins);
        gfx_text(0, 71, b, C_DIM, 1);
    }
}

// -------------------------------------------------------------- clock ----
static void draw_clock(void)
{
    net_status_t n;
    netmgr_get(&n);
    gfx_clear(C_BLACK);

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char b[48];
    strftime(b, sizeof(b), "%a %d %b", &tm_now);
    gfx_text(0, 0, b, C_GREY, 1);

    const char *state = !n.connected ? "OFFLINE" : (n.has_ip ? "ONLINE" : "LINK");
    gfx_text_right(LCD_W, 0, state,
                   !n.connected ? C_RED : (n.has_ip ? C_GREEN : C_AMBER), 1);
    gfx_hline(0, 9, LCD_W, C_DIM);

    if (n.time_synced) strftime(b, sizeof(b), "%H:%M", &tm_now);
    else               snprintf(b, sizeof(b), "--:--");
    gfx_text(28, 13, b, C_WHITE, 3);
    strftime(b, sizeof(b), "%S", &tm_now);
    gfx_text(138, 29, b, C_DIM, 1);

    if (n.connected) {
        snprintf(b, sizeof(b), "%.10s", n.ssid);
        gfx_text(0, 40, b, C_LIME, 1);
        snprintf(b, sizeof(b), "%d", n.rssi);
        gfx_text_right(90, 40, b, rssi_colour(n.rssi), 1);
        snprintf(b, sizeof(b), "%uG ch%u %s", n.band, n.channel, n.phy);
        gfx_text_right(LCD_W, 40, b, n.band == 5 ? C_CYAN : C_AMBER, 1);
    } else {
        gfx_text(0, 40, "no link", C_RED, 1);
    }

    // ---- latency sparkline: 64 samples, 2px each, timeouts as red spikes ---
    const int spark_x = 0, spark_y = 50, spark_h = 20, base = spark_y + spark_h;
    uint16_t peak = 40;
    for (int i = 0; i < NET_RTT_HIST; i++) if (n.rtt[i] > peak) peak = n.rtt[i];
    if (peak > 400) peak = 400;

    for (int i = 0; i < NET_RTT_HIST; i++) {
        int idx = (n.rtt_head + i) % NET_RTT_HIST;       // oldest first
        uint16_t v = n.rtt[idx];
        int x = spark_x + i * 2;
        if (v == 0) {
            if (n.ping_sent) gfx_vline(x, spark_y, spark_h, C_RED);   // timeout
            continue;
        }
        int h = (v * spark_h) / peak;
        if (h < 1) h = 1;
        if (h > spark_h) h = spark_h;
        gfx_fill_rect(x, base - h, 2, h, v > 150 ? C_AMBER : C_LIME);
    }
    gfx_hline(spark_x, base, NET_RTT_HIST * 2, C_DIM);

    if (n.rtt_last) snprintf(b, sizeof(b), "%ums", n.rtt_last);
    else            snprintf(b, sizeof(b), "--");
    gfx_text_right(LCD_W, 52, b, C_WHITE, 1);
    gfx_text_right(LCD_W, 62, "rtt", C_DIM, 1);

    int loss = n.ping_sent ? (int)((n.ping_lost * 100) / n.ping_sent) : 0;
    uint32_t up = n.connected ? (uint32_t)(esp_timer_get_time() / 1000000) - n.connected_since : 0;
    snprintf(b, sizeof(b), "drop%u %d%% %luh%02lum", (unsigned)n.disconnects, loss,
             (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60));
    gfx_text(0, 72, b, C_DIM, 1);
}

// ---------------------------------------------------------------- anim ----
static char    s_anim_files[ANIM_MAX_FILES][ANIM_PATH_LEN];
static int     s_anim_idx = 0;
static anim_t  s_anim;
static bool    s_anim_open = false;

static void anim_enter(void)
{
    if (!storage_mounted()) return;
    if (s_anim_count == 0) s_anim_count = anim_scan_dir(s_anim_files, ANIM_MAX_FILES);
    if (s_anim_count == 0) return;
    if (!s_anim_open) {
        if (anim_open(&s_anim, s_anim_files[s_anim_idx]) == ESP_OK) s_anim_open = true;
    }
}

static void anim_leave(void)
{
    if (s_anim_open) { anim_close(&s_anim); s_anim_open = false; }
    if (s_anim_count) s_anim_idx = (s_anim_idx + 1) % s_anim_count;   // rotate next time
}

static int draw_anim(void)
{
    if (!storage_mounted()) {
        gfx_clear(C_BLACK);
        gfx_text(24, 30, "NO SD CARD", C_RED, 1);
        gfx_text(2,  42, storage_error(), C_DIM, 1);
        return 500;
    }
    if (!s_anim_open) {
        gfx_clear(C_BLACK);
        gfx_text(10, 26, "no .anim files in", C_GREY, 1);
        gfx_text(28, 38, "/sd/anim/", C_LIME, 1);
        gfx_text(4,  54, "run tools/mkanim.py", C_DIM, 1);
        return 500;
    }

    uint16_t delay = 60;
    if (!anim_draw_next(&s_anim, &delay)) {
        anim_close(&s_anim);
        s_anim_open = false;
        return 200;
    }
    return delay;
}

// -------------------------------------------------------------- worker ----
// Network work lives off the render task so a 4-second dual-band sweep never
// stalls the display.
static void worker_task(void *arg)
{
    int64_t last_gh = 0;
    bool    gh_first = true;

    while (1) {
        bool force = s_force_refresh;
        s_force_refresh = false;

        if (s_screen == SCR_WIFI &&
            (force || scanner_age_ms() > SCAN_INTERVAL_MS) && !scanner_busy()) {
            scanner_run();
        }

        int64_t now_ms = esp_timer_get_time() / 1000;
        bool gh_due = gh_first || force || (now_ms - last_gh) > GH_INTERVAL_MS;
        if (gh_due && s_screen == SCR_GITHUB) {
            net_status_t n; netmgr_get(&n);
            if (n.has_ip) {
                github_refresh();
                last_gh = now_ms;
                gh_first = false;
            }
        }

        netmgr_refresh_link();

        // LED mirrors link health at low brightness.
        net_status_t n; netmgr_get(&n);
        if (!n.connected)          led_set(60, 0, 0, 2);
        else if (!n.has_ip)        led_set(60, 40, 0, 2);
        else if (n.rtt_last > 150) led_set(60, 40, 0, 2);
        else                       led_set(0, 60, 20, 2);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// -------------------------------------------------------------- render ----
static void ui_task(void *arg)
{
    screen_t last = SCR_COUNT;

    while (1) {
        screen_t cur = s_screen;
        if (cur != last) {
            if (last == SCR_ANIM) anim_leave();
            if (cur == SCR_ANIM)  anim_enter();
            last = cur;
            s_screen_changed = false;
        }

        int delay_ms = 250;
        switch (cur) {
            case SCR_WIFI:     draw_wifi();   break;
            case SCR_GITHUB:   draw_github(); break;
            case SCR_CLOCK:    draw_clock();  break;
            case SCR_FSOCIETY: delay_ms = fsociety_draw(); break;
            case SCR_ANIM:     delay_ms = draw_anim(); break;
            default: break;
        }
        display_flush();

#ifdef CONFIG_FB_DUMP
        // Walk each screen once and dump it, then go quiet. Dumping forever
        // blocks the render task whenever the host stops draining the CDC.
        static int64_t next_dump = 22000000;   // let the first survey finish
        static uint8_t dumped = 0;
        if (dumped < SCR_COUNT * 3 && esp_timer_get_time() > next_dump) {
            fb_dump(SCR_NAME[cur]);
            dumped++;
            next_dump = esp_timer_get_time() + 9100000;   // avoid aliasing with the 48-frame loop
            s_screen = next_screen(cur);
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void ui_start(void)
{
    if (storage_mounted()) s_anim_count = anim_scan_dir(s_anim_files, ANIM_MAX_FILES);
    fsociety_init();

    xTaskCreate(button_task, "btn",    3072, NULL, 6, NULL);
    xTaskCreate(worker_task, "worker", 8192, NULL, 4, NULL);
    xTaskCreate(ui_task,     "ui",     6144, NULL, 5, NULL);
    ESP_LOGI(TAG, "ui started");
}
