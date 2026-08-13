idf_component_register(SRCS         "access_control.c"
                       INCLUDE_DIRS "include"
                       REQUIRES     aliro_reader app_config
                       PRIV_REQUIRES driver esp_timer)
