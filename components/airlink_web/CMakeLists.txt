idf_component_register(SRCS "airlink_web.c" INCLUDE_DIRS "include"
    EMBED_FILES "${PROJECT_DIR}/web/index.html.gz"
    REQUIRES airlink_core airlink_config airlink_router airlink_uart airlink_wifi airlink_usb
             airlink_can airlink_diag airlink_ota airlink_led esp_http_server
             esp_app_format esp_system mbedtls espressif__cjson)
