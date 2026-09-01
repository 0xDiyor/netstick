#pragma once
#include <stdlib.h>
#include <stdint.h>
static inline uint32_t esp_random(void) { return (uint32_t)rand(); }
