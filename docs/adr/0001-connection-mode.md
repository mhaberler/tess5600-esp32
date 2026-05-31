# ADR 0001 — Connection mode: persistent vs poll

**Status:** Accepted

## Context

The firmware connects to 2–4 TESS 5600 sensors. Two connection strategies were considered:

- **Persistent:** connect once after scan, stay connected indefinitely, let BLE notify callbacks drive output.
- **Poll:** connect → read characteristics once → disconnect → repeat on a timer.

## Decision

Persistent is the default. Poll is available as a build-time opt-in via `#define POLL_MODE` and `#define POLL_INTERVAL_MS`.

## Rationale

The TESS 5600 autonomously drives notifications at ~1.2 s intervals once subscribed. Persistent connections avoid repeated connect/disconnect overhead and give lower latency. Poll mode exists because some deployment contexts (battery-powered ESP32, unreliable RF environment) benefit from disconnecting between readings to reduce radio-on time.

The choice is build-time rather than runtime because it changes memory layout (persistent keeps one `BLEClient*` per sensor alive; poll can reuse a single client) and the reconnect strategy differs between modes.

## Consequences

- Persistent mode requires a per-sensor reconnect-with-backoff strategy for drops.
- Poll mode requires a timer and serialised connect/disconnect sequencing across the fleet.
- Switching modes requires a firmware rebuild.
