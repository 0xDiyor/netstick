#pragma once
#include <stdint.h>
void led_init(void);
// brightness is the APA102 5-bit global current field, 0-31.
void led_set(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);
void led_off(void);
