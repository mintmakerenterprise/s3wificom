#pragma once

static unsigned long state_timer = 0;
static unsigned long wifiCheckTimer = 0;
static unsigned long lastMqttLoopTime = 0;

void mqtt_callback(char *topic, byte *payload, unsigned int length) {
  String msg;
  for (int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  DynamicJsonDocument doc(1024);
  deserializeJson(doc, msg);
  mqtt_digirom_command = doc["digirom"].as<String>();
  application_id = doc["application_id"].as<String>();
  hide_output = boolean(doc["hide_output"]);
  api_response = boolean(doc["api_response"]);
  ack_id = doc["ack_id"].as<String>();

  if (ack_id != "" && ack_id != "null") {
    String digiout;
    if (application_id != "") {
      digiout = "{\"name\":\"espcom\", \"board\":\"m5stick\", \"version\":\"" + String(fw_ver) + "\", \"device_uuid\": \"" + secrets_device_uuid + "\", \"ack_id\": \"" + ack_id + "\", \"application_id\": \"" + application_id + "\"}";
    } else {
      digiout = "{\"name\":\"espcom\", \"board\":\"m5stick\", \"version\":\"" + String(fw_ver) + "\", \"device_uuid\": \"" + secrets_device_uuid + "\", \"ack_id\": \"" + ack_id + "\", \"application_id\": null}";
    }
    mqttClient.publish(mqtt_topic_output.c_str(), digiout.c_str());
  }
  
  // ใช้ M5.Display ในการแสดงผลสถานะตอนรับคำสั่งใหม่
  M5.Display.fillRect(0, 26, 240, 24, TFT_ORANGE);
  M5.Display.setTextColor(TFT_BLACK, TFT_ORANGE);
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.drawCenterString("Get new command", 120, 30);
}

void wificom_init() {
  uiNeedsUpdate = true;
  drawUI();
  if (currentWificomState != WIFI_IDLE) return;

  wificomStatus = "Init...";
  uiNeedsUpdate = true;

  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);

  secrets_mqtt_username = readStringFromEEPROM(ADDR_MQTT_USER);
  secrets_mqtt_password = readStringFromEEPROM(ADDR_MQTT_PASS);
  secrets_user_uuid = readStringFromEEPROM(ADDR_USER_UUID);
  secrets_device_uuid = readStringFromEEPROM(ADDR_DEVICE_UUID);

  String ssid1 = readStringFromEEPROM(ADDR_WIFI_SSID_1);
  String pass1 = readStringFromEEPROM(ADDR_WIFI_PASS_1);
  String ssid2 = readStringFromEEPROM(ADDR_WIFI_SSID_2);
  String pass2 = readStringFromEEPROM(ADDR_WIFI_PASS_2);
  String ssid3 = readStringFromEEPROM(ADDR_WIFI_SSID_3);
  String pass3 = readStringFromEEPROM(ADDR_WIFI_PASS_3);

  bool hasWifi = false;
  wifiMulti = WiFiMulti();
  if (ssid1.length() > 0) {
    wifiMulti.addAP(ssid1.c_str(), pass1.c_str());
    hasWifi = true;
  }
  if (ssid2.length() > 0) {
    wifiMulti.addAP(ssid2.c_str(), pass2.c_str());
    hasWifi = true;
  }
  if (ssid3.length() > 0) {
    wifiMulti.addAP(ssid3.c_str(), pass3.c_str());
    hasWifi = true;
  }

  if (!hasWifi) {
    wificomStatus = "No WiFi Config";
    //Serial.println("WIFICOM: No WiFi configured.");
    currentWificomState = WIFI_IDLE;
    uiNeedsUpdate = true;
  } else {
    wificomStatus = "Connecting WiFi";
    currentWificomState = WIFI_CONNECTING;
    uiNeedsUpdate = true;
  }
}

void wificom_stop() {
  mqttClient.disconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  currentWificomState = WIFI_IDLE;
  wificomStatus = "Idle";

  if (current_digirom != nullptr) {
    delete current_digirom;
    current_digirom = nullptr;
  }
  lastExecutionResult[0] = '\0';

  uiNeedsUpdate = true;
  Serial.println("WIFICOM: Stopped.");
}

void wificom_loop() {

  if (wificomStatus != lastWificomStatus) {
    uiNeedsUpdate = true;
    lastWificomStatus = wificomStatus;
  }

  if (currentState != STATE_WIFICOM_CONNECTING && currentState != STATE_WIFICOM) {
    if (currentWificomState != WIFI_IDLE) {
      wificom_stop();
    }
    return;
  }

  if (currentState == STATE_WIFICOM_CONNECTING && currentWificomState == WIFI_IDLE) {
    wificom_init();
  }

  switch (currentWificomState) {
    case WIFI_CONNECTING:
      if (millis() - wifiCheckTimer > 500) {
        wifiCheckTimer = millis();
        if (wifiMulti.run() == WL_CONNECTED) {
          wificomStatus = "WiFi OK";
          Serial.println("WIFICOM: WiFi Connected.");
          Serial.print("WIFICOM: SSID: ");
          Serial.println(WiFi.SSID());
          currentWificomState = WIFI_SUCCESS;
          wificomStatus = "Checking Update";
          uiNeedsUpdate = true;
        } else if (millis() - state_timer > 20000) {
          wificomStatus = "WiFi Failed";
          Serial.println("WIFICOM: WiFi Connection Failed (Timeout).");
          currentWificomState = WIFI_FAILED;
          state_timer = millis();
        }
      }
      break;

    case WIFI_SUCCESS:
      {
        mqttClient.setBufferSize(1024);
        mqttClient.setServer("mqtt.wificom.dev", 1883);
        mqttClient.setCallback(mqtt_callback);
        currentWificomState = MQTT_CONNECTING;
        wificomStatus = "Connecting MQTT";
        uiNeedsUpdate = true;
        state_timer = millis();
      }
      break;

    case MQTT_CONNECTING:
      wificomStatus = "Connecting MQTT";
      mqtt_io_prefix = secrets_mqtt_username + "/f/";
      mqtt_topic_identifier = secrets_user_uuid + '-' + secrets_device_uuid;
      mqtt_topic_input = mqtt_io_prefix + mqtt_topic_identifier + "/wificom-input";
      mqtt_topic_output = mqtt_io_prefix + mqtt_topic_identifier + "/wificom-output";

      if (mqttClient.connect(clientId.c_str(), secrets_mqtt_username.c_str(), secrets_mqtt_password.c_str())) {
        mqttClient.subscribe(mqtt_topic_input.c_str());
        currentWificomState = MQTT_SUCCESS;
        wificomStatus = "MQTT OK";
        Serial.println("WIFICOM: MQTT Connected.");
        uiNeedsUpdate = true;
      } else if (millis() - state_timer > 10000) {
        wificomStatus = "MQTT Failed";
        Serial.println("WIFICOM: MQTT Connection Failed (Timeout/Credentials?).");
        currentWificomState = MQTT_FAILED;
        state_timer = millis();
        uiNeedsUpdate = true;
      }
      break;

    case MQTT_SUCCESS:
      wificomStatus = "Ready";
      currentState = STATE_WIFICOM;
      currentWificomState = WIFI_READY;
      mqtt_digirom_command = "";
      lastMqttLoopTime = millis();
      lastExecutionResult[0] = '\0';
      uiNeedsUpdate = true;
      break;

    case WIFI_READY:
      if (!mqttClient.connected()) {
        Serial.println("WIFICOM: MQTT disconnected. Reconnecting...");
        currentWificomState = MQTT_CONNECTING;
        wificomStatus = "Reconnecting";
        state_timer = millis();
        return;
      }

      mqttClient.loop();

      if (mqtt_digirom_command.length() > 0) {
        String cmd = mqtt_digirom_command;
        mqtt_digirom_command = "";

        current_digirom = nullptr;
        // --- [NEW] DMComm Serial Printing Logic ---
        const char *digirom_str = cmd.c_str();
        DigiROMType rom_type = digiROMType(digirom_str);

        if (rom_type.signal_type != kSignalTypeInfo && !hide_output) {
          Serial.print(F("got "));
          Serial.print(cmd.length());
          Serial.print(F(" bytes: "));
          Serial.print(cmd);
          Serial.print(F(" -> "));
        }

        current_digirom = createDigiROM(cmd.c_str());
        // --- [NEW] DMComm Status Print ---
        if (rom_type.signal_type == kSignalTypeInfo) {
          // Serial.print(DMCOMM_BUILD_INFO); // We don't have this defined, skip for now
        } else if (current_digirom != nullptr) {
          Serial.print(F("(new DigiROM)"));
        } else {
          Serial.print(F("(paused)"));

          // แสดงผลสถานะ PAUSED ลงบนหน้าจอ
          M5.Display.fillRect(0, 26, 240, 24, TFT_DARKGREY);
          M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREY);
          M5.Display.setFont(&fonts::FreeSans9pt7b);
          M5.Display.drawCenterString("PAUSED", 120, 30);
        }
        Serial.println();
        // --- End Status Print ---
      }

      if (millis() - lastMqttLoopTime > 5000) {
        lastMqttLoopTime = millis();
        if (current_digirom != nullptr) {
          // วาดแถบสถานะกำลังทำงาน (EXECUTE) ด้วยสีแดง
          M5.Display.fillRect(0, 0, 240, 26, TFT_RED);
          M5.Display.setTextColor(TFT_WHITE, TFT_RED);
          M5.Display.setFont(&fonts::FreeSans9pt7b);
          M5.Display.drawCenterString("EXECUTE", 120, 4);

          controller.execute(*current_digirom, 3000);
          String resultString = "";
          StringPrinter stringPrinter(resultString);
          current_digirom->printResult(stringPrinter);
          resultString.trim();

          if (resultString.length() > 0) {
            String digiout = "{\"name\":\"espcom\", \"board\":\"m5stick\", \"version\":\"" + String(fw_ver) + "\", \"application_uuid\": \"" + application_id + "\",\"device_uuid\": \"" + secrets_device_uuid + "\",\"output\": \"" + resultString + "\"}";
            mqttClient.publish(mqtt_topic_output.c_str(), digiout.c_str());
          } else {
            String digiout = "{\"name\":\"espcom\", \"board\":\"m5stick\", \"version\":\"" + String(fw_ver) + "\", \"device_uuid\": \"" + secrets_device_uuid + "\", \"output\": \"None\", \"application_uuid\": null}";
            if (application_id != "" && application_id != "null") {
              digiout = "{\"name\":\"espcom\", \"board\":\"m5stick\", \"version\":\"" + String(fw_ver) + "\", \"device_uuid\": \"" + secrets_device_uuid + "\", \"output\": \"None\", \"application_uuid\": \"" + application_id + "\"}";
            }
            mqttClient.publish(mqtt_topic_output.c_str(), digiout.c_str());
          }
          char tempResult[sizeof(lastExecutionResult)];
          strncpy(tempResult, resultString.c_str(), sizeof(tempResult) - 1);
          tempResult[sizeof(tempResult) - 1] = '\0';
          if (strcmp(lastExecutionResult, tempResult) != 0) {
            strcpy(lastExecutionResult, tempResult);
          }

        } else {
          String digiout = "{\"name\":\"espcom\", \"board\":\"m5stick\", \"version\":\"" + String(fw_ver) + "\", \"device_uuid\": \"" + secrets_device_uuid + "\", \"output\": \"None\", \"application_uuid\": null}";
          if (application_id != "" && application_id != "null") {
            digiout = "{\"name\":\"espcom\", \"board\":\"m5stick\", \"version\":\"" + String(fw_ver) + "\", \"device_uuid\": \"" + secrets_device_uuid + "\", \"output\": \"None\", \"application_uuid\": \"" + application_id + "\"}";
          }
          mqttClient.publish(mqtt_topic_output.c_str(), digiout.c_str());
        }
        uiNeedsUpdate = true;
      }
      break;

    case WIFI_FAILED:
    case MQTT_FAILED:
      if (millis() - state_timer > 5000) {
        wificom_stop();
        currentState = STATE_MAIN_MENU;
        uiNeedsUpdate = true;
      }
      break;

    case WIFI_IDLE:
      break;
  }
}

void handleWifiCom() {
  if (btnCancelPressed) {
    wificom_stop();
    currentState = STATE_MAIN_MENU;
    lastActivityTime = millis();
    textScrollOffset = 0;
    lastTextScrollTime = millis();
    uiNeedsUpdate = true;
  }
}
