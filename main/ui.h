#pragma once
#include <stdint.h>
void ui_start(void);
void ui_set_brightness(uint8_t pct);   // persists to NVS
uint8_t ui_brightness(void);
