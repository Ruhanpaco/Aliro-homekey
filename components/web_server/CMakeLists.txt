# The UI is embedded in the application rather than kept on a filesystem
# partition, so one firmware.bin carries the reader and its interface together.
# The cost is that every byte of it is app-partition space, so compress it
# first: gzip takes ~77% off and browsers inflate it from Content-Encoding.
#
# Guarded on CMAKE_BUILD_EARLY_EXPANSION. ESP-IDF runs every component's
# CMakeLists twice -- once in a cut-down pass that only collects REQUIRES, with
# most variables (${python} among them) undefined. Doing the work in that pass
# fails with an empty command line.
set(_embed_files "")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(_web_dir "${CMAKE_CURRENT_BINARY_DIR}/web")
    file(MAKE_DIRECTORY "${_web_dir}")

    if(NOT python)
        set(python "python3")
    endif()

    foreach(_page index setup)
        set(_src "${COMPONENT_DIR}/web/${_page}.html")
        set(_gz "${_web_dir}/${_page}.html.gz")

        execute_process(
            COMMAND "${python}" "${COMPONENT_DIR}/../../tools/gzip_file.py" "${_src}" "${_gz}"
            RESULT_VARIABLE _gz_result
            OUTPUT_VARIABLE _gz_output
            ERROR_VARIABLE _gz_output)
        if(NOT _gz_result EQUAL 0)
            message(FATAL_ERROR "web_server: could not compress ${_src}\n${_gz_output}")
        endif()
        message(STATUS "web_server: ${_gz_output}")

        # Re-run CMake, and so re-compress, whenever the page is edited.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")
        list(APPEND _embed_files "${_gz}")
    endforeach()
endif()

idf_component_register(SRCS         "web_server.c"
                       INCLUDE_DIRS "include"
                       REQUIRES     app_config net_manager matter_lock
                       PRIV_REQUIRES esp_http_server json esp_app_format esp_hw_support
                                     esp_timer esp_system freertos
                                     app_update esp_partition esp_wifi
                       EMBED_FILES  ${_embed_files})
