#include "survey.h"
#include "netmgr.h"
#include "netprobe.h"
#include "speed.h"
#include "storage.h"
#include "secrets.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "survey";

static survey_status_t   s_st;
static survey_point_t   *s_points;          // PSRAM
static SemaphoreHandle_t s_lock;
static volatile bool     s_abort;
static char              s_rooms[SURVEY_MAX_POINTS][14];
static int               s_n_rooms;

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void step(const char *what)
{
    lock();
    s_st.step++;
    snprintf(s_st.activity, sizeof(s_st.activity), "%s", what);
    unlock();
    ESP_LOGI(TAG, "[%d/%d] %s", s_st.step, s_st.steps, what);
}

static void load_rooms(void)
{
    s_n_rooms = 0;
    if (!storage_mounted()) return;
    FILE *f = fopen("/sd/survey/rooms.txt", "r");
    if (!f) return;
    char line[64];
    while (s_n_rooms < SURVEY_MAX_POINTS && fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *e = p + strlen(p);
        while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = '\0';
        if (!*p || *p == '#') continue;
        snprintf(s_rooms[s_n_rooms++], 14, "%s", p);
    }
    fclose(f);
    ESP_LOGI(TAG, "%d room names from rooms.txt", s_n_rooms);
}

static void set_next_label(void)
{
    int i = s_st.n_points;
    if (i < s_n_rooms) snprintf(s_st.next_label, sizeof(s_st.next_label), "%s", s_rooms[i]);
    else snprintf(s_st.next_label, sizeof(s_st.next_label), "P%d", i + 1);
}

void survey_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_points = heap_caps_calloc(SURVEY_MAX_POINTS, sizeof(survey_point_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_points) s_points = calloc(SURVEY_MAX_POINTS, sizeof(survey_point_t));
    s_st.both_bands = true;
}

void survey_set_both_bands(bool both) { lock(); s_st.both_bands = both; unlock(); }
bool survey_both_bands(void) { return s_st.both_bands; }

static void csv_header(FILE *f)
{
    fprintf(f, "point,label,iso_time,band_ghz,bssid,channel,rssi_dbm,scan_rssi_dbm,phy,bw_mhz,"
               "ping_avg_ms,ping_max_ms,loss_pct,dl_mbps,ul_mbps,speed_src,note\n");
}

static void csv_row(FILE *f, int idx, const survey_point_t *p, int b)
{
    const survey_band_t *x = &p->band[b];
    time_t t = p->ts; struct tm tm; gmtime_r(&t, &tm);
    char iso[24]; strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
    if (x->valid) {
        fprintf(f, "%d,\"%s\",%s,%s,%02X:%02X:%02X:%02X:%02X:%02X,%u,%d,%d,%s,%u,%u,%u,%u,%lu.%lu,%lu.%lu,%s,\n",
                idx + 1, p->label, iso, b ? "5" : "2.4",
                x->bssid[0], x->bssid[1], x->bssid[2], x->bssid[3], x->bssid[4], x->bssid[5],
                x->channel, x->rssi, x->scan_rssi, x->phy, x->bw_mhz, x->ping_avg, x->ping_max, x->loss_pct,
                (unsigned long)x->dl_x10 / 10, (unsigned long)x->dl_x10 % 10,
                (unsigned long)x->ul_x10 / 10, (unsigned long)x->ul_x10 % 10, x->src);
    } else {
        char sr[6] = "";
        if (x->seen) snprintf(sr, sizeof(sr), "%d", x->scan_rssi);
        fprintf(f, "%d,\"%s\",%s,%s,,,,%s,,,,,,,,,%s\n", idx + 1, p->label, iso, b ? "5" : "2.4", sr, x->note);
    }
}

bool survey_begin(void)
{
    lock();
    if (s_st.active) { unlock(); return false; }
    memset(&s_st, 0, sizeof(s_st));
    s_st.both_bands = true;
    s_st.active = true;
    unlock();
    load_rooms();

    if (storage_mounted()) {
        time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
        strftime(s_st.file, sizeof(s_st.file), "/sd/survey/%Y%m%d-%H%M%S.csv", &tm);
        FILE *f = fopen(s_st.file, "w");
        if (f) { csv_header(f); fclose(f); }
        else s_st.file[0] = '\0';
    }
    printf("\n==== netstick survey session %s ====\n", s_st.file[0] ? s_st.file : "(no sd, console only)");
    FILE *c = stdout; csv_header(c);
    set_next_label();
    return true;
}

void survey_end(void)
{
    lock();
    s_st.active = false;
    unlock();
    netmgr_connect_auto(false);
    printf("==== survey session closed: %d points ====\n\n", s_st.n_points);
}

void survey_abort(void) { s_abort = true; }

// Scan only for our SSID and report the strongest BSS per band.
static int scan_ssid(uint8_t best[2][6], int8_t best_rssi[2], uint8_t best_ch[2], bool seen[2])
{
    seen[0] = seen[1] = false;
    best_rssi[0] = best_rssi[1] = -127;
    netmgr_scan_hold(true);
    wifi_scan_config_t cfg = {
        .ssid = (uint8_t *)netmgr_cfg_ssid(),
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 40, .max = 90 },
        .scan_time.passive = 150,
        .home_chan_dwell_time = 20,
        .channel_bitmap = { .ghz_2_channels = 0x7FFE, .ghz_5_channels = 0x1FFFFFFE },
        .coex_background_scan = true,
    };
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) { ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err)); netmgr_scan_hold(false); return 0; }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 32) n = 32;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!recs) { esp_wifi_clear_ap_list(); netmgr_scan_hold(false); return 0; }
    esp_wifi_scan_get_ap_records(&n, recs);
    for (int i = 0; i < n; i++) {
        int b = recs[i].primary >= 32 ? 1 : 0;
        seen[b] = true;
        if (recs[i].rssi > best_rssi[b]) {
            best_rssi[b] = recs[i].rssi;
            best_ch[b] = recs[i].primary;
            memcpy(best[b], recs[i].bssid, 6);
        }
    }
    free(recs);
    netmgr_scan_hold(false);
    return n;
}

static void measure(survey_band_t *x, const char *band_name)
{
    char a[27];
    net_status_t n;
    netmgr_refresh_link();
    netmgr_get(&n);
    memcpy(x->bssid, n.bssid, 6);
    x->channel = n.channel;
    x->rssi    = n.rssi;
    snprintf(x->phy, sizeof(x->phy), "%s", n.phy);
    x->bw_mhz  = n.bw_mhz;

    snprintf(a, sizeof(a), "%s ping gateway", band_name); step(a);
    ping_result_t p;
    probe_ping(n.gw, 10, 200, 1000, &p);
    x->ping_avg = p.avg_ms; x->ping_max = p.max_ms; x->loss_pct = p.loss_pct;
    if (s_abort) return;

    if (s_st.server_found) {
        snprintf(a, sizeof(a), "%s download (lan)", band_name); step(a);
        speed_lan_download(s_st.server, 3000, &x->dl_x10);
        if (s_abort) return;
        snprintf(a, sizeof(a), "%s upload (lan)", band_name); step(a);
        speed_lan_upload(s_st.server, 3000, &x->ul_x10);
        snprintf(x->src, sizeof(x->src), "lan");
    } else {
        snprintf(a, sizeof(a), "%s download (http)", band_name); step(a);
        char err[24];
        probe_http_download(SPEED_URL, 3000, &x->dl_x10, err, sizeof(err));
        step("no lan server, no upload");
        snprintf(x->src, sizeof(x->src), "http");
    }
    // Re-read RSSI after the traffic; it is the more honest number.
    netmgr_refresh_link(); netmgr_get(&n);
    x->rssi = n.rssi;
    x->valid = true;
}

static void sample_task(void *arg)
{
    survey_point_t *pt = &s_points[s_st.n_points];
    memset(pt, 0, sizeof(*pt));
    snprintf(pt->label, sizeof(pt->label), "%s", s_st.next_label);
    pt->ts = (uint32_t)time(NULL);
    s_abort = false;

    lock();
    s_st.step = 0;
    s_st.steps = s_st.both_bands ? 9 : 5;
    unlock();

    // Locate the LAN speed server once per session.
    if (!s_st.server_found) {
        step("looking for lan server");
        char host[16];
        if (speed_find_server(host, sizeof(host))) {
            lock(); s_st.server_found = true; snprintf(s_st.server, sizeof(s_st.server), "%s", host); unlock();
        }
    } else step("lan server known");

    step("scanning both bands");
    uint8_t best[2][6], best_ch[2]; int8_t best_rssi[2]; bool seen[2];
    pt->n_aps_ssid = scan_ssid(best, best_rssi, best_ch, seen);
    for (int b = 0; b < 2; b++) { pt->band[b].seen = seen[b]; pt->band[b].scan_rssi = seen[b] ? best_rssi[b] : 0; }

    net_status_t n; netmgr_get(&n);
    if (!n.has_ip && !netmgr_wait_ip(8000)) {
        step("no link, giving up");
        goto out;
    }
    netmgr_get(&n);
    int cur = n.band == 5 ? 1 : 0;
    measure(&pt->band[cur], cur ? "5G" : "2.4G");
    if (s_abort) goto out;

    if (s_st.both_bands) {
        int other = cur ^ 1;
        survey_band_t *x = &pt->band[other];
        if (!seen[other]) {
            snprintf(x->note, sizeof(x->note), "not visible");
            step(other ? "5G not visible here" : "2.4G not visible here");
            lock(); s_st.step = s_st.steps - 1; unlock();
        } else {
            step(other ? "joining 5G AP" : "joining 2.4G AP");
            if (netmgr_connect_bssid(best[other], 15000)) {
                measure(x, other ? "5G" : "2.4G");
            } else {
                snprintf(x->note, sizeof(x->note), "join failed");
                lock(); s_st.step = s_st.steps - 1; unlock();
            }
        }
        // Unpin so normal roaming resumes; stay on whatever band we are on.
        netmgr_connect_auto(false);
    }

out:
    step("saving");
    {
        int idx = s_st.n_points;
        for (int b = 0; b < 2; b++) csv_row(stdout, idx, pt, b);
        if (s_st.file[0]) {
            FILE *f = fopen(s_st.file, "a");
            if (f) { for (int b = 0; b < 2; b++) csv_row(f, idx, pt, b); fclose(f); }
        }
    }
    lock();
    if (s_st.n_points < SURVEY_MAX_POINTS - 1) s_st.n_points++;
    s_st.sampling = false;
    s_st.activity[0] = '\0';
    unlock();
    set_next_label();
    vTaskDelete(NULL);
}

bool survey_sample(void)
{
    lock();
    if (!s_st.active || s_st.sampling) { unlock(); return false; }
    s_st.sampling = true;
    unlock();
    xTaskCreatePinnedToCore(sample_task, "survey", 8192, NULL, 5, NULL, 0);
    return true;
}

void survey_get(survey_status_t *out) { lock(); *out = s_st; unlock(); }

bool survey_point(int i, survey_point_t *out)
{
    if (i < 0 || i >= s_st.n_points) return false;
    lock(); *out = s_points[i]; unlock();
    return true;
}
