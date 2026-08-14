/* Minimal spi_master stub: enough of the API for the PN532 driver to compile
 * against a simulated chip on the host. The test provides the transmit. */
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef enum { SPI1_HOST = 0, SPI2_HOST = 1, SPI3_HOST = 2 } spi_host_device_t;
#define SPI_DMA_CH_AUTO 3
#define SPI_DEVICE_BIT_LSBFIRST (1 << 3)

typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz; } spi_bus_config_t;
typedef struct { int clock_speed_hz, mode, spics_io_num, queue_size; uint32_t flags; } spi_device_interface_config_t;
typedef struct spi_device_t *spi_device_handle_t;
typedef struct { size_t length; const void *tx_buffer; void *rx_buffer; } spi_transaction_t;

esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *cfg, int dma);
esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *cfg, spi_device_handle_t *out);
esp_err_t spi_device_polling_transmit(spi_device_handle_t dev, spi_transaction_t *t);
