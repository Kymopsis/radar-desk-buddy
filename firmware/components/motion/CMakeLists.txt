idf_component_register(
    SRCS "motion.c"
    INCLUDE_DIRS "include"
    REQUIRES radar_link servo esp_timer freertos device_cfg
)
