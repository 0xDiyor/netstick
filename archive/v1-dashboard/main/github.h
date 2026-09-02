#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define GH_CAL_WEEKS 53

typedef struct {
    bool     valid;
    bool     authed;            // true if a token was used (GraphQL path)
    char     user[40];
    int      followers, following, repos, stars;
    int      open_prs, open_issues;
    int      contrib_total, contrib_today, streak_cur, streak_max;
    uint8_t  cal[GH_CAL_WEEKS][7];   // contribution levels 0-4, [0][0] is oldest
    int      cal_weeks;              // weeks actually populated
    int64_t  fetched_at;             // unix seconds
    char     err[72];
} gh_stats_t;

esp_err_t github_refresh(void);
void      github_get(gh_stats_t *out);
