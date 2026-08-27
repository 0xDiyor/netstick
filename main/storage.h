#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t storage_mount(void);      // never formats - a failed mount stays failed
bool      storage_mounted(void);
const char *storage_error(void);
uint64_t  storage_total_mb(void);
uint64_t  storage_free_mb(void);
