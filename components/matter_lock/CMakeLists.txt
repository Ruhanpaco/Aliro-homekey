# Two very different components behind one header.
#
# With CONFIG_ALIRO_MATTER_ENABLE the real thing is built, which needs esp-matter
# on the include path -- see the EXTRA_COMPONENT_DIRS block in the top-level
# CMakeLists.txt. Without it only the stubs are compiled, so an ordinary
# ESP-IDF build (no esp-matter installed at all) still links and every caller
# keeps the same API.
if(CONFIG_ALIRO_MATTER_ENABLE)
    idf_component_register(SRCS         "matter_lock.cpp" "matter_aliro_delegate.cpp" "matter_lock_store.cpp"
                           INCLUDE_DIRS "include"
                           REQUIRES     esp_matter chip aliro_reader
                           PRIV_REQUIRES nvs_flash)

    # CHIP_HAVE_CONFIG_H is how the Matter headers find their generated
    # configuration; -Wno-overloaded-virtual because the Door Lock server
    # deliberately overloads emberAfPluginDoorLockGetSchedule by argument type.
    target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H" "-Wno-overloaded-virtual")
else()
    idf_component_register(SRCS         "matter_lock_disabled.c"
                           INCLUDE_DIRS "include")
endif()
