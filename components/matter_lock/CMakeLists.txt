# Every source, every time, with the *contents* switched by
# CONFIG_ALIRO_MATTER_ENABLE rather than the file list.
#
# Not a style choice. ESP-IDF evaluates a component's CMakeLists twice, and the
# first pass -- the one that collects REQUIRES -- runs before component Kconfig
# files have been processed, so CONFIG_ symbols from this project do not exist
# yet. An `if(CONFIG_ALIRO_MATTER_ENABLE)` around idf_component_register
# therefore took the false branch when the requirements were collected and the
# true branch when the sources were compiled: the Matter sources built with no
# Matter include path, and the first #include failed.
#
# ESP_MATTER_PATH is an environment variable, which both passes can see, and it
# is exactly the condition that decides whether these components exist at all.
set(_requires aliro_reader)
if(DEFINED ENV{ESP_MATTER_PATH})
    list(APPEND _requires esp_matter chip)
endif()

# WHOLE_ARCHIVE because the dependency here is a cycle the linker cannot walk.
# We call into esp_matter, and esp_matter's door lock cluster calls back into
# emberAfDoorLockClusterInitCallback, which lives here. Archives are scanned in
# order and only for symbols already undefined, so the real sequence was: this
# archive scanned with nothing yet wanting it, then main pulls in
# matter_lock_start, which pulls in esp_matter, whose cluster table then wants
# our callback -- from a position the linker has already gone past.
#
#     undefined reference to `_Z34emberAfDoorLockClusterInitCallbackt'
#
# Espressif's example never meets this because its callbacks sit in main.
idf_component_register(SRCS         "matter_lock.cpp" "matter_aliro_delegate.cpp" "matter_lock_store.cpp"
                                    "matter_lock_disabled.c"
                       INCLUDE_DIRS "include"
                       REQUIRES     ${_requires}
                       PRIV_REQUIRES nvs_flash
                       WHOLE_ARCHIVE)

# CHIP_HAVE_CONFIG_H is how the Matter headers find their generated
# configuration. -Wno-overloaded-virtual because the Door Lock server
# deliberately overloads emberAfPluginDoorLockGetSchedule by argument type, and
# it is scoped to C++ because GCC rejects it on a C command line.
target_compile_options(${COMPONENT_LIB} PRIVATE
                       "-DCHIP_HAVE_CONFIG_H"
                       "$<$<COMPILE_LANGUAGE:CXX>:-Wno-overloaded-virtual>")
