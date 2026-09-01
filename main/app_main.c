// netstick: network doctor + walking site survey for the LilyGO T-Dongle-C5.
#include "board.h"
#include "display.h"
#include "gfx.h"
#include "led.h"
#include "storage.h"
#include "netmgr.h"
#include "survey.h"
#include "ui.h"
#include "secrets.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "main";
static int s_line;

// Boot log under the logo, in the small font so five lines fit.
static void boot_say(const char *tag, const char *msg, uint16_t col)
{
    if (s_line >= 4) return;
    int y = 48 + s_line * 8;
    gfx_text_f(&font_5x8, 2, y, tag, col);
    gfx_text_f(&font_5x8, 2 + 5 * 5, y, msg, C_FG);
    s_line++;
    display_flush();
}

static void boot_logo(void)
{
    gfx_clear(C_BG);
    static const char *art[4] = {
        " .=======.___",
        " | net   |___)   NETSTICK",
        " | stick |       v1.0",
        " '=======' ",
    };
    for (int i = 0; i < 4; i++) gfx_text(0, i * 12, art[i], C_FG);
    gfx_text(CX(17), 12, "NETSTICK", C_BRIGHT);      // redraw the name in bright
    gfx_text(CX(17), 24, "v1.0", C_DIM);
    gfx_hline(0, 46, LCD_W, C_DIM);
    display_flush();
}

void app_main(void)
{
    ESP_LOGI(TAG, "%s booting", BOARD_NAME);
    ESP_ERROR_CHECK(display_init());
    led_init();
    led_set(255, 120, 0, 2);
    boot_logo();
    display_backlight(100);

    if (storage_mount() == ESP_OK) {
        char b[40];
        snprintf(b, sizeof(b), "sd %lluGB, %lluGB free", storage_total_mb() / 1024, storage_free_mb() / 1024);
        boot_say("[ok]", b, C_OK);
    } else {
        boot_say("[--]", "sd: no card / not FAT32", C_WARN);
    }

    survey_init();
    ESP_ERROR_CHECK(netmgr_init());
    char b[40];
    if (!netmgr_has_creds()) {
        boot_say("[--]", "wifi: none saved, see wifi", C_WARN);
    } else {
        snprintf(b, sizeof(b), "wifi: joining %.18s", netmgr_cfg_ssid());
        boot_say("[..]", b, C_FG);
    }

    if (netmgr_has_creds() && netmgr_wait_ip(8000)) {
        net_status_t n; netmgr_get(&n);
        snprintf(b, sizeof(b), "ip %s  %s ch%u", n.ip, n.band == 5 ? "5G" : "2.4G", n.channel);
        boot_say("[ok]", b, C_OK);
    } else if (netmgr_has_creds()) {
        boot_say("[..]", "no ip yet, keeps trying", C_WARN);
    }
    vTaskDelay(pdMS_TO_TICKS(900));
    ui_start();
}
