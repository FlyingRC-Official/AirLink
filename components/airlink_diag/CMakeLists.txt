idf_component_register(SRCS "airlink_diag.c" INCLUDE_DIRS "include"
    REQUIRES airlink_core airlink_router airlink_uart espcoredump esp_system esp_timer nvs_flash)
