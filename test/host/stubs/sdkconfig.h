#pragma once
#define CONFIG_IDF_TARGET_ESP32 1
#define CONFIG_IDF_TARGET "esp32"
#define CONFIG_ALIRO_DEFAULT_DEVICE_NAME "aliro-homekey"
#define CONFIG_ALIRO_READER_GROUP_ID "00112233445566778899AABBCCDDEEFF"
#define CONFIG_ALIRO_NFC_SPI_SCK 18
#define CONFIG_ALIRO_NFC_SPI_MISO 19
#define CONFIG_ALIRO_NFC_SPI_MOSI 23
#define CONFIG_ALIRO_NFC_SPI_CS 5
#define CONFIG_ALIRO_NFC_IRQ (-1)
#define CONFIG_ALIRO_NFC_RST (-1)
#define CONFIG_ALIRO_LOCK_GPIO 2
/* Left undefined on purpose: ESP-IDF does not define a bool Kconfig that
 * is 'n'. Defining it as 0 here would hide code that reads it as a value. */
/* #undef CONFIG_ALIRO_LOCK_ACTIVE_LOW */
#define CONFIG_ALIRO_LOCK_UNLOCK_MS 3000
#define CONFIG_ALIRO_AP_PASSWORD "aliro1234"
