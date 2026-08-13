idf_component_register(SRCS         "serial_console.c"
                       INCLUDE_DIRS "include"
                       REQUIRES     app_config
                       PRIV_REQUIRES console access_control aliro_reader net_manager
                                     esp_system esp_hw_support esp_app_format)
