// S3 Playground firmware: status LED, USB (TinyUSB composite), Wi-Fi + OTA.
// A freshly OTA'd image must reach Wi-Fi within VERIFY_MS or the bootloader rolls back to the previous one.
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "net.h"
#include "nvs_flash.h"
#include "status_led.h"
#include "usb.h"

static const char *TAG = "main";
#define VERIFY_MS 60000

void app_main(void)
{
    status_led_init();
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "%s %s (IDF %s)", app->project_name, app->version, app->idf_ver);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    usb_start();
    net_start();

    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "new image on %s: waiting for Wi-Fi to confirm it", run->label);
        if (net_wait_ip(VERIFY_MS)) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "image confirmed");
        } else {
            ESP_LOGE(TAG, "no Wi-Fi within %d s: rolling back", VERIFY_MS / 1000);
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }
}
