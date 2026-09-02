// Dual-band Wi-Fi survey. On the ESP32-C5 a single esp_wifi_scan_start() with
// band mode AUTO sweeps 2.4 GHz and 5 GHz, so one pass gives us both.
// Note: a full sweep goes off-channel for several seconds, so an associated
// link will see a latency spike while this runs. Scans are therefore only
// triggered from the analyzer screen, not continuously in the background.
#include "scanner.h"
#include "board.h"
#include "storage.h"
#include "netmgr.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "scan";

// Every 5 GHz channel the ESP32-C5 can legally land on, in order.
const uint16_t ch5_list[CH5_COUNT] = {
    36, 40, 44, 48, 52, 56, 60, 64,
    100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165, 169, 173, 177
};

static scan_result_t    s_res;
static SemaphoreHandle_t s_lock;
static volatile bool     s_busy;

static void lock_init(void) { if (!s_lock) s_lock = xSemaphoreCreateMutex(); }

static int ch5_index(uint8_t ch)
{
    for (int i = 0; i < CH5_COUNT; i++) if (ch5_list[i] == ch) return i;
    return -1;
}

static const char *auth_str(wifi_auth_mode_t a)
{
    switch (a) {
        case WIFI_AUTH_OPEN:             return "open";
        case WIFI_AUTH_WEP:              return "WEP";
        case WIFI_AUTH_WPA_PSK:          return "WPA";
        case WIFI_AUTH_WPA2_PSK:         return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:     return "WPA/2";
        case WIFI_AUTH_WPA3_PSK:         return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:    return "WPA2/3";
        case WIFI_AUTH_WAPI_PSK:         return "WAPI";
        case WIFI_AUTH_OWE:              return "OWE";
        case WIFI_AUTH_WPA3_ENT_192:     return "ENT192";
        default:                         return "ENT";
    }
}

static uint16_t bw_mhz(wifi_bandwidth_t bw)
{
    switch (bw) {
        case WIFI_BW_HT20: return 20;
        case WIFI_BW_HT40: return 40;
        case WIFI_BW80:    return 80;
        case WIFI_BW160:   return 160;
        case WIFI_BW80_BW80: return 160;
        default:           return 20;
    }
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const scan_ap_t *)b)->rssi - ((const scan_ap_t *)a)->rssi;
}

static void log_to_sd(const scan_result_t *r)
{
    if (!storage_mounted()) return;

    time_t t = (time_t)r->ts_unix;
    struct tm tm_now;
    localtime_r(&t, &tm_now);

    char path[64];
    strftime(path, sizeof(path), SD_MOUNT_POINT "/wifi/scan-%Y-%m-%d.csv", &tm_now);

    bool need_header = false;
    FILE *probe = fopen(path, "r");
    if (!probe) need_header = true; else fclose(probe);

    FILE *f = fopen(path, "a");
    if (!f) { ESP_LOGW(TAG, "cannot open %s", path); return; }
    if (need_header) fprintf(f, "iso_time,bssid,ssid,band_ghz,channel,rssi_dbm,bw_mhz,phy,auth\n");

    char iso[32];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tm_now);

    for (int i = 0; i < r->count; i++) {
        const scan_ap_t *ap = &r->aps[i];
        fprintf(f, "%s,%02X:%02X:%02X:%02X:%02X:%02X,\"%s\",%u,%u,%d,%u,%s,%s\n",
                iso, ap->bssid[0], ap->bssid[1], ap->bssid[2],
                ap->bssid[3], ap->bssid[4], ap->bssid[5],
                ap->ssid[0] ? ap->ssid : "(hidden)",
                ap->band, ap->channel, ap->rssi, ap->bw_mhz, ap->phy, ap->auth);
    }
    fclose(f);
    ESP_LOGI(TAG, "logged %u APs to %s", r->count, path);
}

esp_err_t scanner_run(void)
{
    lock_init();
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_busy = true;

    int64_t t0 = esp_timer_get_time();

    // The driver rejects a scan outright while an association is in progress.
    netmgr_scan_hold(true);

    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,                   // 0 = use the bitmap below
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        // Kept short: every extra ms per channel is time the associated link
        // spends off-channel, and there are 42 channels to visit.
        .scan_time.active = { .min = 40, .max = 90 },
        // DFS channels must be scanned passively; 150 ms is one beacon interval
        // plus margin, versus the 360 ms default.
        .scan_time.passive = 150,
        .home_chan_dwell_time = 20,
        // bit0 is the "skip this band" flag and bits 1..N select channels, so
        // an all-zero bitmap scans NOTHING. Set every channel bit explicitly:
        // 2.4 GHz channels 1-14, and 5 GHz 36-177 per wifi_5g_channel_bit_t.
        .channel_bitmap = {
            .ghz_2_channels = 0x7FFE,       // bits 1..14
            .ghz_5_channels = 0x1FFFFFFE,   // bits 1..28
        },
        .coex_background_scan = true,
    };

    esp_err_t err = esp_wifi_scan_start(&cfg, true);   // blocking
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start: %s", esp_err_to_name(err));
        netmgr_scan_hold(false);
        s_busy = false;
        return err;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    uint16_t want = found > SCAN_MAX_AP ? SCAN_MAX_AP : found;

    wifi_ap_record_t *recs = calloc(want ? want : 1, sizeof(wifi_ap_record_t));
    if (!recs) {
        esp_wifi_clear_ap_list();
        netmgr_scan_hold(false);
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }

    uint16_t got = want;
    esp_wifi_scan_get_ap_records(&got, recs);

    // Static rather than a ~3 KB stack frame; s_busy serialises access.
    static scan_result_t r;
    memset(&r, 0, sizeof(r));
    r.total_found = found;
    r.ts_unix = (int64_t)time(NULL);

    for (int i = 0; i < got && r.count < SCAN_MAX_AP; i++) {
        wifi_ap_record_t *a = &recs[i];
        scan_ap_t *o = &r.aps[r.count++];

        memcpy(o->ssid, a->ssid, 32);
        o->ssid[32] = '\0';
        memcpy(o->bssid, a->bssid, 6);
        o->rssi    = a->rssi;
        o->channel = a->primary;
        // wifi_ap_record_t carries no band field; 2.4 GHz is channels 1-14 and
        // 5 GHz starts at 32, so the primary channel number tells us the band.
        o->band    = (a->primary >= 32) ? 5 : 2;
        o->bw_mhz  = bw_mhz(a->bandwidth);
        o->open    = (a->authmode == WIFI_AUTH_OPEN);
        snprintf(o->auth, sizeof(o->auth), "%s", auth_str(a->authmode));

        const char *phy = a->phy_11ax ? "11ax" : a->phy_11ac ? "11ac"
                        : a->phy_11n  ? "11n"  : a->phy_11g  ? "11g" : "11b";
        snprintf(o->phy, sizeof(o->phy), "%s", phy);

        if (o->band == 5) {
            r.n_5++;
            int idx = ch5_index(o->channel);
            if (idx >= 0 && r.hist5[idx] < 255) r.hist5[idx]++;
        } else {
            r.n_24++;
            if (o->channel >= 1 && o->channel <= CH24_COUNT) r.hist24[o->channel - 1]++;
        }
        if (a->phy_11ax) r.n_ax++;
        if (a->phy_11ac) r.n_ac++;
        if (o->open)     r.n_open++;
    }
    free(recs);
    esp_wifi_clear_ap_list();
    netmgr_scan_hold(false);

    qsort(r.aps, r.count, sizeof(scan_ap_t), cmp_rssi);

    // Busiest / quietest channel per band, for the "use this channel" hint.
    uint8_t best = 255, worst = 0;
    for (int i = 0; i < CH24_COUNT; i++) {
        if (i != 0 && i != 5 && i != 10) continue;      // only 1/6/11 are non-overlapping
        if (r.hist24[i] > worst) { worst = r.hist24[i]; r.busiest_24 = i + 1; }
        if (r.hist24[i] < best)  { best  = r.hist24[i]; r.quietest_24 = i + 1; }
    }
    best = 255; worst = 0;
    for (int i = 0; i < CH5_COUNT; i++) {
        if (r.hist5[i] > worst) { worst = r.hist5[i]; r.busiest_5 = ch5_list[i]; }
        if (r.hist5[i] < best)  { best  = r.hist5[i]; r.quietest_5 = ch5_list[i]; }
    }

    r.duration_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    r.age_ms_at   = (uint32_t)(esp_timer_get_time() / 1000);
    r.valid = true;

    log_to_sd(&r);
    r.logged = storage_mounted();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_res = r;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "%u APs (%u@2.4 %u@5, %u ax) in %u ms",
             r.count, r.n_24, r.n_5, r.n_ax, r.duration_ms);
    s_busy = false;
    return ESP_OK;
}

void scanner_get(scan_result_t *out)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_res;
    xSemaphoreGive(s_lock);
}

void scanner_get_summary(scan_summary_t *out)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->count       = s_res.count;
    out->total_found = s_res.total_found;
    out->n_24        = s_res.n_24;
    out->n_5         = s_res.n_5;
    out->n_ax        = s_res.n_ax;
    out->n_ac        = s_res.n_ac;
    out->n_open      = s_res.n_open;
    memcpy(out->hist24, s_res.hist24, sizeof(out->hist24));
    memcpy(out->hist5,  s_res.hist5,  sizeof(out->hist5));
    out->busiest_24  = s_res.busiest_24;
    out->busiest_5   = s_res.busiest_5;
    out->quietest_24 = s_res.quietest_24;
    out->quietest_5  = s_res.quietest_5;
    out->duration_ms = s_res.duration_ms;
    out->valid       = s_res.valid;
    out->logged      = s_res.logged;
    xSemaphoreGive(s_lock);
}

int scanner_get_top(scan_ap_t *out, int max)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_res.count < max ? s_res.count : max;
    for (int i = 0; i < n; i++) out[i] = s_res.aps[i];
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t scanner_age_ms(void)
{
    if (!s_res.valid) return UINT32_MAX;
    return (uint32_t)(esp_timer_get_time() / 1000) - s_res.age_ms_at;
}

bool scanner_busy(void) { return s_busy; }
