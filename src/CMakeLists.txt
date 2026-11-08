idf_component_register(
    SRCS
        "main.cpp"
        "app_config.cpp"
        "app_state.cpp"
        "audio_pipe.cpp"
        "bt_manager.cpp"
        "log_buffer.cpp"
        "nvs_persistence.cpp"
        "web_server.cpp"
        "wifi_manager.cpp"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "web/index.html"
    REQUIRES
        app_update
        bt
        esp_app_format
        esp_event
        esp_http_server
        esp_netif
        esp_psram
        esp_system
        esp_timer
        esp_wifi
        freertos
        log
        mbedtls
        nvs_flash
        espressif__mdns
        sendspin__sendspin-cpp
        bblanchon__arduinojson
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
