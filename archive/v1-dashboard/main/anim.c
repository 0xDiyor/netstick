#include "anim.h"
#include "board.h"
#include "gfx.h"
#include "esp_log.h"
#include <dirent.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "anim";

int anim_scan_dir(char list[][ANIM_PATH_LEN], int max)
{
    DIR *d = opendir(SD_MOUNT_POINT "/anim");
    if (!d) return 0;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcasecmp(dot, ".anim") != 0) continue;
        snprintf(list[n], ANIM_PATH_LEN, SD_MOUNT_POINT "/anim/%s", e->d_name);
        n++;
    }
    closedir(d);
    return n;
}

esp_err_t anim_open(anim_t *a, const char *path)
{
    memset(a, 0, sizeof(*a));
    a->f = fopen(path, "rb");
    if (!a->f) return ESP_ERR_NOT_FOUND;

    uint8_t hdr[16];
    if (fread(hdr, 1, sizeof(hdr), a->f) != sizeof(hdr) ||
        memcmp(hdr, ANIM_MAGIC, 4) != 0) {
        fclose(a->f); a->f = NULL;
        ESP_LOGW(TAG, "%s: not an ANM1 file", path);
        return ESP_ERR_INVALID_ARG;
    }

    a->w             = hdr[4]  | (hdr[5]  << 8);
    a->h             = hdr[6]  | (hdr[7]  << 8);
    a->frames        = hdr[8]  | (hdr[9]  << 8);
    a->default_delay = hdr[10] | (hdr[11] << 8);
    a->data_start    = 16;

    if (a->w == 0 || a->h == 0 || a->w > LCD_W || a->h > LCD_H || a->frames == 0) {
        ESP_LOGW(TAG, "%s: bad geometry %ux%u x%u", path, a->w, a->h, a->frames);
        fclose(a->f); a->f = NULL;
        return ESP_ERR_INVALID_SIZE;
    }

    a->row = malloc(a->w * 2);
    if (!a->row) { fclose(a->f); a->f = NULL; return ESP_ERR_NO_MEM; }

    const char *slash = strrchr(path, '/');
    snprintf(a->name, sizeof(a->name), "%s", slash ? slash + 1 : path);
    ESP_LOGI(TAG, "%s: %ux%u, %u frames @ %ums", a->name, a->w, a->h,
             a->frames, a->default_delay);
    return ESP_OK;
}

bool anim_draw_next(anim_t *a, uint16_t *delay_ms)
{
    if (!a->f) return false;

    if (a->cur >= a->frames) {                 // loop
        a->cur = 0;
        fseek(a->f, a->data_start, SEEK_SET);
    }

    uint8_t fh[4];
    if (fread(fh, 1, 4, a->f) != 4) return false;
    uint16_t d = fh[0] | (fh[1] << 8);
    *delay_ms = d ? d : (a->default_delay ? a->default_delay : 60);

    const int x0 = (LCD_W - a->w) / 2;         // centre smaller frames
    const int y0 = (LCD_H - a->h) / 2;

    if (a->w == LCD_W && a->h == LCD_H) {
        // Fast path: the frame is exactly the panel, read straight into the FB.
        if (fread(g_fb, 2, (size_t)LCD_W * LCD_H, a->f) != (size_t)LCD_W * LCD_H) return false;
    } else {
        gfx_clear(C_BLACK);
        for (int y = 0; y < a->h; y++) {
            if (fread(a->row, 2, a->w, a->f) != a->w) return false;
            memcpy(&g_fb[(y0 + y) * LCD_W + x0], a->row, a->w * 2);
        }
    }

    a->cur++;
    return true;
}

void anim_close(anim_t *a)
{
    if (a->f) fclose(a->f);
    free(a->row);
    memset(a, 0, sizeof(*a));
}
