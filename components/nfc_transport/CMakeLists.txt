idf_component_register(SRCS         "nfc_transport_stub.c" "nfc_transport_select.c" "pn532.c"
                       INCLUDE_DIRS "include"
                       REQUIRES     app_config
                       PRIV_REQUIRES driver esp_timer)
