idf_component_register(SRCS "board.c" "ui.c" "sd_card.c" "mirror.c" "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_lcd esp_driver_spi esp_driver_sdspi esp_driver_gpio esp_wifi nvs_flash esp_timer esp_netif fatfs lwip
                       PRIV_REQUIRES spi_flash)
