idf_component_register(SRCS "airlink_mavlink.c" "airlink_router.c"
    INCLUDE_DIRS "include" REQUIRES airlink_core esp_timer freertos)
