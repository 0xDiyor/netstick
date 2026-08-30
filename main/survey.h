#pragma once
#include <stdbool.h>
#include <stdint.h>

// Walking site survey. Stand somewhere, tap: the stick scans for your SSID on
// both bands, measures the current band (RSSI, gateway ping, throughput), then
// hops to the strongest AP on the other band and measures that too. Every
// point is appended to a CSV on the SD card and echoed to the console.

#define SURVEY_MAX_POINTS 64

typedef struct {
    bool     valid;
    bool     seen;              // AP on this band was visible in the scan
    char     note[14];          // "not visible", "join failed"
    uint8_t  bssid[6];
    uint8_t  channel;
    int8_t   rssi;              // as associated
    int8_t   scan_rssi;         // as seen in the scan
    char     phy[8];
    uint16_t bw_mhz;
    uint16_t ping_avg, ping_max;
    uint8_t  loss_pct;
    uint32_t dl_x10, ul_x10;    // Mbps * 10, 0 = not measured
    char     src[5];            // "lan" / "http" / ""
} survey_band_t;

typedef struct {
    char          label[14];
    uint32_t      ts;           // unix
    survey_band_t band[2];      // [0] = 2.4 GHz, [1] = 5 GHz
    uint8_t       n_aps_ssid;   // APs broadcasting our SSID seen at this spot
} survey_point_t;

typedef struct {
    bool     active;            // a session is open
    bool     sampling;
    int      n_points;
    int      step, steps;       // progress within a sample
    char     activity[27];
    char     file[40];          // CSV path, "" when no SD
    bool     server_found;
    char     server[16];
    bool     both_bands;        // setting
    char     next_label[14];    // label the next tap will use
} survey_status_t;

void survey_init(void);
void survey_set_both_bands(bool both);
bool survey_both_bands(void);
bool survey_begin(void);            // opens a session (new CSV); false if one is active
void survey_end(void);              // closes it (restores auto band selection)
bool survey_sample(void);           // starts a measurement at the current spot
void survey_abort(void);            // asks a running sample to stop between steps
void survey_get(survey_status_t *out);
bool survey_point(int i, survey_point_t *out);   // 0-based, false if out of range
