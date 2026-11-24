idf_component_register(
    SRCS "main.c" "net.c" "usb.c" "status_led.c" "mic.c" "audio.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "index.html"
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server app_update esp_app_format bootloader_support
                  esp_driver_gpio esp_driver_i2s esp_timer
)

# TinyUSB audio class settings (esp_tinyusb's tusb_config.h has none): force-include our header into TinyUSB.
# PUBLIC, so esp_tinyusb and this component see exactly the same CFG_TUD_* values.
idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
    set(tusb_component tinyusb)
else()
    set(tusb_component espressif__tinyusb)
endif()
idf_component_get_property(tusb_lib ${tusb_component} COMPONENT_LIB)
target_compile_options(${tusb_lib} PUBLIC "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/tusb_audio_cfg.h")
target_compile_options(${COMPONENT_LIB} PRIVATE "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/tusb_audio_cfg.h")
