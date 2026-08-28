#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

typedef struct {
    bool     started;               // driver up, will keep trying to join
    bool     has_creds;             // a network is configured at all
    bool     connected;
    bool     has_ip;
    bool     time_synced;
    char     ssid[33];
    uint8_t  bssid[6];
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  band;                  // 2 or 5 (GHz), 0 if unknown
    uint16_t bw_mhz;
    char     phy[8];                // negotiated: "HE20", "HT40", "11g" ...
    bool     ax;                    // AP advertises 802.11ax
    char     ip[16], mask[16], gw[16], dns[2][16];
    char     ip6_global[46];        // "" when none
    char     ip6_ll[46];
    uint32_t dhcp_ms;               // association -> address, last time
    uint32_t disconnects;
    uint32_t connected_since;       // seconds since boot
    uint8_t  last_reason;           // wifi_err_reason_t of the last drop
    char     reason_str[20];
} net_status_t;

esp_err_t   netmgr_init(void);

// Credentials live in NVS (set from the phone via the wifi screen). secrets.h
// only seeds them the first time, and only if it is not the placeholder.
bool        netmgr_has_creds(void);
const char *netmgr_cfg_ssid(void);                        // configured SSID, "" if none
void        netmgr_set_creds(const char *ssid, const char *pass);   // saves + reconnects
void        netmgr_forget(void);                          // drops link, clears NVS
void        netmgr_get(net_status_t *out);
bool        netmgr_wait_ip(uint32_t timeout_ms);
void        netmgr_refresh_link(void);          // re-read RSSI/channel/phy from the driver
esp_netif_t *netmgr_netif(void);

// The driver refuses esp_wifi_scan_start() while an association attempt is in
// flight, so a sweep has to pause the reconnect loop around itself.
void        netmgr_scan_hold(bool hold);

// Survey support: drop the current link and re-associate to one specific BSS
// (the strongest AP on a given band, say). Blocks until an address arrives or
// the timeout passes. netmgr_connect_auto() lifts the pin again; with
// reconnect=true it also drops the link so the driver picks freely.
bool        netmgr_connect_bssid(const uint8_t bssid[6], uint32_t timeout_ms);
void        netmgr_connect_auto(bool reconnect);
bool        netmgr_pinned(void);

const char *netmgr_reason_str(uint8_t reason);
