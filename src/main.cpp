
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

static const char* batteryMAC = "dc:23:4d:eb:88:46";  // CHANGE TO YOUR BATTERY MAC
static BLEAdvertisedDevice* myDevice = nullptr;
static bool doConnect = false;
static bool connected = false;

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.getAddress().toString() == String(batteryMAC)) {
      Serial.printf("Found Parkside battery: %s\n", advertisedDevice.getAddress().toString().c_str());
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      BLEDevice::getScan()->stop();
    }
  }
};

void connectToServer() {
  BLEClient* pClient = BLEDevice::createClient();
  pClient->connect(myDevice);

  Serial.println("Connected to battery");

  std::map<std::string, BLERemoteService*>* services = pClient->getServices();
  for (auto& servicePair : *services) {
    BLERemoteService* pService = servicePair.second;
    Serial.printf("Service: %s\n", pService->getUUID().toString().c_str());

    std::map<std::string, BLERemoteCharacteristic*>* chars = pService->getCharacteristics();
    for (auto& charPair : *chars) {
      BLERemoteCharacteristic* pChar = charPair.second;
      Serial.printf("  Char: %s\n",
                    pChar->getUUID().toString().c_str());

      if (pChar->canRead()) {
        String value = pChar->readValue();
        Serial.printf("    Value (hex): ");
        for (unsigned char c : value) Serial.printf("%02X ", c);
        Serial.println();
      }
    }
  }
  connected = true;
}

void setup() {
  Serial.begin(115200);
  Serial.println("Parkside Battery Decoder (Arduino 3 builtin BLE)");

  BLEDevice::init("");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->start(10, false);
}

void loop() {
  if (doConnect) {
    connectToServer();
    doConnect = false;
  }
  delay(1000);
}