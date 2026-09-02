#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

// .anim container written by tools/mkanim.py
//   0  magic "ANM1"
//   4  u16 width      (LE)
//   6  u16 height     (LE)
//   8  u16 frame_count
//   10 u16 default_delay_ms
//   12 u32 flags (reserved, 0)
//   16 frames: [u16 delay_ms][u16 reserved][w*h*2 bytes RGB565 BIG-endian]
// Pixels are stored big-endian so a frame can go straight to the ST7735.
#define ANIM_MAGIC "ANM1"
#define ANIM_MAX_FILES 12
#define ANIM_PATH_LEN  272   // /sd/anim/ + a 255-char LFN

typedef struct {
    FILE     *f;
    uint16_t  w, h, frames, default_delay;
    uint16_t  cur;
    long      data_start;
    uint16_t *row;          // scratch row when the frame is narrower than the panel
    char      name[48];
} anim_t;

int  anim_scan_dir(char list[][ANIM_PATH_LEN], int max);   // returns count found
esp_err_t anim_open(anim_t *a, const char *path);
bool anim_draw_next(anim_t *a, uint16_t *delay_ms);        // draws into g_fb
void anim_close(anim_t *a);
