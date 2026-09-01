#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
#define NVS_READWRITE 1
#define NVS_READONLY 0
static inline esp_err_t nvs_open(const char *n, int m, nvs_handle_t *h) { (void)n; (void)m; (void)h; return ESP_FAIL; }
static inline esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v) { return ESP_FAIL; }
static inline esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { return ESP_OK; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { return ESP_OK; }
static inline void nvs_close(nvs_handle_t h) { }
