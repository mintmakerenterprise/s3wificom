#include <WiFi.h>
#include <WiFiMulti.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <EEPROM.h>
#include <PubSubClient.h>
#include <ImprovWiFiLibrary.h>
#include <DMComm.h>
#include <TFT_eSPI.h>

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

static const int INPUT_PIN = 3;     // DMComm RX
static const int OUTPUT_PIN = 1;    // DMComm TX

static const int EEPROM_SIZE = 512;
static const int ANALOG_RESOLUTION = 12;

static const char* AP_SSID = "WiFiCom-S3-Setup";
static const char* AP_PASS = "12345678";

String boardname = "TTGO_TDisplay_S3";
String version = "esp32s3-wificom-tbpubsub-buffer-2026-05-03";

// Set true only when debugging from Arduino Serial Monitor.
// Keep false for W0rld / ACOM serial mode so the USB serial output is protocol-clean.
static const bool SERIAL_DEBUG = true;

// ========================================
// Theme
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

// Important update:
// Use DigitalProngInput like the newer M5 WiFiCom source.
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
// WiFiCom presence / heartbeat
// ========================================
// WiFiCom.dev needs a presence packet with output:"" to keep device online.
// However, output:"" can overwrite real last_output.
// So heartbeat is paused during/after command execution to let website poll RX safely.
unsigned long last_presence_ms = 0;
unsigned long heartbeat_pause_until_ms = 0;
const unsigned long PRESENCE_INTERVAL_MS = 15000;      // keep below 60s offline threshold
const unsigned long HEARTBEAT_PAUSE_MS = 30000;        // RX polling protection window

// ========================================
// Repeat command engine
// ========================================
String active_digirom = "";
String active_source = "";
String pending_digirom = "";
String pending_source = "";
bool active_repeat_enabled = false;
unsigned long last_execute_ms = 0;
unsigned long execute_interval_ms = 1000;

// ========================================
// Menu state
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
  SCREEN_PUNCH_EXECUTE = 10
};

int prog_index = SCREEN_MAIN;
int menu_id = 0;

String mainMenuItems[] = {
  "WiFi Setup",
  "WiFiCom",
  "ACOM Mode",
  "PunchingBag",
  "Battery Info",
  "Shutdown"
};
const int MAIN_MENU_COUNT = sizeof(mainMenuItems) / sizeof(mainMenuItems[0]);

int wifi_setup_menu_id = 0;
String wifiSetupItems[] = {
  "Show Saved Details",
  "Start Hotspot",
  "Clear Config",
  "Back"
};
const int WIFI_SETUP_COUNT = sizeof(wifiSetupItems) / sizeof(wifiSetupItems[0]);

// PunchingBag state
String* temp_array = nullptr;
String* temp_code_array = nullptr;
int punch_index = 0;
int sub_punch_index = 0;
int submenu_size = 0;
String current_punch_name = "";
String current_punch_code = "";

// ========================================
// Button helper
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
// Serial debug helpers
// ========================================
namespace UI {
  void info(const String& s) { if (SERIAL_DEBUG) Serial.println("[INFO] " + s); }
  void warn(const String& s) { if (SERIAL_DEBUG) Serial.println("[WARN] " + s); }
  void stat(const String& s) { if (SERIAL_DEBUG) Serial.println("[STAT] " + s); }
}

// ========================================
// EEPROM helpers
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
// Battery helper
// ========================================
uint32_t getBatteryVoltageMv() {
  uint32_t raw = analogRead(PIN_BAT_ADC);
  uint32_t mv = (raw * 2UL * 3300UL) / 4095UL;
  return mv;
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
// Tiny JSON-ish parser
// ========================================
String jsonGetString(const String& src, const String& key) {
  String pattern = "\"" + key + "\"";
  int keyPos = src.indexOf(pattern);
  if (keyPos < 0) return "";

  int colonPos = src.indexOf(':', keyPos + pattern.length());
  if (colonPos < 0) return "";

  int i = colonPos + 1;
  while (i < (int)src.length() &&
         (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r')) {
    i++;
  }

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
  while (end < (int)src.length() &&
         src[end] != ',' &&
         src[end] != '}' &&
         src[end] != '\n' &&
         src[end] != '\r') {
    end++;
  }

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
// Display helpers
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
  tft.print(" ");
  tft.print("TX1 RX3");

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
    tft.setTextSize(small ? 1 : 2);
    tft.setCursor(x + 18, y + (small ? 4 : 1));
    tft.println(shown);
  } else {
    tft.setTextColor(UI_TEXT_DIM, UI_BG);
    tft.setTextSize(small ? 1 : 2);
    tft.setCursor(x + 18, y + (small ? 4 : 1));
    tft.println(shown);
  }
}

void drawCard(int y, int h, uint16_t borderColor = UI_BORDER) {
  tft.fillRoundRect(8, y, tft.width() - 16, h, 8, UI_PANEL_ALT);
  tft.drawRoundRect(8, y, tft.width() - 16, h, 8, borderColor);
}

void drawBigCenteredValue(int y, const String& text, uint16_t color = UI_SUCCESS) {
  tft.setTextColor(color, UI_BG);
  tft.setTextSize(3);

  int char_w = 18;
  int w = text.length() * char_w;
  int x = (tft.width() - w) / 2;
  if (x < 0) x = 0;

  tft.setCursor(x, y);
  tft.println(text);
}

void drawInlineKV(int row, const String& label, const String& value,
                  uint16_t valueColor = UI_TEXT,
                  bool small = true) {
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

void drawInlineWide(int row, const String& label, const String& value,
                    uint16_t valueColor = UI_TEXT) {
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
// Config web pages
// ========================================
String configPageHtml(const String& message = "") {
  String html =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>WiFiCom S3 Setup</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;padding:20px;max-width:700px;margin:auto;}"
    "input[type=text]{width:100%;padding:10px;margin:6px 0 14px;box-sizing:border-box;}"
    "input[type=submit],button{width:100%;padding:12px;margin:8px 0;}"
    ".msg{color:green;font-weight:bold;margin-bottom:10px;}"
    "</style></head><body>"
    "<h2>ESP32-S3 WiFiCom Setup</h2>";

  if (message != "") html += "<div class=\"msg\">" + message + "</div>";

  html +=
    "<form action=\"/save\">"
    "<h3>WiFi</h3>"
    "<label>WiFi SSID 1</label><input type=\"text\" name=\"wifi_ssid1\" value=\"" + readStringFromEEPROM(0) + "\">"
    "<label>WiFi Password 1</label><input type=\"text\" name=\"wifi_password1\" value=\"" + readStringFromEEPROM(50) + "\">"
    "<label>WiFi SSID 2</label><input type=\"text\" name=\"wifi_ssid2\" value=\"" + readStringFromEEPROM(100) + "\">"
    "<label>WiFi Password 2</label><input type=\"text\" name=\"wifi_password2\" value=\"" + readStringFromEEPROM(150) + "\">"
    "<label>WiFi SSID 3</label><input type=\"text\" name=\"wifi_ssid3\" value=\"" + readStringFromEEPROM(200) + "\">"
    "<label>WiFi Password 3</label><input type=\"text\" name=\"wifi_password3\" value=\"" + readStringFromEEPROM(250) + "\">"
    "<h3>MQTT / Device</h3>"
    "<label>mqtt_username</label><input type=\"text\" name=\"mqtt_username\" value=\"" + readStringFromEEPROM(300) + "\">"
    "<label>mqtt_password</label><input type=\"text\" name=\"mqtt_password\" value=\"" + readStringFromEEPROM(350) + "\">"
    "<label>user_uuid</label><input type=\"text\" name=\"user_uuid\" value=\"" + readStringFromEEPROM(400) + "\">"
    "<label>device_uuid</label><input type=\"text\" name=\"device_uuid\" value=\"" + readStringFromEEPROM(450) + "\">"
    "<input type=\"submit\" value=\"SAVE\">"
    "</form>"
    "<button onclick=\"location.href='/reboot'\">REBOOT</button>"
    "</body></html>";

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
// WiFi / MQTT helpers
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
  bool mqtt_ok = !(secrets_mqtt_username == "" ||
                   secrets_mqtt_password == "" ||
                   secrets_user_uuid == "" ||
                   secrets_device_uuid == "");
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

  // Official WiFiCom library uses lower-case MQTT username for both login and topic prefix.
  // Without this, MQTT can appear connected while backend last_ping/last_output never updates.
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

  // TBPubSubClient requires separate RX/TX buffer sizes.
  // Final WiFiCom output JSON + long topic can exceed the default buffer.
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
  UI::info("MQTT subscribed: " + mqtt_topic_input);

  // Initial online announcement immediately after entering WiFiCom mode.
  publishWiFiComPresence();
  last_presence_ms = millis();

  if (SERIAL_DEBUG) {
    Serial.println("===== MQTT CONFIG =====");
    Serial.print("mqtt username login: ");
    Serial.println(mqtt_username_login);
    Serial.print("user uuid: ");
    Serial.println(secrets_user_uuid);
    Serial.print("device uuid: ");
    Serial.println(secrets_device_uuid);
    Serial.print("input topic: ");
    Serial.println(mqtt_topic_input);
    Serial.print("output topic: ");
    Serial.println(mqtt_topic_output);
    Serial.println("=======================");
  }

  return true;
}

// ========================================
// WiFiCom presence / heartbeat
// ========================================
void publishWiFiComPresence() {
  if (!client.connected()) return;
  if (secrets_device_uuid == "") return;
  if (mqtt_topic_output == "") return;

  // IMPORTANT:
  // WiFiCom.dev appears to require output:"" in presence to update online status.
  // This can overwrite app last_output, so we pause heartbeat around real command output.
  String presence =
    "{\"name\":\"espcom\","
    "\"board\":\"" + boardname + "\","
    "\"version\":\"" + version + "\","
    "\"device_uuid\":\"" + secrets_device_uuid + "\","
    "\"application_uuid\":\"0\","
    "\"ack_id\":\"\","
    "\"output\":\"\"}";

  bool ok = client.publish(mqtt_topic_output.c_str(), presence.c_str());

  // Debug only. This is printed to USB Serial, NOT sent to MQTT.
  if (SERIAL_DEBUG) {
    Serial.print("[MQTT] Message to ");
    Serial.println(mqtt_topic_output);
    Serial.print("[MQTT] PRESENCE SENT ");
    Serial.println(ok ? "OK:" : "FAILED:");
    Serial.println(presence);
  }
}

// ========================================
// MQTT callback
// ========================================
void mqttcallback(char* topic, byte* payload, unsigned int length) {
  if (SERIAL_DEBUG) {
    Serial.print("[MQTT] Message from ");
    Serial.println(topic);
  }

  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  if (SERIAL_DEBUG) {
    Serial.println("[MQTT] RAW:");
    Serial.println(msg);
  }

  pending_digirom = jsonGetString(msg, "digirom");
  pending_source = "MQTT";

  // Protect the upcoming app output from being overwritten by heartbeat output:"".
  // Website should poll last_output during this window.
  if (pending_digirom != "") {
    heartbeat_pause_until_ms = millis() + HEARTBEAT_PAUSE_MS;
  }

  // Official WiFiCom v2 MQTT input uses application_id.
  // application_uuid fallback is kept for custom/API compatibility.
  application_id = jsonGetString(msg, "application_id");
  if (application_id == "") {
    application_id = jsonGetString(msg, "application_uuid");
  }

  ack_id = jsonGetString(msg, "ack_id");
  hide_output = jsonGetBool(msg, "hide_output", false);
  api_response = jsonGetBool(msg, "api_response", false);

  if (SERIAL_DEBUG) {
    Serial.print("[MQTT] digirom: ");
    Serial.println(pending_digirom);
    Serial.print("[MQTT] application_id/application_uuid: ");
    Serial.println(application_id);
    Serial.print("[MQTT] ack_id: ");
    Serial.println(ack_id);
    Serial.print("[MQTT] api_response: ");
    Serial.println(api_response ? "true" : "false");
    Serial.print("[MQTT] hide_output: ");
    Serial.println(hide_output ? "true" : "false");
  }

  // Acknowledge the command if the backend provided ack_id.
  if (ack_id != "" && ack_id != "null" && client.connected()) {
    String safeAckId = ack_id;
    safeAckId.replace("\\", "\\\\");
    safeAckId.replace("\"", "\\\"");

    String safeApplicationId = application_id;
    safeApplicationId.replace("\\", "\\\\");
    safeApplicationId.replace("\"", "\\\"");

    String ackMsg =
      "{\"application_uuid\":\"" + safeApplicationId +
      "\",\"device_uuid\":\"" + secrets_device_uuid +
      "\",\"ack_id\":\"" + safeAckId + "\"}";

    bool ok = client.publish(mqtt_topic_output.c_str(), ackMsg.c_str());

    if (SERIAL_DEBUG) {
      Serial.print("[MQTT] Message to ");
      Serial.println(mqtt_topic_output);
      Serial.print("[MQTT] ACK SENT ");
      Serial.println(ok ? "OK:" : "FAILED:");
      Serial.println(ackMsg);
    }
  }

  if (!hide_output && SERIAL_DEBUG) {
    Serial.print("[MQTT] final digirom: ");
    Serial.println(pending_digirom);
  }
}

// ========================================
// DigiROM helpers
// ========================================
void freeBufferDigiROM() {
  if (buffer_digirom != nullptr) {
    delete buffer_digirom;
    buffer_digirom = nullptr;
  }
}

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

// Clean the DMComm result before sending to WiFiCom / W0rld.
// W0rld expects only packets like:
//   r:938E s:008E r:B44E ...
// It should NOT receive serial debug text such as:
//   [DMComm] execute done, turn=2
//   [DMComm] result:
String cleanDigiResultForW0rld(String raw) {
  raw.replace("\r", " ");
  raw.replace("\n", " ");

  // Defensive cleaning, in case any debug text accidentally enters the result string.
  int resultTag = raw.lastIndexOf("[DMComm] result:");
  if (resultTag >= 0) {
    raw = raw.substring(resultTag + String("[DMComm] result:").length());
  }

  // Remove common serial/debug fragments if present.
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
    UI::stat(source + ": paused / empty");
    last_digi_result = "(paused)";
    return;
  }

  const bool serialProtocolMode = (source == "SERIAL");

  if (SERIAL_DEBUG && !serialProtocolMode) {
    Serial.print("[");
    Serial.print(source);
    Serial.print("] got command: ");
    Serial.println(cmd);
  }

  buildDigiROMFromString(cmd);
  if (buffer_digirom == nullptr) {
    UI::warn("Unsupported DigiROM command");
    last_digi_result = "(unsupported digirom)";
    if (serialProtocolMode) {
      Serial.println(last_digi_result);
    }
    return;
  }

  if (SERIAL_DEBUG && !serialProtocolMode) {
    Serial.println("[DMComm] execute start");
    Serial.print("[DMComm] timeout ms = ");
    Serial.println(dmcommExecuteTimeout());
  }

  controller.execute(*buffer_digirom, dmcommExecuteTimeout());

  if (SERIAL_DEBUG && !serialProtocolMode) {
    Serial.println("[DMComm] execute returned");
  }

  String raw_digi_result = "";
  StringStream digistream(raw_digi_result);
  buffer_digirom->printResult(digistream);

  // Keep the public/output result clean for WiFiCom / W0rld.
  // Serial debug is printed separately below and is not included in MQTT output.
  last_digi_result = cleanDigiResultForW0rld(raw_digi_result);

  // Official M5 WiFiCom behavior: do not publish a blank output.
  // This lets us separate MQTT/server output problems from actual RX capture problems.
  // If the website shows RX: None, MQTT output works and the remaining issue is DMComm RX capture.
  if (last_digi_result == "") {
    last_digi_result = "None";
  }

  if (serialProtocolMode) {
    // ACOM / W0rld serial mode: output ONLY clean packet result.
    // No debug prefixes, no battery logs, no extra text.
    Serial.println(last_digi_result);
  } else if (SERIAL_DEBUG) {
    Serial.print("[DMComm] execute done, turn=");
    Serial.println(buffer_digirom->turn());

    Serial.print("[DMComm] raw result: ");
    Serial.println(raw_digi_result);

    Serial.print("[W0RLD_PKT] ");
    Serial.println(last_digi_result);
  }

  if (client.connected()) {
    // Official WiFiCom output format.
    // Packet output must remain clean: s:... r:... only.
    String safeResult = last_digi_result;
    safeResult.replace("\\", "\\\\");
    safeResult.replace("\"", "\\\"");
    safeResult.replace("\n", "\\n");
    safeResult.replace("\r", "");

    String safeApplicationId = application_id;
    safeApplicationId.replace("\\", "\\\\");
    safeApplicationId.replace("\"", "\\\"");

    String safeAckId = ack_id;
    safeAckId.replace("\\", "\\\\");
    safeAckId.replace("\"", "\\\"");

    String digiout =
      "{\"name\":\"espcom\",\"board\":\"" + boardname +
      "\",\"version\":\"" + version +
      "\",\"device_uuid\":\"" + secrets_device_uuid +
      "\",\"application_uuid\":\"" + safeApplicationId +
      "\",\"ack_id\":\"" + safeAckId +
      "\",\"output\":\"" + safeResult + "\"}";

    if (SERIAL_DEBUG) {
      Serial.print("[MQTT] output topic length = ");
      Serial.println(mqtt_topic_output.length());
      Serial.print("[MQTT] output payload length = ");
      Serial.println(digiout.length());
      Serial.print("[MQTT] total approx packet length = ");
      Serial.println(mqtt_topic_output.length() + digiout.length() + 10);
      Serial.println("[MQTT] configured RX/TX buffer = 1024/1024");
    }

    bool ok = client.publish(mqtt_topic_output.c_str(), digiout.c_str());

    // Real RX output has just been sent. Pause heartbeat so output:"" heartbeat
    // does not overwrite last_output before the website polls it.
    last_presence_ms = millis();
    heartbeat_pause_until_ms = millis() + HEARTBEAT_PAUSE_MS;

    if (SERIAL_DEBUG) {
      Serial.print("[MQTT] Message to ");
      Serial.println(mqtt_topic_output);
      Serial.print("[MQTT] OUTPUT SENT ");
      Serial.println(ok ? "OK:" : "FAILED:");
      Serial.println(digiout);
    }
  }

}

void executeActiveDigiROM() {
  if (active_digirom == "") return;
  mqtt_digirom = active_digirom;
  executePendingDigiROM(active_source);
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

bool repeatAllowedForCurrentPage() {
  return (prog_index == SCREEN_WIFICOM || prog_index == SCREEN_ACOM || prog_index == SCREEN_PUNCH_EXECUTE);
}

// ========================================
// PunchingBag dataset mapping
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
// Screen functions
// ========================================
void ShowMainMenu() {
  stopActiveCommand();
  prog_index = SCREEN_MAIN;

  drawHeader("WiFiCom Menu");
  for (int i = 0; i < MAIN_MENU_COUNT; i++) {
    drawMenuItem(i, i == menu_id, mainMenuItems[i], false);
  }
  drawFooter("Menu", "B:Next A:OK");
}

void showWifiSetupMenu() {
  stopActiveCommand();
  prog_index = SCREEN_WIFI_SETUP_MENU;

  drawHeader("WiFi Setup");
  for (int i = 0; i < WIFI_SETUP_COUNT; i++) {
    drawMenuItem(i, i == wifi_setup_menu_id, wifiSetupItems[i], true);
  }
  drawFooter("Setup", "B:Next A:OK");
}

void showWifiSetupDetails() {
  stopActiveCommand();
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
  stopActiveCommand();
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
  drawInlineKV(0, "Mode", ap_mode_started ? "AP running" : "AP failed",
               ap_mode_started ? UI_SUCCESS : UI_ERROR);
  drawInlineKV(1, "SSID", AP_SSID, UI_ACCENT);
  drawInlineKV(2, "PASS", AP_PASS, UI_TEXT);
  drawInlineKV(3, "Open", "192.168.4.1", UI_SUCCESS);

  drawFooter("Hotspot", "B:Ref A:Back");
}

void showWifiSetupCleared() {
  stopActiveCommand();
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
  drawInlineKV(2, "MQTT", client.connected() ? "Connected" : "Disconnected",
               client.connected() ? UI_SUCCESS : UI_ERROR);
  drawInlineKV(3, "Active", compactResult(active_digirom, 18), UI_ACCENT);
  drawInlineWide(4, "Result", last_digi_result == "" ? "(none)" : last_digi_result, UI_TEXT);

  drawFooter("WiFiCom", "B:Ref A:Back");
}

void acommode() {
  prog_index = SCREEN_ACOM;

  drawHeader("ACOM Mode");
  drawInlineKV(0, "TX", "GPIO1", UI_ACCENT);
  drawInlineKV(1, "RX", "GPIO3", UI_ACCENT);
  drawInlineKV(2, "Mode", "Serial", UI_SUCCESS);
  drawInlineKV(3, "Active", compactResult(active_digirom, 18), UI_ACCENT);
  drawInlineWide(4, "Result", last_digi_result == "" ? "(none)" : last_digi_result, UI_TEXT);

  drawFooter("Serial", "B:Ref A:Back");
}

void PunchingBag() {
  stopActiveCommand();
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
  stopActiveCommand();
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
  stopActiveCommand();
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
    case 0: showWifiSetupMenu(); break;
    case 1: wificom(); break;
    case 2: acommode(); break;
    case 3: PunchingBag(); break;
    case 4: screenBatteryInfo(); break;
    case 5: shutdownDevice(); break;
    default: ShowMainMenu(); break;
  }
}

// ========================================
// Setup / loop
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

  // Important:
  // Do NOT force GPIO1 manually here.
  // Let DMComm / DComOutput control the TX pin.

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
  UI::info("Battery ADC = GPIO4");
  UI::info("Buttons = GPIO0 / GPIO14");
  UI::info("LCD backlight pin = GPIO38");

  ShowMainMenu();
}

void loop() {
  if (client.connected()) {
    client.loop();

    // Periodic presence/heartbeat while MQTT is connected.
    // Pause during RX window to avoid overwriting application last_output.
    if (millis() > heartbeat_pause_until_ms &&
        millis() - last_presence_ms >= PRESENCE_INTERVAL_MS) {
      last_presence_ms = millis();
      publishWiFiComPresence();
    }
  } else {
    // If user is on WiFiCom screen, reconnect automatically.
    if (prog_index == SCREEN_WIFICOM && WiFi.status() == WL_CONNECTED) {
      if (SERIAL_DEBUG) {
        Serial.println("[MQTT] disconnected, reconnecting...");
      }
      connectWiFiAndMQTT();
    }
  }

  if (pending_digirom != "") {
    if (SERIAL_DEBUG) {
      Serial.println("[FLOW] pending_digirom moved to active command");
      Serial.print("[FLOW] pending_source = ");
      Serial.println(pending_source);
      Serial.print("[FLOW] pending_digirom = ");
      Serial.println(pending_digirom);
    }

    active_digirom = pending_digirom;
    active_source = pending_source;
    pending_digirom = "";
    pending_source = "";
    last_execute_ms = 0;

    // MQTT/API commands execute once immediately.
    // ACOM and PunchingBag can still use repeat behavior.
    if (active_source == "MQTT") {
      executeActiveDigiROM();

      active_digirom = "";
      active_source = "";
      active_repeat_enabled = false;

      if (prog_index == SCREEN_WIFICOM) {
        wificom();
      }
    }
  }

  active_repeat_enabled = repeatAllowedForCurrentPage() && active_digirom != "" && active_source != "MQTT";

  if (active_repeat_enabled && millis() - last_execute_ms >= execute_interval_ms) {
    last_execute_ms = millis();
    executeActiveDigiROM();

    if (prog_index == SCREEN_WIFICOM) {
      wificom();
    } else if (prog_index == SCREEN_ACOM) {
      acommode();
    } else if (prog_index == SCREEN_PUNCH_EXECUTE) {
      drawExecutePage();
    }
  }

  if (btnB.wasPressed()) {
    switch (prog_index) {
      case SCREEN_MAIN:
        menu_id++;
        if (menu_id >= MAIN_MENU_COUNT) menu_id = 0;
        ShowMainMenu();
        break;

      case SCREEN_WIFI_SETUP_MENU:
        wifi_setup_menu_id++;
        if (wifi_setup_menu_id >= WIFI_SETUP_COUNT) wifi_setup_menu_id = 0;
        showWifiSetupMenu();
        break;

      case SCREEN_WIFI_SETUP_DETAILS:
        showWifiSetupDetails();
        break;

      case SCREEN_WIFI_SETUP_HOTSPOT:
        startWifiSetupHotspot();
        break;

      case SCREEN_WIFI_SETUP_CLEAR:
        showWifiSetupCleared();
        break;

      case SCREEN_WIFICOM:
        wificom();
        break;

      case SCREEN_ACOM:
        acommode();
        break;

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

      case SCREEN_BATTERY:
        screenBatteryInfo();
        break;

      case SCREEN_PUNCH_EXECUTE:
        drawExecutePage();
        break;
    }
  }

  if (btnA.wasPressed()) {
    switch (prog_index) {
      case SCREEN_MAIN:
        runProgram();
        break;

      case SCREEN_WIFI_SETUP_MENU:
        switch (wifi_setup_menu_id) {
          case 0: showWifiSetupDetails(); break;
          case 1: startWifiSetupHotspot(); break;
          case 2: showWifiSetupCleared(); break;
          case 3: ShowMainMenu(); break;
          default: ShowMainMenu(); break;
        }
        break;

      case SCREEN_WIFI_SETUP_DETAILS:
      case SCREEN_WIFI_SETUP_HOTSPOT:
      case SCREEN_WIFI_SETUP_CLEAR:
        showWifiSetupMenu();
        break;

      case SCREEN_WIFICOM:
      case SCREEN_ACOM:
      case SCREEN_BATTERY:
      case SCREEN_PUNCH_EXECUTE:
        ShowMainMenu();
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

  if (millis() - last_batt_ms > 15000) {
    last_batt_ms = millis();

    // Keep ACOM / WiFiCom / Punch execute protocol clean.
    // Do not print battery/debug text while W0rld or WiFiCom may be reading Serial/MQTT result.
    if (prog_index != SCREEN_ACOM &&
        prog_index != SCREEN_WIFICOM &&
        prog_index != SCREEN_PUNCH_EXECUTE) {
      batt_show_serial();
    }
  }

  if (reboot_flag) {
    delay(3000);
    ESP.restart();
  }
}
