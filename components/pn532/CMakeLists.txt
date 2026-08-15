# The bare PN532 driver: no configuration system, no app_config, nothing above
# the chip. That is what lets the same file serve this firmware and an
# esp-matter door lock, which share no configuration code at all.
idf_component_register(SRCS         "pn532.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES driver esp_timer)
