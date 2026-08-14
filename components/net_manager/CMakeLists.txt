idf_component_register(SRCS         "net_manager.c" "dns_hijack.c"
                       INCLUDE_DIRS "include"
                       REQUIRES     app_config esp_wifi esp_netif
                       PRIV_REQUIRES esp_event lwip esp_hw_support esp_timer)
