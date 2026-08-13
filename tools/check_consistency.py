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
    "CONFIG_SOC_", "CONFIG_ESP_MAIN",
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


def component_requires() -> dict[str, set[str]]:
    """Map component name -> everything it names in REQUIRES/PRIV_REQUIRES."""
    result = {}
    for cmake in list(COMPONENTS.glob("*/CMakeLists.txt")) + [ROOT / "main" / "CMakeLists.txt"]:
        name = "main" if cmake.parent.name == "main" else cmake.parent.name
        text = cmake.read_text()
        deps: set[str] = set()
        for match in re.finditer(r"(?:PRIV_)?REQUIRES\s+((?:[\w.]+\s*)+)", text):
            deps.update(match.group(1).split())
        deps.discard("REQUIRES")
        deps.discard("PRIV_REQUIRES")
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


# Symbols only ever read from sdkconfig defaults, never from C.
DEFAULTS_USED = set(sdkconfig_defaults())

check_config_symbols()
check_sdkconfig_defaults()
check_component_dependencies()
check_embedded_symbols()

for note in notes:
    print(f"note:    {note}")
for problem in problems:
    print(f"PROBLEM: {problem}")

print(f"\n{len(problems)} problem(s), {len(notes)} note(s)")
sys.exit(1 if problems else 0)
