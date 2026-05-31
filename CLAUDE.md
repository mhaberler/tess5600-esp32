# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Build default env (m5stack-nanoc6)
pio run

# Build specific environment
pio run -e m5stack-nanoc6

# Build + generate merged firmware bin (used by CI)
pio run -e m5stack-nanoc6 -t firmware

# Upload to device
pio run -e m5stack-nanoc6 -t upload

# Serial monitor
pio device monitor

# Clean
pio run -t clean
```

No tests exist in this project.

## Architecture

Single-file Arduino firmware in [src/main.cpp](src/main.cpp): BLE scan for a Parkside battery by MAC address, connect as GATT client, enumerate all services/characteristics and dump raw hex values to serial. Intended as a discovery/reverse-engineering tool, not a production reporter.

**Target hardware**: `m5stack-nanoc6` (ESP32-C6) is the active `default_envs`. The `platformio.ini` also defines many other M5Stack/ESP32 boards as commented-out alternatives — the `[ci]` section controls what CI builds (`m5stack-nanoc6` + `esp32p4_waveshare_devkit`).

**Build system layering** in `platformio.ini`:
- Base `[env]` defines platform (pioarduino ESP32), framework, and shared flags
- Feature sections `[improv]`, `[ota]`, `[ghota]` each add optional `-D` flags
- `[release]`/`[debug]` compose feature flags + debug level
- `[build-target]` points at `[release]` (change to `[debug]` for debug builds)
- Per-board `[env:*]` sections extend board hardware sections + `build-target`
- LED abstraction: boards select `[led-single]`, `[led-rgb-ws2812]`, `[led-rgb-sk6812]`, or `[led-none]`; corresponding `-DLED_SCENARIO_*` flags drive LED code

**Build scripts** (in `scripts/`, currently commented out in `extra_scripts`):
- `inject_build_info.py` — injects `BUILD_SHA`, `BUILD_DATE`, `BUILD_TAG`, `BUILD_FIRMWARE_URI`, `SGO_DEFAULT_*` as C preprocessor defines
- `firmware_naming.py` — shared logic for generating firmware filenames (`<project>_<env>_firmware_<version>.bin` / `<project>_<env>_ota.bin`)
- `generate_merged_firmware.py` — post-build: merges bootloader + partition table + app into single flashable bin in `firmware/`
- `inject_lib_versions.py`, `bump_version.py` — optional version management

To re-enable build scripts, uncomment the `extra_scripts =` block in `[env]`.

## Key pin defines (nanoc6 env)

| Define | Value |
|--------|-------|
| `BUTTON_PIN` | `GPIO_NUM_9` |
| `LED_PIN` | `GPIO_NUM_7` |
| `SDA_PIN` | `GPIO_NUM_2` |
| `SCL_PIN` | `GPIO_NUM_1` |

## CI

- `build-firmware.yml` — manual dispatch, builds `[ci]` envs (or override via input)
- `release.yml` — triggered on `v*` tags, builds `[ci]` envs and creates GitHub Release with firmware assets
