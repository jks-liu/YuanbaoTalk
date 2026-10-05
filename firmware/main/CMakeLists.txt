idf_component_register(SRCS "main.c" "yt_protocol.c" "yt_settings.c" "yt_router.c" "yt_gateway.c" "yt_audio.c" "yt_selftest.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer esp_driver_gpio esp_driver_i2s
    espressif__cjson espressif__esp_websocket_client espressif__esp_audio_codec espressif__esp-sr)
