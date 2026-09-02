// Copy this file to main/secrets.h and fill it in. secrets.h is gitignored.
#pragma once

#define WIFI_SSID       "your-network"
#define WIFI_PASS       "your-password"

// GitHub username to show stats for.
#define GITHUB_USER     "your-github-username"

// Optional but strongly recommended: a classic PAT or fine-grained token with
// read-only scope. With a token the firmware uses the GraphQL API and gets the
// full contribution calendar in one request. Without one it falls back to the
// unauthenticated REST API: followers/repos/open-PR count only, no heatmap,
// and a 60 requests/hour rate limit.
#define GITHUB_TOKEN    ""

// POSIX TZ string for the clock screen. Default is US Mountain.
#define TZ_STRING       "MST7MDT,M3.2.0,M11.1.0"

// Host to ping for the latency sparkline on the clock screen.
#define PING_TARGET     "1.1.1.1"
