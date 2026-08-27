#include "button.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static QueueHandle_t s_q;
static volatile uint32_t s_held_ms;

static void button_task(void *arg)
{
    bool    was_down = false, hold_fired = false;
    int64_t down_at = 0;
    int     stable = 0;

    while (1) {
        // Simple debounce: a level has to persist for two 10 ms polls.
        bool raw = gpio_get_level(PIN_BTN) == 0;
        static bool last_raw = false;
        stable = (raw == last_raw) ? stable + 1 : 0;
        last_raw = raw;
        bool down = (stable >= 1) ? raw : was_down;

        int64_t now = esp_timer_get_time();
        if (down && !was_down) {
            down_at = now;
            hold_fired = false;
        }
        if (down) {
            uint32_t ms = (uint32_t)((now - down_at) / 1000);
            s_held_ms = ms;
            if (!hold_fired && ms >= BTN_HOLD_MS) {
                hold_fired = true;
                btn_event_t e = BTN_HOLD;
                xQueueSend(s_q, &e, 0);
            }
        } else {
            if (was_down && !hold_fired) {
                uint32_t ms = (uint32_t)((now - down_at) / 1000);
                if (ms >= 25) {
                    btn_event_t e = BTN_TAP;
                    xQueueSend(s_q, &e, 0);
                }
            }
            s_held_ms = 0;
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    s_q = xQueueCreate(8, sizeof(btn_event_t));
    xTaskCreate(button_task, "btn", 2048, NULL, 6, NULL);
}

btn_event_t button_poll(void)
{
    btn_event_t e;
    if (xQueueReceive(s_q, &e, 0) == pdTRUE) return e;
    return BTN_NONE;
}

uint32_t button_held_ms(void) { return s_held_ms; }

void button_inject(btn_event_t e) { xQueueSend(s_q, &e, 0); }
