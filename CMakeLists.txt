cmake_minimum_required(VERSION 3.16)

# esp-matter is optional. With ESP_MATTER_PATH set -- which is the case inside
# the espressif/esp-matter container, and after sourcing its export.sh locally
# -- its components join the build and CONFIG_ALIRO_MATTER_ENABLE can turn the
# Matter door lock on. Without it, this is an ordinary ESP-IDF project and
# components/matter_lock compiles down to stubs.
if(DEFINED ENV{ESP_MATTER_PATH})
    set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
    set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

    set(EXTRA_COMPONENT_DIRS
        "${MATTER_SDK_PATH}/config/esp32/components"
        "${ESP_MATTER_PATH}/components")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(aliro_homekey)

if(CONFIG_ALIRO_MATTER_ENABLE)
    # What the Matter SDK expects of anything that includes its headers. -Os
    # is not a preference here: at -O2 the combined image does not fit an OTA
    # slot on a 4 MB board.
    idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
    idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
    idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
endif()
