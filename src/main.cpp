
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
#define SCAN_SECONDS 30

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

// ── Sensor registry ───────────────────────────────────────────────────────────

struct SensorInfo {
  String   mac;
  String   deviceName, manufacturer, model, hwRev, fwRev, swRev;
  uint16_t appearance   = 0;
  float    connMinMs    = 0, connMaxMs = 0;
  uint16_t connLatency  = 0, connTimeoutMs = 0;
  uint32_t dataRateMs   = 0, dataRateMinMs = 0, dataRateMaxMs = 0;
};

// ── Sensor record ─────────────────────────────────────────────────────────────

struct Sensor {
  BLEAddress   addr;
  char         tag[9];    // "XX:XX:XX\0" — last 3 octets
  BLEClient*   client    = nullptr;
  bool         active    = false;
  bool         infoRead  = false;
  SensorInfo   info;
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

static String readStrChar(BLERemoteService* svc, const char* uuid) {
  auto* c = svc->getCharacteristic(BLEUUID(uuid));
  return (c && c->canRead()) ? c->readValue() : String();
}

static void readStaticInfo(Sensor& s, BLEClient* client) {
  const char* tag = s.tag;
  SensorInfo& info = s.info;

  info.mac = s.addr.toString().c_str();

  // Generic Access (0x1800)
  auto* gap = client->getService(BLEUUID((uint16_t)0x1800));
  if (gap) {
    info.deviceName = readStrChar(gap, "00002a00-0000-1000-8000-00805f9b34fb");
    auto* c = gap->getCharacteristic(BLEUUID((uint16_t)0x2a01));
    if (c && c->canRead()) {
      String raw = c->readValue();
      if (raw.length() >= 2)
        info.appearance = (uint8_t)raw[0] | ((uint8_t)raw[1] << 8);
    }
    c = gap->getCharacteristic(BLEUUID((uint16_t)0x2a04));
    if (c && c->canRead()) {
      String raw = c->readValue();
      if (raw.length() >= 8) {
        auto u16 = [&](int i){ return (uint16_t)((uint8_t)raw[i]|((uint8_t)raw[i+1]<<8)); };
        info.connMinMs     = u16(0) * 1.25f;
        info.connMaxMs     = u16(2) * 1.25f;
        info.connLatency   = u16(4);
        info.connTimeoutMs = u16(6) * 10;
      }
    }
  }

  // Device Information (0x180A)
  auto* di = client->getService(BLEUUID((uint16_t)0x180a));
  if (di) {
    info.manufacturer = readStrChar(di, "00002a29-0000-1000-8000-00805f9b34fb");
    info.model        = readStrChar(di, "00002a24-0000-1000-8000-00805f9b34fb");
    info.hwRev        = readStrChar(di, "00002a27-0000-1000-8000-00805f9b34fb");
    info.fwRev        = readStrChar(di, "00002a26-0000-1000-8000-00805f9b34fb");
    info.swRev        = readStrChar(di, "00002a28-0000-1000-8000-00805f9b34fb");
  }

  // Print once
  logf(tag, "--- Device descriptor ---");
  logf(tag, "mac:          %s",     info.mac.c_str());
  logf(tag, "name:         %s",     info.deviceName.c_str());
  logf(tag, "appearance:   0x%04X", info.appearance);
  logf(tag, "conn params:  min=%.1fms max=%.1fms latency=%u timeout=%ums",
       info.connMinMs, info.connMaxMs, info.connLatency, info.connTimeoutMs);
  logf(tag, "manufacturer: %s",     info.manufacturer.c_str());
  logf(tag, "model:        %s",     info.model.c_str());
  logf(tag, "hw rev:       %s",     info.hwRev.c_str());
  logf(tag, "fw rev:       %s",     info.fwRev.c_str());
  logf(tag, "sw rev:       %s",     info.swRev.c_str());
}

static bool connectSensor(Sensor& s) {
  const char* tag = s.tag;

  if (!s.client) s.client = BLEDevice::createClient();
  if (!s.client->connect(s.addr)) {
    logf(tag, "connection failed");
    return false;
  }
  logf(tag, "connected");

  if (!s.infoRead) {
    readStaticInfo(s, s.client);
    s.infoRead = true;
  }

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

    // Data rate (f000ab32) — read once: 3x uint32_t LE: current_ms, min_ms, max_ms
    if (!s.infoRead) {
      auto* rateC = svc->getCharacteristic(BLEUUID(DATARATE_CHAR));
      if (rateC && rateC->canRead()) {
        String raw = rateC->readValue();
        if (raw.length() >= 12) {
          auto u32 = [&](int i) -> uint32_t {
            return (uint8_t)raw[i] | ((uint8_t)raw[i+1]<<8) | ((uint8_t)raw[i+2]<<16) | ((uint8_t)raw[i+3]<<24);
          };
          s.info.dataRateMs    = u32(0);
          s.info.dataRateMinMs = u32(4);
          s.info.dataRateMaxMs = u32(8);
          logf(tag, "data rate: %ums  (min %ums  max %ums)",
               s.info.dataRateMs, s.info.dataRateMinMs, s.info.dataRateMaxMs);
        } else {
          Serial.printf("[%s] data rate raw (%u bytes):", tag, raw.length());
          for (size_t i = 0; i < raw.length(); i++) Serial.printf(" %02X", (uint8_t)raw[i]);
          Serial.println();
        }
      }
    }

    // Data (f000ab31) — subscribe + decode
    auto* dataC = svc->getCharacteristic(BLEUUID(DATA_CHAR));
    if (!dataC) { logf(tag, "data char not found"); s.client->disconnect(); return false; }

#ifdef POLL_MODE
    // Poll mode: read once, decode, then caller disconnects
    if (dataC->canRead()) {
      String raw = dataC->readValue();
      decodeData(tag, (uint8_t*)raw.c_str(), raw.length());
    } else {
      logf(tag, "data char not readable");
    }
#else
    // Persistent mode: subscribe to notifications
    if (dataC->canNotify()) {
      dataC->registerForNotify([tag = String(s.tag)](BLERemoteCharacteristic*, uint8_t* d, size_t len, bool) {
        decodeData(tag.c_str(), d, len);
      });
      logf(tag, "subscribed to data notifications");
    } else {
      logf(tag, "data char has no notify — cannot use persistent mode");
      s.client->disconnect();
      return false;
    }
#endif
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

    bool hasBaseUUID = dev.haveServiceUUID() && dev.isAdvertisingService(BLEUUID(BASE_UUID));
    bool hasName     = dev.haveName() && dev.getName() == DEVICE_NAME;

    // Require name match always. UUID alone is too broad (other devices use the TI base UUID).
    // Accept UUID+name (active scan, scan response received) or name-only (scan response not yet in).
    if (!hasName) return;
    if (dev.haveServiceUUID() && !hasBaseUUID) return;  // has UUIDs but wrong ones — reject

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
  scan->start(SCAN_SECONDS, [](BLEScanResults) {
    Serial.printf("Scan complete: %d sensor(s) found\n", sensorCount);
    scanDone = true;
  }, false);
}

void loop() {
  if (!scanDone) return;

#ifdef POLL_MODE
  // One sensor per loop() call, round-robin. Only one BLE connection active at a time.
  static int pollIdx = 0;
  if (sensorCount == 0) { delay(100); return; }

  Sensor& s = sensors[pollIdx];
  uint32_t now = millis();

  if (now >= s.pollAt) {
    s.pollAt = now + POLL_INTERVAL_MS;
    if (s.client && s.client->isConnected()) {
      s.client->disconnect();
      delay(200);  // let NimBLE release the connection before opening the next
    }
    connectSensor(s);
    if (s.client && s.client->isConnected()) {
      s.client->disconnect();
      delay(200);
    }
  }

  pollIdx = (pollIdx + 1) % sensorCount;
  delay(50);
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
