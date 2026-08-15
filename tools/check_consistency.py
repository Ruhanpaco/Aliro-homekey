#!/usr/bin/env python3
"""Static checks that catch the usual first-build failures without ESP-IDF.

    tools/check_consistency.py

None of this replaces a real build. It catches the mistakes that are cheap to
make and annoying to find over a serial console:

  * a CONFIG_ symbol used in C but defined in no Kconfig (or vice versa)
  * an sdkconfig default for a symbol this project never declares
  * a component including another component's header without requiring it
  * an embedded-file symbol that does not match what CMake embeds
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
COMPONENTS = ROOT / "components"

# Declared by ESP-IDF, not by us. Using them is fine; declaring them is not.
IDF_CONFIG_PREFIXES = (
    "CONFIG_IDF_TARGET", "CONFIG_FREERTOS", "CONFIG_ESP_CONSOLE", "CONFIG_HTTPD",
    "CONFIG_MBEDTLS", "CONFIG_LOG", "CONFIG_PARTITION", "CONFIG_COMPILER",
    "CONFIG_ESPTOOLPY", "CONFIG_SPIRAM", "CONFIG_LWIP", "CONFIG_ESP_WIFI",
    "CONFIG_SOC_", "CONFIG_ESP_MAIN", "CONFIG_BOOTLOADER",
)

problems: list[str] = []
notes: list[str] = []


def c_sources() -> list[pathlib.Path]:
    files = list(COMPONENTS.rglob("*.c")) + list(COMPONENTS.rglob("*.h"))
    files += list((ROOT / "main").rglob("*.c"))
    return [f for f in files if "build" not in f.parts]


def declared_config_symbols() -> dict[str, pathlib.Path]:
    declared = {}
    for kconfig in list(ROOT.rglob("Kconfig")) + list(ROOT.rglob("Kconfig.projbuild")):
        if "build" in kconfig.parts or "managed_components" in kconfig.parts:
            continue
        for name in re.findall(r"^\s*config\s+([A-Z0-9_]+)", kconfig.read_text(), re.M):
            declared["CONFIG_" + name] = kconfig.relative_to(ROOT)
    return declared


def used_config_symbols() -> dict[str, set[str]]:
    used: dict[str, set[str]] = {}
    for path in c_sources():
        for name in re.findall(r"\bCONFIG_[A-Z0-9_]+", path.read_text()):
            used.setdefault(name, set()).add(str(path.relative_to(ROOT)))
    return used


def check_config_symbols() -> None:
    declared = declared_config_symbols()
    used = used_config_symbols()

    for name, where in sorted(used.items()):
        if name in declared or name.startswith(IDF_CONFIG_PREFIXES):
            continue
        problems.append(f"{name} used in {', '.join(sorted(where))} but declared in no Kconfig")

    for name, kconfig in sorted(declared.items()):
        if name.startswith("CONFIG_HTTPD") or name.startswith("CONFIG_ESP_"):
            problems.append(f"{name} declared in {kconfig} shadows an ESP-IDF symbol")
        elif name not in used and name not in DEFAULTS_USED:
            notes.append(f"{name} declared in {kconfig} but never used")


def sdkconfig_defaults() -> dict[str, set[str]]:
    found: dict[str, set[str]] = {}
    files = [ROOT / "sdkconfig.defaults"] + sorted((ROOT / "boards").glob("sdkconfig.defaults.*"))
    for path in files:
        if not path.exists():
            continue
        for name in re.findall(r"^(CONFIG_[A-Z0-9_]+)=", path.read_text(), re.M):
            found.setdefault(name, set()).add(path.name)
    return found


def check_sdkconfig_defaults() -> None:
    declared = declared_config_symbols()
    for name, where in sorted(sdkconfig_defaults().items()):
        if name in declared or name.startswith(IDF_CONFIG_PREFIXES):
            continue
        problems.append(f"{name} set in {', '.join(sorted(where))} but declared in no Kconfig")


# Argument keywords of idf_component_register. A dependency list ends at the
# next one of these, not at the next newline.
CMAKE_KEYWORDS = {
    "SRCS", "SRC_DIRS", "INCLUDE_DIRS", "PRIV_INCLUDE_DIRS", "REQUIRES",
    "PRIV_REQUIRES", "EMBED_FILES", "EMBED_TXTFILES", "LDFRAGMENTS",
    "KCONFIG", "KCONFIG_PROJBUILD", "WHOLE_ARCHIVE", "REQUIRED_IDF_TARGETS",
}


def component_requires() -> dict[str, set[str]]:
    """Map component name -> everything it names in REQUIRES/PRIV_REQUIRES."""
    result = {}
    for cmake in list(COMPONENTS.glob("*/CMakeLists.txt")) + [ROOT / "main" / "CMakeLists.txt"]:
        name = "main" if cmake.parent.name == "main" else cmake.parent.name
        # These calls contain no nested parens, so the first ')' ends the call.
        body = re.search(r"idf_component_register\(([^)]*)\)", cmake.read_text(), re.S)
        deps: set[str] = set()
        if body:
            collecting = False
            for token in re.findall(r'"[^"]*"|[\w./${}-]+', body.group(1)):
                if token in CMAKE_KEYWORDS:
                    collecting = token in ("REQUIRES", "PRIV_REQUIRES")
                elif collecting and not token.startswith('"'):
                    deps.add(token)
        result[name] = deps
    return result


def public_headers() -> dict[str, str]:
    """Map header filename -> owning component."""
    owners = {}
    for component in COMPONENTS.iterdir():
        if not component.is_dir():
            continue
        for header in list(component.glob("include/*.h")) + list(component.glob("*.h")):
            owners[header.name] = component.name
    return owners


# ESP-IDF components this project is allowed to name in REQUIRES. Anything
# else is either a local component or a typo — and a typo here fails the build
# at CMake time with "unknown name", which is a slow way to learn it.
# Notably: OTA lives in `app_update`, there is no `esp_ota_ops` component.
KNOWN_IDF_COMPONENTS = {
    "app_update", "bt", "console", "driver", "esp_adc", "esp_app_format",
    "esp_common", "esp_driver_gpio", "esp_driver_i2c", "esp_driver_spi",
    "esp_event", "esp_hw_support", "esp_http_client", "esp_http_server",
    "esp_netif", "esp_partition", "esp_pm", "esp_ringbuf", "esp_rom",
    "esp_system", "esp_timer", "esp_wifi", "espressif__esp_aliro_lib",
    "esp_aliro_lib", "freertos", "hal", "heap", "json", "log", "lwip",
    "mbedtls", "mqtt", "newlib", "nvs_flash", "protocol_examples_common",
    "pthread", "soc", "spi_flash", "vfs", "wpa_supplicant",
    # From esp-matter, present only when ESP_MATTER_PATH is set. Named here
    # rather than special-cased, because a typo in them fails exactly the same
    # way -- just in a build most people never run.
    "chip", "esp_matter", "esp_matter_console",
}


def check_requires_exist() -> None:
    local = {c.name for c in COMPONENTS.iterdir() if c.is_dir()}
    for component, deps in component_requires().items():
        for dep in sorted(deps):
            if dep in local or dep in KNOWN_IDF_COMPONENTS:
                continue
            problems.append(
                f"component '{component}' requires '{dep}', which is neither a local "
                f"component nor a known ESP-IDF one"
            )


# ESP-IDF headers whose component has to be named in REQUIRES. Only the ones
# this project actually uses: a missing entry here costs a CI round trip, and
# that is exactly what this table exists to prevent.
IDF_HEADER_OWNERS = {
    "esp_timer.h": "esp_timer",
    "esp_chip_info.h": "esp_hw_support",
    "esp_mac.h": "esp_hw_support",
    "esp_random.h": "esp_hw_support",
    "esp_app_desc.h": "esp_app_format",
    "esp_ota_ops.h": "app_update",   # NOT esp_ota_ops; no such component
    "esp_partition.h": "esp_partition",
    "esp_console.h": "console",
    "esp_http_server.h": "esp_http_server",
    "esp_wifi.h": "esp_wifi",
    "esp_netif.h": "esp_netif",
    "esp_event.h": "esp_event",
    "esp_system.h": "esp_system",
    "esp_restart.h": "esp_system",
    "mqtt_client.h": "mqtt",
    "cJSON.h": "json",
    "nvs.h": "nvs_flash",
    "nvs_flash.h": "nvs_flash",
    "driver/gpio.h": "driver",
    "driver/spi_master.h": "driver",
    "driver/i2c_master.h": "driver",
}


def check_idf_header_requires() -> None:
    requires = component_requires()
    for component, deps in requires.items():
        base = ROOT / "main" if component == "main" else COMPONENTS / component
        for source in list(base.rglob("*.c")) + list(base.rglob("*.h")):
            if "build" in source.parts:
                continue
            for include in re.findall(r"#include\s+<([\w./]+\.h)>", source.read_text()):
                owner = IDF_HEADER_OWNERS.get(include)
                if owner is None or owner in deps:
                    continue
                problems.append(
                    f"{source.relative_to(ROOT)} includes <{include}> "
                    f"but '{component}' does not require '{owner}'"
                )


def check_component_dependencies() -> None:
    requires = component_requires()
    owners = public_headers()

    for cmake_owner, deps in requires.items():
        base = ROOT / "main" if cmake_owner == "main" else COMPONENTS / cmake_owner
        for source in list(base.rglob("*.c")) + list(base.rglob("*.h")):
            if "build" in source.parts:
                continue
            for include in re.findall(r'#include\s+"([\w./]+\.h)"', source.read_text()):
                header = pathlib.Path(include).name
                owner = owners.get(header)
                if owner is None or owner == cmake_owner or owner in deps:
                    continue
                problems.append(
                    f"{source.relative_to(ROOT)} includes {header} (component '{owner}') "
                    f"but '{cmake_owner}' does not require it"
                )


def check_embedded_symbols() -> None:
    """target_add_binary_data/EMBED_FILES names must match the asm symbols."""
    for cmake in list(COMPONENTS.glob("*/CMakeLists.txt")) + [ROOT / "main" / "CMakeLists.txt"]:
        text = cmake.read_text()
        embedded = re.findall(r'target_add_binary_data\([^"]*"([^"]+)"', text)
        embedded += re.findall(r'EMBED_FILES\s+"([^"]+)"', text)
        base = cmake.parent
        for rel in embedded:
            if not (base / rel).exists():
                # Generated at configure time (the dev identity) is expected.
                if "certs/" not in rel:
                    problems.append(f"{cmake.relative_to(ROOT)} embeds {rel}, which does not exist")
                continue
            symbol = "_binary_" + re.sub(r"[^\w]", "_", pathlib.Path(rel).name) + "_start"
            sources = list(base.rglob("*.c"))
            if not any(symbol in s.read_text() for s in sources if "build" not in s.parts):
                notes.append(f"{cmake.relative_to(ROOT)} embeds {rel}; no source references {symbol}")

        # And the other direction. The forward check above only sees quoted
        # literals, so a component that builds its EMBED_FILES list in a
        # variable -- as web_server does, to embed compressed pages -- would
        # sail past it with no check at all. Work back from the symbols the C
        # actually declares to a file that could produce them.
        declared = {re.sub(r"[^\w]", "_", pathlib.Path(r).name) for r in embedded}
        available = set(declared)
        for f in base.rglob("*"):
            if f.is_file() and "build" not in f.parts:
                stem = re.sub(r"[^\w]", "_", f.name)
                available.add(stem)
                available.add(stem + "_gz")  # compressed during the build
        for src in base.rglob("*.c"):
            if "build" in src.parts:
                continue
            for sym in re.findall(r'asm\("_binary_(\w+?)_(?:start|end)"\)', src.read_text()):
                if sym not in available:
                    problems.append(
                        f"{src.relative_to(ROOT)} declares _binary_{sym}_start "
                        f"but {cmake.parent.relative_to(ROOT)} embeds no such file")


# Symbols only ever read from sdkconfig defaults, never from C.
DEFAULTS_USED = set(sdkconfig_defaults())

check_config_symbols()
check_sdkconfig_defaults()
check_requires_exist()
check_idf_header_requires()
check_component_dependencies()
check_embedded_symbols()

for note in notes:
    print(f"note:    {note}")
for problem in problems:
    print(f"PROBLEM: {problem}")

print(f"\n{len(problems)} problem(s), {len(notes)} note(s)")
sys.exit(1 if problems else 0)
