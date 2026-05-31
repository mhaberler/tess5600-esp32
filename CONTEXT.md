---
name: te5600-ble-reporter
description: ESP32 firmware that scans for TE Connectivity TESS 5600 pressure/temperature sensors and reports their data over serial.
---

## Glossary

### Sensor
A single TE Connectivity TESS 5600 BLE device. Identified by MAC address. Exposes pressure, temperature, Pmin, Pmax, sensor status, and battery level over GATT.

### Session
The lifecycle of one Sensor connection: scan → connect → subscribe → receive notifications → (optionally) disconnect. Two modes exist (see Connection Mode).

### Connection Mode
Build-time choice controlling Session lifecycle:
- **Persistent** (default): connect once after scan, stay connected, notify callbacks drive all output.
- **Poll** (opt-in via `#define POLL_MODE`): connect → read once → disconnect → repeat on a configurable timer interval.

### Fleet
The set of all Sensors discovered during a single scan pass. Size: 2–4 devices.

### Data Rate Characteristic (f000ab32)
Third characteristic in the pressure service (`f000ab30`). Properties: notify + read + write. 12-byte payload, three `uint32_t` little-endian fields:
- Bytes 0–3: current data rate (ms)
- Bytes 4–7: minimum admissible interval (ms, hardware floor)
- Bytes 8–11: maximum admissible interval (ms, hardware ceiling)

Only bytes 0–3 are writable. Valid write range: [min, max]. Read on connect; write to change rate.
