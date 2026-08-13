/*
 * SPDX-FileCopyrightText: 2026 Aliro HomeKey contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "serial_console.h"

#include "access_control.h"
#include "aliro_reader.h"
#include "app_config.h"
#include "net_manager.h"

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_check.h>
#include <esp_console.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_tag = "aliro/console";

static serial_console_hooks_t s_hooks;

/* --- commands ------------------------------------------------------------ */

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const app_config_t *cfg = app_config_get();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const esp_app_desc_t *app = esp_app_get_description();

    net_status_t net;
    net_manager_get_status(&net);

    const int64_t up_s = esp_timer_get_time() / 1000000;

    printf("device     %s\n", cfg->device_name);
    printf("chip       %s rev %d, %d core(s)\n", CONFIG_IDF_TARGET, chip.revision, chip.cores);
    printf("firmware   %s (IDF %s)\n", app ? app->version : "?", app ? app->idf_ver : "?");
    printf("uptime     %02d:%02d:%02d\n", (int)(up_s / 3600), (int)((up_s / 60) % 60), (int)(up_s % 60));
    printf("heap       %u free, %u low water\n", (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size());
    printf("network    %s%s%s\n",
           net.mode == NET_MODE_STA ? "station" : net.mode == NET_MODE_SETUP_AP ? "setup AP" : "offline",
           net.ssid[0] ? " on " : "", net.ssid);
    printf("address    %s\n", net.ip[0] ? net.ip : "-");
    printf("lock       %s (GPIO %d, active %s, %u ms)\n", access_control_is_locked() ? "locked" : "unlocked",
           cfg->lock.gpio, cfg->lock.active_low ? "low" : "high", (unsigned)cfg->lock.unlock_ms);
    printf("nfc        %s\n", s_hooks.transport_name ? s_hooks.transport_name() : "?");
    printf("creds      %u\n", (unsigned)(s_hooks.credential_count ? s_hooks.credential_count() : 0));
    printf("mqtt       %s\n", !s_hooks.mqtt_enabled || !s_hooks.mqtt_enabled() ? "disabled"
                              : s_hooks.mqtt_connected && s_hooks.mqtt_connected() ? "connected"
                                                                                   : "disconnected");
    return 0;
}

static int cmd_config(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Masked: the console is a serial port, but a photographed terminal is
     * still a leaked password. */
    char *json = app_config_to_json(app_config_get(), false);
    if (!json) {
        printf("out of memory\n");
        return 1;
    }
    printf("%s\n", json);
    free(json);
    return 0;
}

static int cmd_nfc(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    const nfc_hw_config_t *nfc = &app_config_get()->nfc;
    if (nfc->bus == NFC_BUS_SPI) {
        printf("bus        SPI%d\n", nfc->spi_host + 1);
        printf("pins       sck=%d miso=%d mosi=%d cs=%d\n", nfc->spi_sck, nfc->spi_miso, nfc->spi_mosi, nfc->spi_cs);
        printf("clock      %lu Hz\n", (unsigned long)nfc->spi_freq_hz);
    } else if (nfc->bus == NFC_BUS_I2C) {
        printf("bus        I2C\n");
        printf("pins       sda=%d scl=%d\n", nfc->i2c_sda, nfc->i2c_scl);
        printf("address    0x%02X\n", nfc->i2c_addr);
        printf("clock      %lu Hz\n", (unsigned long)nfc->i2c_freq_hz);
    } else {
        printf("bus        none\n");
    }
    printf("irq / rst  %d / %d\n", nfc->irq_pin, nfc->rst_pin);
    printf("driver     %s\n", s_hooks.transport_name ? s_hooks.transport_name() : "?");
    if (s_hooks.transport_name && strcmp(s_hooks.transport_name(), "stub") == 0) {
        printf("\nThe stub transport never detects a device. No chip driver is\n"
               "written yet, so the wiring above is stored and validated but\n"
               "nothing drives it.\n");
    }
    return 0;
}

static int cmd_identity(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("group id   %s\n", app_config_get()->group_id_hex);
    return aliro_reader_log_identity() == ESP_OK ? 0 : 1;
}

static int cmd_unlock(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const esp_err_t err = access_control_unlock();
    printf(err == ESP_OK ? "unlocked\n" : "failed: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: wifi <ssid> [password]\n");
        return 1;
    }

    app_config_t cfg = *app_config_get();
    snprintf(cfg.net.ssid, sizeof(cfg.net.ssid), "%s", argv[1]);
    if (argc >= 3) {
        snprintf(cfg.net.password, sizeof(cfg.net.password), "%s", argv[2]);
    }

    char reason[128] = {0};
    if (app_config_save(&cfg, reason, sizeof(reason)) != ESP_OK) {
        printf("rejected: %s\n", reason);
        return 1;
    }
    printf("saved. restart to join '%s'\n", cfg.net.ssid);
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (app_config_reset() != ESP_OK) {
        printf("failed\n");
        return 1;
    }
    printf("configuration erased. restart to apply.\n");
    return 0;
}

static int cmd_restart(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("restarting\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

/* --- lifecycle ----------------------------------------------------------- */

esp_err_t serial_console_start(const serial_console_hooks_t *hooks)
{
    if (hooks) {
        s_hooks = *hooks;
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "aliro>";
    repl_cfg.max_cmdline_length = 128;

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t dev_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&dev_cfg, &repl_cfg, &repl), k_tag, "UART REPL failed");
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl), k_tag,
                        "USB serial JTAG REPL failed");
#else
#error "No supported console device; enable UART or USB Serial/JTAG console in menuconfig"
#endif

    const esp_console_cmd_t commands[] = {
        {.command = "status", .help = "Device, network, reader and MQTT state", .func = cmd_status},
        {.command = "config", .help = "Print the running configuration", .func = cmd_config},
        {.command = "nfc", .help = "NFC wiring and driver state", .func = cmd_nfc},
        {.command = "identity", .help = "Reader group id, sub-id and public key", .func = cmd_identity},
        {.command = "unlock", .help = "Drive the lock output now", .func = cmd_unlock},
        {.command = "wifi", .help = "wifi <ssid> [password] - set and save credentials", .func = cmd_wifi},
        {.command = "factory-reset", .help = "Erase the stored configuration", .func = cmd_factory_reset},
        {.command = "restart", .help = "Reboot the device", .func = cmd_restart},
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&commands[i]), k_tag, "register '%s' failed",
                            commands[i].command);
    }
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), k_tag, "register help failed");

    ESP_RETURN_ON_ERROR(esp_console_start_repl(repl), k_tag, "REPL start failed");

    ESP_LOGI(k_tag, "debug console ready - type 'help' for commands");
    return ESP_OK;
}
