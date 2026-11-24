// Wi-Fi (credentials from secrets.h), mDNS "s3-playground.local", and a small HTTP server:
//   GET  /           status page (index.html, embedded)
//   GET  /api/info   JSON status
//   POST /ota        new firmware image (body = build/s3_playground.bin, header X-OTA-Token)
#include "net.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "mic.h"
#include "status_led.h"
#include "usb.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy main/secrets.h.example to main/secrets.h and fill it in"
#endif

static const char *TAG = "net";
static EventGroupHandle_t events;
#define GOT_IP BIT0
static esp_netif_t *sta;
static int retries;

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_timer_handle_t reconnect_timer;

static void reconnect(void *arg) { esp_wifi_connect(); }

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(events, GOT_IP);
        status_led_state(++retries > 10 ? LED_WIFI_FAILED : LED_WIFI_JOINING);
        esp_timer_stop(reconnect_timer);                                  // keep trying forever, gently
        esp_timer_start_once(reconnect_timer, (retries > 10 ? 5000 : 500) * 1000ULL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got IP " IPSTR " -> http://" HOSTNAME ".local/", IP2STR(&e->ip_info.ip));
        retries = 0;
        status_led_state(LED_OK);
        xEventGroupSetBits(events, GOT_IP);
    }
}

// ---- HTTP handlers ----
static esp_err_t get_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t get_info(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(sta, &ip);
    wifi_ap_record_t ap = {0};
    int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    usb_status_t u = usb_status();
    char buf[640];
    snprintf(buf, sizeof(buf),
             "{\"project\":\"%s\",\"version\":\"%s\",\"idf\":\"%s\",\"built\":\"%s %s\","
             "\"uptime_s\":%lld,\"heap_free\":%u,\"psram_free\":%u,\"reset_reason\":%d,"
             "\"ip\":\"" IPSTR "\",\"rssi\":%d,\"partition\":\"%s\","
             "\"usb\":{\"mounted\":%s,\"suspended\":%s,\"cdc_open\":%s,\"key_presses\":%lu}}",
             app->project_name, app->version, app->idf_ver, app->date, app->time, esp_timer_get_time() / 1000000,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             esp_reset_reason(), IP2STR(&ip.ip), rssi, run->label, u.mounted ? "true" : "false",
             u.suspended ? "true" : "false", u.cdc_open ? "true" : "false", (unsigned long)u.key_presses);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t get_mic(httpd_req_t *req)
{
    mic_level_t m = mic_read_level();
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"rms_db\":%.1f,\"peak_db\":%.1f,\"alive\":%s,\"rate\":%d}", m.rms_db, m.peak_db,
             m.alive ? "true" : "false", m.rate);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, buf);
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t post_ota(httpd_req_t *req)
{
    char token[80] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token", token, sizeof(token)) != ESP_OK || strcmp(token, OTA_TOKEN) != 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "bad or missing X-OTA-Token");
        return ESP_FAIL;
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target || req->content_len == 0 || req->content_len > target->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no image or image too big");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: %d bytes -> %s", req->content_len, target->label);
    status_led_state(LED_OTA);

    static char buf[4096];
    esp_ota_handle_t ota = 0;
    size_t done = 0;
    bool started = false;
    const char *err = NULL;
    while (done < req->content_len) {
        int n = httpd_req_recv(req, buf, sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            err = "upload interrupted";
            break;
        }
        if (!started) {
            // refuse images that are not this project (e.g. a MicroPython .bin by mistake)
            size_t off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
            const esp_app_desc_t *nd = (const esp_app_desc_t *)(buf + off);
            if (n < (int)(off + sizeof(esp_app_desc_t)) || nd->magic_word != ESP_APP_DESC_MAGIC_WORD ||
                strcmp(nd->project_name, esp_app_get_description()->project_name) != 0) {
                err = "not an s3_playground image";
                break;
            }
            ESP_LOGI(TAG, "OTA: new version %s", nd->version);
            if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
                err = "ota_begin failed";
                break;
            }
            started = true;
        }
        if (esp_ota_write(ota, buf, n) != ESP_OK) {
            err = "flash write failed";
            break;
        }
        done += n;
    }
    if (!err && esp_ota_end(ota) != ESP_OK) {
        err = "image did not validate";
        started = false;           // esp_ota_end frees the handle even on failure
    } else if (err && started) {
        esp_ota_abort(ota);
    }
    if (!err && esp_ota_set_boot_partition(target) != ESP_OK) {
        err = "set_boot_partition failed";
    }
    if (err) {
        ESP_LOGE(TAG, "OTA failed: %s", err);
        status_led_state(LED_OK);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA ok, rebooting into %s", target->label);
    httpd_resp_sendstr(req, "ok, rebooting\n");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void start_http(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.recv_wait_timeout = 10;
    cfg.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_index},
        {.uri = "/api/info", .method = HTTP_GET, .handler = get_info},
        {.uri = "/api/mic", .method = HTTP_GET, .handler = get_mic},
        {.uri = "/ota", .method = HTTP_POST, .handler = post_ota},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}

void net_start(void)
{
    events = xEventGroupCreate();
    const esp_timer_create_args_t ta = {.callback = reconnect, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&ta, &reconnect_timer));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, HOSTNAME);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));      // snappy web page; USB powers us anyway
    status_led_state(LED_WIFI_JOINING);
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(HOSTNAME);
    mdns_instance_name_set("S3 Playground");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    start_http();
}

bool net_wait_ip(int timeout_ms)
{
    return xEventGroupWaitBits(events, GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & GOT_IP;
}
