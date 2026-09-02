// Pin map for the LilyGO T-Dongle-C5 (ESP32-C5, 16MB flash / 8MB PSRAM).
// Values taken from LilyGO's reference design: Xinyuan-LilyGO/T-Dongle-C5 include/pin_config.h
#pragma once

#include "driver/spi_master.h"

#define BOARD_NAME          "T-Dongle-C5"

// One SPI bus is shared by the LCD and the microSD card; they differ only by CS.
#define BOARD_SPI_HOST      SPI2_HOST
#define PIN_SPI_MOSI        2
#define PIN_SPI_MISO        7
#define PIN_SPI_SCK         6

// ST7735 0.96" IPS, 80x160 native. We drive it rotated to 160x80 landscape.
#define PIN_LCD_CS          10
#define PIN_LCD_DC          3
#define PIN_LCD_RST         1
#define PIN_LCD_BL          0     // backlight, ACTIVE LOW
#define LCD_W               160
#define LCD_H               80
#define LCD_X_OFFSET        1     // landscape offsets (portrait is 26/1, swapped when rotated)
#define LCD_Y_OFFSET        26
#define LCD_PCLK_HZ         (40 * 1000 * 1000)

// microSD in SPI mode on the shared bus.
#define PIN_SD_CS           23
#define SD_MOUNT_POINT      "/sd"

// Single APA102 (clock/data are dedicated pins, no bus sharing).
#define PIN_LED_CI          4
#define PIN_LED_DI          5

// Side button, ACTIVE LOW with internal pull-up.
#define PIN_BTN             28
