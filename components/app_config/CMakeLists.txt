idf_component_register(SRCS         "app_config.c" "gpio_rules.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES cjson nvs_flash driver esp_hw_support)
