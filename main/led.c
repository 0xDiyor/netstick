// Single APA102 on dedicated clock/data pins - bit-banged, no bus sharing needed.
#include "led.h"
#include "board.h"
#include "driver/gpio.h"

static inline void clk_pulse(void)
{
    gpio_set_level(PIN_LED_CI, 1);
    gpio_set_level(PIN_LED_CI, 0);
}

static void send_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(PIN_LED_DI, (b >> i) & 1);
        clk_pulse();
    }
}

void led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_LED_CI) | (1ULL << PIN_LED_DI),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    led_off();
}

void led_set(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness)
{
    if (brightness > 31) brightness = 31;

    for (int i = 0; i < 4; i++) send_byte(0x00);        // start frame
    send_byte(0xE0 | brightness);
    send_byte(b);                                        // APA102 order is B, G, R
    send_byte(g);
    send_byte(r);
    for (int i = 0; i < 4; i++) send_byte(0xFF);        // end frame
}

void led_off(void) { led_set(0, 0, 0, 0); }
