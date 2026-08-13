idf_component_register(SRCS         "web_server.c" "log_ring.c"
                       INCLUDE_DIRS "include"
                       REQUIRES     app_config net_manager
                       PRIV_REQUIRES esp_http_server json esp_app_format esp_hw_support
                                     esp_timer esp_system freertos
                                     app_update esp_partition esp_wifi
                       EMBED_FILES  "web/index.html")
