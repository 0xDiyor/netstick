// Host-side renderer: compiles the real gfx + ui + boot code against fake
// drivers and writes each screen state as raw RGB565 for tools/fbview.py.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"

int64_t sim_now_us = 0;
uint16_t *g_fb;
static char s_scene[64];
static int s_scene_n;

esp_err_t board_spi_init(void) { return ESP_OK; }
esp_err_t display_init(void) { g_fb = calloc(LCD_W * LCD_H, 2); return ESP_OK; }
void display_backlight(uint8_t p) { (void)p; }
uint8_t display_backlight_get(void) { return 100; }
void display_flush(void)
{
    char path[128];
    snprintf(path, sizeof(path), "sim/out/%02d-%s.raw", s_scene_n, s_scene);
    FILE *f = fopen(path, "wb");
    // fbview expects big-endian RGB565 (what the panel gets) - the fb already is.
    fwrite(g_fb, 2, LCD_W * LCD_H, f);
    fclose(f);
}
static void scene(const char *name) { snprintf(s_scene, sizeof(s_scene), "%s", name); s_scene_n++; }

// ---- fake drivers -------------------------------------------------------
#include "button.h"
static uint32_t sim_held;
void button_init(void) {}
btn_event_t button_poll(void) { return BTN_NONE; }
uint32_t button_held_ms(void) { return sim_held; }
void button_inject(btn_event_t e) { (void)e; }
#include "led.h"
void led_init(void) {}
void led_set(uint8_t r, uint8_t g, uint8_t b, uint8_t br) {}
void led_off(void) {}
#include "storage.h"
static bool sim_sd = true;
esp_err_t storage_mount(void) { return sim_sd ? ESP_OK : ESP_FAIL; }
bool storage_mounted(void) { return sim_sd; }
const char *storage_error(void) { return "unreadable FS - format FAT32?"; }
uint64_t storage_total_mb(void) { return 59 * 1024; }
uint64_t storage_free_mb(void) { return 58 * 1024; }
#include "netmgr.h"
static net_status_t sim_net;
esp_err_t netmgr_init(void) { return ESP_OK; }
void netmgr_get(net_status_t *o) { *o = sim_net; }
bool netmgr_wait_ip(uint32_t t) { return sim_net.has_ip; }
void netmgr_refresh_link(void) {}
void netmgr_connect_auto(bool r) {}
static char sim_cfg_ssid[33] = "Rudov-Home-5G";
bool netmgr_has_creds(void) { return sim_cfg_ssid[0] != 0; }
const char *netmgr_cfg_ssid(void) { return sim_cfg_ssid; }
void netmgr_set_creds(const char *s, const char *p) {}
void netmgr_forget(void) { sim_cfg_ssid[0] = 0; }
#include "provision.h"
static prov_status_t sim_prov;
void prov_start(void) {}
void prov_stop(void) {}
void prov_get(prov_status_t *o) { *o = sim_prov; }
#include "doctor.h"
static doctor_report_t sim_doc;
void doctor_start(void) {}
void doctor_get(doctor_report_t *o) { *o = sim_doc; }
bool doctor_busy(void) { return sim_doc.running; }
#include "survey.h"
static survey_status_t sim_sv;
static survey_point_t sim_pt;
static bool sim_both = true;
void survey_init(void) {}
void survey_set_both_bands(bool b) { sim_both = b; }
bool survey_both_bands(void) { return sim_both; }
bool survey_begin(void) { sim_sv.active = true; return true; }
void survey_end(void) { sim_sv.active = false; }
bool survey_sample(void) { return true; }
void survey_abort(void) {}
void survey_get(survey_status_t *o) { *o = sim_sv; }
bool survey_point(int i, survey_point_t *o) { if (i < 0 || i >= sim_sv.n_points) return false; *o = sim_pt; return true; }

// Pull the real UI and boot code in so their static functions are reachable.
#include "ui.c"
#define TAG TAG_main
#define app_main app_main_unused
#include "app_main.c"

static void net_connected(void)
{
    memset(&sim_net, 0, sizeof(sim_net));
    sim_net.started = sim_net.connected = sim_net.has_ip = sim_net.time_synced = sim_net.has_creds = true;
    strcpy(sim_net.ssid, "Rudov-Home-5G");
    uint8_t b[6] = { 0xA4, 0x2B, 0x8C, 0x11, 0xD2, 0x7F }; memcpy(sim_net.bssid, b, 6);
    sim_net.rssi = -58; sim_net.channel = 44; sim_net.band = 5; sim_net.bw_mhz = 40; sim_net.ax = true;
    strcpy(sim_net.phy, "HE20");
    strcpy(sim_net.ip, "10.0.0.42"); strcpy(sim_net.mask, "255.255.255.0"); strcpy(sim_net.gw, "10.0.0.1");
    strcpy(sim_net.dns[0], "10.0.0.1");
    strcpy(sim_net.ip6_global, "2601:280:c000:1a::9f2");
    sim_net.dhcp_ms = 1240; sim_net.disconnects = 2;
}
static void net_joining(void)
{
    memset(&sim_net, 0, sizeof(sim_net));
    sim_net.started = sim_net.has_creds = true; sim_net.last_reason = 201; strcpy(sim_net.reason_str, "no AP found");
}

static void doc_fill(bool running)
{
    memset(&sim_doc, 0, sizeof(sim_doc));
    static const char *names[DOC_N] = { "LINK", "DHCP", "GW", "DNS", "WAN", "PORTL", "TLS", "IPV6", "NAT", "TIME", "SPEED" };
    static const char *sum[DOC_N] = { "-58dBm 5G ch44", "10.0.0.42", "3ms 0% loss", "nxdomain hijack", "14ms 0% loss",
                                      "none 212ms", "ok 640ms", "ok 21ms", "ok, 2 hops", "synced", "46.3 Mbps" };
    static const doc_state_t st[DOC_N] = { DOC_PASS, DOC_PASS, DOC_PASS, DOC_WARN, DOC_PASS, DOC_PASS, DOC_PASS, DOC_PASS, DOC_PASS, DOC_PASS, DOC_PASS };
    for (int i = 0; i < DOC_N; i++) {
        strcpy(sim_doc.checks[i].name, names[i]);
        strcpy(sim_doc.checks[i].summary, sum[i]);
        sim_doc.checks[i].state = st[i];
    }
    strcpy(sim_doc.checks[DOC_DNS].detail[0], "resolver 10.0.0.1");
    strcpy(sim_doc.checks[DOC_DNS].detail[1], "3/3 ok, avg 38ms");
    strcpy(sim_doc.checks[DOC_DNS].detail[2], "bogus name -> 198.51.100.7");
    if (running) {
        sim_doc.running = true; sim_doc.current = 2;
        sim_doc.checks[2].state = DOC_RUNNING; sim_doc.checks[2].summary[0] = 0;
        for (int i = 3; i < DOC_N; i++) { sim_doc.checks[i].state = DOC_PENDING; sim_doc.checks[i].summary[0] = 0; }
        strcpy(sim_doc.activity, "ping gateway");
    } else {
        sim_doc.done = true; sim_doc.current = -1; sim_doc.n_pass = 10; sim_doc.n_warn = 1; sim_doc.elapsed_ms = 23400;
    }
}

static void sv_fill(void)
{
    memset(&sim_sv, 0, sizeof(sim_sv));
    sim_sv.active = true; sim_sv.n_points = 3; sim_sv.both_bands = true; sim_sv.server_found = true;
    strcpy(sim_sv.server, "10.0.0.5"); strcpy(sim_sv.file, "/sd/survey/x.csv"); strcpy(sim_sv.next_label, "kitchen");
    memset(&sim_pt, 0, sizeof(sim_pt));
    strcpy(sim_pt.label, "kitchen");
    sim_pt.band[0] = (survey_band_t){ .valid = true, .seen = true, .channel = 6, .rssi = -63, .ping_avg = 4, .ping_max = 9, .dl_x10 = 382, .ul_x10 = 210, .src = "lan", .phy = "HT40", .bw_mhz = 40 };
    sim_pt.band[1] = (survey_band_t){ .valid = true, .seen = true, .channel = 44, .rssi = -58, .ping_avg = 3, .ping_max = 6, .dl_x10 = 1125, .ul_x10 = 641, .src = "lan", .phy = "HE20", .bw_mhz = 20 };
}

int main(void)
{
    display_init();
    s_frame = 7;   // cursor visible

    // Boot screen exactly as app_main draws it.
    scene("boot"); net_joining();
    boot_logo();
    boot_say("[ok]", "sd 59GB, 58GB free", C_OK);
    boot_say("[..]", "wifi: joining Rudov-Home-5G", C_DIM);
    boot_say("[..]", "no ip yet, keeps trying", C_WARN);

    net_connected();
    scene("home"); s_scr = SCR_HOME; home_draw(); display_flush();
    scene("home-cursor-survey"); s_home.cur = 1; home_draw(); display_flush();
    scene("home-hold-progress"); sim_held = 320; home_draw(); display_flush(); sim_held = 0;
    scene("home-joining"); net_joining(); s_home.cur = 0; home_draw(); display_flush(); net_connected();

    doc_fill(true);
    scene("doctor-running"); s_scr = SCR_DOCTOR; s_dv = DV_LIST; doc_draw(); display_flush();
    doc_fill(false);
    scene("doctor-results-p1"); s_doc_page = 0; doc_draw(); display_flush();
    scene("doctor-results-p3"); s_doc_page = 2; doc_draw(); display_flush();
    scene("doctor-detail-dns"); s_dv = DV_DETAIL; s_doc_detail = DOC_DNS; doc_draw(); display_flush();
    scene("doctor-menu"); s_dv = DV_MENU; s_doc_menu.cur = 1; doc_draw(); display_flush();
    sim_doc.checks[DOC_LINK].state = DOC_FAIL; strcpy(sim_doc.checks[DOC_LINK].summary, "no AP found");
    for (int i = 1; i < DOC_N; i++) { sim_doc.checks[i].state = DOC_SKIP; strcpy(sim_doc.checks[i].summary, "no link"); }
    sim_doc.n_pass = 0; sim_doc.n_warn = 0; sim_doc.n_fail = 1;
    scene("doctor-nolink"); s_dv = DV_LIST; s_doc_page = 0; net_joining(); doc_draw(); display_flush(); net_connected();

    sv_fill();
    scene("survey-live"); s_scr = SCR_SURVEY; s_sv = SV_LIVE; sv_draw(); display_flush();
    scene("survey-live-weak"); sim_net.rssi = -79; sim_net.band = 2; sim_net.channel = 6; strcpy(sim_net.phy, "HT20"); sim_net.bw_mhz = 20; sv_draw(); display_flush(); net_connected();
    scene("survey-live-nolink"); net_joining(); sv_draw(); display_flush(); net_connected();
    sim_sv.sampling = true; sim_sv.step = 5; sim_sv.steps = 9; strcpy(sim_sv.activity, "5G download (lan)"); sim_now_us = 14 * 1000000LL;
    scene("survey-sampling"); s_sv = SV_SAMPLING; sv_draw(); display_flush();
    sim_sv.sampling = false;
    scene("survey-result"); s_sv = SV_RESULT; s_sv_point = 2; sv_draw(); display_flush();
    sim_pt.band[1].valid = false; strcpy(sim_pt.band[1].note, "not visible");
    scene("survey-result-one-band"); sv_draw(); display_flush();
    sv_fill();
    scene("survey-menu"); sv_build_menu(); s_sv = SV_MENU; s_sv_menu.cur = 3; sv_draw(); display_flush();

    scene("wifi-menu"); s_scr = SCR_WIFI; wifi_enter(); wifi_draw(); display_flush();
    sim_prov.state = PROV_WAITING; strcpy(sim_prov.ap_ssid, "netstick-bdd0"); strcpy(sim_prov.ap_ip, "192.168.4.1"); sim_prov.clients = 1;
    scene("wifi-setup-waiting"); s_wv = WV_SETUP; wifi_draw(); display_flush();
    sim_prov.state = PROV_JOINED; strcpy(sim_prov.chosen, "Rudov-Home-5G"); strcpy(sim_prov.msg, "ip 10.0.0.42");
    scene("wifi-setup-joined"); wifi_draw(); display_flush();
    sim_prov.state = PROV_FAILED; strcpy(sim_prov.msg, "bad password?");
    scene("wifi-setup-failed"); wifi_draw(); display_flush();
    scene("home-no-network"); sim_net.has_creds = false; sim_net.connected = sim_net.has_ip = false; sim_cfg_ssid[0] = 0; s_scr = SCR_HOME; s_home.cur = 2; home_draw(); display_flush(); net_connected(); strcpy(sim_cfg_ssid, "Rudov-Home-5G");
    srand(7);
    scene("saver"); s_scr = SCR_SAVER; saver_enter(); sim_now_us = 5 * 1000000LL; s_frame = 10; saver_draw(); display_flush();
    scene("saver-glitch"); s_frame = 11; s_glitch_until = 0; while (s_glitch_until <= s_frame) { s_glitch_until = 0; saver_draw(); } saver_draw(); display_flush();
    scene("saver-hint"); sim_now_us = 0; s_frame = 40; saver_draw(); display_flush();
    scene("status-1"); s_scr = SCR_STATUS; s_st_page = 0; st_draw(); display_flush();
    scene("status-2"); s_st_page = 1; st_draw(); display_flush();
    scene("status-3"); s_st_page = 2; st_draw(); display_flush();
    scene("setup"); s_scr = SCR_SETUP; s_setup.cur = 0; setup_draw(); display_flush();
    printf("%d scenes\n", s_scene_n);
    return 0;
}
