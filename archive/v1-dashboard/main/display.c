#include "display.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "display";

uint16_t *g_fb = NULL;

static esp_lcd_panel_io_handle_t s_io = NULL;
static SemaphoreHandle_t s_flush_done = NULL;
static bool s_spi_ready = false;
static uint8_t s_bl_percent = 100;

#define LCD_FB_BYTES (LCD_W * LCD_H * 2)

// ---------------------------------------------------------------- SPI bus ---
// The LCD and the SD card sit on the same bus (MOSI 2 / MISO 7 / SCK 6) and are
// selected by their own CS lines, so the bus is initialised once, here, and both
// drivers attach to it as separate devices.
esp_err_t board_spi_init(void)
{
    if (s_spi_ready) return ESP_OK;

    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_SPI_MOSI,
        .miso_io_num     = PIN_SPI_MISO,
        .sclk_io_num     = PIN_SPI_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_FB_BYTES + 64,   // a full-screen blit in one go
    };
    esp_err_t err = spi_bus_initialize(BOARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    s_spi_ready = true;
    return ESP_OK;
}

// ------------------------------------------------------------- backlight ---
// GPIO0 drives the backlight and is ACTIVE LOW, hence output_invert.
static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch = {
        .gpio_num   = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
        .flags.output_invert = 1,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

void display_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_bl_percent = percent;
    uint32_t duty = (1023u * percent) / 100u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

uint8_t display_backlight_get(void) { return s_bl_percent; }

// ------------------------------------------------------------- ST7735 ------
// Init sequence and the 26/1 window offsets come from LilyGO's panel driver for
// this exact "green tab" 160x80 module. MADCTL 0xA8 = MY|MV|BGR, i.e. rotation 3.
#define ST_SWRESET 0x01
#define ST_SLPOUT  0x11
#define ST_NORON   0x13
#define ST_INVON   0x21
#define ST_DISPON  0x29
#define ST_CASET   0x2A
#define ST_RASET   0x2B
#define ST_RAMWR   0x2C
#define ST_MADCTL  0x36
#define ST_COLMOD  0x3A
#define ST_FRMCTR1 0xB1
#define ST_FRMCTR2 0xB2
#define ST_FRMCTR3 0xB3
#define ST_INVCTR  0xB4
#define ST_PWCTR1  0xC0
#define ST_PWCTR2  0xC1
#define ST_PWCTR3  0xC2
#define ST_PWCTR4  0xC3
#define ST_PWCTR5  0xC4
#define ST_VMCTR1  0xC5
#define ST_GMCTRP1 0xE0
#define ST_GMCTRN1 0xE1

#define MADCTL_LANDSCAPE 0xA8

typedef struct { uint8_t cmd; uint8_t len; uint8_t delay_ms; uint8_t data[16]; } st_cmd_t;

static const st_cmd_t s_init[] = {
    { ST_SWRESET, 0,  150, {0} },
    { ST_SLPOUT,  0,  255, {0} },
    { ST_FRMCTR1, 3,  0,   {0x01, 0x2C, 0x2D} },
    { ST_FRMCTR2, 3,  0,   {0x01, 0x2C, 0x2D} },
    { ST_FRMCTR3, 6,  0,   {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D} },
    { ST_INVCTR,  1,  0,   {0x07} },
    { ST_PWCTR1,  3,  0,   {0xA2, 0x02, 0x84} },
    { ST_PWCTR2,  1,  0,   {0xC5} },
    { ST_PWCTR3,  2,  0,   {0x0A, 0x00} },
    { ST_PWCTR4,  2,  0,   {0x8A, 0x2A} },
    { ST_PWCTR5,  2,  0,   {0x8A, 0xEE} },
    { ST_VMCTR1,  1,  0,   {0x0E} },
    { ST_INVON,   0,  0,   {0} },                 // this panel is inverted
    { ST_COLMOD,  1,  0,   {0x05} },              // 16-bit / RGB565
    { ST_MADCTL,  1,  0,   {MADCTL_LANDSCAPE} },
    { ST_GMCTRP1, 16, 0,   {0x02,0x1c,0x07,0x12,0x37,0x32,0x29,0x2d,0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10} },
    { ST_GMCTRN1, 16, 0,   {0x03,0x1d,0x07,0x06,0x2E,0x2C,0x29,0x2D,0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10} },
    { ST_NORON,   0,  10,  {0} },
    { ST_DISPON,  0,  100, {0} },
};

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

esp_err_t display_init(void)
{
    ESP_ERROR_CHECK(board_spi_init());

    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << PIN_LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&rst));

    backlight_init();
    display_backlight(0);           // keep it dark until the first frame is ready

    // Hardware reset before any traffic on the bus.
    gpio_set_level(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));

    s_flush_done = xSemaphoreCreateBinary();
    if (!s_flush_done) return ESP_ERR_NO_MEM;

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num         = PIN_LCD_CS,
        .dc_gpio_num         = PIN_LCD_DC,
        .spi_mode            = 0,
        .pclk_hz             = LCD_PCLK_HZ,
        .trans_queue_depth   = 4,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .on_color_trans_done = on_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_SPI_HOST,
                                             &io_cfg, &s_io));

    for (size_t i = 0; i < sizeof(s_init) / sizeof(s_init[0]); i++) {
        const st_cmd_t *c = &s_init[i];
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, c->cmd, c->len ? c->data : NULL, c->len));
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }

    // DMA needs the framebuffer in internal RAM; 25.6 kB is cheap.
    g_fb = heap_caps_malloc(LCD_FB_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!g_fb) return ESP_ERR_NO_MEM;
    memset(g_fb, 0, LCD_FB_BYTES);

    display_flush();
    ESP_LOGI(TAG, "ST7735 up: %dx%d landscape", LCD_W, LCD_H);
    return ESP_OK;
}

void display_flush(void)
{
    const uint16_t x0 = LCD_X_OFFSET, x1 = LCD_X_OFFSET + LCD_W - 1;
    const uint16_t y0 = LCD_Y_OFFSET, y1 = LCD_Y_OFFSET + LCD_H - 1;
    const uint8_t ca[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    const uint8_t ra[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };

    esp_lcd_panel_io_tx_param(s_io, ST_CASET, ca, 4);
    esp_lcd_panel_io_tx_param(s_io, ST_RASET, ra, 4);
    esp_lcd_panel_io_tx_color(s_io, ST_RAMWR, g_fb, LCD_FB_BYTES);
    xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(500));
}
