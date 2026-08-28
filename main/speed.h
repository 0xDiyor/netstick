#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Throughput against tools/survey_server.py on the LAN (preferred - measures
// the Wi-Fi link, not your ISP), or an HTTP download as a fallback.
#define SPEED_PORT 7777

bool speed_find_server(char *host, size_t host_len);         // config or UDP broadcast discovery
bool speed_lan_download(const char *host, int ms, uint32_t *mbps_x10);
bool speed_lan_upload(const char *host, int ms, uint32_t *mbps_x10);
