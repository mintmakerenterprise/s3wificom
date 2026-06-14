#include <WiFi.h>
#include <WiFiMulti.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <EEPROM.h>
#include <PubSubClient.h>
#include <ImprovWiFiLibrary.h>
#include <DMComm.h>
#include <TFT_eSPI.h>
#include <NimBLEDevice.h>

#include "lib/StringStream.h"
#include "lib/menu.h"

using namespace DMComm;

// ========================================
// Board / app config
// ========================================
static const char* MQTT_BROKER = "mqtt.wificom.dev";
static const int MQTT_PORT = 1883;

static const int PIN_BTN_A = 0;
static const int PIN_BTN_B = 14;
static const int PIN_BAT_ADC = 4;
static const int PIN_LCD_BL = 38;

static const int INPUT_PIN = 3;
static const int OUTPUT_PIN = 1;

static const int EEPROM_SIZE = 512;
static const int ANALOG_RESOLUTION = 12;

static const char* AP_SSID = "WiFiCom-S3-Setup";
static const char* AP_PASS = "12345678";

String boardname = "TTGO_TDisplay_S3";
String version = "esp32s3-wificom-tbpubsub-buffer-2026-05-03";

static const bool SERIAL_DEBUG = true;

// ========================================
// Theme Colors
// ========================================
const uint16_t UI_BG            = TFT_BLACK;
const uint16_t UI_PANEL         = 0x10A2;
const uint16_t UI_PANEL_ALT     = 0x0841;
const uint16_t UI_BORDER        = 0x2965;
const uint16_t UI_HEADER        = 0x0598;
const uint16_t UI_TEXT          = TFT_WHITE;
const uint16_t UI_TEXT_DIM      = 0xBDF7;
const uint16_t UI_LABEL         = 0xFFE0;
const uint16_t UI_ACCENT        = 0x04FF;
const uint16_t UI_SUCCESS       = 0x07E0;
const uint16_t UI_WARN          = 0xFD20;
const uint16_t UI_ERROR         = 0xF800;
const uint16_t UI_SELECT_BG     = 0x043F;
const uint16_t UI_SELECT_EDGE   = 0x05FF;
const uint16_t UI_FOOT_BG       = 0x0000;

// ========================================
// Globals
// ========================================
WiFiMulti wifiMulti;
AsyncWebServer server(80);
WiFiClient espClient;
PubSubClient client(espClient);
ImprovWiFi improvSerial(&Serial);
TFT_eSPI tft = TFT_eSPI();

DComOutput output = DComOutput(OUTPUT_PIN, DMCOMM_NO_PIN);
DigitalProngInput input = DigitalProngInput(INPUT_PIN);
ClassicCommunicator classic_comm = ClassicCommunicator(output, input);
ColorCommunicator color_comm = ColorCommunicator(output, input);
Controller controller = Controller();

String secrets_mqtt_username = "";
String secrets_mqtt_password = "";
String secrets_user_uuid = "";
String secrets_device_uuid = "";

String mqtt_io_prefix;
String mqtt_topic_identifier;
String mqtt_topic_input;
String mqtt_topic_output;

String mqtt_digirom = "";
String application_id = "";
String ack_id = "";
String last_digi_result = "";

bool hide_output = false;
bool api_response = false;
bool reboot_flag = false;
bool ap_mode_started = false;
bool server_started = false;
bool wifi_multi_loaded = false;

BaseDigiROM* buffer_digirom = nullptr;
unsigned long last_batt_ms = 0;

// ========================================
// NimBLE BTCOM Globals
// ========================================
static const char *NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char *NUS_CHAR_UUID_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
static const char *NUS_CHAR_UUID_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

static NimBLEServer *pServer = nullptr;
static NimBLECharacteristic *pRxCharacteristic = nullptr;
static NimBLECharacteristic *pTxCharacteristic = nullptr;

String btcomStatus = "Advertising";
bool btcom_initialized = false;

extern int prog_index;
void btcommode();

namespace UI {
  void info(const String& s) {
    if (SERIAL_DEBUG) Serial.println("[INFO] " + s);
  }
  void warn(const String& s) {
    if (SERIAL_DEBUG) Serial.println("[WARN] " + s);
  }
  void stat(const String& s) {
    if (SERIAL_DEBUG) Serial.println("[STAT] " + s);
  }
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
    UI::info("BTCOM Client connected");
    pServer->updateConnParams(connInfo.getConnHandle(), 24, 48, 0, 180);
    btcomStatus = "Connected";
    if (prog_index == 11) btcommode();
  }
  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override {
    UI::info("BTCOM Client disconnected");
    btcomStatus = "Advertising";
    NimBLEDevice::startAdvertising();
    if (prog_index == 11) btcommode();
  }
};

// ========================================
// Command Engine States
// ========================================
String active_digirom = "";
String active_source = "";
String pending_digirom = "";
String pending_source = "";
bool active_repeat_enabled = false;
unsigned long last_execute_ms = 0;
unsigned long execute_interval_ms = 1000;

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
        UI::info("BTCOM Received: " + BLECOMM);
        
        pending_digirom = BLECOMM;
        pending_source = "BTCOM";
        application_id = "btcom";
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

  WiFi.mode(WIFI_STA);
  delay(300); 

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String btname = "BT-COM-";
  
  if (mac.length() >= 4) {
    btname += mac.substring(mac.length() - 4);
  } else {
    btname += String(random(1000, 9999));
  }

  NimBLEDevice::init(btname.c_str());
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); 

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  
  NimBLEService *nus = pServer->createService(NUS_SERVICE_UUID);

  pRxCharacteristic = nus->createCharacteristic(
      NUS_CHAR_UUID_RX, 
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pRxCharacteristic->setCallbacks(new RxCallbacks());

  pTxCharacteristic = nus->createCharacteristic(
      NUS_CHAR_UUID_TX, 
      NIMBLE_PROPERTY::NOTIFY
  );
  
  nus->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName(btname.c_str());
  adv->addServiceUUID(nus->getUUID());

  NimBLEDevice::startAdvertising();
  
  btcomStatus = "Advertising";
  btcom_initialized = true;
  UI::info("BTCOM started: " + btname);
}

void sendBtcomResult(String res) {
  if (pTxCharacteristic && pServer && pServer->getConnectedCount() > 0) {
    String outStr = res + "\n";
    pTxCharacteristic->setValue(std::string(outStr.c_str(), outStr.length()));
    pTxCharacteristic->notify();
  }
}

// ========================================
// WiFiCom Presence
// ========================================
unsigned long last_presence_ms = 0;
unsigned long heartbeat_pause_until_ms = 0;
const unsigned long PRESENCE_INTERVAL_MS = 15000;
const unsigned long HEARTBEAT_PAUSE_MS = 30000;

// ========================================
// Menus & UI States
// ========================================
enum ScreenMode {
  SCREEN_MAIN = 0,
  SCREEN_WIFI_SETUP_MENU = 1,
  SCREEN_WIFI_SETUP_DETAILS = 2,
  SCREEN_WIFI_SETUP_HOTSPOT = 3,
  SCREEN_WIFI_SETUP_CLEAR = 4,
  SCREEN_WIFICOM = 5,
  SCREEN_ACOM = 6,
  SCREEN_PUNCH_DEVICE = 7,
  SCREEN_PUNCH_SUBMENU = 8,
  SCREEN_BATTERY = 9,
  SCREEN_PUNCH_EXECUTE = 10,
  SCREEN_BTCOM = 11,
  SCREEN_WIFICOM_MENU = 12,
  SCREEN_SETTING_MENU = 13,
  SCREEN_ACOM_MENU = 14
};

int prog_index = SCREEN_MAIN;
int menu_id = 0;

String mainMenuItems[] = {
  "WiFiCom",
  "ACOM Mode",
  "BTCOM Mode",
  "Setting"
};
const int MAIN_MENU_COUNT = 4;

int wificom_menu_id = 0;
String wificomMenuItems[] = {
  "Start WiFiCom",
  "WiFi Setup",
  "Back"
};
const int WIFICOM_MENU_COUNT = 3;

int acom_menu_id = 0;
String acomMenuItems[] = {
  "Serial start",
  "PunchingBag",
  "Back"
};
const int ACOM_MENU_COUNT = 3;

int setting_menu_id = 0;
String settingMenuItems[] = {
  "Battery Info",
  "Shutdown",
  "Back"
};
const int SETTING_MENU_COUNT = 3;

int wifi_setup_menu_id = 0;
String wifiSetupItems[] = {
  "Show Saved Details",
  "Start Hotspot",
  "Clear Config",
  "Back"
};
const int WIFI_SETUP_COUNT = 4;

String* temp_array = nullptr;
String* temp_code_array = nullptr;
int punch_index = 0;
int sub_punch_index = 0;
int submenu_size = 0;
String current_punch_name = "";
String current_punch_code = "";

// ========================================
// Button Logic
// ========================================
struct ButtonState {
  int pin = -1;
  bool prev = HIGH;
  unsigned long lastPressEvent = 0;

  void begin(int p) {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
    prev = digitalRead(pin);
  }

  bool wasPressed() {
    bool now = digitalRead(pin);
    bool hit = (prev == HIGH && now == LOW);
    prev = now;
    if (hit && millis() - lastPressEvent > 180) {
      lastPressEvent = millis();
      return true;
    }
    return false;
  }
};
ButtonState btnA;
ButtonState btnB;

// ========================================
// EEPROM Helpers
// ========================================
void writeStringToEEPROM(int addrOffset, const String& strToWrite) {
  byte len = strToWrite.length();
  EEPROM.write(addrOffset, len);
  for (int i = 0; i < len; i++) {
    EEPROM.write(addrOffset + 1 + i, strToWrite[i]);
  }
  EEPROM.commit();
}

String readStringFromEEPROM(int addrOffset) {
  int newStrLen = EEPROM.read(addrOffset);
  if (newStrLen < 0 || newStrLen > 50) return "";
  char data[newStrLen + 1];
  for (int i = 0; i < newStrLen; i++) {
    data[i] = EEPROM.read(addrOffset + 1 + i);
  }
  data[newStrLen] = '\0';
  return String(data);
}

// ========================================
// Battery Helpers
// ========================================
uint32_t getBatteryVoltageMv() {
  uint32_t raw = analogRead(PIN_BAT_ADC);
  return (raw * 2UL * 3300UL) / 4095UL;
}

int getBatteryPercent() {
  int pct = map((int)getBatteryVoltageMv(), 3300, 4200, 0, 100);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

void batt_show_serial() {
  if (!SERIAL_DEBUG) return;
  Serial.print("[BAT] ");
  Serial.print(getBatteryVoltageMv());
  Serial.println(" mV");
}

// ========================================
// JSON Parser
// ========================================
String jsonGetString(const String& src, const String& key) {
  String pattern = "\"" + key + "\"";
  int keyPos = src.indexOf(pattern);
  if (keyPos < 0) return "";
  
  int colonPos = src.indexOf(':', keyPos + pattern.length());
  if (colonPos < 0) return "";
  
  int i = colonPos + 1;
  while (i < (int)src.length() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r')) i++;
  
  if (i >= (int)src.length()) return "";
  if (src.startsWith("null", i)) return "";
  
  if (src[i] == '"') {
    i++;
    String out = "";
    while (i < (int)src.length()) {
      char c = src[i];
      if (c == '\\' && i + 1 < (int)src.length()) {
        out += src[i + 1];
        i += 2;
        continue;
      }
      if (c == '"') break;
      out += c;
      i++;
    }
    return out;
  }
  
  int end = i;
  while (end < (int)src.length() && src[end] != ',' && src[end] != '}' && src[end] != '\n' && src[end] != '\r') end++;
  
  String out = src.substring(i, end);
  out.trim();
  return out;
}

bool jsonGetBool(const String& src, const String& key, bool defaultValue = false) {
  String value = jsonGetString(src, key);
  value.trim();
  value.toLowerCase();
  if (value == "true") return true;
  if (value == "false") return false;
  return defaultValue;
}

// ========================================
// Display Drawing Helpers
// ========================================
String wifiStatusShort() {
  return WiFi.status() == WL_CONNECTED ? "W:ON" : "W:OFF";
}

String mqttStatusShort() {
  return client.connected() ? "M:ON" : "M:OFF";
}

void drawFooterStatusBar(const String& centerText = "", const String& btnHint = "B:Next A:OK") {
  int y = tft.height() - 12;
  tft.fillRect(0, y, tft.width(), 12, UI_FOOT_BG);
  tft.drawFastHLine(0, y, tft.width(), UI_BORDER);
  
  tft.setTextSize(1);
  tft.setTextColor(UI_TEXT_DIM, UI_FOOT_BG);
  tft.setCursor(4, y + 2);
  tft.print(getBatteryPercent());
  tft.print("% ");
  tft.print(wifiStatusShort());
  tft.print(" ");
  tft.print(mqttStatusShort());
  tft.print(" TX1 RX3");
  
  if (centerText != "") {
    tft.setTextColor(UI_LABEL, UI_FOOT_BG);
    tft.setCursor(118, y + 2);
    tft.print(centerText);
  }
  
  int x = tft.width() - (btnHint.length() * 6) - 4;
  if (x < 170) x = 170;
  tft.setTextColor(UI_ACCENT, UI_FOOT_BG);
  tft.setCursor(x, y + 2);
  tft.print(btnHint);
}

void setupDisplay() {
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(UI_BG);
  tft.setTextColor(UI_TEXT, UI_BG);
  tft.setTextSize(2);
}

void drawHeader(const String& title) {
  tft.fillScreen(UI_BG);
  tft.fillRoundRect(6, 4, tft.width() - 12, 24, 6, UI_PANEL);
  tft.drawRoundRect(6, 4, tft.width() - 12, 24, 6, UI_HEADER);
  tft.setTextColor(UI_HEADER, UI_PANEL);
  tft.setTextSize(2);
  tft.setCursor(12, 8);
  tft.println(title);
}

void drawFooter(const String& centerText = "", const String& btnHint = "B:Next A:OK") {
  drawFooterStatusBar(centerText, btnHint);
}

void drawMenuItem(int row, bool selected, const String& text, bool small = false) {
  int y = 36 + row * 20;
  int x = 8;
  int w = tft.width() - 16;
  int h = 18;
  
  String shown = text;
  shown.trim();
  if (shown.length() > 24) shown = shown.substring(0, 24);
  
  tft.fillRect(x, y, w, h, UI_BG);
  
  if (selected) {
    tft.fillRoundRect(x, y, w, h, 5, UI_SELECT_BG);
    tft.drawRoundRect(x, y, w, h, 5, UI_SELECT_EDGE);
    tft.fillTriangle(x + 6, y + 9, x + 12, y + 4, x + 12, y + 14, UI_SELECT_EDGE);
    tft.setTextColor(UI_TEXT, UI_SELECT_BG);
  } else {
    tft.setTextColor(UI_TEXT_DIM, UI_BG);
  }
  
  tft.setTextSize(small ? 1 : 2);
  tft.setCursor(x + 18, y + (small ? 4 : 1));
  tft.println(shown);
}

void drawCard(int y, int h, uint16_t borderColor = UI_BORDER) {
  tft.fillRoundRect(8, y, tft.width() - 16, h, 8, UI_PANEL_ALT);
  tft.drawRoundRect(8, y, tft.width() - 16, h, 8, borderColor);
}

void drawBigCenteredValue(int y, const String& text, uint16_t color = UI_SUCCESS) {
  tft.setTextColor(color, UI_BG);
  tft.setTextSize(3);
  int w = text.length() * 18;
  int x = (tft.width() - w) / 2;
  if (x < 0) x = 0;
  tft.setCursor(x, y);
  tft.println(text);
}

void drawInlineKV(int row, const String& label, const String& value, uint16_t valueColor = UI_TEXT, bool small = true) {
  int y = 38 + row * 20;
  String shown = value;
  shown.replace("\n", " ");
  shown.replace("\r", " ");
  if (shown.length() > 22) shown = shown.substring(0, 22);
  
  tft.fillRect(8, y, tft.width() - 16, 18, UI_BG);
  tft.setTextSize(small ? 1 : 2);
  tft.setTextColor(UI_LABEL, UI_BG);
  tft.setCursor(10, y + (small ? 4 : 0));
  tft.print(label);
  tft.print(" : ");
  tft.setTextColor(valueColor, UI_BG);
  tft.println(shown);
}

void drawInlineWide(int row, const String& label, const String& value, uint16_t valueColor = UI_TEXT) {
  int y = 38 + row * 20;
  String shown = value;
  shown.replace("\n", " ");
  shown.replace("\r", " ");
  if (shown.length() > 24) shown = shown.substring(0, 24);
  
  tft.fillRect(8, y, tft.width() - 16, 18, UI_BG);
  tft.setTextSize(1);
  tft.setTextColor(UI_LABEL, UI_BG);
  tft.setCursor(10, y + 4);
  tft.print(label);
  tft.print(" : ");
  tft.setTextColor(valueColor, UI_BG);
  tft.println(shown);
}

String compactResult(const String& input, int maxLen = 22) {
  String out = input;
  out.replace("\n", " ");
  out.replace("\r", " ");
  out.trim();
  if (out.length() > maxLen) out = out.substring(0, maxLen);
  return out;
}

// ========================================
// EEPROM Setup Webserver
// ========================================
String configPageHtml(const String& message = "") {
  String html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<title>WiFiCom S3 Setup</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;padding:20px;max-width:700px;margin:auto;}";
  html += "input[type=text]{width:100%;padding:10px;margin:6px 0 14px;box-sizing:border-box;}";
  html += "input[type=submit],button{width:100%;padding:12px;margin:8px 0;}";
  html += ".msg{color:green;font-weight:bold;margin-bottom:10px;}";
  html += "</style></head><body>";
  html += "<h2>ESP32-S3 WiFiCom Setup</h2>";
  
  if (message != "") {
    html += "<div class=\"msg\">" + message + "</div>";
  }
  
  html += "<form action=\"/save\">";
  html += "<h3>WiFi</h3>";
  html += "<label>WiFi SSID 1</label><input type=\"text\" name=\"wifi_ssid1\" value=\"" + readStringFromEEPROM(0) + "\">";
  html += "<label>WiFi Password 1</label><input type=\"text\" name=\"wifi_password1\" value=\"" + readStringFromEEPROM(50) + "\">";
  html += "<label>WiFi SSID 2</label><input type=\"text\" name=\"wifi_ssid2\" value=\"" + readStringFromEEPROM(100) + "\">";
  html += "<label>WiFi Password 2</label><input type=\"text\" name=\"wifi_password2\" value=\"" + readStringFromEEPROM(150) + "\">";
  html += "<label>WiFi SSID 3</label><input type=\"text\" name=\"wifi_ssid3\" value=\"" + readStringFromEEPROM(200) + "\">";
  html += "<label>WiFi Password 3</label><input type=\"text\" name=\"wifi_password3\" value=\"" + readStringFromEEPROM(250) + "\">";
  
  html += "<h3>MQTT / Device</h3>";
  html += "<label>mqtt_username</label><input type=\"text\" name=\"mqtt_username\" value=\"" + readStringFromEEPROM(300) + "\">";
  html += "<label>mqtt_password</label><input type=\"text\" name=\"mqtt_password\" value=\"" + readStringFromEEPROM(350) + "\">";
  html += "<label>user_uuid</label><input type=\"text\" name=\"user_uuid\" value=\"" + readStringFromEEPROM(400) + "\">";
  html += "<label>device_uuid</label><input type=\"text\" name=\"device_uuid\" value=\"" + readStringFromEEPROM(450) + "\">";
  
  html += "<input type=\"submit\" value=\"SAVE\">";
  html += "</form>";
  html += "<button onclick=\"location.href='/reboot'\">REBOOT</button>";
  html += "</body></html>";
  
  return html;
}

void startConfigServer() {
  if (server_started) return;
  
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/html", configPageHtml());
  });
  
  server.on("/save", HTTP_GET, [](AsyncWebServerRequest* request) {
    auto getv = [&](const char* name) -> String {
      if (request->hasParam(name)) return request->getParam(name)->value();
      return "";
    };
    
    writeStringToEEPROM(0, getv("wifi_ssid1"));
    writeStringToEEPROM(50, getv("wifi_password1"));
    writeStringToEEPROM(100, getv("wifi_ssid2"));
    writeStringToEEPROM(150, getv("wifi_password2"));
    writeStringToEEPROM(200, getv("wifi_ssid3"));
    writeStringToEEPROM(250, getv("wifi_password3"));
    
    writeStringToEEPROM(300, getv("mqtt_username"));
    writeStringToEEPROM(350, getv("mqtt_password"));
    writeStringToEEPROM(400, getv("user_uuid"));
    writeStringToEEPROM(450, getv("device_uuid"));
    
    request->send(200, "text/html", configPageHtml("Saved successfully. Reboot now."));
  });
  
  server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/html", "<html><body><h2>Rebooting in 3 seconds...</h2></body></html>");
    reboot_flag = true;
  });
  
  server.begin();
  server_started = true;
}

void publishWiFiComPresence();

// ========================================
// Teardown / Memory Helpers
// ========================================
void showExitingScreen(const String& moduleName) {
  drawHeader("Exiting...");
  drawInlineKV(2, "Closing", moduleName, UI_WARN);
  drawFooter("Please Wait", "");
  delay(500);
}

void freeBufferDigiROM() {
  if (buffer_digirom != nullptr) {
    delete buffer_digirom;
    buffer_digirom = nullptr;
  }
}

void stopActiveCommand() {
  active_digirom = "";
  active_source = "";
  pending_digirom = "";
  pending_source = "";
  active_repeat_enabled = false;
  mqtt_digirom = "";
  application_id = "";
}

void teardownWiFiCom() {
  stopActiveCommand();
  if (client.connected()) {
    client.disconnect();
    UI::info("MQTT Disconnected");
  }
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    UI::info("WiFi Disconnected");
  }
  freeBufferDigiROM();
}

void teardownBtcom() {
  stopActiveCommand();
  if (btcom_initialized) {
    NimBLEDevice::stopAdvertising();
    btcomStatus = "Offline";
    btcom_initialized = false;
    NimBLEDevice::deinit(true);
    UI::info("BTCOM Teardown Complete");
  }
  freeBufferDigiROM();
}

// ========================================
// WiFi / EEPROM Checks
// ========================================
void loadSavedSecrets() {
  secrets_mqtt_username = readStringFromEEPROM(300);
  secrets_mqtt_password = readStringFromEEPROM(350);
  secrets_user_uuid = readStringFromEEPROM(400);
  secrets_device_uuid = readStringFromEEPROM(450);
}

bool credentialsExist() {
  String wifi_ssid1 = readStringFromEEPROM(0);
  String wifi_ssid2 = readStringFromEEPROM(100);
  String wifi_ssid3 = readStringFromEEPROM(200);
  loadSavedSecrets();
  
  bool wifi_ok = !(wifi_ssid1 == "" && wifi_ssid2 == "" && wifi_ssid3 == "");
  bool mqtt_ok = !(secrets_mqtt_username == "" || secrets_mqtt_password == "" || secrets_user_uuid == "" || secrets_device_uuid == "");
  
  return wifi_ok && mqtt_ok;
}

void clearSavedConfig() {
  writeStringToEEPROM(0, "");
  writeStringToEEPROM(50, "");
  writeStringToEEPROM(100, "");
  writeStringToEEPROM(150, "");
  writeStringToEEPROM(200, "");
  writeStringToEEPROM(250, "");
  writeStringToEEPROM(300, "");
  writeStringToEEPROM(350, "");
  writeStringToEEPROM(400, "");
  writeStringToEEPROM(450, "");
  wifi_multi_loaded = false;
}

void prepareWifiMultiIfNeeded() {
  if (wifi_multi_loaded) return;
  
  String wifi_ssid1 = readStringFromEEPROM(0);
  String wifi_password1 = readStringFromEEPROM(50);
  String wifi_ssid2 = readStringFromEEPROM(100);
  String wifi_password2 = readStringFromEEPROM(150);
  String wifi_ssid3 = readStringFromEEPROM(200);
  String wifi_password3 = readStringFromEEPROM(250);
  
  if (wifi_ssid1 != "") wifiMulti.addAP(wifi_ssid1.c_str(), wifi_password1.c_str());
  if (wifi_ssid2 != "") wifiMulti.addAP(wifi_ssid2.c_str(), wifi_password2.c_str());
  if (wifi_ssid3 != "") wifiMulti.addAP(wifi_ssid3.c_str(), wifi_password3.c_str());
  
  wifi_multi_loaded = true;
}

bool connectWiFiAndMQTT() {
  if (!credentialsExist()) return false;
  
  loadSavedSecrets();
  String mqtt_username_login = secrets_mqtt_username;
  mqtt_username_login.toLowerCase();
  
  mqtt_io_prefix = mqtt_username_login + "/f/";
  mqtt_topic_identifier = secrets_user_uuid + "-" + secrets_device_uuid;
  mqtt_topic_input = mqtt_io_prefix + mqtt_topic_identifier + "/wificom-input";
  mqtt_topic_output = mqtt_io_prefix + mqtt_topic_identifier + "/wificom-output";
  
  WiFi.mode(WIFI_STA);
  prepareWifiMultiIfNeeded();
  
  drawHeader("WiFiCom");
  drawInlineKV(0, "WiFi", "Connecting...", UI_WARN);
  drawFooter("WiFiCom", "B:Ref A:Back");
  
  UI::stat("Trying WiFi...");
  unsigned long t0 = millis();
  
  while (wifiMulti.run() != WL_CONNECTED) {
    delay(250);
    if (millis() - t0 > 15000) {
      UI::warn("WiFi connect timeout");
      return false;
    }
  }
  
  UI::info("WiFi connected: " + WiFi.localIP().toString());
  
  client.setServer(MQTT_BROKER, MQTT_PORT);
  client.setBufferSize(1024, 1024);
  client.setCallback(mqttcallback);
  
  drawInlineKV(0, "WiFi", "Connected", UI_SUCCESS);
  drawInlineKV(1, "MQTT", "Connecting...", UI_WARN);
  
  UI::stat("Trying MQTT...");
  char clientid[32];
  snprintf(clientid, sizeof(clientid), "esp32s3-%lu", (unsigned long)millis());
  
  if (!client.connect(clientid, mqtt_username_login.c_str(), secrets_mqtt_password.c_str())) {
    UI::warn("MQTT connect failed");
    return false;
  }
  
  client.subscribe(mqtt_topic_input.c_str());
  publishWiFiComPresence();
  last_presence_ms = millis();
  
  return true;
}

void publishWiFiComPresence() {
  if (!client.connected() || secrets_device_uuid == "" || mqtt_topic_output == "") return;
  
  String presence = "{\"name\":\"espcom\",\"board\":\"" + boardname + "\",\"version\":\"" + version + "\",\"device_uuid\":\"" + secrets_device_uuid + "\",\"application_uuid\":\"0\",\"ack_id\":\"\",\"output\":\"\"}";
  client.publish(mqtt_topic_output.c_str(), presence.c_str());
}

void mqttcallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  
  pending_digirom = jsonGetString(msg, "digirom");
  pending_source = "MQTT";
  
  if (pending_digirom != "") {
    heartbeat_pause_until_ms = millis() + HEARTBEAT_PAUSE_MS;
  }
  
  application_id = jsonGetString(msg, "application_id");
  if (application_id == "") {
    application_id = jsonGetString(msg, "application_uuid");
  }
  
  ack_id = jsonGetString(msg, "ack_id");
  hide_output = jsonGetBool(msg, "hide_output", false);
  api_response = jsonGetBool(msg, "api_response", false);
  
  if (ack_id != "" && ack_id != "null" && client.connected()) {
    String safeAckId = ack_id;
    safeAckId.replace("\\", "\\\\");
    safeAckId.replace("\"", "\\\"");
    
    String safeAppId = application_id;
    safeAppId.replace("\\", "\\\\");
    safeAppId.replace("\"", "\\\"");
    
    String ackMsg = "{\"application_uuid\":\"" + safeAppId + "\",\"device_uuid\":\"" + secrets_device_uuid + "\",\"ack_id\":\"" + safeAckId + "\"}";
    client.publish(mqtt_topic_output.c_str(), ackMsg.c_str());
  }
}

// ========================================
// DMComm Execution
// ========================================
void buildDigiROMFromString(const String& s) {
  freeBufferDigiROM();
  const char* digirom = s.c_str();
  DigiROMType rom_type = digiROMType(digirom);
  
  switch (rom_type.signal_type) {
    case kSignalTypeV:
    case kSignalTypeX:
    case kSignalTypeY:
      buffer_digirom = new ClassicDigiROM(digirom);
      break;
    case kSignalTypeC:
      buffer_digirom = new WordsDigiROM(digirom);
      break;
    default:
      buffer_digirom = nullptr;
      break;
  }
}

unsigned long dmcommExecuteTimeout() {
#ifdef DMCOMM_LISTEN_TIMEOUT_MILLIS
  return DMCOMM_LISTEN_TIMEOUT_MILLIS;
#else
  return 5000;
#endif
}

String cleanDigiResultForW0rld(String raw) {
  raw.replace("\r", " ");
  raw.replace("\n", " ");
  
  int resultTag = raw.lastIndexOf("[DMComm] result:");
  if (resultTag >= 0) {
    raw = raw.substring(resultTag + String("[DMComm] result:").length());
  }
  
  int execTag = raw.lastIndexOf("[DMComm] execute done");
  if (execTag >= 0) {
    int nextResultTag = raw.indexOf("[DMComm] result:", execTag);
    if (nextResultTag >= 0) {
      raw = raw.substring(nextResultTag + String("[DMComm] result:").length());
    }
  }
  
  raw.replace("[DMComm] execute done, turn=1", "");
  raw.replace("[DMComm] execute done, turn=2", "");
  raw.replace("[DMComm] result:", "");
  raw.trim();
  
  while (raw.indexOf("  ") >= 0) {
    raw.replace("  ", " ");
  }
  return raw;
}

void executePendingDigiROM(const String& source) {
  if (mqtt_digirom == "") return;
  
  String cmd = mqtt_digirom;
  mqtt_digirom = "";
  cmd.trim();
  
  if (cmd == "" || cmd == "p" || cmd == "P") {
    last_digi_result = "(paused)";
    return;
  }
  
  const bool serialProtocolMode = (source == "SERIAL");
  buildDigiROMFromString(cmd);
  
  if (buffer_digirom == nullptr) {
    last_digi_result = "(unsupported digirom)";
    if (serialProtocolMode) {
      Serial.println(last_digi_result);
    }
    return;
  }
  
  controller.execute(*buffer_digirom, dmcommExecuteTimeout());
  
  String raw_digi_result = "";
  StringStream digistream(raw_digi_result);
  buffer_digirom->printResult(digistream);
  
  last_digi_result = cleanDigiResultForW0rld(raw_digi_result);
  if (last_digi_result == "") {
    last_digi_result = "None";
  }
  
  if (serialProtocolMode) {
    Serial.println(last_digi_result);
  }
  
  if (client.connected()) {
    String safeResult = last_digi_result;
    safeResult.replace("\\", "\\\\");
    safeResult.replace("\"", "\\\"");
    safeResult.replace("\n", "\\n");
    safeResult.replace("\r", "");
    
    String safeAppId = application_id;
    safeAppId.replace("\\", "\\\\");
    safeAppId.replace("\"", "\\\"");
    
    String safeAckId = ack_id;
    safeAckId.replace("\\", "\\\\");
    safeAckId.replace("\"", "\\\"");
    
    String digiout = "{\"name\":\"espcom\",\"board\":\"" + boardname + "\",\"version\":\"" + version + "\",\"device_uuid\":\"" + secrets_device_uuid + "\",\"application_uuid\":\"" + safeAppId + "\",\"ack_id\":\"" + safeAckId + "\",\"output\":\"" + safeResult + "\"}";
    
    client.publish(mqtt_topic_output.c_str(), digiout.c_str());
    last_presence_ms = millis();
    heartbeat_pause_until_ms = millis() + HEARTBEAT_PAUSE_MS;
  }
}

void executeActiveDigiROM() {
  if (active_digirom == "") return;
  mqtt_digirom = active_digirom;
  executePendingDigiROM(active_source);
}

bool repeatAllowedForCurrentPage() {
  return (prog_index == SCREEN_WIFICOM || prog_index == SCREEN_ACOM || prog_index == SCREEN_PUNCH_EXECUTE || prog_index == SCREEN_BTCOM);
}

// ========================================
// PunchingBag Database
// ========================================
void selectPunchingDataset() {
  submenu_size = 0;
  temp_array = nullptr;
  temp_code_array = nullptr;
  
  switch (punch_index) {
    case 0:  submenu_size = 2;  temp_array = dmog_n;   temp_code_array = dmog_c;   break;
    case 1:  submenu_size = 8;  temp_array = dmx_n;    temp_code_array = dmx_c;    break;
    case 2:  submenu_size = 39; temp_array = dm20_n;   temp_code_array = dm20_c;   break;
    case 3:  submenu_size = 11; temp_array = dmc_n;    temp_code_array = dmc_c;    break;
    case 4:  submenu_size = 1;  temp_array = dmmini_n; temp_code_array = dmmini_c; break;
    case 5:  submenu_size = 1;  temp_array = xros_n;   temp_code_array = xros_c;   break;
    case 6:  submenu_size = 26; temp_array = penz_n;   temp_code_array = penz_c;   break;
    case 7:  submenu_size = 29; temp_array = pen20_n;  temp_code_array = pen20_c;  break;
    case 8:  submenu_size = 35; temp_array = penc_n;   temp_code_array = penc_c;   break;
    case 9:  submenu_size = 18; temp_array = penog_n;  temp_code_array = penog_c;  break;
    case 10: submenu_size = 45; temp_array = penx_n;   temp_code_array = penx_c;   break;
    case 11: submenu_size = 2;  temp_array = d2_n;     temp_code_array = d2_c;     break;
    case 12: submenu_size = 15; temp_array = d3_n;     temp_code_array = d3_c;     break;
    case 13: submenu_size = 14; temp_array = dscan_n;  temp_code_array = dscan_c;  break;
    case 14: submenu_size = 2;  temp_array = accel_n;  temp_code_array = accel_c;  break;
    case 15: submenu_size = 1;  temp_array = ic_n;     temp_code_array = ic_c;     break;
  }
  
  if (submenu_size <= 0) sub_punch_index = 0;
  if (sub_punch_index >= submenu_size) sub_punch_index = 0;
  if (sub_punch_index < 0) sub_punch_index = 0;
}

void drawExecutePage() {
  drawHeader("Execute");
  drawInlineKV(0, "Title", compactResult(current_punch_name, 18), UI_ACCENT);
  drawInlineKV(1, "Repeat", "Enabled", UI_SUCCESS);
  drawInlineKV(2, "Code", compactResult(current_punch_code, 18), UI_LABEL);
  drawInlineWide(3, "Result", last_digi_result == "" ? "(none)" : last_digi_result, UI_SUCCESS);
  drawFooter("Running", "B:Ref A:Back");
}

void PunchingStart(const String& name, const String& code) {
  prog_index = SCREEN_PUNCH_EXECUTE;
  current_punch_name = name;
  current_punch_code = code;
  pending_digirom = code;
  pending_source = "PUNCH";
  application_id = "punchingbag";
  drawExecutePage();
}

// ========================================
// Screen Drawing Functions
// ========================================
void ShowMainMenu() {
  prog_index = SCREEN_MAIN;
  drawHeader("Main Menu");
  for (int i = 0; i < MAIN_MENU_COUNT; i++) {
    drawMenuItem(i, i == menu_id, mainMenuItems[i], false);
  }
  drawFooter("Menu", "B:Next A:OK");
}

void showWificomMenu() {
  prog_index = SCREEN_WIFICOM_MENU;
  drawHeader("WiFiCom Menu");
  for (int i = 0; i < WIFICOM_MENU_COUNT; i++) {
    drawMenuItem(i, i == wificom_menu_id, wificomMenuItems[i], true);
  }
  drawFooter("Select", "B:Next A:OK");
}

void showAcomMenu() {
  prog_index = SCREEN_ACOM_MENU;
  drawHeader("ACOM Mode");
  for (int i = 0; i < ACOM_MENU_COUNT; i++) {
    drawMenuItem(i, i == acom_menu_id, acomMenuItems[i], true);
  }
  drawFooter("Select", "B:Next A:OK");
}

void showSettingMenu() {
  prog_index = SCREEN_SETTING_MENU;
  drawHeader("Settings");
  for (int i = 0; i < SETTING_MENU_COUNT; i++) {
    drawMenuItem(i, i == setting_menu_id, settingMenuItems[i], true);
  }
  drawFooter("Select", "B:Next A:OK");
}

void showWifiSetupMenu() {
  prog_index = SCREEN_WIFI_SETUP_MENU;
  drawHeader("WiFi Setup");
  for (int i = 0; i < WIFI_SETUP_COUNT; i++) {
    drawMenuItem(i, i == wifi_setup_menu_id, wifiSetupItems[i], true);
  }
  drawFooter("Setup", "B:Next A:OK");
}

void showWifiSetupDetails() {
  prog_index = SCREEN_WIFI_SETUP_DETAILS;
  drawHeader("Saved Details");
  if (credentialsExist()) {
    drawInlineKV(0, "SSID1", readStringFromEEPROM(0), UI_ACCENT);
    drawInlineKV(1, "SSID2", readStringFromEEPROM(100), UI_TEXT_DIM);
    drawInlineKV(2, "SSID3", readStringFromEEPROM(200), UI_TEXT_DIM);
    drawInlineKV(3, "MQTT", readStringFromEEPROM(300), UI_SUCCESS);
    drawInlineWide(4, "UUID", readStringFromEEPROM(450), UI_TEXT_DIM);
  } else {
    drawInlineKV(1, "Config", "Missing", UI_ERROR);
  }
  drawFooter("Saved", "B:Ref A:Back");
}

void startWifiSetupHotspot() {
  prog_index = SCREEN_WIFI_SETUP_HOTSPOT;
  if (!ap_mode_started) {
    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAP(AP_SSID, AP_PASS);
    if (ok) {
      ap_mode_started = true;
      startConfigServer();
    }
  }
  drawHeader("Hotspot Setup");
  drawInlineKV(0, "Mode", ap_mode_started ? "AP running" : "AP failed", ap_mode_started ? UI_SUCCESS : UI_ERROR);
  drawInlineKV(1, "SSID", AP_SSID, UI_ACCENT);
  drawInlineKV(2, "PASS", AP_PASS, UI_TEXT);
  drawInlineKV(3, "Open", "192.168.4.1", UI_SUCCESS);
  drawFooter("Hotspot", "B:Ref A:Back");
}

void showWifiSetupCleared() {
  prog_index = SCREEN_WIFI_SETUP_CLEAR;
  clearSavedConfig();
  drawHeader("WiFi Setup");
  drawInlineKV(1, "Config", "Cleared", UI_WARN);
  drawFooter("Cleared", "A:Back");
}

void wificom() {
  prog_index = SCREEN_WIFICOM;
  drawHeader("WiFiCom");
  
  if (!credentialsExist()) {
    drawInlineKV(1, "Config", "Missing", UI_ERROR);
    drawInlineKV(2, "Action", "Use WiFi Setup", UI_WARN);
    drawFooter("WiFiCom", "B:Ref A:Back");
    return;
  }
  
  if (WiFi.status() != WL_CONNECTED || !client.connected()) {
    bool ok = connectWiFiAndMQTT();
    if (!ok) {
      drawInlineKV(1, "WiFi", "Failed", UI_ERROR);
      drawInlineKV(2, "MQTT", "Failed", UI_ERROR);
      drawFooter("Retry", "B:Ref A:Back");
      return;
    }
  }
  
  drawInlineKV(0, "WiFi", "Connected", UI_SUCCESS);
  drawInlineKV(1, "IP", WiFi.localIP().toString(), UI_ACCENT);
  drawInlineKV(2, "MQTT", client.connected() ? "Connected" : "Disconnected", client.connected() ? UI_SUCCESS : UI_ERROR);
  drawInlineKV(3, "Active", compactResult(active_digirom, 18), UI_ACCENT);
  drawInlineWide(4, "Result", last_digi_result == "" ? "(none)" : last_digi_result, UI_TEXT);
  drawFooter("WiFiCom", "B:Ref A:Back");
}

void acommode() {
  prog_index = SCREEN_ACOM;
  drawHeader("Serial Start");
  drawInlineKV(0, "TX", "GPIO1", UI_ACCENT);
  drawInlineKV(1, "RX", "GPIO3", UI_ACCENT);
  drawInlineKV(2, "Mode", "Serial", UI_SUCCESS);
  drawInlineKV(3, "Active", compactResult(active_digirom, 18), UI_ACCENT);
  drawInlineWide(4, "Result", last_digi_result == "" ? "(none)" : last_digi_result, UI_TEXT);
  drawFooter("Serial", "B:Ref A:Back");
}

void btcommode() {
  prog_index = SCREEN_BTCOM;
  drawHeader("BTCOM Mode");
  drawInlineKV(0, "Status", btcomStatus, btcomStatus == "Connected" ? UI_SUCCESS : UI_ACCENT);
  drawInlineKV(1, "RX/TX", "NUS Service", UI_ACCENT);
  drawInlineKV(2, "Active", compactResult(active_digirom, 18), UI_ACCENT);
  drawInlineWide(3, "Result", last_digi_result == "" ? "(none)" : last_digi_result, UI_TEXT);
  drawFooter("Bluetooth", "B:Ref A:Back");
}

void PunchingBag() {
  prog_index = SCREEN_PUNCH_DEVICE;
  drawHeader("PunchingBag");
  
  int start = (punch_index / 5) * 5;
  for (int i = 0; i < 5; i++) {
    int idx = start + i;
    if (idx < 16) {
      drawMenuItem(i, idx == punch_index, device[idx], true);
    }
  }
  drawFooter("Device", "B:Next A:OK");
}

void displaySubMenu() {
  prog_index = SCREEN_PUNCH_SUBMENU;
  selectPunchingDataset();
  drawHeader("Sub Menu");
  
  if (temp_array == nullptr || submenu_size == 0) {
    drawInlineKV(1, "Status", "No data", UI_ERROR);
    drawFooter("Empty", "A:Back");
    return;
  }
  
  int start = (sub_punch_index / 5) * 5;
  for (int i = 0; i < 5; i++) {
    int idx = start + i;
    if (idx < submenu_size) {
      drawMenuItem(i, idx == sub_punch_index, temp_array[idx], true);
    }
  }
  drawFooter("Command", "B:Next A:Run");
}

void screenBatteryInfo() {
  prog_index = SCREEN_BATTERY;
  uint32_t mv = getBatteryVoltageMv();
  
  drawHeader("Battery Info");
  drawCard(42, 90, UI_BORDER);
  
  tft.setTextColor(UI_LABEL, UI_BG);
  tft.setTextSize(1);
  tft.setCursor(18, 52);
  tft.println("Battery Voltage");
  
  drawBigCenteredValue(82, String(mv) + " mV", UI_SUCCESS);
  drawFooter("Battery", "B:Ref A:Back");
}

void shutdownDevice() {
  stopActiveCommand();
  if (client.connected()) client.disconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  
  drawHeader("Shutdown");
  drawInlineKV(1, "State", "Sleeping...", UI_WARN);
  drawFooter("Sleep", "Wake:BtnB");
  delay(500);
  
  digitalWrite(PIN_LCD_BL, LOW);
  while (digitalRead(PIN_BTN_A) == LOW || digitalRead(PIN_BTN_B) == LOW) {
    delay(10);
  }
  delay(100);
  
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BTN_B, 0);
  esp_deep_sleep_start();
}

void runProgram() {
  switch (menu_id) {
    case 0: showWificomMenu(); break;
    case 1: showAcomMenu(); break;
    case 2: startBtcom(); btcommode(); break; 
    case 3: showSettingMenu(); break;
    default: ShowMainMenu(); break;
  }
}

// ========================================
// Setup Function
// ========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  EEPROM.begin(EEPROM_SIZE);
  analogReadResolution(ANALOG_RESOLUTION);
  
  btnA.begin(PIN_BTN_A);
  btnB.begin(PIN_BTN_B);
  
  setupDisplay();
  
  controller.add(classic_comm);
  controller.add(color_comm);
  
  String fristrun = readStringFromEEPROM(500);
  if (fristrun != "free") {
    clearSavedConfig();
    writeStringToEEPROM(500, "free");
  }
  
  UI::info("DMComm TX = GPIO1");
  UI::info("DMComm RX = GPIO3");
  UI::info("Input mode = DigitalProngInput");
  UI::info("DMComm execute timeout = " + String(dmcommExecuteTimeout()) + " ms");
  UI::info("Repeat interval = " + String(execute_interval_ms) + " ms");
  
  ShowMainMenu();
}

// ========================================
// Main Loop
// ========================================
void loop() {
  
  // 1. Network Handlers
  if (client.connected()) {
    client.loop();
    if (millis() > heartbeat_pause_until_ms && millis() - last_presence_ms >= PRESENCE_INTERVAL_MS) {
      last_presence_ms = millis();
      publishWiFiComPresence();
    }
  } else {
    if (prog_index == SCREEN_WIFICOM && WiFi.status() == WL_CONNECTED) {
      connectWiFiAndMQTT();
    }
  }

  // 2. Command Processing Buffer
  if (pending_digirom != "") {
    active_digirom = pending_digirom;
    active_source = pending_source;
    pending_digirom = "";
    pending_source = "";
    last_execute_ms = 0;
    
    if (active_source == "MQTT") {
      executeActiveDigiROM();
      active_digirom = "";
      active_source = "";
      active_repeat_enabled = false;
      if (prog_index == SCREEN_WIFICOM) wificom();
    } 
    else if (active_source == "BTCOM") {
      executeActiveDigiROM();
      sendBtcomResult(last_digi_result);
      active_digirom = "";
      active_source = "";
      active_repeat_enabled = false;
      if (prog_index == SCREEN_BTCOM) btcommode();
    }
  }

  // 3. Repeat Command Engine
  active_repeat_enabled = repeatAllowedForCurrentPage() && active_digirom != "" && active_source != "MQTT" && active_source != "BTCOM";
  if (active_repeat_enabled && millis() - last_execute_ms >= execute_interval_ms) {
    last_execute_ms = millis();
    executeActiveDigiROM();
    
    if (prog_index == SCREEN_WIFICOM) wificom();
    else if (prog_index == SCREEN_ACOM) acommode();
    else if (prog_index == SCREEN_PUNCH_EXECUTE) drawExecutePage();
  }

  // 4. Button B (Scroll/Next) Handler
  if (btnB.wasPressed()) {
    switch (prog_index) {
      case SCREEN_MAIN: 
        menu_id++; 
        if (menu_id >= MAIN_MENU_COUNT) menu_id = 0; 
        ShowMainMenu(); 
        break;
      
      case SCREEN_WIFICOM_MENU: 
        wificom_menu_id++; 
        if (wificom_menu_id >= WIFICOM_MENU_COUNT) wificom_menu_id = 0; 
        showWificomMenu(); 
        break;

      case SCREEN_ACOM_MENU: 
        acom_menu_id++; 
        if (acom_menu_id >= ACOM_MENU_COUNT) acom_menu_id = 0; 
        showAcomMenu(); 
        break;
      
      case SCREEN_SETTING_MENU: 
        setting_menu_id++; 
        if (setting_menu_id >= SETTING_MENU_COUNT) setting_menu_id = 0; 
        showSettingMenu(); 
        break;
      
      case SCREEN_WIFI_SETUP_MENU: 
        wifi_setup_menu_id++; 
        if (wifi_setup_menu_id >= WIFI_SETUP_COUNT) wifi_setup_menu_id = 0; 
        showWifiSetupMenu(); 
        break;
      
      case SCREEN_WIFI_SETUP_DETAILS: showWifiSetupDetails(); break; 
      case SCREEN_WIFI_SETUP_HOTSPOT: startWifiSetupHotspot(); break; 
      case SCREEN_WIFI_SETUP_CLEAR: showWifiSetupCleared(); break;
      case SCREEN_WIFICOM: wificom(); break; 
      case SCREEN_ACOM: acommode(); break; 
      case SCREEN_BTCOM: btcommode(); break;
      
      case SCREEN_PUNCH_DEVICE: 
        punch_index++; 
        if (punch_index >= 16) punch_index = 0; 
        PunchingBag(); 
        break;
      
      case SCREEN_PUNCH_SUBMENU: 
        sub_punch_index++; 
        selectPunchingDataset(); 
        if (sub_punch_index >= submenu_size) sub_punch_index = 0; 
        displaySubMenu(); 
        break;
      
      case SCREEN_BATTERY: screenBatteryInfo(); break; 
      case SCREEN_PUNCH_EXECUTE: drawExecutePage(); break;
    }
  }

  // 5. Button A (Select/Back) Handler
  if (btnA.wasPressed()) {
    switch (prog_index) {
      case SCREEN_MAIN: runProgram(); break;
      
      case SCREEN_WIFICOM_MENU: 
        switch (wificom_menu_id) { 
          case 0: wificom(); break; 
          case 1: showWifiSetupMenu(); break; 
          case 2: ShowMainMenu(); break; 
        } 
        break;

      case SCREEN_ACOM_MENU: 
        switch (acom_menu_id) { 
          case 0: acommode(); break; 
          case 1: PunchingBag(); break; 
          case 2: ShowMainMenu(); break; 
        } 
        break;
      
      case SCREEN_SETTING_MENU: 
        switch (setting_menu_id) { 
          case 0: screenBatteryInfo(); break; 
          case 1: shutdownDevice(); break; 
          case 2: ShowMainMenu(); break; 
        } 
        break;
      
      case SCREEN_WIFI_SETUP_MENU: 
        switch (wifi_setup_menu_id) { 
          case 0: showWifiSetupDetails(); break; 
          case 1: startWifiSetupHotspot(); break; 
          case 2: showWifiSetupCleared(); break; 
          case 3: showWificomMenu(); break; 
          default: showWificomMenu(); break; 
        } 
        break;
      
      case SCREEN_WIFI_SETUP_DETAILS: 
      case SCREEN_WIFI_SETUP_HOTSPOT: 
      case SCREEN_WIFI_SETUP_CLEAR: 
        showWifiSetupMenu(); 
        break;
      
      case SCREEN_WIFICOM: 
        showExitingScreen("WiFi & MQTT"); 
        teardownWiFiCom(); 
        showWificomMenu(); 
        break;
      
      case SCREEN_BTCOM: 
        showExitingScreen("Bluetooth"); 
        teardownBtcom(); 
        ShowMainMenu(); 
        break;
      
      case SCREEN_ACOM: 
      case SCREEN_PUNCH_EXECUTE: 
        stopActiveCommand(); 
        freeBufferDigiROM(); 
        showAcomMenu(); // Now routes back to the ACOM submenu
        break;
      
      case SCREEN_BATTERY: 
        showSettingMenu(); 
        break;
      
      case SCREEN_PUNCH_DEVICE: 
        selectPunchingDataset(); 
        sub_punch_index = 0; 
        displaySubMenu(); 
        break;
      
      case SCREEN_PUNCH_SUBMENU: 
        if (temp_array != nullptr && temp_code_array != nullptr && submenu_size > 0) { 
          PunchingStart(temp_array[sub_punch_index], temp_code_array[sub_punch_index]); 
        } else { 
          PunchingBag(); 
        } 
        break;
    }
  }

  // 6. Serial Input Handler
  if (Serial.available()) {
    String serialCmd = Serial.readStringUntil('\n');
    serialCmd.trim();
    if (serialCmd != "") {
      pending_digirom = serialCmd;
      pending_source = "SERIAL";
      application_id = "serial";
      if (prog_index != SCREEN_ACOM) {
        acommode();
      }
    }
  }

  // 7. Battery Timer & Reboot Handlers
  if (millis() - last_batt_ms > 15000) {
    last_batt_ms = millis();
    if (prog_index != SCREEN_ACOM && prog_index != SCREEN_WIFICOM && prog_index != SCREEN_PUNCH_EXECUTE && prog_index != SCREEN_BTCOM) {
      batt_show_serial();
    }
  }
  
  if (reboot_flag) {
    delay(3000);
    ESP.restart();
  }
}

