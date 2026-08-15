idf_component_register(SRCS         "m5nfc.c"
                       INCLUDE_DIRS "."
                       REQUIRES     nfc_transport
                       KCONFIG      "${CMAKE_CURRENT_LIST_DIR}/Kconfig")
