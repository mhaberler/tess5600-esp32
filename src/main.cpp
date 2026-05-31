
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// TE Connectivity M5600/U5600 pressure/temperature sensor fleet reporter.
// Protocol: https://www.ttieurope.com/content/dam/tti-europe/manufacturers/te-connectivity/resources/TE_M5600_U5600_Software_Manual.pdf
//
// Build options (define in platformio.ini build_flags or here):
//   POLL_MODE          — connect/read/disconnect on a timer instead of persistent connections
//   POLL_INTERVAL_MS   — poll interval in ms (default 10000). Only used with POLL_MODE.

#define POLL_MODE 1

#ifndef POLL_INTERVAL_MS
#define POLL_INTERVAL_MS 10000
#endif

// UUIDs
static const char* BASE_UUID       = "f0000000-0451-4000-b000-000000000000";
static const char* PRESSURE_SVC    = "f000ab30-0451-4000-b000-000000000000";
static const char* DATA_CHAR       = "f000ab31-0451-4000-b000-000000000000";
static const char* DATARATE_CHAR   = "f000ab32-0451-4000-b000-000000000000";
static const char* STATUS_CHAR     = "f000ab3f-0451-4000-b000-000000000000";
static const char* BATTERY_SVC     = "f000180f-0451-4000-b000-000000000000";
static const char* BATTERY_CHAR    = "f0002a19-0451-4000-b000-000000000000";

static const char* DEVICE_NAME     = "TESS 5600";

static const int   MAX_SENSORS     = 4;
static const int   RECONNECT_BASE_MS = 2000;
static const int   RECONNECT_MAX_MS  = 30000;

// ── Sensor record ────────────────────────────────────────────────────────────

struct Sensor {
  BLEAddress   addr;
  char         tag[9];    // "XX:XX:XX\0" — last 3 octets
  BLEClient*   client    = nullptr;
  bool         active    = false;
  uint32_t     reconnectAt = 0;
  int          reconnectDelay = RECONNECT_BASE_MS;
#ifdef POLL_MODE
  uint32_t     pollAt    = 0;
#endif

  Sensor() : addr("00:00:00:00:00:00") { tag[0] = '\0'; }
};

static Sensor    sensors[MAX_SENSORS];
static int       sensorCount = 0;
static bool      scanDone    = false;

// ── Helpers ──────────────────────────────────────────────────────────────────

static void makeTag(const String& mac, char* out) {
  // "C9:69:81:7A:D1:6C" → "7A:D1:6C"
  int colon = 0, i = 0;
  for (; i < (int)mac.length() && colon < 3; i++)
    if (mac[i] == ':') colon++;
  strncpy(out, mac.c_str() + i, 8);
  out[8] = '\0';
  // uppercase
  for (int j = 0; out[j]; j++) out[j] = toupper(out[j]);
}

static void logf(const char* tag, const char* fmt, ...) {
  Serial.printf("[%s] ", tag);
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

// ── Data decode ──────────────────────────────────────────────────────────────

static void decodeData(const char* tag, uint8_t* d, size_t len) {
  if (len < 14) { logf(tag, "data: short packet (%u bytes)", len); return; }

  int16_t T    = (int16_t)((d[1] << 8) | d[0]);
  int32_t P    = (int32_t)((d[5]<<24)|(d[4]<<16)|(d[3]<<8)|d[2]);
  int32_t Pmin = (int32_t)((d[9]<<24)|(d[8]<<16)|(d[7]<<8)|d[6]);
  int32_t Pmax = (int32_t)((d[13]<<24)|(d[12]<<16)|(d[11]<<8)|d[10]);

  if (T == 0x7FFF)
    logf(tag, "temperature: ERROR");
  else
    logf(tag, "temperature: %.2f °C", T / 100.0f);

  if (P == (int32_t)0x7FFFFFFF)
    logf(tag, "pressure: ERROR");
  else
    logf(tag, "pressure: %.1f Pa  /  %.4f psi", P / 10.0f, P / 10.0f / 6894.7f);

  if (Pmin != (int32_t)0x7FFFFFFF)
    logf(tag, "Pmin: %.1f Pa", Pmin / 10.0f);
  if (Pmax != (int32_t)0x7FFFFFFF)
    logf(tag, "Pmax: %.1f Pa", Pmax / 10.0f);
}

// ── Per-sensor connect ────────────────────────────────────────────────────────

static void readStringChar(BLERemoteService* svc, const char* uuid,
                           const char* tag, const char* label) {
  auto* c = svc->getCharacteristic(BLEUUID(uuid));
  if (c && c->canRead())
    logf(tag, "%s: %s", label, c->readValue().c_str());
}

static void readGAP(BLEClient* client, const char* tag) {
  auto* svc = client->getService(BLEUUID((uint16_t)0x1800));
  if (!svc) return;
  logf(tag, "--- Generic Access ---");
  readStringChar(svc, "00002a00-0000-1000-8000-00805f9b34fb", tag, "name");
  // Appearance
  auto* c = svc->getCharacteristic(BLEUUID((uint16_t)0x2a01));
  if (c && c->canRead()) {
    String raw = c->readValue();
    if (raw.length() >= 2)
      logf(tag, "appearance: 0x%04X", (uint8_t)raw[0] | ((uint8_t)raw[1] << 8));
  }
  // Conn params
  c = svc->getCharacteristic(BLEUUID((uint16_t)0x2a04));
  if (c && c->canRead()) {
    String raw = c->readValue();
    if (raw.length() >= 8) {
      auto u16 = [&](int i){ return (uint16_t)((uint8_t)raw[i]|((uint8_t)raw[i+1]<<8)); };
      logf(tag, "conn params: min=%.1fms max=%.1fms latency=%u timeout=%ums",
           u16(0)*1.25f, u16(2)*1.25f, u16(4), (unsigned)u16(6)*10);
    }
  }
}

static void readDeviceInfo(BLEClient* client, const char* tag) {
  auto* svc = client->getService(BLEUUID((uint16_t)0x180a));
  if (!svc) return;
  logf(tag, "--- Device Information ---");
  readStringChar(svc, "00002a29-0000-1000-8000-00805f9b34fb", tag, "manufacturer");
  readStringChar(svc, "00002a24-0000-1000-8000-00805f9b34fb", tag, "model");
  readStringChar(svc, "00002a27-0000-1000-8000-00805f9b34fb", tag, "hw rev");
  readStringChar(svc, "00002a26-0000-1000-8000-00805f9b34fb", tag, "fw rev");
  readStringChar(svc, "00002a28-0000-1000-8000-00805f9b34fb", tag, "sw rev");
}

static bool connectSensor(Sensor& s) {
  const char* tag = s.tag;

  if (!s.client) s.client = BLEDevice::createClient();
  if (!s.client->connect(s.addr)) {
    logf(tag, "connection failed");
    return false;
  }
  logf(tag, "connected");

  readGAP(s.client, tag);
  readDeviceInfo(s.client, tag);

  // ── Battery ──────────────────────────────────────────────────────────────
  {
    auto* svc = s.client->getService(BLEUUID(BATTERY_SVC));
    if (svc) {
      auto* c = svc->getCharacteristic(BLEUUID(BATTERY_CHAR));
      if (c) {
        if (c->canRead()) {
          String raw = c->readValue();
          if (raw.length() >= 1)
            logf(tag, "battery: %u%%", (uint8_t)raw[0]);
        }
        if (c->canNotify()) {
          c->registerForNotify([tag = String(s.tag)](BLERemoteCharacteristic*, uint8_t* d, size_t len, bool) {
            if (len >= 1) logf(tag.c_str(), "battery: %u%%", d[0]);
          });
        }
      }
    }
  }

  // ── Pressure service ─────────────────────────────────────────────────────
  {
    auto* svc = s.client->getService(BLEUUID(PRESSURE_SVC));
    if (!svc) { logf(tag, "pressure service not found"); s.client->disconnect(); return false; }

    // Status (read only)
    auto* statusC = svc->getCharacteristic(BLEUUID(STATUS_CHAR));
    if (statusC && statusC->canRead()) {
      String raw = statusC->readValue();
      uint8_t st = raw.length() > 0 ? (uint8_t)raw[0] : 0xFF;
      logf(tag, "sensor status: %s (0x%02X)", st == 0 ? "OK" : "ERROR", st);
    }

    // Data rate (f000ab32) — 3x uint32_t LE: current_ms, min_ms, max_ms
    auto* rateC = svc->getCharacteristic(BLEUUID(DATARATE_CHAR));
    if (rateC && rateC->canRead()) {
      String raw = rateC->readValue();
      if (raw.length() >= 12) {
        auto u32 = [&](int i) -> uint32_t {
          return (uint8_t)raw[i] | ((uint8_t)raw[i+1]<<8) | ((uint8_t)raw[i+2]<<16) | ((uint8_t)raw[i+3]<<24);
        };
        logf(tag, "data rate: %ums  (min %ums  max %ums)", u32(0), u32(4), u32(8));
      } else {
        Serial.printf("[%s] data rate raw (%u bytes):", tag, raw.length());
        for (size_t i = 0; i < raw.length(); i++) Serial.printf(" %02X", (uint8_t)raw[i]);
        Serial.println();
      }
    }

    // Data (f000ab31) — subscribe + decode
    auto* dataC = svc->getCharacteristic(BLEUUID(DATA_CHAR));
    if (!dataC) { logf(tag, "data char not found"); s.client->disconnect(); return false; }

    if (dataC->canNotify()) {
      dataC->registerForNotify([tag = String(s.tag)](BLERemoteCharacteristic*, uint8_t* d, size_t len, bool) {
        decodeData(tag.c_str(), d, len);
      });
      logf(tag, "subscribed to data notifications");
    } else if (dataC->canRead()) {
#ifdef POLL_MODE
      String raw = dataC->readValue();
      decodeData(tag, (uint8_t*)raw.c_str(), raw.length());
#else
      logf(tag, "data char has no notify — cannot use persistent mode");
      s.client->disconnect();
      return false;
#endif
    }
  }

  s.active = true;
  s.reconnectDelay = RECONNECT_BASE_MS;
  return true;
}

// ── Reconnect on drop ─────────────────────────────────────────────────────────

static void scheduleReconnect(Sensor& s) {
  s.active = false;
  s.reconnectAt = millis() + s.reconnectDelay;
  logf(s.tag, "disconnected — retry in %dms", s.reconnectDelay);
  s.reconnectDelay = min(s.reconnectDelay * 2, RECONNECT_MAX_MS);
}

// ── Scan ──────────────────────────────────────────────────────────────────────

class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    if (sensorCount >= MAX_SENSORS) return;
    bool match = (dev.haveServiceUUID() && dev.isAdvertisingService(BLEUUID(BASE_UUID)))
              || (dev.haveName() && dev.getName() == DEVICE_NAME);
    if (!match) return;

    // Deduplicate
    String addr = dev.getAddress().toString();
    for (int i = 0; i < sensorCount; i++)
      if (sensors[i].addr.toString() == addr) return;

    Sensor& s = sensors[sensorCount++];
    s.addr = dev.getAddress();
    makeTag(addr, s.tag);
    logf(s.tag, "found: %s", addr.c_str());
  }
};

// ── Arduino entry points ──────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  Serial.println("TE5600 Fleet Reporter");
#ifdef POLL_MODE
  Serial.printf("Mode: POLL every %dms\n", POLL_INTERVAL_MS);
#else
  Serial.println("Mode: PERSISTENT");
#endif

  BLEDevice::init("");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks(), false);
  scan->setActiveScan(true);  // 128-bit UUID in scan response — active scan required
  scan->start(10, [](BLEScanResults) {
    Serial.printf("Scan complete: %d sensor(s) found\n", sensorCount);
    scanDone = true;
  }, false);
}

void loop() {
  if (!scanDone) return;

#ifdef POLL_MODE
  uint32_t now = millis();
  for (int i = 0; i < sensorCount; i++) {
    Sensor& s = sensors[i];
    if (now < s.pollAt) continue;
    s.pollAt = now + POLL_INTERVAL_MS;
    if (s.client && s.client->isConnected()) s.client->disconnect();
    connectSensor(s);
    if (s.client && s.client->isConnected()) s.client->disconnect();
  }
#else
  uint32_t now = millis();
  for (int i = 0; i < sensorCount; i++) {
    Sensor& s = sensors[i];
    if (s.active) {
      if (!s.client || !s.client->isConnected()) scheduleReconnect(s);
    } else if (now >= s.reconnectAt) {
      connectSensor(s);
    }
  }
  delay(500);
#endif
}
