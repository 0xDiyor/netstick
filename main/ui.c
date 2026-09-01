// Terminal-style UI for one button.
//
// Two gestures, used the same way everywhere:
//   TAP   next / advance   (move the cursor, next page, mark a spot)
//   HOLD  select / menu    (open the item under the cursor, or the context menu)
// The footer always says what the two gestures do right now, and fills up
// while the button is held so the hold never fires by surprise.
//
// Layout on the 160x80 panel with the 6x12 font (26 columns):
//   y  0  header: "> title_"            link chip on the right
//   y 13  separator
//   y 15  body row 0
//   y 27  body row 1
//   y 39  body row 2
//   y 51  body row 3
//   y 66  separator
//   y 68  footer: "tap:xxx  hold:yyy"
#include "ui.h"
#include "gfx.h"
#include "display.h"
#include "button.h"
#include "led.h"
#include "netmgr.h"
#include "storage.h"
#include "doctor.h"
#include "survey.h"
#include "provision.h"
#include "secrets.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ui";

#define ROW(i)   (15 + 12 * (i))
#define FOOT_Y   68
#define RIGHT    (LCD_W - 1)

// ------------------------------------------------------------- settings ---
static uint8_t s_brightness = 100;
static uint8_t s_saver_min = 0;        // auto screensaver after N idle minutes, 0 = off

void ui_set_brightness(uint8_t pct)
{
    s_brightness = pct;
    display_backlight(pct);
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "bl", pct); nvs_commit(h); nvs_close(h); }
}
uint8_t ui_brightness(void) { return s_brightness; }

static void settings_load(void)
{
    nvs_handle_t h;
    uint8_t v;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "bl", &v) == ESP_OK && v >= 5 && v <= 100) s_brightness = v;
        if (nvs_get_u8(h, "bands", &v) == ESP_OK) survey_set_both_bands(v != 0);
        if (nvs_get_u8(h, "saver", &v) == ESP_OK && v <= 10) s_saver_min = v;
        nvs_close(h);
    }
    display_backlight(s_brightness);
}

static void settings_save_saver(uint8_t minutes)
{
    s_saver_min = minutes;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "saver", minutes); nvs_commit(h); nvs_close(h); }
}

static void settings_save_bands(bool both)
{
    survey_set_both_bands(both);
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "bands", both); nvs_commit(h); nvs_close(h); }
}

// ------------------------------------------------------------- widgets ---
static uint32_t s_frame;
static const char SPIN[4] = { '|', '/', '-', '\\' };

static void link_chip(void)
{
    net_status_t n; netmgr_get(&n);
    char s[12]; uint16_t c;
    if (n.has_ip)          { snprintf(s, sizeof(s), "%s %d", n.band == 5 ? "5G" : "2G", n.rssi); c = n.band == 5 ? C_INFO : C_ALT; }
    else if (n.connected)  { snprintf(s, sizeof(s), "dhcp%c", SPIN[(s_frame / 3) & 3]); c = C_WARN; }
    else if (n.started && n.has_creds) { snprintf(s, sizeof(s), "join%c", SPIN[(s_frame / 3) & 3]); c = C_WARN; }
    else if (n.started)    { snprintf(s, sizeof(s), "no net"); c = C_FAIL; }
    else                   { snprintf(s, sizeof(s), "no wifi"); c = C_FAIL; }
    gfx_text_right(RIGHT, 0, s, c);
}

static void header(const char *title, bool chip)
{
    gfx_text(0, 0, ">", C_DIM);
    int w = gfx_text(CX(1), 0, title, C_BRIGHT);
    if ((s_frame / 6) & 1) gfx_text(CX(1) + w, 0, "_", C_FG);   // blinking cursor
    if (chip) link_chip();
    gfx_hline(0, 13, LCD_W, C_DIM);
}

static void footer(const char *tap, const char *hold)
{
    gfx_hline(0, 66, LCD_W, C_DIM);
    uint32_t held = button_held_ms();
    if (held) {
        int w = (int)((uint64_t)LCD_W * (held > BTN_HOLD_MS ? BTN_HOLD_MS : held) / BTN_HOLD_MS);
        gfx_fill_rect(0, FOOT_Y, w, 12, held >= BTN_HOLD_MS ? C_DIM : C_FAINT);
    }
    char s[27];
    snprintf(s, sizeof(s), "tap:%s", tap);
    gfx_text(0, FOOT_Y, s, C_DIM);
    snprintf(s, sizeof(s), "hold:%s", hold);
    gfx_text_right(RIGHT, FOOT_Y, s, C_DIM);
}

// A menu is a list of up to 4 rows; TAP moves, HOLD selects.
typedef struct { const char *item[6]; int n; int cur; } menu_t;

static void menu_draw(const char *title, const menu_t *m, const char *hold)
{
    gfx_clear(C_BG);
    header(title, true);
    int first = m->cur >= 4 ? m->cur - 3 : 0;
    for (int i = 0; i < 4 && first + i < m->n; i++) {
        int idx = first + i;
        char s[27];
        snprintf(s, sizeof(s), "%c %-24s", idx == m->cur ? '>' : ' ', m->item[idx]);
        if (idx == m->cur) gfx_text_bg(0, ROW(i), s, C_INV_FG, C_INV_BG);
        else gfx_text(0, ROW(i), s, C_FG);
    }
    footer("move", hold);
}

// Returns the selected index on HOLD, -1 otherwise.
static int menu_input(menu_t *m, btn_event_t e)
{
    if (e == BTN_TAP) m->cur = (m->cur + 1) % m->n;
    else if (e == BTN_HOLD) return m->cur;
    return -1;
}

static void progress_bar(int y, int num, int den)
{
    char s[27] = "[";
    int fill = den > 0 ? (24 * num) / den : 0;
    for (int i = 0; i < 24; i++) s[1 + i] = i < fill ? '#' : '.';
    s[25] = ']'; s[26] = '\0';
    gfx_text(0, y, s, C_FG);
}

static const char *rssi_word(int rssi)
{
    return rssi >= -55 ? "great" : rssi >= -65 ? "good" : rssi >= -72 ? "fair" : rssi >= -80 ? "weak" : "poor";
}
static uint16_t rssi_col(int rssi) { return rssi >= -65 ? C_OK : rssi >= -75 ? C_WARN : C_FAIL; }

static void rssi_bar(int y, int rssi)
{
    int bars = (rssi + 90) / 4;
    if (bars < 0) bars = 0;
    if (bars > 10) bars = 10;
    char s[16] = "[";
    for (int i = 0; i < 10; i++) s[1 + i] = i < bars ? '|' : '.';
    s[11] = ']'; s[12] = '\0';
    gfx_text(0, y, s, rssi_col(rssi));
    gfx_printf(CX(13), y, rssi_col(rssi), "%s", rssi_word(rssi));
}

static void fmt_mbps(char *out, size_t n, uint32_t x10)
{
    if (x10 == 0) snprintf(out, n, "-");
    else if (x10 >= 1000) snprintf(out, n, "%lu", (unsigned long)x10 / 10);
    else snprintf(out, n, "%lu.%lu", (unsigned long)x10 / 10, (unsigned long)x10 % 10);
}

// ------------------------------------------------------------- screens ---
typedef enum { SCR_HOME, SCR_DOCTOR, SCR_SURVEY, SCR_WIFI, SCR_STATUS, SCR_SETUP, SCR_SAVER } screen_t;
static screen_t s_scr = SCR_HOME;

// ---- home ---------------------------------------------------------------
static menu_t s_home = { { "doctor  run 11 checks", "survey  walk & measure",
                           "wifi    join a network", "status  link details",
                           "setup   brightness..", "saver   dedsec skull" }, 6, 0 };

static void home_draw(void)
{
    menu_draw("netstick", &s_home, "open");
}

// ---- doctor -------------------------------------------------------------
typedef enum { DV_LIST, DV_DETAIL, DV_MENU } doc_view_t;
static doc_view_t s_dv;
static int  s_doc_page, s_doc_detail;
static menu_t s_doc_menu = { { "details", "run again", "home" }, 3, 0 };

static void doc_enter(void)
{
    doctor_report_t r; doctor_get(&r);
    if (!r.done && !r.running) doctor_start();
    s_dv = DV_LIST; s_doc_page = 0; s_doc_detail = 0;
}

static void doc_row(int y, const doc_check_t *c, bool blink)
{
    const char *tag; uint16_t col;
    switch (c->state) {
        case DOC_PASS:    tag = "[ok]"; col = C_OK;   break;
        case DOC_WARN:    tag = "[!!]"; col = C_WARN; break;
        case DOC_FAIL:    tag = "[xx]"; col = C_FAIL; break;
        case DOC_SKIP:    tag = "[--]"; col = C_DIM;  break;
        case DOC_RUNNING: tag = "[  ]"; col = C_BRIGHT; break;
        default:          tag = "[  ]"; col = C_DIM;  break;
    }
    gfx_text(0, y, tag, col);
    if (c->state == DOC_RUNNING) { char sp[2] = { SPIN[(s_frame / 2) & 3], 0 }; gfx_text(CX(1), y, sp, C_BRIGHT); }
    gfx_printf(CX(5), y, c->state == DOC_PENDING ? C_DIM : C_FG, "%-6s", c->name);
    gfx_text(CX(11), y, c->summary, c->state == DOC_RUNNING ? C_DIM : col);
}

static void doc_draw(void)
{
    doctor_report_t r; doctor_get(&r);
    if (s_dv == DV_MENU) { menu_draw("doctor", &s_doc_menu, "select"); return; }

    gfx_clear(C_BG);
    if (s_dv == DV_DETAIL) {
        const doc_check_t *c = &r.checks[s_doc_detail];
        char t[27]; snprintf(t, sizeof(t), "doctor %d/%d %s", s_doc_detail + 1, DOC_N, c->name);
        header(t, false);
        const char *tag = c->state == DOC_PASS ? "[ok]" : c->state == DOC_WARN ? "[!!]" : c->state == DOC_FAIL ? "[xx]" : "[--]";
        uint16_t col = c->state == DOC_PASS ? C_OK : c->state == DOC_WARN ? C_WARN : c->state == DOC_FAIL ? C_FAIL : C_DIM;
        gfx_text_right(RIGHT, 0, tag, col);
        gfx_text(0, ROW(0), c->summary, col);
        for (int i = 0; i < 3; i++) gfx_text(0, ROW(1 + i), c->detail[i], i == 0 ? C_BRIGHT : C_FG);
        footer("next", "menu");
        return;
    }

    // List view: 4 rows per page, running check kept in view.
    char t[27];
    if (r.running) snprintf(t, sizeof(t), "doctor %d/%d", r.current < 0 ? 0 : r.current + 1, DOC_N);
    else if (r.n_fail) snprintf(t, sizeof(t), "doctor %d fail", r.n_fail);
    else if (r.n_warn) snprintf(t, sizeof(t), "doctor %d warn", r.n_warn);
    else snprintf(t, sizeof(t), "doctor all ok");
    header(t, r.running);
    if (!r.running) {
        const char *verdict = r.n_fail ? "FAIL" : r.n_warn ? "WARN" : "PASS";
        gfx_text_right(RIGHT, 0, verdict, r.n_fail ? C_FAIL : r.n_warn ? C_WARN : C_OK);
    }
    if (r.running && r.current >= 0) s_doc_page = r.current / 4;

    int first = s_doc_page * 4;
    for (int i = 0; i < 4; i++) {
        int idx = first + i;
        if (idx < DOC_N) doc_row(ROW(i), &r.checks[idx], true);
        else if (idx == DOC_N && !r.running) {
            gfx_printf(0, ROW(i), C_DIM, "%d ok %d warn %d fail %lus", r.n_pass, r.n_warn, r.n_fail,
                       (unsigned long)r.elapsed_ms / 1000);
        }
    }
    if (r.running) {
        // Activity line replaces the footer's tap hint while a check runs.
        char a[27]; snprintf(a, sizeof(a), "%c %.14s", SPIN[(s_frame / 2) & 3], r.activity);
        footer("", "menu");
        gfx_fill_rect(0, FOOT_Y, CX(16), 12, C_BG);
        gfx_text(0, FOOT_Y, a, C_DIM);
    } else footer("scroll", "menu");
}

static void doc_input(btn_event_t e)
{
    doctor_report_t r; doctor_get(&r);
    switch (s_dv) {
    case DV_LIST:
        if (e == BTN_TAP && !r.running) s_doc_page = (s_doc_page + 1) % 3;
        else if (e == BTN_HOLD) { s_dv = DV_MENU; s_doc_menu.cur = 0; }
        break;
    case DV_DETAIL:
        if (e == BTN_TAP) s_doc_detail = (s_doc_detail + 1) % DOC_N;
        else if (e == BTN_HOLD) { s_dv = DV_MENU; s_doc_menu.cur = 0; }
        break;
    case DV_MENU: {
        int sel = menu_input(&s_doc_menu, e);
        if (sel == 0) { s_dv = DV_DETAIL; s_doc_detail = 0; }
        else if (sel == 1) { if (!r.running) doctor_start(); s_dv = DV_LIST; s_doc_page = 0; }
        else if (sel == 2) s_scr = SCR_HOME;
        break; }
    }
}

// ---- survey -------------------------------------------------------------
typedef enum { SV_LIVE, SV_SAMPLING, SV_RESULT, SV_POINTS, SV_MENU } sv_view_t;
static sv_view_t s_sv;
static int s_sv_point;
static int64_t s_sv_t0;
static menu_t s_sv_menu;
static char s_sv_bands_item[24];

static void sv_enter(void)
{
    survey_begin();
    s_sv = SV_LIVE;
}

static void sv_build_menu(void)
{
    survey_status_t s; survey_get(&s);
    snprintf(s_sv_bands_item, sizeof(s_sv_bands_item), "bands: %s", s.both_bands ? "both" : "current only");
    int n = 0;
    s_sv_menu.item[n++] = "resume";
    s_sv_menu.item[n++] = "review points";
    s_sv_menu.item[n++] = s_sv_bands_item;
    s_sv_menu.item[n++] = "end session, home";
    s_sv_menu.n = n; s_sv_menu.cur = 0;
}

static void sv_draw_point(int idx, const survey_point_t *p, const char *hdr_extra)
{
    char t[27];
    snprintf(t, sizeof(t), "survey %s", p->label);
    header(t, false);
    gfx_text_right(RIGHT, 0, hdr_extra, C_DIM);
    gfx_text(0, ROW(0), "band", C_DIM);
    gfx_text(CX(5), ROW(0), "rssi", C_DIM);
    gfx_text(CX(9), ROW(0), " ping", C_DIM);
    gfx_text(CX(15), ROW(0), " down", C_DIM);
    gfx_text(CX(21), ROW(0), "   up", C_DIM);
    for (int b = 0; b < 2; b++) {
        const survey_band_t *x = &p->band[b];
        int y = ROW(1 + b);
        uint16_t bc = b ? C_INFO : C_ALT;
        gfx_text(0, y, b ? "5G" : "2.4G", bc);
        if (!x->valid) {
            gfx_text(CX(5), y, x->note[0] ? x->note : "not measured", C_DIM);
            if (x->seen) gfx_printf(CX(19), y, C_DIM, "scan%d", x->scan_rssi);
            continue;
        }
        char dl[8], ul[8];
        fmt_mbps(dl, sizeof(dl), x->dl_x10); fmt_mbps(ul, sizeof(ul), x->ul_x10);
        gfx_printf(CX(5), y, rssi_col(x->rssi), "%d", x->rssi);
        gfx_printf(CX(9), y, x->loss_pct ? C_WARN : C_FG, "%3ums", x->ping_avg > 999 ? 999 : x->ping_avg);
        gfx_printf(CX(15), y, C_BRIGHT, "%5s", dl);
        gfx_printf(CX(21), y, C_BRIGHT, "%5s", ul);
    }
    survey_band_t *best = NULL;
    for (int b = 0; b < 2; b++) if (p->band[b].valid && (!best || p->band[b].dl_x10 > best->dl_x10)) best = (survey_band_t *)&p->band[b];
    if (best) gfx_printf(0, ROW(3), C_DIM, "best %s ch%u %s %s", best == &p->band[1] ? "5G" : "2.4G", best->channel, best->phy,
                         best->src[0] ? best->src : "");
    else gfx_text(0, ROW(3), "nothing measured", C_DIM);
}

static void sv_draw(void)
{
    survey_status_t s; survey_get(&s);
    if (s.sampling && s_sv != SV_SAMPLING) { s_sv = SV_SAMPLING; }
    if (!s.sampling && s_sv == SV_SAMPLING) { s_sv = SV_RESULT; s_sv_point = s.n_points - 1; }

    if (s_sv == SV_MENU) { menu_draw("survey", &s_sv_menu, "select"); return; }
    gfx_clear(C_BG);

    if (s_sv == SV_SAMPLING) {
        char t[27]; snprintf(t, sizeof(t), "survey %s", s.next_label);
        header(t, true);
        gfx_printf(0, ROW(0), C_BRIGHT, "measuring %.12s", s.next_label);
        gfx_printf_f(&font_6x12, CX(20), ROW(0), C_DIM, "[%d/%d]", s.step, s.steps);
        gfx_text(0, ROW(1), s.activity, C_FG);
        progress_bar(ROW(2), s.step, s.steps);
        gfx_printf(0, ROW(3), C_DIM, "%c %lus  %s", SPIN[(s_frame / 2) & 3],
                   (unsigned long)((esp_timer_get_time() - s_sv_t0) / 1000000),
                   s.server_found ? "lan server" : "no lan server");
        footer("-", "cancel");
        return;
    }
    if (s_sv == SV_RESULT || s_sv == SV_POINTS) {
        survey_point_t p;
        if (survey_point(s_sv_point, &p)) {
            char x[12]; snprintf(x, sizeof(x), "%d/%d", s_sv_point + 1, s.n_points);
            sv_draw_point(s_sv_point, &p, x);
        } else { header("survey", true); gfx_text(0, ROW(1), "no points yet", C_DIM); }
        footer(s_sv == SV_RESULT ? "continue" : "next point", "menu");
        return;
    }

    // Live view: big RSSI, band details, ready-for-next-point line.
    net_status_t n; netmgr_get(&n);
    char t[27]; snprintf(t, sizeof(t), "survey %d pt%s", s.n_points, s.n_points == 1 ? "" : "s");
    header(t, true);
    if (n.connected) {
        gfx_printf_f(&font_12x24, 0, ROW(0), rssi_col(n.rssi), "%d", n.rssi);
        int x = n.rssi <= -100 ? 48 : 40;
        gfx_printf(x, ROW(0), C_DIM, "dBm %s ch%u", n.band == 5 ? "5GHz" : "2.4GHz", n.channel);
        gfx_printf(x, ROW(1), C_FG, "%s %uMHz %s", n.phy, n.bw_mhz, n.ax ? "ax" : "");
        rssi_bar(ROW(2), n.rssi);
    } else {
        gfx_text(0, ROW(0), "no link", C_FAIL);
        if (n.has_creds) {
            gfx_printf(0, ROW(1), C_DIM, "joining %.19s", netmgr_cfg_ssid());
            gfx_text(0, ROW(2), n.reason_str[0] ? n.reason_str : "", C_DIM);
        } else {
            gfx_text(0, ROW(1), "no network saved", C_DIM);
            gfx_text(0, ROW(2), "home > wifi to set one", C_DIM);
        }
    }
    gfx_printf(0, ROW(3), C_BRIGHT, "next: %-12s", s.next_label);
    gfx_text_right(RIGHT, ROW(3), s.file[0] ? "sd" : "no sd", s.file[0] ? C_DIM : C_WARN);
    footer("mark spot", "menu");
}

static void sv_input(btn_event_t e)
{
    survey_status_t s; survey_get(&s);
    switch (s_sv) {
    case SV_LIVE:
        if (e == BTN_TAP) { if (survey_sample()) { s_sv = SV_SAMPLING; s_sv_t0 = esp_timer_get_time(); } }
        else if (e == BTN_HOLD) { sv_build_menu(); s_sv = SV_MENU; }
        break;
    case SV_SAMPLING:
        if (e == BTN_HOLD) survey_abort();
        break;
    case SV_RESULT:
        if (e == BTN_TAP) s_sv = SV_LIVE;
        else if (e == BTN_HOLD) { sv_build_menu(); s_sv = SV_MENU; }
        break;
    case SV_POINTS:
        if (e == BTN_TAP) { if (s.n_points) s_sv_point = (s_sv_point + 1) % s.n_points; }
        else if (e == BTN_HOLD) { sv_build_menu(); s_sv = SV_MENU; }
        break;
    case SV_MENU: {
        int sel = menu_input(&s_sv_menu, e);
        if (sel == 0) s_sv = SV_LIVE;
        else if (sel == 1) { s_sv = SV_POINTS; s_sv_point = 0; }
        else if (sel == 2) { settings_save_bands(!s.both_bands); sv_build_menu(); s_sv_menu.cur = 2; }
        else if (sel == 3) { survey_end(); s_scr = SCR_HOME; }
        break; }
    }
}

// ---- wifi ---------------------------------------------------------------
typedef enum { WV_MENU, WV_SETUP } wifi_view_t;
static wifi_view_t s_wv;
static menu_t s_wifi_menu;
static char s_wifi_item0[28];

static void wifi_build_menu(void)
{
    net_status_t n; netmgr_get(&n);
    if (n.has_creds) snprintf(s_wifi_item0, sizeof(s_wifi_item0), "rejoin %.17s", netmgr_cfg_ssid());
    else snprintf(s_wifi_item0, sizeof(s_wifi_item0), "no network saved");
    s_wifi_menu.item[0] = "setup via phone";
    s_wifi_menu.item[1] = s_wifi_item0;
    s_wifi_menu.item[2] = "forget network";
    s_wifi_menu.item[3] = "home";
    s_wifi_menu.n = 4;
}

static void wifi_enter(void)
{
    wifi_build_menu();
    s_wifi_menu.cur = 0;
    s_wv = WV_MENU;
}

static void wifi_draw(void)
{
    if (s_wv == WV_MENU) { wifi_build_menu(); menu_draw("wifi", &s_wifi_menu, "select"); return; }
    prov_status_t p; prov_get(&p);
    gfx_clear(C_BG);
    header("wifi setup", false);
    char sp[2] = { SPIN[(s_frame / 2) & 3], 0 };
    switch (p.state) {
    case PROV_SCANNING:
        gfx_text_right(RIGHT, 0, sp, C_WARN);
        gfx_text(0, ROW(1), "scanning networks...", C_FG);
        break;
    case PROV_WAITING:
        gfx_text_right(RIGHT, 0, p.clients ? "phone on" : "AP up", p.clients ? C_OK : C_INFO);
        gfx_text(0, ROW(0), "on your phone, join wifi", C_DIM);
        gfx_printf(0, ROW(1), C_BRIGHT, "  %s", p.ap_ssid);
        gfx_text(0, ROW(2), "then open (if no popup)", C_DIM);
        gfx_printf(0, ROW(3), C_BRIGHT, "  http://%s", p.ap_ip);
        break;
    case PROV_JOINING:
        gfx_text_right(RIGHT, 0, sp, C_WARN);
        gfx_printf(0, ROW(0), C_FG, "saved %.19s", p.chosen);
        gfx_text(0, ROW(1), "joining...", C_BRIGHT);
        break;
    case PROV_JOINED:
        gfx_text_right(RIGHT, 0, "[ok]", C_OK);
        gfx_printf(0, ROW(0), C_BRIGHT, "joined %.19s", p.chosen);
        gfx_text(0, ROW(1), p.msg, C_OK);
        gfx_text(0, ROW(2), "saved for next time", C_DIM);
        break;
    case PROV_FAILED:
        gfx_text_right(RIGHT, 0, "[xx]", C_FAIL);
        gfx_printf(0, ROW(0), C_BRIGHT, "%.26s", p.chosen);
        gfx_text(0, ROW(1), p.msg, C_FAIL);
        gfx_text(0, ROW(2), "wrong password? try again", C_DIM);
        break;
    default:
        gfx_text(0, ROW(1), "starting...", C_DIM);
        break;
    }
    footer("-", p.state == PROV_JOINED || p.state == PROV_FAILED ? "done" : "cancel");
}

static void wifi_input(btn_event_t e)
{
    if (s_wv == WV_SETUP) {
        if (e == BTN_HOLD) { prov_stop(); s_wv = WV_MENU; s_wifi_menu.cur = 0; }
        return;
    }
    int sel = menu_input(&s_wifi_menu, e);
    if (sel == 0) { s_wv = WV_SETUP; prov_start(); }
    else if (sel == 1) { if (netmgr_has_creds()) netmgr_connect_auto(true); }
    else if (sel == 2) { netmgr_forget(); wifi_build_menu(); }
    else if (sel == 3) s_scr = SCR_HOME;
}

// ---- status -------------------------------------------------------------
static int s_st_page;
static menu_t s_st_menu = { { "back", "rejoin wifi", "home" }, 3, 0 };
static bool s_st_in_menu;

static void st_draw(void)
{
    if (s_st_in_menu) { menu_draw("status", &s_st_menu, "select"); return; }
    net_status_t n; netmgr_get(&n);
    gfx_clear(C_BG);
    char t[27]; snprintf(t, sizeof(t), "status %d/3", s_st_page + 1);
    header(t, true);
    if (s_st_page == 0) {
        if (!n.connected) {
            gfx_text(0, ROW(0), n.has_creds ? "not joined" : "no network saved", C_FAIL);
            gfx_printf(0, ROW(1), C_DIM, "ssid %.20s", n.has_creds ? netmgr_cfg_ssid() : "-");
            gfx_printf(0, ROW(2), C_DIM, "last: %s", n.reason_str[0] ? n.reason_str : "-");
            gfx_printf(0, ROW(3), C_DIM, "drops %lu", (unsigned long)n.disconnects);
        } else {
            gfx_printf(0, ROW(0), C_BRIGHT, "%.26s", n.ssid);
            gfx_printf(0, ROW(1), C_FG, "%02X:%02X:%02X:%02X:%02X:%02X", n.bssid[0], n.bssid[1], n.bssid[2], n.bssid[3], n.bssid[4], n.bssid[5]);
            gfx_printf(0, ROW(2), n.band == 5 ? C_INFO : C_ALT, "%s ch%u %s %uMHz%s", n.band == 5 ? "5GHz" : "2.4GHz", n.channel, n.phy, n.bw_mhz, n.ax ? " ax" : "");
            rssi_bar(ROW(3), n.rssi);
            gfx_printf(CX(19), ROW(3), rssi_col(n.rssi), "%ddBm", n.rssi);
        }
    } else if (s_st_page == 1) {
        gfx_printf(0, ROW(0), C_BRIGHT, "ip  %s", n.has_ip ? n.ip : "-");
        gfx_printf(0, ROW(1), C_FG, "gw  %s", n.has_ip ? n.gw : "-");
        gfx_printf(0, ROW(2), C_FG, "dns %s", n.dns[0][0] ? n.dns[0] : "-");
        if (n.ip6_global[0]) gfx_printf(0, ROW(3), C_INFO, "v6  %.22s", n.ip6_global);
        else gfx_printf(0, ROW(3), C_DIM, "v6  %s", n.ip6_ll[0] ? "link-local only" : "none");
    } else {
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
        gfx_printf(0, ROW(0), C_FG, "up %luh%02lum  drops %lu", (unsigned long)up / 3600, (unsigned long)(up / 60) % 60, (unsigned long)n.disconnects);
        if (storage_mounted()) gfx_printf(0, ROW(1), C_FG, "sd %lluGB free %lluGB", storage_total_mb() / 1024, storage_free_mb() / 1024);
        else gfx_printf(0, ROW(1), C_WARN, "sd: %s", storage_error());
        gfx_printf(0, ROW(2), C_FG, "heap %luk psram %luk", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
                   (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
        gfx_printf(0, ROW(3), C_DIM, "time %s  dhcp %lums", n.time_synced ? "ntp" : "unset", (unsigned long)n.dhcp_ms);
    }
    footer("next page", "menu");
}

static void st_input(btn_event_t e)
{
    if (s_st_in_menu) {
        int sel = menu_input(&s_st_menu, e);
        if (sel == 0) s_st_in_menu = false;
        else if (sel == 1) { netmgr_connect_auto(true); s_st_in_menu = false; }
        else if (sel == 2) { s_st_in_menu = false; s_scr = SCR_HOME; }
        return;
    }
    if (e == BTN_TAP) s_st_page = (s_st_page + 1) % 3;
    else if (e == BTN_HOLD) { s_st_in_menu = true; s_st_menu.cur = 0; }
}

// ---- setup --------------------------------------------------------------
static menu_t s_setup = { { 0 }, 4, 0 };
static char s_setup_bl[24], s_setup_bands[24], s_setup_saver[24];

static void setup_refresh(void)
{
    snprintf(s_setup_bl, sizeof(s_setup_bl), "brightness: %u%%", s_brightness);
    snprintf(s_setup_bands, sizeof(s_setup_bands), "survey bands: %s", survey_both_bands() ? "both" : "current");
    if (s_saver_min) snprintf(s_setup_saver, sizeof(s_setup_saver), "auto saver: %u min", s_saver_min);
    else snprintf(s_setup_saver, sizeof(s_setup_saver), "auto saver: off");
    s_setup.item[0] = s_setup_bl;
    s_setup.item[1] = s_setup_bands;
    s_setup.item[2] = s_setup_saver;
    s_setup.item[3] = "home";
    s_setup.n = 4;
}

static void setup_draw(void) { setup_refresh(); menu_draw("setup", &s_setup, "select"); }

static void setup_input(btn_event_t e)
{
    int sel = menu_input(&s_setup, e);
    if (sel == 0) ui_set_brightness(s_brightness >= 100 ? 50 : s_brightness >= 50 ? 15 : 100);
    else if (sel == 1) settings_save_bands(!survey_both_bands());
    else if (sel == 2) settings_save_saver(s_saver_min == 0 ? 2 : s_saver_min == 2 ? 5 : s_saver_min == 5 ? 10 : 0);
    else if (sel == 3) s_scr = SCR_HOME;
}

// ---- screensaver: DedSec skull over random text ---------------------------
// 5x8 font: 32 columns x 10 rows fill the panel exactly. The skull is a pixel
// bitmap drawn with characters; the background is text noise that keeps
// mutating. Every couple of seconds a few rows tear sideways in red / cyan.
#define SV_COLS 32
#define SV_ROWS 10
#define SKULL_W 15
static const char *SKULL[SV_ROWS] = {
    "....#######....",
    "..###########..",
    ".#############.",
    ".#############.",
    ".##...###...##.",
    ".##...###...##.",
    ".######.######.",
    "..####...####..",
    "...#.#.#.#.#...",
    "....#######....",
};
static char    s_bg[SV_ROWS][SV_COLS];
static int64_t s_saver_t0;
static uint32_t s_glitch_until;          // frame number
static int8_t   s_row_off[SV_ROWS];
static uint16_t s_row_col[SV_ROWS];

static char noise_char(void)
{
    static const char set[] = "0011ABCDEF#$%&*+-<>/\\|=?~x:;          ";   // spaces thin the field
    return set[esp_random() % (sizeof(set) - 1)];
}

static void saver_enter(void)
{
    for (int r = 0; r < SV_ROWS; r++) for (int c = 0; c < SV_COLS; c++) s_bg[r][c] = noise_char();
    s_saver_t0 = esp_timer_get_time();
    s_glitch_until = 0;
}

static void saver_draw(void)
{
    gfx_clear(C_BG);
    // Mutate a few dozen cells per frame so the text field crawls.
    for (int i = 0; i < 48; i++) s_bg[esp_random() % SV_ROWS][esp_random() % SV_COLS] = noise_char();
    for (int r = 0; r < SV_ROWS; r++) {
        for (int c = 0; c < SV_COLS; c++) {
            char s[2] = { s_bg[r][c], 0 };
            bool hot = ((r * 7 + c * 13 + s_frame / 3) % 29) == 0;      // sparse bright cells
            gfx_text_f(&font_5x8, c * 5, r * 8, s, hot ? C_FG : C_FAINT);
        }
    }
    // Glitch scheduler: ~3% chance per frame to tear for 3-7 frames.
    if (s_frame >= s_glitch_until) {
        for (int r = 0; r < SV_ROWS; r++) { s_row_off[r] = 0; s_row_col[r] = C_BRIGHT; }
        if (esp_random() % 100 < 3) {
            s_glitch_until = s_frame + 3 + esp_random() % 5;
            int n = 2 + esp_random() % 3;
            for (int i = 0; i < n; i++) {
                int r = esp_random() % SV_ROWS;
                s_row_off[r] = (int8_t)((esp_random() % 7) - 3);
                s_row_col[r] = (esp_random() & 1) ? C_FAIL : C_INFO;
            }
        }
    } else if ((s_frame & 1) == 0) {
        // re-roll offsets mid-glitch so it jitters rather than slides
        for (int r = 0; r < SV_ROWS; r++) if (s_row_off[r]) s_row_off[r] = (int8_t)((esp_random() % 7) - 3);
    }
    bool blink = ((s_frame / 4) % 23) == 0;                // eye sockets flash shut now and then
    static const char skin[] = "#%@&8";
    int x0 = (SV_COLS - SKULL_W) / 2;
    for (int r = 0; r < SV_ROWS; r++) {
        for (int c = 0; c < SKULL_W; c++) {
            bool on = SKULL[r][c] == '#';
            bool eye = (r == 4 || r == 5) && ((c >= 3 && c <= 5) || (c >= 9 && c <= 11));
            if (eye && blink) on = true;
            if (!on) continue;
            int col = x0 + c + s_row_off[r];
            if (col < 0 || col >= SV_COLS) continue;
            char s[2] = { skin[(r * 3 + c + s_frame / 6) % (sizeof(skin) - 1)], 0 };
            uint16_t colr = s_row_col[r];
            if (eye && blink) colr = C_FAIL;
            gfx_text_fbg(&font_5x8, col * 5, r * 8, s, colr, C_BG);
        }
    }
    // Ephemeral hint so the one-button rule still holds.
    if (esp_timer_get_time() - s_saver_t0 < 2500000) gfx_text_fbg(&font_5x8, LCD_W - 5 * 11, 0, "press:home", C_DIM, C_BG);
}

// ----------------------------------------------------------------- led ---
static void led_update(void)
{
    net_status_t n; netmgr_get(&n);
    if (s_scr == SCR_DOCTOR) {
        doctor_report_t r; doctor_get(&r);
        if (r.running) { int p = (s_frame / 2) & 15; led_set(0, 0, 255, p < 8 ? p : 15 - p); return; }
        if (r.done) { if (r.n_fail) led_set(255, 0, 0, 4); else if (r.n_warn) led_set(255, 120, 0, 4); else led_set(0, 255, 0, 3); return; }
    }
    if (s_scr == SCR_SAVER) { int p = (s_frame / 3) & 15; led_set(0, 255, 60, p < 8 ? p / 2 : (15 - p) / 2); return; }
    if (s_scr == SCR_SURVEY) {
        survey_status_t s; survey_get(&s);
        if (s.sampling) { int p = (s_frame / 2) & 15; led_set(0, 120, 255, p < 8 ? p : 15 - p); return; }
    }
    if (n.has_ip) led_set(0, 255, 60, 1);
    else if (n.has_creds && n.started) led_set(255, 120, 0, 2);
    else led_set(255, 0, 0, 2);
}

// -------------------------------------------------------------- render ---
#ifdef CONFIG_FB_DUMP
#include "mbedtls/base64.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
static volatile bool s_dump_req;

// Base64 the framebuffer to the console so tools/fbview.py can render it.
static void fb_dump(void)
{
    const size_t raw = (size_t)LCD_W * LCD_H * 2;
    size_t cap = ((raw + 2) / 3) * 4 + 8, olen = 0;
    unsigned char *b64 = malloc(cap);
    if (!b64) return;
    esp_log_level_set("*", ESP_LOG_NONE);
    if (mbedtls_base64_encode(b64, cap, &olen, (const unsigned char *)g_fb, raw) == 0) {
        printf("\n<<<FB frame %d %d\n", LCD_W, LCD_H);
        fflush(stdout);
        for (size_t i = 0; i < olen; i += 512) {
            size_t n = (olen - i) < 512 ? (olen - i) : 512;
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

// Drive the UI from the host: 't' tap, 'h' hold, 'd' dump the current frame.
static void console_task(void *arg)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();
    while (1) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100)) == 1) {
            if (c == 't') button_inject(BTN_TAP);
            else if (c == 'h') button_inject(BTN_HOLD);
            else if (c == 'd') s_dump_req = true;
        }
    }
}
#endif

static void render_task(void *arg)
{
    int64_t last_input = esp_timer_get_time();
    while (1) {
        btn_event_t e;
        while ((e = button_poll()) != BTN_NONE) {
            last_input = esp_timer_get_time();
            switch (s_scr) {
            case SCR_HOME: {
                int sel = menu_input(&s_home, e);
                if (sel == 0) { s_scr = SCR_DOCTOR; doc_enter(); }
                else if (sel == 1) { s_scr = SCR_SURVEY; sv_enter(); }
                else if (sel == 2) { s_scr = SCR_WIFI; wifi_enter(); }
                else if (sel == 3) { s_scr = SCR_STATUS; s_st_page = 0; s_st_in_menu = false; }
                else if (sel == 4) { s_scr = SCR_SETUP; s_setup.cur = 0; }
                else if (sel == 5) { s_scr = SCR_SAVER; saver_enter(); }
                break; }
            case SCR_DOCTOR: doc_input(e); break;
            case SCR_SURVEY: sv_input(e); break;
            case SCR_WIFI:   wifi_input(e); break;
            case SCR_STATUS: st_input(e); break;
            case SCR_SETUP:  setup_input(e); break;
            case SCR_SAVER:  s_scr = SCR_HOME; break;      // any press wakes
            }
        }
        if (s_scr == SCR_HOME && s_saver_min && esp_timer_get_time() - last_input > (int64_t)s_saver_min * 60000000LL) {
            s_scr = SCR_SAVER; saver_enter(); last_input = esp_timer_get_time();
        }
        switch (s_scr) {
            case SCR_HOME:   home_draw();  break;
            case SCR_DOCTOR: doc_draw();   break;
            case SCR_SURVEY: sv_draw();    break;
            case SCR_WIFI:   wifi_draw();  break;
            case SCR_STATUS: st_draw();    break;
            case SCR_SETUP:  setup_draw(); break;
            case SCR_SAVER:  saver_draw(); break;
        }
        display_flush();
        led_update();
        s_frame++;
#ifdef CONFIG_FB_DUMP
        if (s_dump_req) { s_dump_req = false; fb_dump(); }
#endif
        vTaskDelay(pdMS_TO_TICKS(66));
    }
}

void ui_start(void)
{
    settings_load();
    if (!netmgr_has_creds()) s_home.cur = 2;
    button_init();
#ifdef CONFIG_FB_DUMP
    xTaskCreate(console_task, "con", 3072, NULL, 3, NULL);
#endif
    xTaskCreatePinnedToCore(render_task, "render", 6144, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "ui up");
}
