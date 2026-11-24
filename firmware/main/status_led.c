// WS2812 on GPIO48 (the board's "RGB" pads must be bridged). Shows what the firmware is doing.
#include "status_led.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define LED_GPIO 48

static led_strip_handle_t strip;
static volatile led_state_t state = LED_BOOT;
static volatile int64_t flash_until;
static volatile uint8_t flash_rgb[3];

static void put(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(strip, 0, r, g, b);
    led_strip_refresh(strip);
}

static void led_task(void *arg)
{
    for (int tick = 0;; tick++) {
        bool on2 = (tick / 25) % 2;   // 2 Hz at 20 ms ticks
        bool on5 = (tick / 10) % 2;   // 5 Hz
        bool on1 = (tick / 50) % 2;   // 1 Hz
        if (esp_timer_get_time() < flash_until) {
            put(flash_rgb[0], flash_rgb[1], flash_rgb[2]);
        } else {
            switch (state) {
            case LED_BOOT:         put(6, 6, 6); break;
            case LED_WIFI_JOINING: put(0, 0, on2 ? 12 : 0); break;
            case LED_OK:           put(0, 6, 0); break;
            case LED_WIFI_FAILED:  put(on1 ? 12 : 0, 0, 0); break;
            case LED_OTA:          put(on5 ? 16 : 0, 0, on5 ? 16 : 0); break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void status_led_init(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {.resolution_hz = 10 * 1000 * 1000};
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &strip));
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
}

void status_led_state(led_state_t s) { state = s; }

void status_led_flash(uint8_t r, uint8_t g, uint8_t b)
{
    flash_rgb[0] = r;
    flash_rgb[1] = g;
    flash_rgb[2] = b;
    flash_until = esp_timer_get_time() + 120 * 1000;
}
