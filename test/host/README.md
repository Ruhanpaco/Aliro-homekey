# Host tests

```bash
test/host/run.sh
```

Runs the configuration logic — validation, JSON patching, secret masking,
persistence, and the GPIO rules — on the development machine. No ESP-IDF, no
board.

This exists because the interesting failure modes in `app_config` are not
hardware failures. A pin table that offers a flash pin, a partial update that
wipes a stored Wi-Fi password, a value that wraps into a valid range instead of
being rejected: all of that is testable in a second on a laptop, and all of it
is painful to find over a serial console.

The ESP-IDF headers used by `app_config` are stubbed in `stubs/`, and NVS is a
fake keyed on a single in-memory string. The stubs model an **ESP32**: GPIO
0–39 with 34–39 input-only, three SPI hosts. Change `stubs/driver/gpio.h` and
`stubs/sdkconfig.h` to exercise another target's rules.

What is *not* covered: anything that needs the real SDK — the reader, the
transport, the HTTP handlers. Those need a firmware build.
