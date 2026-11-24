// "S3 Audio": UAC2 stereo speaker. Nothing is played yet (no DAC) - everything the PC sends is measured:
// requested vs. actually delivered sample rate, format, how many bits carry signal, per-channel levels,
// clipping, mono content, the PC's volume/mute, lost packets. The status LED pulses with the music.
#include "audio.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/usb_dwc_struct.h"
#include "status_led.h"

static const char *TAG = "audio";

static const uint32_t rates[] = {44100, 48000, 88200, 96000};
#define N_RATES (sizeof(rates) / sizeof(rates[0]))
#define VOL_MIN (-96 * 256)        // 1/256 dB units
#define VOL_MAX 0
#define VOL_RES 128                // 0.5 dB steps
#define FULL 8388608.0f            // everything is analysed on a 24-bit scale

static uint8_t itf_stream;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

// set by the PC (TinyUSB task)
static volatile uint32_t rate_set = 48000;
static volatile uint8_t alt;       // 0 closed, 1 = 16-bit, 2 = 24-bit
static volatile int8_t muted;
static volatile int16_t volume = 0;
static volatile uint32_t opens;

// packet accounting (USB ISR)
static uint32_t pk_count, pk_frames;
static uint16_t pk_min = 0xFFFF, pk_max;
static int64_t win_start_us;
// lost packets, counted against the host's own 1 ms USB frame clock (no clock drift, no window jitter):
// frames elapsed since the stream's first packet vs. packets received. A late interrupt makes the difference
// jump by 1 for a single packet only, so a loss is committed only if it holds for a whole block of packets.
static bool fr_started;
static uint32_t fr_last, fr_elapsed, fr_packets, fr_block;
static int32_t fr_block_min, fr_lost, lost_base;

// sample analysis (audio task)
static double sq[2];
static int32_t peak[2];
static uint32_t clipped, bits_or;
static bool not_mono;

static inline uint8_t bytes_per_sample(void) { return alt == 2 ? 3 : 2; }

// ---- event log ----
#define N_EVENTS 14
static struct { uint32_t ms; char text[40]; } events[N_EVENTS];
static uint32_t n_events;

static void event(const char *fmt, ...)
{
    char t[40];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&lock);
    uint32_t i = n_events++ % N_EVENTS;
    events[i].ms = (uint32_t)(esp_timer_get_time() / 1000);
    memcpy(events[i].text, t, sizeof(t));
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "%s", t);
}

int audio_events(char *out, int max)
{
    int len = 0;
    out[0] = 0;
    portENTER_CRITICAL(&lock);
    uint32_t n = n_events, first = n > N_EVENTS ? n - N_EVENTS : 0;
    for (uint32_t k = first; k < n && len < max - 60; k++) {
        uint32_t i = k % N_EVENTS;
        len += snprintf(out + len, max - len, "%s[%lu,\"%s\"]", k == first ? "" : ",", (unsigned long)events[i].ms, events[i].text);
    }
    portEXIT_CRITICAL(&lock);
    return len;
}

// ---- UAC2 control requests ----
static bool clock_get(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t sel = TU_U16_HIGH(req->wValue);
    if (sel == AUDIO20_CS_CTRL_SAM_FREQ && req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_4_t cur = {.bCur = (int32_t)rate_set};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &cur, sizeof(cur));
    }
    if (sel == AUDIO20_CS_CTRL_SAM_FREQ && req->bRequest == AUDIO20_CS_REQ_RANGE) {
        audio20_control_range_4_n_t(N_RATES) r = {.wNumSubRanges = N_RATES};
        for (int i = 0; i < N_RATES; i++) {
            r.subrange[i].bMin = r.subrange[i].bMax = (int32_t)rates[i];
            r.subrange[i].bRes = 0;
        }
        if (req->wLength > 2) {
            event("PC read the rate list");
        }
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &r, sizeof(r));
    }
    if (sel == AUDIO20_CS_CTRL_CLK_VALID && req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_1_t v = {.bCur = 1};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &v, sizeof(v));
    }
    return false;
}

static bool fu_get(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t sel = TU_U16_HIGH(req->wValue);
    if (sel == AUDIO20_FU_CTRL_MUTE && req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_1_t m = {.bCur = muted};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &m, sizeof(m));
    }
    if (sel == AUDIO20_FU_CTRL_VOLUME && req->bRequest == AUDIO20_CS_REQ_RANGE) {
        audio20_control_range_2_n_t(1) r = {.wNumSubRanges = 1, .subrange[0] = {.bMin = VOL_MIN, .bMax = VOL_MAX, .bRes = VOL_RES}};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &r, sizeof(r));
    }
    if (sel == AUDIO20_FU_CTRL_VOLUME && req->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_2_t v = {.bCur = volume};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, req, &v, sizeof(v));
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t entity = TU_U16_HIGH(req->wIndex);
    if (entity == S3A_ENTITY_CLOCK) return clock_get(rhport, req);
    if (entity == S3A_ENTITY_FU) return fu_get(rhport, req);
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *req, uint8_t *buf)
{
    uint8_t entity = TU_U16_HIGH(req->wIndex), sel = TU_U16_HIGH(req->wValue);
    TU_VERIFY(req->bRequest == AUDIO20_CS_REQ_CUR);
    if (entity == S3A_ENTITY_CLOCK && sel == AUDIO20_CS_CTRL_SAM_FREQ && req->wLength == 4) {
        uint32_t r = (uint32_t)((audio20_control_cur_4_t const *)buf)->bCur;
        for (int i = 0; i < N_RATES; i++) {
            if (rates[i] == r) {
                rate_set = r;
                event("PC set rate %lu Hz", (unsigned long)r);
                return true;
            }
        }
        event("PC asked for %lu Hz (refused)", (unsigned long)r);
        return false;
    }
    if (entity == S3A_ENTITY_FU && sel == AUDIO20_FU_CTRL_MUTE && req->wLength == 1) {
        muted = ((audio20_control_cur_1_t const *)buf)->bCur;
        event("PC mute %s", muted ? "ON" : "off");
        return true;
    }
    if (entity == S3A_ENTITY_FU && sel == AUDIO20_FU_CTRL_VOLUME && req->wLength == 2) {
        volume = ((audio20_control_cur_2_t const *)buf)->bCur;
        event("PC volume %.1f dB", volume / 256.0f);
        return true;
    }
    return false;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t itf = TU_U16_LOW(req->wIndex), a = TU_U16_LOW(req->wValue);
    if (itf == itf_stream && a != 0) {
        portENTER_CRITICAL(&lock);
        alt = a;
        opens++;
        lost_base += fr_lost;          // keep earlier streams' losses, start a fresh count
        fr_lost = 0;
        fr_started = false;
        portEXIT_CRITICAL(&lock);
        event("PC opened stream: %d-bit %lu Hz", a == 2 ? 24 : 16, (unsigned long)rate_set);
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *req)
{
    uint8_t itf = TU_U16_LOW(req->wIndex), a = TU_U16_LOW(req->wValue);
    if (itf == itf_stream && a == 0 && alt != 0) {
        alt = 0;
        event("PC closed stream");
    }
    return true;
}

// Every received isochronous packet (USB interrupt). Only counts; the samples are analysed in audio_task.
bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt)
{
    uint16_t frames = n_bytes / ((cur_alt == 2 ? 3 : 2) * S3A_CHANNELS);
    uint32_t fn = USB_DWC.dsts_reg.soffn & 0x7FF;     // full-speed frame number, wraps every 2048 ms
    portENTER_CRITICAL_ISR(&lock);
    if (!fr_started) {
        fr_started = true;
        fr_last = fn;
        fr_elapsed = fr_packets = fr_block = 0;
        fr_block_min = INT32_MAX;
        fr_lost = 0;
    } else {
        fr_elapsed += (fn - fr_last) & 0x7FF;
        fr_last = fn;
    }
    fr_packets++;
    int32_t behind = (int32_t)(fr_elapsed + 1 - fr_packets);
    if (behind < fr_block_min) fr_block_min = behind;
    if (++fr_block == 64) {
        if (fr_block_min > fr_lost) fr_lost = fr_block_min;
        fr_block = 0;
        fr_block_min = INT32_MAX;
    }
    pk_count++;
    pk_frames += frames;
    if (frames < pk_min) pk_min = frames;
    if (frames > pk_max) pk_max = frames;
    portEXIT_CRITICAL_ISR(&lock);
    return true;
}

// ---- sample analysis ----
static void audio_task(void *arg)
{
    static uint8_t buf[4 * S3A_EP_SZ_MAX];
    int32_t led_peak = 0;
    int64_t led_t = 0;
    for (;;) {
        if (alt == 0 || !tud_audio_mounted()) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        uint8_t bps = bytes_per_sample();
        uint16_t frame = bps * S3A_CHANNELS;
        uint16_t avail = tud_audio_available();
        avail -= avail % frame;
        if (avail == 0) {
            vTaskDelay(1);
            continue;
        }
        uint16_t n = tud_audio_read(buf, avail < sizeof(buf) ? avail : sizeof(buf) - sizeof(buf) % frame);
        double s2[2] = {0, 0};
        int32_t pk[2] = {0, 0};
        uint32_t clip = 0, bor = 0;
        bool diff = false;
        for (uint16_t i = 0; i + frame <= n; i += frame) {
            int32_t x[2];
            for (int c = 0; c < 2; c++) {
                const uint8_t *p = buf + i + c * bps;
                x[c] = bps == 2 ? (int32_t)(int16_t)(p[0] | p[1] << 8) * 256
                                : (int32_t)((uint32_t)(p[0] | p[1] << 8 | p[2] << 16) << 8) >> 8;
                int32_t a = x[c] < 0 ? -x[c] : x[c];
                if (a > pk[c]) pk[c] = a;
                if (a >= 0x7FFF00) clip++;
                s2[c] += (double)x[c] * x[c];
                bor |= (uint32_t)x[c];
            }
            diff |= x[0] != x[1];
        }
        portENTER_CRITICAL(&lock);
        for (int c = 0; c < 2; c++) {
            sq[c] += s2[c];
            if (pk[c] > peak[c]) peak[c] = pk[c];
        }
        clipped += clip;
        bits_or |= bor;
        not_mono |= diff;
        portEXIT_CRITICAL(&lock);

        // LED: peak over ~30 ms -> brightness, green -> yellow -> red as it gets louder
        int32_t p = pk[0] > pk[1] ? pk[0] : pk[1];
        if (p > led_peak) led_peak = p;
        int64_t now = esp_timer_get_time();
        if (now - led_t > 30000) {
            float db = led_peak ? 20.0f * log10f(led_peak / FULL) : -120.0f;
            float k = muted ? 0 : fminf(fmaxf((db + 50.0f) / 50.0f, 0.0f), 1.0f);   // -50..0 dBFS
            float r = fminf(1.0f, fmaxf(0.0f, (k - 0.4f) * 2.5f)), g = fminf(1.0f, (1.0f - k) * 2.5f), bright = k * k * 60;
            status_led_audio((uint8_t)(r * bright), (uint8_t)(g * bright), 0);
            led_peak = 0;
            led_t = now;
        }
    }
}

static float db(float v) { return v < 1.0f ? -140.0f : 20.0f * log10f(v / FULL); }

audio_stats_t audio_read_stats(void)
{
    int64_t now = esp_timer_get_time();
    audio_stats_t s = {0};
    portENTER_CRITICAL(&lock);
    float win = (now - win_start_us) / 1e6f;
    uint32_t frames = pk_frames;
    s.packets = pk_count;
    s.pkt_min = pk_count ? pk_min : 0;
    s.pkt_max = pk_max;
    for (int c = 0; c < 2; c++) {
        s.rms_db[c] = db(frames ? (float)sqrt(sq[c] / frames) : 0);
        s.peak_db[c] = db((float)peak[c]);
        sq[c] = 0;
        peak[c] = 0;
    }
    s.clipped = clipped;
    uint32_t bor = bits_or;
    s.mono = frames && !not_mono;
    pk_count = pk_frames = pk_max = clipped = bits_or = 0;
    pk_min = 0xFFFF;
    not_mono = false;
    win_start_us = now;
    s.dropped = lost_base + fr_lost;
    portEXIT_CRITICAL(&lock);

    s.streaming = alt != 0;
    s.bits = alt == 2 ? 24 : 16;
    s.bits_used = bor ? (uint8_t)(24 - __builtin_ctz(bor)) : 0;
    if (s.bits_used > s.bits && alt == 1) s.bits_used = 16;
    s.rate_set = rate_set;
    s.window_s = win;
    s.rate_measured = win > 0 ? frames / win : 0;
    s.muted = muted;
    s.volume_db = volume / 256.0f;
    s.opens = opens;
    return s;
}

void audio_start(uint8_t itf)
{
    itf_stream = itf;
    win_start_us = esp_timer_get_time();
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 9, NULL, 1);
}
