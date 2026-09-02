#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SCAN_MAX_AP     96
#define CH24_COUNT      14      // channels 1..14
#define CH5_COUNT       28      // 36..177, matching wifi_5g_channel_bit_t

typedef struct {
    char     ssid[33];
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  band;              // 2 or 5
    uint16_t bw_mhz;            // 20 / 40 / 80 / 160
    char     phy[6];            // 11ax / 11ac / 11n / 11g / 11b
    char     auth[10];
    bool     open;
} scan_ap_t;

typedef struct {
    scan_ap_t aps[SCAN_MAX_AP]; // sorted by RSSI, strongest first
    uint16_t  count;
    uint16_t  total_found;
    uint16_t  n_24, n_5, n_ax, n_ac, n_open;
    uint8_t   hist24[CH24_COUNT];
    uint8_t   hist5[CH5_COUNT];
    uint8_t   busiest_24, busiest_5;   // channel numbers
    uint8_t   quietest_24, quietest_5;
    int64_t   ts_unix;
    uint32_t  age_ms_at;        // esp_timer ms when the scan completed
    uint32_t  duration_ms;
    bool      valid;
    bool      logged;
} scan_result_t;

// The full result is ~3 KB, which is too much to copy onto a render task's
// stack every frame, so the UI pulls a small summary plus the few APs it shows.
typedef struct {
    uint16_t count, total_found;
    uint16_t n_24, n_5, n_ax, n_ac, n_open;
    uint8_t  hist24[CH24_COUNT];
    uint8_t  hist5[CH5_COUNT];
    uint8_t  busiest_24, busiest_5, quietest_24, quietest_5;
    uint32_t duration_ms;
    bool     valid, logged;
} scan_summary_t;

extern const uint16_t ch5_list[CH5_COUNT];

esp_err_t scanner_run(void);            // blocking, sweeps both bands
void      scanner_get(scan_result_t *out);          // full copy - heap/static callers only
void      scanner_get_summary(scan_summary_t *out); // cheap, stack friendly
int       scanner_get_top(scan_ap_t *out, int max); // strongest APs, returns count
uint32_t  scanner_age_ms(void);
bool      scanner_busy(void);
