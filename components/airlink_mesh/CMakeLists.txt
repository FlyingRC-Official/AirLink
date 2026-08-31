idf_component_register(
    SRCS "airlink_mesh.c" "airlink_mesh_codec.c" "airlink_mesh_config.c"
    INCLUDE_DIRS "include"
    REQUIRES airlink_core airlink_config airlink_router airlink_uart airlink_ota
             esp_wifi esp_netif nvs_flash mbedtls espressif__cjson esp_timer
             esp_app_format freertos log)
