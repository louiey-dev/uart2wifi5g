idf_component_register(SRCS "main.c" "bsp_softap.c" "bsp_uart_handler.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_driver_uart esp_driver_gpio esp_netif esp_event lwip log
                    INCLUDE_DIRS ".")
