#pragma once
#include <stdint.h>

typedef enum {
    LED_BOOT,          // white, dim
    LED_WIFI_JOINING,  // blue, blinking
    LED_OK,            // green, dim
    LED_WIFI_FAILED,   // red, slow blink (firmware still runs, USB still works)
    LED_OTA,           // purple, fast blink
} led_state_t;

void status_led_init(void);
void status_led_state(led_state_t s);
void status_led_flash(uint8_t r, uint8_t g, uint8_t b);   // one short flash on top of the state colour
