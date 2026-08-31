#pragma once
#include <stdbool.h>
#include <stdint.h>

// Phone-based Wi-Fi setup. The stick raises an open access point with a
// captive portal; the phone's "sign in to network" sheet shows a page listing
// nearby networks; submitting saves the credentials and the stick joins.

typedef enum { PROV_OFF, PROV_SCANNING, PROV_WAITING, PROV_JOINING, PROV_JOINED, PROV_FAILED } prov_state_t;

typedef struct {
    prov_state_t state;
    char     ap_ssid[20];       // "netstick-XXXX"
    char     ap_ip[16];         // "192.168.4.1"
    uint8_t  clients;           // phones associated to the AP
    uint16_t n_scanned;         // networks offered in the list
    char     chosen[33];        // SSID the phone picked
    char     msg[27];           // result line for the screen
} prov_status_t;

void prov_start(void);          // scan, then AP + DNS + HTTP (idempotent)
void prov_stop(void);           // tear down, back to plain station mode
void prov_get(prov_status_t *out);
