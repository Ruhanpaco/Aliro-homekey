idf_component_register(SRCS         "mqtt_manager.c"
                       INCLUDE_DIRS "include"
                       REQUIRES     app_config
                       PRIV_REQUIRES mqtt cjson access_control esp_app_format)
