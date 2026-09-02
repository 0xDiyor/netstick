// T-Dongle-C5 dashboard: dual-band Wi-Fi analyzer + ambient screens.
#include "board.h"
#include "display.h"
#include "gfx.h"
#include "led.h"
#include "storage.h"
#include "netmgr.h"
#include "ui.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "main";

static int s_line = 0;

// Tiny boot console on the panel so a failure is visible without a serial cable.
static void boot_say(const char *msg, uint16_t colour)
{
    if (s_line >= 7) return;
    gfx_text(2, 18 + s_line * 9, msg, colour, 1);
    s_line++;
    display_flush();
}

void app_main(void)
{
    ESP_LOGI(TAG, "%s booting", BOARD_NAME);

    ESP_ERROR_CHECK(display_init());
    led_init();

    gfx_clear(C_BLACK);
    gfx_text(2, 2, "T-DONGLE-C5", C_LIME, 1);
    gfx_text_right(LCD_W - 2, 2, "dash", C_DIM, 1);
    gfx_hline(0, 12, LCD_W, C_DIM);
    display_flush();
    display_backlight(100);

    if (storage_mount() == ESP_OK) {
        char b[40];
        snprintf(b, sizeof(b), "SD %lluMB free %lluMB",
                 storage_total_mb() / 1024 ? storage_total_mb() / 1024 : storage_total_mb(),
                 storage_free_mb());
        boot_say(b, C_GREEN);
    } else {
        boot_say("SD: no card / not FAT32", C_RED);
    }

    boot_say("wifi: dual-band...", C_GREY);
    ESP_ERROR_CHECK(netmgr_init());

    if (netmgr_wait_ip(15000)) {
        net_status_t n;
        netmgr_get(&n);
        char b[40];
        snprintf(b, sizeof(b), "ip %s", n.ip);
        boot_say(b, C_GREEN);
        snprintf(b, sizeof(b), "%uGHz ch%u %s", n.band, n.channel, n.phy);
        boot_say(b, n.band == 5 ? C_CYAN : C_AMBER);
    } else {
        boot_say("no ip yet, retrying", C_AMBER);
    }

    vTaskDelay(pdMS_TO_TICKS(1200));
    ui_start();
}
