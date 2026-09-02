#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define NET_RTT_HIST 64

typedef struct {
    bool     connected;
    bool     has_ip;
    bool     time_synced;
    char     ssid[33];
    char     ip[16];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  band;                    // 2 or 5 (GHz), 0 if unknown
    char     phy[8];                  // "11ax", "11ac", "11n", "11g"
    uint32_t disconnects;
    uint32_t connected_since;         // seconds since boot
    uint16_t rtt[NET_RTT_HIST];       // ring buffer, 0 means timeout
    uint8_t  rtt_head;
    uint16_t rtt_last;
    uint32_t ping_sent;
    uint32_t ping_lost;
} net_status_t;

esp_err_t netmgr_init(void);
void      netmgr_get(net_status_t *out);
bool      netmgr_wait_ip(uint32_t timeout_ms);
void      netmgr_refresh_link(void);   // re-read RSSI/channel from the driver

// The Wi-Fi driver refuses esp_wifi_scan_start() while an association attempt
// is in flight, so a survey has to pause the reconnect loop around itself.
void      netmgr_scan_hold(bool hold);
