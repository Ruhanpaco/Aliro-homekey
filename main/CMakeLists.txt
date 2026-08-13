idf_component_register(SRCS         "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES     aliro_reader access_control nfc_transport app_config net_manager web_server mqtt_manager
                       PRIV_REQUIRES nvs_flash)

# Development identity. Generated on demand and never committed: see
# main/certs/README.md and tools/gen_reader_identity.sh.
set(ALIRO_CERTS_DIR "${CMAKE_CURRENT_LIST_DIR}/certs")
if(NOT EXISTS "${ALIRO_CERTS_DIR}/reader_privkey.pem")
    message(STATUS "aliro: no development identity found, generating one in main/certs")
    execute_process(
        COMMAND "${CMAKE_CURRENT_LIST_DIR}/../tools/gen_reader_identity.sh" "${ALIRO_CERTS_DIR}"
        RESULT_VARIABLE _gen_result
        OUTPUT_VARIABLE _gen_output
        ERROR_VARIABLE  _gen_output)
    if(NOT _gen_result EQUAL 0)
        message(FATAL_ERROR
            "aliro: could not generate a development identity (openssl required).\n"
            "Run tools/gen_reader_identity.sh manually.\n${_gen_output}")
    endif()
endif()

target_add_binary_data(${COMPONENT_TARGET} "certs/reader_pubkey.pem" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "certs/reader_privkey.pem" TEXT)
target_add_binary_data(${COMPONENT_TARGET} "certs/credential_pubkey.pem" TEXT)
