# TESS 5600 BLE Fleet Reporter

ESP32-C6 firmware (PlatformIO / Arduino) that scans for TE Connectivity TESS 5600 wireless pressure/temperature transducers, connects to up to four simultaneously, and logs readings to serial.

## Hardware

| Component | Details                                                                     |
| --------- | --------------------------------------------------------------------------- |
| MCU       | M5Stack NanoC6 (ESP32-C6)                                                   |
| Sensors   | TE Connectivity M5600 / U5600 (TESS 5600), up to `MAX_SENSORS` (default 16) |
| Interface | BLE GATT, 115200 baud serial output                                         |

## What it does

1. **Scans** for `SCAN_SECONDS` (default 30 s), collecting all devices advertising the name `TESS 5600`
2. **Connects** to each found sensor (up to 4) in round-robin
3. On first connection per sensor, reads and prints the **device descriptor** once:

   - MAC address, BLE device name, appearance, connection parameters
   - Manufacturer, model, hardware/firmware/software revision
   - Data rate (current / min / max interval in ms)
4. On every poll, reads and prints **live data**:

   - Battery level (%)
   - Sensor status (OK / ERROR)
   - Temperature (°C)
   - Pressure (bar, Pa, psi)
   - Pmin / Pmax recorded extremes

## Serial output example

```
TE5600 Fleet Reporter
Mode: POLL every 10000ms
[B9:B7:42] found: c7:86:03:b9:b7:42
[52:1A:91] found: 64:e8:21:52:1a:91
Scan complete: 2 sensor(s) found

[B9:B7:42] connected
[B9:B7:42] --- Device descriptor ---
[B9:B7:42] mac:          c7:86:03:b9:b7:42
[B9:B7:42] manufacturer: TE Connectivity Sensor Solutions
[B9:B7:42] model:        5600
[B9:B7:42] fw rev:       2.2 (Mar 20 2017 - 08:17:37)
[B9:B7:42] data rate: 5000ms  (min 100ms  max 5000ms)
[B9:B7:42] battery: 100%
[B9:B7:42] sensor status: OK (0x00)
[B9:B7:42] waiting 5000ms for sensor update...
[B9:B7:42] temperature: 28.36 °C
[B9:B7:42] pressure: 7.9673 bar  (796727.4 Pa  /  115.5555 psi)
[B9:B7:42] Pmax: 20.6841 bar  (2068410.0 Pa)
```

Log prefix is the last 3 MAC octets — unique within a fleet and short enough to scan at a glance.

## Build options

Defined at the top of `src/main.cpp` or via `build_flags` in `platformio.ini`:

| Define             | Default | Effect                                                                                  |
| ------------------ | ------- | --------------------------------------------------------------------------------------- |
| `POLL_MODE`        | on      | Connect → read → disconnect cycle (see below)                                           |
| `POLL_INTERVAL_MS` | `10000` | ms between polls per sensor (poll mode only)                                            |
| `SCAN_SECONDS`     | `30`    | BLE scan duration at boot                                                               |
| `DUMP_RAW_DATA`    | on      | Print raw hex of the 14-byte data payload alongside decoded values. Remove to suppress. |

### Connection modes

**Poll mode** (`POLL_MODE` defined, default):
Connect → wait one sensor update cycle → read all characteristics → disconnect → wait `POLL_INTERVAL_MS` → repeat. One sensor active at a time, round-robin across the fleet. Safe for the ESP32-C6 single-radio BLE stack. Because only one connection is open at a time, there is no BLE stack limit on fleet size — the effective limit is `MAX_SENSORS` (default 16, raise freely). Note: with N sensors each taking ~6 s per poll, a full round-robin takes `N × 6 s`, so actual read frequency per sensor decreases as the fleet grows.

**Persistent mode** (`POLL_MODE` not defined):
Connect once per sensor and stay connected. Pressure/temperature delivered via BLE notify callbacks at the sensor's own data rate (~1.2 s). Requires a per-sensor reconnect-with-backoff strategy if a device drops.

## GATT map

### Service `f000ab30` — Pressure / Temperature

| Characteristic | UUID       | Access       | Description                                        |
| -------------- | ---------- | ------------ | -------------------------------------------------- |
| Data           | `f000ab31` | Read, Notify | 14-byte payload (see below)                        |
| Data Rate      | `f000ab32` | Read, Write  | 12-byte payload: current / min / max interval (ms) |
| Status         | `f000ab3f` | Read         | 1 byte: `0x00` = OK, `0x01` = sensor error         |

### Service `f000180f` — Battery

| Characteristic | UUID       | Access       | Description    |
| -------------- | ---------- | ------------ | -------------- |
| Battery Level  | `f0002a19` | Read, Notify | 1 byte, 0–100% |

### Data characteristic payload (14 bytes, little-endian)

| Bytes | Type      | Field       | Conversion                       |
| ----- | --------- | ----------- | -------------------------------- |
| 0–1   | `int16_t` | Temperature | `T / 100` → °C                   |
| 2–5   | `int32_t` | Pressure    | `P / 10` → Pa; `/ 100 000` → bar |
| 6–9   | `int32_t` | Pmin        | same as P                        |
| 10–13 | `int32_t` | Pmax        | same as P                        |

Error sentinel: `0xFFFF` (T) / `0xFFFFFFFF` (P, Pmin, Pmax) — field not yet measured.

### Data rate payload (12 bytes, little-endian)

| Bytes | Type       | Field                            |
| ----- | ---------- | -------------------------------- |
| 0–3   | `uint32_t` | Current data rate (ms)           |
| 4–7   | `uint32_t` | Minimum admissible interval (ms) |
| 8–11  | `uint32_t` | Maximum admissible interval (ms) |

Only bytes 0–3 are writable. Valid range: [min, max] — typically 100 ms to 5000 ms.

## Scan behaviour

Active scan is required — the sensor's 128-bit service UUID (`f0000000-0451-4000-b000-000000000000`) is in the BLE scan response, not the primary advertisement. A device is accepted only if its name matches `TESS 5600`; UUID match is used as an additional confirmation when available.

## Implementation notes — timing and sequencing

These were learned from real failures during development.

### BLEClient must be destroyed between polls (not reused)

Reusing the same `BLEClient*` across poll cycles causes `Client busy, connected to ..., id=0` errors on the second connect attempt. Even after calling `disconnect()`, NimBLE retains internal connection state in the object. The fix is to `delete` the client and call `BLEDevice::createClient()` fresh on every poll. This is safe because poll mode never holds more than one connection at a time.

### Read data only after one full sensor update cycle

If `f000ab31` is read immediately after connecting, the payload is all `0xFF` — the sensor hasn't completed a measurement yet. The sensor's data rate (default 5000 ms, readable from `f000ab32`) is the required wait. The firmware reads the data rate from the descriptor on first connect and waits that long before reading `f000ab31`.

### Serialise all GATT work — one connection at a time

Attempting to connect to a second sensor while a GATT transaction (service discovery, characteristic read) is still in progress on the first produces `rc=7` (disconnected mid-discovery). The ESP32-C6 has a single radio; the BLE stack cannot multiplex concurrent GATT operations. Poll mode enforces strict serialisation: full connect → read → disconnect completes before the next sensor is touched.

### Next poll time is set after disconnect, not before connect

Setting `pollAt = now + POLL_INTERVAL_MS` at the start of a poll cycle causes the timer to expire during the 5+ second sensor wait, triggering an immediate retry on the same client object. `pollAt` must be set after the full cycle (connect + wait + read + disconnect) completes.

### Active scan required for service UUID matching

The TESS 5600 places its 128-bit service UUID (`f0000000-0451-4000-b000-000000000000`) in the BLE **scan response**, not the primary advertisement. Passive scan never requests scan responses, so `isAdvertisingService()` always returns false. Active scan is required. As a belt-and-suspenders fallback, device name (`TESS 5600`) is also checked — this catches the window where the primary advertisement arrives before the scan response.

### Scanning and active GATT connections conflict

Active scan (needed for scan response) interferes with active GATT connections on the ESP32-C6. The firmware scans once at boot (before any connections exist), then stops. A background rescan while sensors are connected would require passive scan, which can't match on the service UUID.

### TE spec error sentinels differ from observed behaviour

The TE M5600/U5600 software manual states error sentinels are `0x7FFF` (T) and `0x7FFFFFFF` (P). In practice the device sends `0xFFFF` and `0xFFFFFFFF`. The firmware checks both.

## References

- [TE M5600/U5600 Software Manual](https://www.ttieurope.com/content/dam/tti-europe/manufacturers/te-connectivity/resources/TE_M5600_U5600_Software_Manual.pdf)
- ADR: [docs/adr/0001-connection-mode.md](docs/adr/0001-connection-mode.md)



# iOS+Android app

see https://github.com/mhaberler/tess5600-app.git

