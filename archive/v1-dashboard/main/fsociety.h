#pragma once
#include <stdint.h>
#include "esp_err.h"

// Built-in fsociety mask screen. The mask is rasterised from geometry once at
// first use (no asset in flash, no SD card needed); the scanlines, slice
// glitches, channel split and noise are generated live, frame by frame.
esp_err_t fsociety_init(void);
uint16_t  fsociety_draw(void);   // renders the next frame into g_fb, returns ms to hold
