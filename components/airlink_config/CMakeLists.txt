idf_component_register(SRCS "airlink_config.c" INCLUDE_DIRS "include"
    REQUIRES airlink_core nvs_flash esp_system esp_netif)
