#!/usr/bin/env python3
"""Build a standalone, shareable preview of the configuration UI.

    tools/build_ui_preview.py [output.html]

The preview is generated from the firmware's own page rather than written by
hand, so it cannot drift from what the ESP32 actually serves. The only things
added are a banner saying it is a preview, and a fake API so every control
still responds without a board on the desk.

Output is head-and-body content with no document wrapper, which is what the
Artifact publisher expects.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / "components/web_server/web/index.html"

BANNER = """
    <div class="preview-note">
      <strong>Preview.</strong> This is the configuration UI that the firmware
      serves from the ESP32 itself, running here against sample data. Edits and
      saves are answered by a stand-in for the device's API, including its
      validation errors &mdash; nothing is stored.
    </div>
"""

BANNER_CSS = """
.preview-note{border-radius:var(--radius-box);padding:.75rem 1rem;margin:1rem 0 0;
  font-size:.875rem;background:color-mix(in oklab,var(--color-info) 18%,transparent);
  color:var(--color-base-content)}
.preview-note strong{font-weight:700}
"""

# A stand-in for web_server.c's handlers, with app_config.c's rules for the
# checks a person is most likely to trip over in a demo.
MOCK = r"""
<script>
(() => {
  "use strict";
  const RESTRICTED = [6,7,8,9,10,11,16,17], STRAPPING = [0,2,4,5,12,15];
  const exists = (n) => n >= 0 && n < 40 && n !== 20 && n !== 24 && (n < 28 || n > 31);
  const pins = [...Array(40).keys()];

  let config = {
    device:{name:"front-door", group_id:"00112233445566778899AABBCCDDEEFF"},
    nfc:{chip:"pn532", bus:"spi", spi_host:2, spi_sck:18, spi_miso:19, spi_mosi:23,
         spi_cs:5, spi_freq_hz:1000000, i2c_sda:21, i2c_scl:22, i2c_freq_hz:400000,
         i2c_addr:36, irq_pin:27, rst_pin:26},
    lock:{gpio:4, active_low:true, unlock_ms:3000},
    net:{ssid:"Hallway", hostname:"front-door", password:"", ap_password:"",
         password_set:true},
    mqtt:{enabled:true, broker:"192.168.1.10", port:1883, username:"aliro",
          client_id:"front-door", base_topic:"aliro/front-door", use_ssl:false,
          allow_insecure:false, ha_discovery:true, publish_taps:true,
          password:"", password_set:true},
  };

  const started = Date.now() - 4531000;
  const status = () => ({
    device:{name:config.device.name, target:"esp32", cores:2, revision:3,
            firmware:"0.1.0", idf:"v5.4",
            uptime_s:Math.floor((Date.now() - started)/1000),
            free_heap:181240, min_free_heap:170112},
    network:{mode:"sta", connected:true, ssid:config.net.ssid || "Hallway",
             ip:"192.168.1.42", rssi:-54},
    reader:{credentials:1, transport:"stub", locked:true},
    mqtt:{enabled:config.mqtt.enabled, connected:config.mqtt.enabled},
  });

  const hardware = () => ({
    target:"esp32", max_pin:39, spi_host_count:3,
    restricted_pins:RESTRICTED, strapping_pins:STRAPPING,
    input_only_pins:pins.filter((n) => exists(n) && n >= 34),
    usable_pins:pins.filter((n) => exists(n) && !RESTRICTED.includes(n)),
  });

  const masked = () => ({...config,
    net:{...config.net, password:"", ap_password:""},
    mqtt:{...config.mqtt, password:""}});

  /* The device is the authority on what is valid; these are its rules. */
  function validate(c) {
    const named = [["lock output", c.lock.gpio]];
    if (c.nfc.bus === "spi") named.push(["SPI SCK", c.nfc.spi_sck], ["SPI MISO", c.nfc.spi_miso],
      ["SPI MOSI", c.nfc.spi_mosi], ["SPI CS", c.nfc.spi_cs]);
    else if (c.nfc.bus === "i2c") named.push(["I2C SDA", c.nfc.i2c_sda], ["I2C SCL", c.nfc.i2c_scl]);
    if (c.nfc.irq_pin >= 0) named.push(["NFC IRQ", c.nfc.irq_pin]);
    if (c.nfc.rst_pin >= 0) named.push(["NFC reset", c.nfc.rst_pin]);

    if (!/^[0-9a-fA-F]{32}$/.test(c.device.group_id))
      return "reader group identifier must be exactly 32 hex characters";
    if (!c.device.name) return "device name must not be empty";
    if (c.lock.unlock_ms < 100 || c.lock.unlock_ms > 60000)
      return "unlock duration must be between 100 and 60000 ms";
    if (c.nfc.spi_freq_hz < 100000 || c.nfc.spi_freq_hz > 20000000)
      return "SPI clock must be between 100 kHz and 20 MHz";
    for (const [label, pin] of named) {
      if (RESTRICTED.includes(pin)) return `${label}: GPIO ${pin} is reserved for flash or PSRAM`;
      if (pin >= 34 && label !== "SPI MISO" && label !== "NFC IRQ")
        return `${label}: GPIO ${pin} is an input-only pin, cannot drive an output`;
    }
    for (let i = 0; i < named.length; i++)
      for (let j = i + 1; j < named.length; j++)
        if (named[i][1] === named[j][1])
          return `GPIO ${named[i][1]} is assigned to both ${named[i][0]} and ${named[j][0]}`;
    if (c.net.ap_password && c.net.ap_password.length < 8)
      return "access point password must be at least 8 characters, or empty for an open network";
    if (!c.net.hostname) return "hostname must not be empty";
    if (c.mqtt.enabled) {
      if (!c.mqtt.broker) return "MQTT is enabled but no broker address is set";
      if (!c.mqtt.port) return "MQTT port must be between 1 and 65535";
      if (!c.mqtt.client_id) return "MQTT client ID must not be empty";
      if (!c.mqtt.base_topic) return "MQTT base topic must not be empty";
      if (c.mqtt.base_topic.endsWith("/")) return "MQTT base topic must not end with '/'";
      if (/[#+]/.test(c.mqtt.base_topic)) return "MQTT base topic must not contain wildcards";
    }
    return null;
  }

  const json = (body, ok = true) =>
    Promise.resolve(new Response(JSON.stringify(body),
      {status: ok ? 200 : 400, headers: {"Content-Type": "application/json"}}));

  window.fetch = (url, options = {}) => {
    const path = String(url).split("?")[0];
    const method = (options.method || "GET").toUpperCase();

    if (path === "/api/status") return json(status());
    if (path === "/api/hardware") return json(hardware());
    if (path === "/api/config" && method === "GET") return json(masked());

    if (path === "/api/config" && method === "POST") {
      const patch = JSON.parse(options.body);
      const merged = {...config};
      for (const [group, values] of Object.entries(patch)) {
        merged[group] = {...config[group]};
        for (const [key, value] of Object.entries(values)) {
          // An empty password means "keep the stored one", exactly as on device.
          if (key.includes("password") && value === "") continue;
          merged[group][key] = value;
        }
      }
      const error = validate(merged);
      if (error) return json({ok:false, error}, false);
      config = merged;
      return json({ok:true, restart_required:true});
    }

    if (path === "/api/config/reset" && method === "POST") {
      config.net.ssid = "";
      config.mqtt.enabled = false;
      return json({ok:true, restart_required:true});
    }
    if (path === "/api/reboot") return json({ok:true});
    if (path === "/api/unlock") return json({ok:true});

    return json({ok:false, error:"no such endpoint"}, false);
  };
})();
</script>
"""


def build(source: str) -> str:
    head = re.search(r"<title>.*?</title>\s*<style>.*?</style>", source, re.S)
    body = re.search(r"<body>(.*)</body>", source, re.S)
    if not head or not body:
        raise SystemExit("could not find <title>/<style>/<body> in the source page")

    page = head.group(0) + "\n" + body.group(1).strip()
    page = page.replace("</style>", BANNER_CSS + "</style>", 1)

    # Banner goes above the routed pages, inside the working column.
    marker = '<div class="page" data-page="info">'
    if marker not in page:
        raise SystemExit("could not find the info page to anchor the banner")
    page = page.replace(marker, BANNER.strip() + "\n    " + marker, 1)

    # The mock must replace fetch before the app's first request.
    app_script = page.index('<script>\n"use strict"')
    return page[:app_script] + MOCK.strip() + "\n" + page[app_script:]


def main() -> None:
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/ui-preview.html"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(build(SOURCE.read_text()))
    print(f"wrote {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
