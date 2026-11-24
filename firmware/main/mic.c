// I2S MEMS mic level meter. The mic sends 24-bit samples left-aligned in 32-bit slots.
// A task reads blocks, removes the mic's DC offset and accumulates energy + peak;
// the web page polls mic_read_level() ~10x per second.
#include "mic.h"

#include <math.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mic";

#define MIC_WS   4
#define MIC_SCK  5
#define MIC_SD   6
#define RATE     48000
#define BLOCK    480                  // 10 ms
#define FULL     8388608.0f           // 2^23 = 24-bit full scale

static i2s_chan_handle_t rx;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static double acc_sq;                 // sum of squares since last read
static uint32_t acc_n;
static float acc_peak;
static bool alive;

static void mic_task(void *arg)
{
    static int32_t buf[BLOCK];
    float x1 = 0, y1 = 0;             // DC-blocking high-pass state
    while (1) {
        size_t got = 0;
        if (i2s_channel_read(rx, buf, sizeof(buf), &got, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        size_t n = got / sizeof(int32_t);
        double sq = 0;
        float pk = 0;
        int32_t mn = INT32_MAX, mx = INT32_MIN;
        for (size_t i = 0; i < n; i++) {
            int32_t raw = buf[i];
            if (raw < mn) mn = raw;
            if (raw > mx) mx = raw;
            float x = (float)(raw >> 8);              // 24-bit signed
            float y = x - x1 + 0.9975f * y1;          // ~20 Hz high-pass
            x1 = x;
            y1 = y;
            sq += (double)y * y;
            float a = fabsf(y);
            if (a > pk) pk = a;
        }
        portENTER_CRITICAL(&lock);
        acc_sq += sq;
        acc_n += n;
        if (pk > acc_peak) acc_peak = pk;
        alive = n && mx != mn;                        // stuck at 0 or -1 = not wired / not powered
        portEXIT_CRITICAL(&lock);
    }
}

void mic_start(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&cc, NULL, &rx));
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK,
            .ws = MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_SD,
        },
    };
    sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;        // L/R pin tied to GND
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &sc));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));
    xTaskCreate(mic_task, "mic", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "I2S mic: WS=%d SCK=%d SD=%d @ %d Hz", MIC_WS, MIC_SCK, MIC_SD, RATE);
}

static float to_db(float v)
{
    return v < 1.0f ? -140.0f : 20.0f * log10f(v / FULL);
}

mic_level_t mic_read_level(void)
{
    portENTER_CRITICAL(&lock);
    double sq = acc_sq;
    uint32_t n = acc_n;
    float pk = acc_peak;
    bool a = alive;
    acc_sq = 0;
    acc_n = 0;
    acc_peak = 0;
    portEXIT_CRITICAL(&lock);
    mic_level_t l = {
        .rms_db = to_db(n ? (float)sqrt(sq / n) : 0),
        .peak_db = to_db(pk),
        .alive = a,
        .rate = RATE,
    };
    return l;
}
