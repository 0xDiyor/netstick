#pragma once
#include <stdint.h>
extern int64_t sim_now_us;
static inline int64_t esp_timer_get_time(void) { return sim_now_us; }
