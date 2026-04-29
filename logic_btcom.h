#pragma once

static const char *NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char *NUS_CHAR_UUID_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static const char *NUS_CHAR_UUID_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

static NimBLEServer *pServer = nullptr;
static NimBLECharacteristic *pRxCharacteristic = nullptr;
static NimBLECharacteristic *pTxCharacteristic = nullptr;

String ble_received_command = "";
bool btcom_initialized = false;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
    Serial.printf("Client %s connected (handle %u)\n",
                  connInfo.getAddress().toString().c_str(),
                  connInfo.getConnHandle());
    pServer->updateConnParams(connInfo.getConnHandle(), 24, 48, 0, 180);
    btcomStatus = "Connected";
    uiNeedsUpdate = true;
  }
  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override {
    Serial.println("Client disconnected, advertising again");
    btcomStatus = "Advertising";
    uiNeedsUpdate = true;
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
private:
  std::string rxBuffer = "";

public:
  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
    std::string val = pCharacteristic->getValue();
    rxBuffer += val;

    if (rxBuffer.find('\n') != std::string::npos || rxBuffer.find('\r') != std::string::npos) {
      if (pTxCharacteristic && pServer->getConnectedCount() > 0) {
        String BLECOMM = String(rxBuffer.c_str());
        BLECOMM.trim();
        Serial.printf("Received %u bytes: %s\n", BLECOMM.length(), BLECOMM.c_str());

        // นำคำสั่งเก็บเข้า buffer รอให้ handleBtcom() นำไป Execute ด้วย U8g2 / Controller ต่อไป
        ble_received_command = BLECOMM;
      }
      rxBuffer = "";
    }
  }
};

void startBtcom() {
  if (btcom_initialized) {
    NimBLEDevice::startAdvertising();
    btcomStatus = "Advertising";
    return;
  }

  // ใช้ Mac Address ของ WiFi สร้างชื่อเพื่อไม่ให้ชื่อซ้ำกัน
  WiFi.mode(WIFI_STA);
  delay(300);
  String btname = "BT-COM-" + String(WiFi.macAddress()).substring(12);
  btname.replace(":", "");

  NimBLEDevice::init(btname.c_str());

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService *nus = pServer->createService(NUS_SERVICE_UUID);

  pRxCharacteristic = nus->createCharacteristic(
    NUS_CHAR_UUID_RX,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pRxCharacteristic->setCallbacks(new RxCallbacks());

  pTxCharacteristic = nus->createCharacteristic(
    NUS_CHAR_UUID_TX,
    NIMBLE_PROPERTY::NOTIFY);

  nus->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName(btname.c_str());
  adv->addServiceUUID(nus->getUUID());

  NimBLEDevice::startAdvertising();

  Serial.println("Advertising started");
  btcomStatus = "Advertising";
  btcom_initialized = true;
}

void handleBtcom() {
  if (btnCancelPressed) {
    currentState = STATE_MAIN_MENU;
    lastActivityTime = millis();
    uiNeedsUpdate = true;
    return;
  }

  if (ble_received_command.length() > 0) {
    if (current_digirom != nullptr) {
      delete current_digirom;
    }
    current_digirom = nullptr;
    const char *digirom_str = ble_received_command.c_str();
    DigiROMType rom_type = digiROMType(digirom_str);

    if (rom_type.signal_type != kSignalTypeInfo) {
      Serial.print(F("got "));
      Serial.print(ble_received_command.length());
      Serial.print(F(" bytes: "));
      Serial.print(ble_received_command);
      Serial.print(F(" -> "));
    }

    current_digirom = createDigiROM(digirom_str);

    if (rom_type.signal_type == kSignalTypeInfo) {
      // Info command
    } else if (current_digirom != nullptr) {
      Serial.print(F("(new DigiROM)"));
    } else {
      Serial.print(F("(paused)"));
    }
    Serial.println();

    if (current_digirom != nullptr) {
      prevState = STATE_BTCOM;
      strncpy(selectedName, "BTCOM Command", sizeof(selectedName) - 1);
      selectedName[sizeof(selectedName) - 1] = '\0';
      currentState = STATE_EXECUTING;
      executionStartTime = millis();
      lastExecutionTime = millis();
      strcpy(executionResult, "");
      lastExecutionResult[0] = '\0';
      textScrollOffset = 0;
      lastTextScrollTime = millis();
      uiNeedsUpdate = true;
    } else {
      strcpy(executionResult, "PAUSED");
      uiNeedsUpdate = true;
    }
    ble_received_command = "";  // ล้างคำสั่งหลังจากรับมา
  }
}

void sendBtcomResult(String res) {
  if (pTxCharacteristic && pServer && pServer->getConnectedCount() > 0) {
    String outStr = res + "\n";
    pTxCharacteristic->setValue(std::string(outStr.c_str(), outStr.length()));
    pTxCharacteristic->notify();
  }
}