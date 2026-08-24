idf_component_register(SRCS "airlink_ota.c" INCLUDE_DIRS "include"
    REQUIRES airlink_core airlink_led app_update esp_app_format mbedtls esp_http_server freertos)
