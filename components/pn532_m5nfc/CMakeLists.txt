idf_component_register(SRCS         "m5nfc.c"
                       INCLUDE_DIRS "."
                       REQUIRES     pn532
                       KCONFIG      "${CMAKE_CURRENT_LIST_DIR}/Kconfig")
