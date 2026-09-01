#pragma once
#include <stdio.h>
#define ESP_LOGI(t, f, ...) do { (void)t; } while (0)
#define ESP_LOGW(t, f, ...) do { (void)t; } while (0)
#define ESP_LOGE(t, f, ...) do { (void)t; } while (0)
typedef enum { ESP_LOG_NONE, ESP_LOG_INFO } esp_log_level_t;
static inline void esp_log_level_set(const char *t, esp_log_level_t l) { (void)t; (void)l; }
