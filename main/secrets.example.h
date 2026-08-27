// Copy this file to main/secrets.h and fill it in. secrets.h is gitignored.
#pragma once

// Optional. Credentials are normally entered from a phone (home > wifi >
// setup via phone) and stored in NVS. These only seed NVS the first time, and
// are ignored while the SSID is the placeholder.
#define WIFI_SSID       "your-network"
#define WIFI_PASS       "your-password"

// POSIX TZ string for timestamps. Default is US Mountain.
#define TZ_STRING       "MST7MDT,M3.2.0,M11.1.0"

// Walking survey throughput server (tools/survey_server.py). Leave empty to
// find it by UDP broadcast on port 7777; set an IP to skip discovery.
#define SURVEY_SERVER   ""

// Plain-HTTP download used by the doctor's speed check and by the survey when
// no LAN server is found. Needs a fast host: speedtest.tele2.net, for example,
// tops out around 3 Mbit/s from the US and would blame the link for it.
#define SPEED_URL       "http://speed.cloudflare.com/__down?bytes=50000000"
