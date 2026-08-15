# The UI is embedded in the application rather than kept on a filesystem
# partition, so one firmware.bin carries the reader and its interface together.
# The cost is that every byte of it is app-partition space -- and it is ~98 KB
# of HTML, CSS and JavaScript.
#
# So compress it first. gzip takes about 77% off, browsers decompress it
# transparently from a Content-Encoding header, and the ~76 KB saved stays
# saved. Done through Python rather than a gzip binary because ESP-IDF's build
# system is Python and a host gzip is not guaranteed.
set(_web_dir "${CMAKE_CURRENT_BINARY_DIR}/web")
file(MAKE_DIRECTORY "${_web_dir}")

set(_embed_files "")
foreach(_page index setup)
    set(_src "${COMPONENT_DIR}/web/${_page}.html")
    set(_gz "${_web_dir}/${_page}.html.gz")

    # mtime=0 so the same source always produces the same bytes.
    execute_process(
        COMMAND ${python} -c
            "import gzip,sys; open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(),9,mtime=0))"
            "${_src}" "${_gz}"
        RESULT_VARIABLE _gz_result)
    if(NOT _gz_result EQUAL 0)
        message(FATAL_ERROR "web_server: could not compress ${_src}")
    endif()

    # Re-run CMake, and so re-compress, whenever the page is edited.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}")
    list(APPEND _embed_files "${_gz}")
endforeach()

idf_component_register(SRCS         "web_server.c" "log_ring.c"
                       INCLUDE_DIRS "include"
                       REQUIRES     app_config net_manager
                       PRIV_REQUIRES esp_http_server json esp_app_format esp_hw_support
                                     esp_timer esp_system freertos
                                     app_update esp_partition esp_wifi
                       EMBED_FILES  ${_embed_files})
