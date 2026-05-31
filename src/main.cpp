
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// TE Connectivity M5600/U5600 pressure sensor
// Protocol: https://www.ttieurope.com/content/dam/tti-europe/manufacturers/te-connectivity/resources/TE_M5600_U5600_Software_Manual.pdf
// Device advertises name "TESS 5600", not the pressure service UUID directly.

static const char* DEVICE_NAME   = "TESS 5600";
static const char* SERVICE_UUID  = "f000ab30-0451-4000-b000-000000000000";
static const char* DATA_UUID     = "f000ab31-0451-4000-b000-000000000000";
static const char* STATUS_UUID   = "f000ab3f-0451-4000-b000-000000000000";
static const char* BASE_UUID   = "f0000000-0451-4000-b000-000000000000";

static BLEAdvertisedDevice* myDevice = nullptr;
static bool doConnect = false;
static bool connected = false;

// Active scan required: 128-bit service UUID is in the scan response, not the primary advert.
// Match on BASE_UUID; fall back to device name if scan response hasn't arrived yet.
class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    bool match = (dev.haveServiceUUID() && dev.isAdvertisingService(BLEUUID(BASE_UUID)))
              || (dev.haveName() && dev.getName() == DEVICE_NAME);
    if (match) {
      Serial.printf("Found %s: %s\n", DEVICE_NAME, dev.getAddress().toString().c_str());
      myDevice = new BLEAdvertisedDevice(dev);
      doConnect = true;
      BLEDevice::getScan()->stop();
    }
  }
};

// DATA characteristic notify callback — 14-byte payload
void onDataNotify(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len < 14) {
    Serial.printf("Short data packet: %u bytes\n", len);
    return;
  }

  int16_t  T    = (int16_t)((data[1] << 8) | data[0]);
  int32_t  P    = (int32_t)((data[5] << 24) | (data[4] << 16) | (data[3] << 8) | data[2]);
  int32_t  Pmin = (int32_t)((data[9] << 24) | (data[8] << 16) | (data[7] << 8) | data[6]);
  int32_t  Pmax = (int32_t)((data[13] << 24) | (data[12] << 16) | (data[11] << 8) | data[10]);

  if (T == 0x7FFF) {
    Serial.println("Temperature: ERROR");
  } else {
    Serial.printf("Temperature:  %.2f °C\n", T / 100.0f);
  }

  if (P == (int32_t)0x7FFFFFFF) {
    Serial.println("Pressure:     ERROR");
  } else {
    Serial.printf("Pressure:     %.1f Pa  /  %.4f psi\n", P / 10.0f, P / 10.0f / 6894.7f);
  }

  if (Pmin != (int32_t)0x7FFFFFFF)
    Serial.printf("Pmin:         %.1f Pa\n", Pmin / 10.0f);
  if (Pmax != (int32_t)0x7FFFFFFF)
    Serial.printf("Pmax:         %.1f Pa\n", Pmax / 10.0f);
}

void connectToSensor() {
  BLEClient* client = BLEDevice::createClient();
  if (!client->connect(myDevice)) {
    Serial.println("Connection failed");
    return;
  }
  Serial.printf("Connected: %s\n", myDevice->getAddress().toString().c_str());

  BLERemoteService* svc = client->getService(BLEUUID(SERVICE_UUID));
  if (!svc) {
    Serial.println("TE5600 service not found");
    client->disconnect();
    return;
  }

  // Read and report status
  BLERemoteCharacteristic* statusChar = svc->getCharacteristic(BLEUUID(STATUS_UUID));
  if (statusChar && statusChar->canRead()) {
    String raw = statusChar->readValue();
    uint8_t s = raw.length() > 0 ? (uint8_t)raw[0] : 0xFF;
    Serial.printf("Sensor status: %s (0x%02X)\n", s == 0x00 ? "OK" : "ERROR", s);
  }

  // Subscribe to DATA notifications
  BLERemoteCharacteristic* dataChar = svc->getCharacteristic(BLEUUID(DATA_UUID));
  if (!dataChar) {
    Serial.println("Data characteristic not found");
    client->disconnect();
    return;
  }

  if (dataChar->canNotify()) {
    dataChar->registerForNotify(onDataNotify);
    Serial.println("Subscribed to notifications. Waiting for data...");
  } else if (dataChar->canRead()) {
    // Fallback: one-shot read if no notify support
    String raw = dataChar->readValue();
    onDataNotify(nullptr, (uint8_t*)raw.c_str(), raw.length(), false);
  }

  connected = true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("TE5600 Pressure Sensor Decoder");

  BLEDevice::init("");
  BLEScan* scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  scan->setActiveScan(true);  // required: 128-bit UUID is in scan response, not primary advert
  scan->start(60, false);
}

void loop() {
  if (doConnect) {
    connectToSensor();
    doConnect = false;
  }
  delay(1000);
}
