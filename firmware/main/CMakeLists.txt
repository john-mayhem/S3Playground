idf_component_register(
    SRCS "main.c" "net.c" "usb.c" "status_led.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "index.html"
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server app_update esp_app_format bootloader_support
                  esp_driver_gpio esp_timer
)
