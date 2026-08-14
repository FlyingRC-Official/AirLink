idf_component_register(SRCS "airlink_wifi.c" INCLUDE_DIRS "include"
    REQUIRES airlink_core airlink_config airlink_router esp_wifi esp_netif nvs_flash lwip esp_timer esp_system)
