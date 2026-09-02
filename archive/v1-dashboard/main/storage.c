#include "storage.h"
#include "board.h"
#include "display.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "storage";

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;
static char s_err[64] = "not mounted";

esp_err_t storage_mount(void)
{
    if (s_mounted) return ESP_OK;
    ESP_ERROR_CHECK(board_spi_init());

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SPI_HOST;
    host.max_freq_khz = 20000;          // conservative: the LCD shares this bus

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = BOARD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,    // never wipe the user's card
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            // Almost always this: IDF's FatFs is built with FF_FS_EXFAT=0, and
            // cards >32GB ship exFAT from the factory.
            snprintf(s_err, sizeof(s_err), "unreadable FS - format FAT32?");
        } else {
            snprintf(s_err, sizeof(s_err), "%s", esp_err_to_name(err));
        }
        ESP_LOGE(TAG, "mount failed: %s (%s)", esp_err_to_name(err), s_err);
        return err;
    }

    s_mounted = true;
    s_err[0] = '\0';
    ESP_LOGI(TAG, "SD mounted: %s, %llu MB", s_card->cid.name, storage_total_mb());
    mkdir(SD_MOUNT_POINT "/wifi", 0777);
    mkdir(SD_MOUNT_POINT "/anim", 0777);
    return ESP_OK;
}

bool storage_mounted(void) { return s_mounted; }
const char *storage_error(void) { return s_err; }

uint64_t storage_total_mb(void)
{
    if (!s_mounted) return 0;
    return ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024);
}

uint64_t storage_free_mb(void)
{
    if (!s_mounted) return 0;
    uint64_t total = 0, avail = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &avail) != ESP_OK) return 0;
    return avail / (1024 * 1024);
}
