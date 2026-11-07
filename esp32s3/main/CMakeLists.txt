idf_component_register(SRCS "main.c" "usb_descriptors.c" "audio_link.c" "web_server.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES usb esp_hw_support tinyusb esp_driver_i2s esp_driver_uart
                                  esp_driver_gpio esp_wifi esp_http_server esp_netif
                                  esp_event esp_timer nvs_flash)
