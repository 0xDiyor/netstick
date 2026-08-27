#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "board.h"

// The framebuffer holds RGB565 pixels ALREADY BYTE-SWAPPED to big-endian, so it
// can be handed straight to the panel over SPI with no per-frame conversion.
// Build colours with RGB() / rgb565() and never interpret a stored word directly.
extern uint16_t *g_fb;

esp_err_t board_spi_init(void);       // shared bus: LCD + microSD
esp_err_t display_init(void);
void display_flush(void);             // blit the whole framebuffer, blocks until DMA is done
void display_backlight(uint8_t percent);
uint8_t display_backlight_get(void);
