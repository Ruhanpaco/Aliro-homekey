idf_component_register(SRCS         "aliro_reader.c"
                       INCLUDE_DIRS "include"
                       REQUIRES     nfc_transport esp_aliro_lib
                       PRIV_REQUIRES esp_timer nvs_flash)
