set(srcs "gdey042z98.c"
    "spi_master_example_main.c"
    "calendar_ui.c"
    "weather.c"
    "weather_parse.c"
    )

idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES driver esp_wifi esp_event esp_netif esp_http_client esp_http_server esp_timer nvs_flash esp_adc)
