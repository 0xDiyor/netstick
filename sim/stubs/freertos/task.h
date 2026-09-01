#pragma once
static inline void vTaskDelay(int t) { (void)t; }
#define xTaskCreate(f, n, s, p, pr, h) (0)
#define xTaskCreatePinnedToCore(f, n, s, p, pr, h, c) (0)
