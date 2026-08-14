idf_component_register(SRCS "airlink_board.c" INCLUDE_DIRS "include"
    REQUIRES airlink_core esp_system spi_flash esp_psram esp_driver_gpio esp_timer)
