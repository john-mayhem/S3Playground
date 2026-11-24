// Native USB port (TinyUSB): one composite device "S3 Playground" with
//   - CDC serial port: a tiny command shell (help / info / led r g b)
//   - HID consumer control: the BOOT button sends Play/Pause to the PC
//   - "S3 Audio": a UAC2 stereo speaker, up to 96 kHz / 24-bit (audio.c analyses what arrives)
#include "usb.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio.h"
#include "status_led.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

static const char *TAG = "usb";

#define BOOT_GPIO 0
#define USB_VID 0x303A    // Espressif
#define USB_PID 0x4010    // our own pick; change bcdDevice when the descriptors change so Windows re-reads them
#define USB_BCD 0x0200    // 0x0200: added S3 Audio

enum { ITF_CDC = 0, ITF_CDC_DATA, ITF_HID, ITF_AUDIO_CTRL, ITF_AUDIO_STREAM, ITF_COUNT };
enum { STR_LANG, STR_MANUF, STR_PRODUCT, STR_SERIAL, STR_CDC, STR_HID, STR_AUDIO, STR_COUNT };

#define EP_CDC_NOTIF 0x81
#define EP_CDC_OUT   0x02
#define EP_CDC_IN    0x82
#define EP_HID_IN    0x83
#define EP_AUDIO_OUT 0x04

static const uint8_t hid_report[] = {TUD_HID_REPORT_DESC_CONSUMER()};

#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN + S3A_DESC_LEN)
static const uint8_t config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC, STR_CDC, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(ITF_HID, STR_HID, HID_ITF_PROTOCOL_NONE, sizeof(hid_report), EP_HID_IN, 16, 10),
    S3A_DESCRIPTOR(ITF_AUDIO_CTRL, ITF_AUDIO_STREAM, EP_AUDIO_OUT, STR_AUDIO),
};

static const tusb_desc_device_t device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,        // composite with IADs
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = USB_BCD,
    .iManufacturer = STR_MANUF,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static char serial_str[13];
static const char *strings[STR_COUNT] = {
    [STR_LANG] = (const char[]){0x09, 0x04},   // English (US)
    [STR_MANUF] = "S3Playground",
    [STR_PRODUCT] = "S3 Playground",
    [STR_SERIAL] = serial_str,
    [STR_CDC] = "S3 Playground Serial",
    [STR_HID] = "S3 Playground Keys",
    [STR_AUDIO] = "S3 Audio",
};

static volatile bool cdc_dtr;
static volatile uint32_t key_presses;

// ---- HID (TinyUSB calls these) ----
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) { return hid_report; }

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t *buf, uint16_t len)
{
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t const *buf, uint16_t len) {}

static void send_consumer_key(uint16_t usage)
{
    if (tud_suspended()) {
        tud_remote_wakeup();
        return;
    }
    if (!tud_mounted()) {
        return;
    }
    for (int i = 0; i < 20 && !tud_hid_ready(); i++) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    tud_hid_report(0, &usage, sizeof(usage));
    vTaskDelay(pdMS_TO_TICKS(30));
    uint16_t none = 0;
    for (int i = 0; i < 20 && !tud_hid_ready(); i++) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    tud_hid_report(0, &none, sizeof(none));
}

static void button_task(void *arg)
{
    gpio_config_t io = {.pin_bit_mask = 1ULL << BOOT_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = 1};
    gpio_config(&io);
    int stable = 1, count = 0;
    for (;;) {
        int v = gpio_get_level(BOOT_GPIO);
        count = (v == stable) ? 0 : count + 1;
        if (count >= 3) {             // 30 ms debounce
            stable = v;
            count = 0;
            if (stable == 0) {
                key_presses++;
                status_led_flash(20, 20, 20);
                send_consumer_key(HID_USAGE_CONSUMER_PLAY_PAUSE);
                ESP_LOGI(TAG, "BOOT pressed -> Play/Pause");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---- CDC shell ----
static void cdc_print(const char *s)
{
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)s, strlen(s));
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}

static void run_command(char *line)
{
    char out[256];
    int r, g, b;
    if (strcmp(line, "help") == 0 || line[0] == 0) {
        cdc_print("commands: info | led <r> <g> <b> (0-255, short flash) | help\r\n");
    } else if (strcmp(line, "info") == 0) {
        const esp_app_desc_t *app = esp_app_get_description();
        snprintf(out, sizeof(out), "%s %s (IDF %s), up %llds, key presses %lu\r\n", app->project_name, app->version,
                 app->idf_ver, esp_timer_get_time() / 1000000, (unsigned long)key_presses);
        cdc_print(out);
    } else if (sscanf(line, "led %d %d %d", &r, &g, &b) == 3) {
        status_led_flash(r & 255, g & 255, b & 255);
        cdc_print("ok\r\n");
    } else {
        snprintf(out, sizeof(out), "unknown: '%.64s' (try help)\r\n", line);
        cdc_print(out);
    }
}

static char line_buf[96];
static size_t line_len;

static void cdc_rx(int itf, cdcacm_event_t *event)
{
    uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
    size_t n = 0;
    if (tinyusb_cdcacm_read(itf, buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &n) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\r' || c == '\n') {
            cdc_print("\r\n");
            line_buf[line_len] = 0;
            run_command(line_buf);
            line_len = 0;
            cdc_print("> ");
        } else if ((c == 8 || c == 127) && line_len) {   // backspace
            line_len--;
            cdc_print("\b \b");
        } else if (c >= 32 && line_len < sizeof(line_buf) - 1) {
            line_buf[line_len++] = c;
            tinyusb_cdcacm_write_queue_char(itf, c);     // echo
            tinyusb_cdcacm_write_flush(itf, 0);
        }
    }
}

static void cdc_line_state(int itf, cdcacm_event_t *event)
{
    bool dtr = event->line_state_changed_data.dtr;
    if (dtr && !cdc_dtr) {
        cdc_print("\r\nS3 Playground serial. Type 'help'.\r\n> ");
    }
    cdc_dtr = dtr;
}

void usb_start(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(serial_str, sizeof(serial_str), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const tinyusb_config_t cfg = {
        .device_descriptor = &device_desc,
        .string_descriptor = strings,
        .string_descriptor_count = STR_COUNT,
        .configuration_descriptor = config_desc,
    };
    audio_start(ITF_AUDIO_STREAM);
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));

    const tinyusb_config_cdcacm_t acm = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx,
        .callback_line_state_changed = cdc_line_state,
    };
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm));
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "USB started: %04X:%04X serial %s", USB_VID, USB_PID, serial_str);
}

usb_status_t usb_status(void)
{
    return (usb_status_t){
        .mounted = tud_mounted(),
        .suspended = tud_suspended(),
        .cdc_open = cdc_dtr,
        .key_presses = key_presses,
    };
}
