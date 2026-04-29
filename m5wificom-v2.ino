#include <M5GFX.h>
#include <M5Unified.h>
#include <DMComm.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <NimBLEDevice.h>

#include "digirom_data.h"

using namespace DMComm;

// -------------------------------------------------------------
// Pin Settings for M5StickC Plus2 (Grove port at the bottom)
// -------------------------------------------------------------
#define PIN_OUTPUT 26 // Yellow wire
#define PIN_INPUT  36 // White wire

DComOutput output(PIN_OUTPUT, DMCOMM_NO_PIN);
//AnalogProngInput input(PIN_INPUT, 3300, 10);
DigitalProngInput input = DigitalProngInput(PIN_INPUT);

ClassicCommunicator classic_comm = ClassicCommunicator(output, input);
ColorCommunicator color_comm = ColorCommunicator(output, input);

Controller controller = Controller();

BaseDigiROM *current_digirom = nullptr;

const char *fw_ver = "2.0";       
const char *model = "m5wificom"; 

// -------------------------------------------------------------
// Canvas for Double Buffering (ลดการกระพริบของหน้าจอ 100%)
// -------------------------------------------------------------
M5Canvas canvas(&M5.Display);

// -------------------------------------------------------------
// Virtual button states
// -------------------------------------------------------------
bool btnUpPressed = false;
bool btnDownPressed = false;
bool btnOkPressed = false;
bool btnCancelPressed = false;

// -------------------------------------------------------------
// System State Machine
// -------------------------------------------------------------
enum AppState {
  STATE_MAIN_MENU,
  STATE_ACOM_USB,
  STATE_BTCOM,
  STATE_WIFICOM,
  STATE_DIGIROM_DEVICE,
  STATE_DIGIROM_NAME,
  STATE_EXECUTING,
  STATE_SETTINGS,
  STATE_WIFICOM_CONNECTING,
  STATE_SLEEP     
};

AppState currentState = STATE_MAIN_MENU;
AppState prevState = STATE_MAIN_MENU;
AppState lastState = STATE_MAIN_MENU;

bool uiNeedsUpdate = true;

const char *mainMenuTitle = "Main Menu";
const char *mainMenuItems[] = { "ACOM USB", "BTCOM", "WIFICOM", "Punching BAG", "Settings", "Sleep" };
const int mainMenuItemCount = 6;
int mainMenuCursor = 0;
int mainMenuWindowStart = 0;

const int MAX_DEVICES = 30;
const int MAX_NAMES_PER_DEVICE = 100;
char *deviceMenu[MAX_DEVICES];
int deviceMenuCount = 0;
int deviceMenuCursor = 0;
int deviceMenuWindowStart = 0;

char *nameMenu[MAX_NAMES_PER_DEVICE];
int nameMenuCount = 0;
int nameMenuCursor = 0;
int nameMenuWindowStart = 0;

char selectedDevice[50] = "";
char selectedName[100] = "";
char executionResult[100] = "";
char lastExecutionResult[100] = "";
unsigned long executionStartTime = 0;
unsigned long lastExecutionTime = 0;

int textScrollOffset = 0;
unsigned long lastTextScrollTime = 0;
const int TEXT_SCROLL_DELAY = 200;
const int TEXT_SCROLL_RESET_DELAY = 1000;
bool isScrollingText = false;

unsigned long lastActivityTime = 0;

// -------------------------------------------------------------
// EEPROM & WiFi Settings
// -------------------------------------------------------------
#define EEPROM_SIZE 650
#define ADDR_WIFI_SSID_1 0
#define ADDR_WIFI_PASS_1 50
#define ADDR_WIFI_SSID_2 100
#define ADDR_WIFI_PASS_2 150
#define ADDR_WIFI_SSID_3 200
#define ADDR_WIFI_PASS_3 250
#define ADDR_MQTT_USER 300
#define ADDR_MQTT_PASS 350
#define ADDR_USER_UUID 400
#define ADDR_DEVICE_UUID 450

WiFiMulti wifiMulti;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

String secrets_mqtt_username;
String secrets_mqtt_password;
String secrets_user_uuid;
String secrets_device_uuid;
String mqtt_io_prefix;
String mqtt_topic_identifier;
String mqtt_topic_input;
String mqtt_topic_output;
String mqtt_digirom_command;
String application_id;
boolean hide_output;
boolean api_response;
String ack_id;
String wificomStatus = "Idle";
String lastWificomStatus = "";
String btcomStatus = "Advertising";

String clientId = "m5stickc2-";

enum WificomState {
  WIFI_IDLE, WIFI_INIT, WIFI_CONNECTING, WIFI_SUCCESS, MQTT_CONNECTING,
  MQTT_SUCCESS, WIFI_READY, WIFI_FAILED, MQTT_FAILED
};
WificomState currentWificomState = WIFI_IDLE;
int currentGameState = 0;
int currentGameType = 0;
#define GAME_MENU 0
#define GAME_TYPE_NONE 0

// -------------------------------------------------------------
// Utility Functions
// -------------------------------------------------------------
void writeStringToEEPROM(int addrOffset, const String &strToWrite) {
  byte len = strToWrite.length();
  if (len > 49) len = 49;
  EEPROM.write(addrOffset, len);
  for (int i = 0; i < len; i++) {
    EEPROM.write(addrOffset + 1 + i, strToWrite[i]);
  }
  EEPROM.write(addrOffset + 1 + len, '\0');
  EEPROM.commit();
}

String readStringFromEEPROM(int addrOffset) {
  int newStrLen = EEPROM.read(addrOffset);
  if (newStrLen > 49 || newStrLen < 0) return "";
  char data[newStrLen + 1];
  for (int i = 0; i < newStrLen; i++) {
    data[i] = EEPROM.read(addrOffset + 1 + i);
  }
  data[newStrLen] = '\0';
  return String(data);
}

BaseDigiROM *createDigiROM(const char *digirom_data) {
  DigiROMType rom_type = digiROMType(digirom_data);
  switch (rom_type.signal_type) {
    case kSignalTypeV:
    case kSignalTypeX:
    case kSignalTypeY: return new ClassicDigiROM(digirom_data);
    case kSignalTypeC: return new WordsDigiROM(digirom_data);
    default: return nullptr;
  }
}

class StringPrinter : public Print {
public:
  StringPrinter(String &target) : _target(target) {}
  virtual size_t write(uint8_t c) { _target += (char)c; return 1; }
  virtual size_t write(const uint8_t *buffer, size_t size) {
    _target.reserve(_target.length() + size);
    for (size_t i = 0; i < size; i++) _target += (char)buffer[i];
    return size;
  }
private:
  String &_target;
};

// Include existing logic files
#include "logic_acom.h"
#include "logic_wificom.h"
#include "logic_btcom.h"
#include "logic_digirom.h"

// -------------------------------------------------------------
// Audio System (M5Unified Speaker)
// -------------------------------------------------------------
void playBeep() {
    M5.Speaker.tone(4000, 30);
}
void playSuccess() {
    M5.Speaker.tone(2000, 100);
    delay(100);
    M5.Speaker.tone(3000, 150);
}
void playFail() {
    M5.Speaker.tone(800, 250);
}

// -------------------------------------------------------------
// 3-Button Navigation Management for M5Unified
// -------------------------------------------------------------
void handleButtons() {
    M5.update(); // Update button states
    bool buttonStateChanged = false;

    // Front button (BtnA) -> OK/Select (Short press) or Cancel/Back (Hold)
    if (M5.BtnA.wasHold()) {
        btnCancelPressed = true;
        buttonStateChanged = true;
        playBeep();
    } else if (M5.BtnA.wasClicked()) {
        btnOkPressed = true;
        buttonStateChanged = true;
        playBeep();
    }
    
    // Right button (BtnB) -> Scroll Down (looping)
    if (M5.BtnB.wasClicked()) {
        btnDownPressed = true;
        buttonStateChanged = true;
        playBeep();
    }
    
    // Left/Power button (BtnPWR or BtnC) -> Scroll Up (looping)
    if (M5.BtnPWR.wasClicked() || M5.BtnC.wasClicked()) {
        btnUpPressed = true;
        buttonStateChanged = true;
        playBeep();
    }

    if (buttonStateChanged) {
        uiNeedsUpdate = true;
    }
}

// -------------------------------------------------------------
// M5GFX Drawing System (Color UI)
// -------------------------------------------------------------
void drawScrollingText(int x, int y, const char *text, int maxWidth, bool selected) {
    if (text == nullptr) return;
    int textWidth = canvas.textWidth(text);

    canvas.setClipRect(x, y - 2, maxWidth, 24);

    if (textWidth <= maxWidth || !selected) {
        canvas.drawString(text, x, y);
    } else {
        isScrollingText = true; // ตั้งค่าเป็น true เฉพาะตอนที่ข้อความยาวกว่าจอจริงๆ
        unsigned long currentTime = millis();
        if (currentTime - lastTextScrollTime > TEXT_SCROLL_DELAY) {
            lastTextScrollTime = currentTime;
            textScrollOffset += 3;
            if (textScrollOffset > textWidth + 10) {
                textScrollOffset = 0;
                lastTextScrollTime += TEXT_SCROLL_RESET_DELAY;
            }
        }
        canvas.drawString(text, x - textScrollOffset, y);
    }
    canvas.clearClipRect();
}

void drawMenu(const char *menuTitle, char **items, int itemCount, int cursor, int windowStart, const char *bottomHint) {
    canvas.fillScreen(TFT_BLACK);
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextDatum(top_left);

    // Title Bar
    canvas.fillRect(0, 0, 240, 26, TFT_DARKGREY);
    canvas.setTextColor(TFT_WHITE, TFT_DARKGREY);
    canvas.drawString(menuTitle, 10, 4);

    // Menu Items
    const int menuTopY = 32;
    const int rowHeight = 22;
    const int maxItemsOnScreen = 4; // Max 4 lines for M5StickC+2

    for (int i = 0; i < maxItemsOnScreen; i++) {
        int itemIndex = windowStart + i;
        if (itemIndex >= itemCount) break;
        if (items[itemIndex] == nullptr) continue;

        int textY = menuTopY + (i * rowHeight);

        if (itemIndex == cursor) {
            // Highlight selector (Orange)
            canvas.fillRect(0, textY - 2, 240, rowHeight, TFT_ORANGE);
            canvas.setTextColor(TFT_BLACK, TFT_ORANGE);
            drawScrollingText(10, textY, items[itemIndex], 220, true);
        } else {
            canvas.setTextColor(TFT_WHITE, TFT_BLACK);
            drawScrollingText(10, textY, items[itemIndex], 220, false);
        }
    }

    // Bottom Hint Bar
    canvas.drawLine(0, 118, 240, 118, TFT_DARKGREY);
    canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    canvas.drawString(bottomHint, 10, 120);
}

void drawUI() {
    isScrollingText = false; // รีเซ็ตสถานะเลื่อนข้อความทุกครั้งก่อนเริ่มวาดจอ ป้องกันจอกระพริบ
    canvas.setFont(&fonts::FreeSans9pt7b);

    switch (currentState) {
        case STATE_MAIN_MENU: {
            char *mainMenuItemsPtr[mainMenuItemCount];
            for (int i = 0; i < mainMenuItemCount; i++) {
                mainMenuItemsPtr[i] = (char *)mainMenuItems[i];
            }
            char fullMenuTitle[40];
            snprintf(fullMenuTitle, sizeof(fullMenuTitle), "Main Menu                 v%s", fw_ver);
            drawMenu(fullMenuTitle, mainMenuItemsPtr, mainMenuItemCount, mainMenuCursor, mainMenuWindowStart, "A:OK B:Dwn PWR:Up");
            break;
        }

        case STATE_DIGIROM_DEVICE:
            drawMenu("Select Device", deviceMenu, deviceMenuCount, deviceMenuCursor, deviceMenuWindowStart, "A:OK B:Dwn PWR:Up");
            break;

        case STATE_DIGIROM_NAME:
            drawMenu(selectedDevice, nameMenu, nameMenuCount, nameMenuCursor, nameMenuWindowStart, "A:Send/Back B:Dwn PWR:Up");
            break;

        case STATE_ACOM_USB:
            canvas.fillScreen(TFT_BLACK);
            canvas.fillRect(0, 0, 240, 26, TFT_DARKGREY);
            canvas.setTextColor(TFT_WHITE, TFT_DARKGREY);
            canvas.drawString("ACOM USB Mode", 10, 4);
            canvas.setTextColor(TFT_GREEN, TFT_BLACK);
            canvas.drawString("Waiting for Serial command...", 10, 50);
            canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
            canvas.drawString("Hold A: Back", 10, 120);
            break;

        case STATE_BTCOM:
            canvas.fillScreen(TFT_BLACK);
            canvas.fillRect(0, 0, 240, 26, TFT_BLUE);
            canvas.setTextColor(TFT_WHITE, TFT_BLUE);
            canvas.drawString("BTCOM Mode", 10, 4);
            canvas.setTextColor(TFT_WHITE, TFT_BLACK);
            canvas.drawString("Status:", 10, 40);
            canvas.setTextColor(TFT_CYAN, TFT_BLACK);
            canvas.drawString(btcomStatus.c_str(), 10, 65);
            canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
            canvas.drawString("Hold A: Back", 10, 120);
            break;

        case STATE_WIFICOM_CONNECTING:
            canvas.fillScreen(TFT_BLACK);
            canvas.fillRect(0, 0, 240, 26, TFT_PURPLE);
            canvas.setTextColor(TFT_WHITE, TFT_PURPLE);
            canvas.drawString("WIFICOM Mode", 10, 4);
            canvas.setTextColor(TFT_WHITE, TFT_BLACK);
            canvas.drawString("Connection Status:", 10, 40);
            canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
            canvas.drawString(wificomStatus.c_str(), 10, 65);
            canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
            canvas.drawString("Hold A: Cancel", 10, 120);
            break;

        case STATE_WIFICOM:
            canvas.fillScreen(TFT_BLACK);
            canvas.fillRect(0, 0, 240, 26, TFT_DARKGREEN);
            canvas.setTextColor(TFT_WHITE, TFT_DARKGREEN);
            canvas.drawString("WIFICOM (Ready)", 10, 4);
            
            if (strlen(lastExecutionResult) > 0) {
                canvas.setTextColor(TFT_WHITE, TFT_BLACK);
                canvas.drawString("Last Result:", 10, 40);
                canvas.setTextColor(TFT_GREEN, TFT_BLACK);
                canvas.drawString(lastExecutionResult, 10, 65);
            }
            canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
            canvas.drawString("Hold A: Disconnect", 10, 120);
            break;

        case STATE_SETTINGS:
            canvas.fillScreen(TFT_BLACK);
            canvas.fillRect(0, 0, 240, 26, TFT_DARKGREY);
            canvas.setTextColor(TFT_WHITE, TFT_DARKGREY);
            canvas.drawString("Device Settings", 10, 4);
            canvas.setTextColor(TFT_WHITE, TFT_BLACK);
            canvas.drawString("Configure Wi-Fi and params", 10, 40);
            canvas.setTextColor(TFT_CYAN, TFT_BLACK);
            canvas.drawString("> Visit m5wificom.n3gp.com <", 10, 65);
            canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
            canvas.drawString("Hold A: Back", 10, 120);
            break;

        case STATE_EXECUTING:
            canvas.fillScreen(TFT_BLACK);
            canvas.fillRect(0, 0, 240, 26, TFT_BLUE);
            //canvas.setTextColor(TFT_WHITE, TFT_RED);
            //canvas.drawCenterString("Executing Command", 120, 4, &fonts::FreeSans9pt7b);
            
            canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
            drawScrollingText(10, 40, selectedName, 220, true);

            canvas.setTextColor(TFT_WHITE, TFT_BLACK);
            if (strlen(executionResult) > 0) {
                if (strcmp(executionResult, "Summon Successfully") == 0 || strcmp(executionResult, "Battle Successfully") == 0) {
                    canvas.setTextColor(TFT_GREEN, TFT_BLACK);
                    canvas.drawString("Success!", 10, 75);
                } else {
                    canvas.drawString("Result:", 10, 75);
                    canvas.drawString(executionResult, 10, 95);
                }
            } else {
                canvas.drawString((millis() - executionStartTime < 500) ? "Starting..." : "Sending data...", 10, 75);
            }

            canvas.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
            canvas.drawString("Hold A: Back", 10, 120);
            break;

        default:
            break;
    }
    
    canvas.pushSprite(0, 0);
}

// -------------------------------------------------------------
// Core System Functions
// -------------------------------------------------------------
void enterDeepSleep() {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawCenterString("Powering off...", 120, 50, &fonts::FreeSans9pt7b);
    M5.Display.drawCenterString("(Press left button to power on)", 120, 80, &fonts::FreeSans9pt7b);
    delay(2000);
    M5.Power.powerOff();
}

void handleMainMenu() {
    const int maxItemsOnScreen = 4;

    // Up Scrolling Logic (PWR Button)
    if (btnUpPressed) {
        mainMenuCursor--;
        if (mainMenuCursor < 0) mainMenuCursor = mainMenuItemCount - 1;
        
        if (mainMenuCursor < mainMenuWindowStart) {
            mainMenuWindowStart = mainMenuCursor;
        } else if (mainMenuCursor == mainMenuItemCount - 1 && mainMenuItemCount > maxItemsOnScreen) {
            mainMenuWindowStart = mainMenuItemCount - maxItemsOnScreen;
        }
        textScrollOffset = 0;
        lastTextScrollTime = millis();
        uiNeedsUpdate = true;
    }

    // Down Scrolling Logic (Right Button)
    if (btnDownPressed) {
        mainMenuCursor++;
        if (mainMenuCursor >= mainMenuItemCount) mainMenuCursor = 0;
        
        if (mainMenuCursor >= mainMenuWindowStart + maxItemsOnScreen) {
            mainMenuWindowStart = mainMenuCursor - (maxItemsOnScreen - 1);
        } else if (mainMenuCursor == 0 && mainMenuItemCount > maxItemsOnScreen) {
            mainMenuWindowStart = 0;
        }
        textScrollOffset = 0;
        lastTextScrollTime = millis();
        uiNeedsUpdate = true;
    }

    if (btnOkPressed) {
        switch (mainMenuCursor) {
            case 0: currentState = STATE_ACOM_USB; break;
            case 1: currentState = STATE_BTCOM; startBtcom(); break;
            case 2: currentState = STATE_WIFICOM_CONNECTING; break;
            case 3: 
                deviceMenuCursor = 0; 
                deviceMenuWindowStart = 0; 
                currentState = STATE_DIGIROM_DEVICE; 
                break;
            case 4: currentState = STATE_SETTINGS; break;
            case 5: currentState = STATE_SLEEP; break;
        }
        textScrollOffset = 0;
        lastTextScrollTime = millis();
        uiNeedsUpdate = true;
    }
}

void performWifiTest(String ssid, String pass) {
    if (ssid.length() == 0) return;
    String status_text = "";
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    unsigned long wifiConnectStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiConnectStart < 15000) { delay(250); }
    WiFi.disconnect(true);
}

void handleSettings() {
    if (btnCancelPressed) {
        currentState = STATE_MAIN_MENU;
        lastActivityTime = millis();
        uiNeedsUpdate = true;
        return;
    }
    
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    int firstColon = command.indexOf(':');
    String status_text = "";

    Serial.print("SETTINGS: Received: ");
    Serial.println(command);

    if (command == "xcom") {
      WiFi.mode(WIFI_STA);
      delay(300);
      String deviceMac = WiFi.macAddress();
      Serial.println("MACADDRESS:" + String(deviceMac));
      Serial.println("MODEL:" + String(model));
      Serial.println("VERSION:" + String(fw_ver));
    } else if (command.equalsIgnoreCase("help")) {
      Serial.println("\n--- Settings Commands ---");
      Serial.println("READ:<addr>:<key>    - Read string from EEPROM");
      Serial.println("WRITE:<addr>:<value> - Write string to EEPROM");
      Serial.println("WIFITEST:<ssid>:<pass> - Test connection manually");
      Serial.println("TESTSLOT:<1|2|3>     - Test connection stored in slot");
      Serial.println("WIFISCAN             - Scan for WiFi networks");
      Serial.println("--- Addresses ---");
      Serial.println("  0: SSID 1,  50: PASS 1");
      Serial.println("100: SSID 2, 150: PASS 2");
      Serial.println("200: SSID 3, 250: PASS 3");
      Serial.println("300: MQTT User, 350: MQTT Pass");
      Serial.println("400: User UUID, 450: Device UUID");
      Serial.println("-----------------------\n");
    } else if (firstColon > 0 || command.equalsIgnoreCase("WIFISCAN")) {
      String type, payload;

      if (firstColon > 0) {
        type = command.substring(0, firstColon);
        payload = command.substring(firstColon + 1);
      } else {
        type = command;
        payload = "";
      }
      type.toUpperCase();

      int secondColon = payload.indexOf(':');

      if (type == "WIFISCAN") {
        Serial.println("SCAN:START");
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);
        int n = WiFi.scanNetworks();
        if (n == 0) {
          Serial.println("SCAN:EMPTY");
        } else {
          for (int i = 0; i < n; ++i) {
            String auth = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "OPEN" : "SECURED";
            Serial.print("SCAN:RESULT:");
            Serial.print(WiFi.SSID(i));
            Serial.print(":");
            Serial.print(WiFi.RSSI(i));
            Serial.print(":");
            Serial.println(auth);
            delay(10);
          }
        }
        Serial.println("SCAN:END");

      } else if (type == "TESTSLOT") {
        int slot = payload.toInt();
        int ssidAddr = -1;
        int passAddr = -1;

        if (slot == 1) {
          ssidAddr = ADDR_WIFI_SSID_1;
          passAddr = ADDR_WIFI_PASS_1;
        } else if (slot == 2) {
          ssidAddr = ADDR_WIFI_SSID_2;
          passAddr = ADDR_WIFI_PASS_2;
        } else if (slot == 3) {
          ssidAddr = ADDR_WIFI_SSID_3;
          passAddr = ADDR_WIFI_PASS_3;
        }

        if (ssidAddr != -1) {
          String ssid = readStringFromEEPROM(ssidAddr);
          String pass = readStringFromEEPROM(passAddr);
          Serial.println("Testing Slot " + String(slot) + " (" + ssid + ")");
          performWifiTest(ssid, pass);
        } else {
          Serial.println("Error: Invalid Slot Number (1-3)");
        }

      } else if (secondColon > 0) {
        if (type == "WRITE") {
          int address = payload.substring(0, secondColon).toInt();
          if (address >= 0 && address < EEPROM_SIZE - 50) {
            String value = payload.substring(secondColon + 1);
            writeStringToEEPROM(address, value);
            status_text = "OK: Wrote to " + String(address);
            Serial.println(status_text);
          } else {
            Serial.println("Error: Invalid EEPROM address for WRITE.");
          }
        } else if (type == "READ") {
          int address = payload.substring(0, secondColon).toInt();
          if (address >= 0 && address < EEPROM_SIZE) {
            String responseKey = payload.substring(secondColon + 1);
            String value = readStringFromEEPROM(address);
            status_text = responseKey + ":" + value;
            Serial.println(status_text);
          } else {
            Serial.println("Error: Invalid EEPROM address for READ.");
          }
        } else if (type == "WIFITEST") {
          String ssid = payload.substring(0, secondColon);
          String password = payload.substring(secondColon + 1);
          performWifiTest(ssid, password);
        } else {
          Serial.println("Error: Unknown command type.");
        }
      } else {
        Serial.println("Error: Command format incorrect. Use TYPE:PAYLOAD.");
      }
    } else {
      Serial.println("Error: Command format incorrect. Use TYPE:PAYLOAD or 'help'.");
    }
  }
}

void handleExecuting() {
    handleAcomUsb();
    handleBtcom();
    if (btnOkPressed || btnCancelPressed) {
        currentState = prevState;
        if (currentState == STATE_MAIN_MENU) lastActivityTime = millis();
        textScrollOffset = 0;
        lastTextScrollTime = millis();
        if (current_digirom != nullptr) { delete current_digirom; current_digirom = nullptr; }
        if (currentState == STATE_WIFICOM) mqtt_digirom_command = "";
        strcpy(executionResult, "");
        strcpy(lastExecutionResult, "");
        strcpy(selectedName, "");
        uiNeedsUpdate = true;
        return;
    }
    if (current_digirom != nullptr) {
        // Draw executing bar bypass canvas for immediate loading screen
        M5.Display.fillRect(0, 0, 240, 26, TFT_RED);
        M5.Display.setTextColor(TFT_WHITE, TFT_RED);
        
          M5.Display.drawCenterString("EXECUTE", 120, 4, &fonts::FreeSans9pt7b);
        //M5.Display.drawCenterString("Sending Command...", 120, 4, &fonts::FreeSans9pt7b);

        controller.execute(*current_digirom, 3000);

        String resultString = "";
        StringPrinter stringPrinter(resultString);
        current_digirom->printResult(stringPrinter);
        resultString.trim();

        char tempResult[sizeof(lastExecutionResult)];
        strncpy(tempResult, resultString.c_str(), sizeof(tempResult) - 1);
        tempResult[sizeof(tempResult) - 1] = '\0';
        strcpy(lastExecutionResult, tempResult);

        bool received = (resultString.indexOf("r:") != -1);
        if (current_digirom->turn() == 2 && received) {
            strcpy(executionResult, "TURN2 Successfully");
            playSuccess();
        } else if (current_digirom->turn() == 1 && received) {
            strcpy(executionResult, "TURN1 Successfully");
            playSuccess();
        } else {
            strcpy(executionResult, tempResult);
            if(received) playSuccess(); else playFail();
        }

        if (current_digirom->turn() == 1) delay(3000);
        uiNeedsUpdate = true;

        if (prevState == STATE_ACOM_USB) {
            Serial.print(resultString); Serial.println();
        } else if (prevState == STATE_BTCOM) {
            Serial.print(resultString); Serial.println();
            sendBtcomResult(resultString);
        }
    } else if (current_digirom == nullptr) {
        if ((strlen(executionResult) == 0 || strcmp(executionResult, "No result") == 0)) {
            strcpy(executionResult, "Success / Idle");
            uiNeedsUpdate = true;
        }
    }
}

// -------------------------------------------------------------
// Setup & Loop
// -------------------------------------------------------------
void setup() {
    Serial.begin(9600);
    EEPROM.begin(EEPROM_SIZE);

    // M5Unified Initialization (Controls Display, Buttons, Audio, Power)
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1); // Horizontal orientation
    M5.Speaker.setVolume(128); // Medium volume

    // Create Canvas to match screen size
    canvas.createSprite(M5.Display.width(), M5.Display.height());

    WiFi.mode(WIFI_STA);
    delay(300);

    controller.add(classic_comm);
    controller.add(color_comm);

    populateDeviceMenu();

    // Boot Screen (M5GFX direct draw)
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawCenterString("M5WIFICOM", 120, 50, &fonts::FreeSansBold12pt7b);
    M5.Display.drawCenterString("Starting system...", 120, 90, &fonts::FreeSans9pt7b);
    delay(2000);

    String deviceMac = WiFi.macAddress();
    lastActivityTime = millis();
    uiNeedsUpdate = true;
    lastState = currentState;

    clientId += String(random(0xffff), HEX);
    
    // Boot sound
    M5.Speaker.tone(2000, 100); delay(100);
    M5.Speaker.tone(3000, 100); delay(100);
    M5.Speaker.tone(4000, 150);
}

void loop() {
    handleButtons();

    bool buttonWasPressed = btnUpPressed || btnDownPressed || btnOkPressed || btnCancelPressed;
    if (buttonWasPressed) {
        lastActivityTime = millis();
    }

    // Auto-Sleep after 15 mins of inactivity in Main Menu
    if (currentState == STATE_MAIN_MENU && (millis() - lastActivityTime > 15 * 60 * 1000)) {
        currentState = STATE_SLEEP;
        uiNeedsUpdate = true;
    }

    if (currentState == STATE_MAIN_MENU && Serial.available() > 0) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        if (command == "xcom") {
            WiFi.mode(WIFI_STA); delay(300);
            Serial.println("MACADDRESS:" + String(WiFi.macAddress()));
            Serial.println("MODEL:" + String(model));
            Serial.println("VERSION:" + String(fw_ver));
        }
    }

    switch (currentState) {
        case STATE_MAIN_MENU: handleMainMenu(); break;
        case STATE_DIGIROM_DEVICE: handleDeviceMenu(); break;
        case STATE_DIGIROM_NAME: handleNameMenu(); break;
        case STATE_ACOM_USB: handleAcomUsb(); break;
        case STATE_BTCOM: handleBtcom(); break;
        case STATE_WIFICOM_CONNECTING:
        case STATE_WIFICOM: handleWifiCom(); break;
        case STATE_SETTINGS: handleSettings(); break;
        case STATE_EXECUTING: handleExecuting(); break;
        case STATE_SLEEP: enterDeepSleep(); break;
    }

    if (currentState == STATE_WIFICOM_CONNECTING || currentState == STATE_WIFICOM) {
        wificom_loop();
    }

    if (currentState != lastState) {
        uiNeedsUpdate = true;
        lastState = currentState;
        if (currentState != STATE_DIGIROM_NAME && currentState != STATE_DIGIROM_DEVICE && currentState != STATE_SLEEP) {
            textScrollOffset = 0;
            lastTextScrollTime = millis();
        }
    }

    // Prevents screen flicker, triggers draw only if required
    if (uiNeedsUpdate || (isScrollingText && millis() - lastTextScrollTime > 30)) {
        drawUI();
        uiNeedsUpdate = false;
    }

    btnUpPressed = false;
    btnDownPressed = false;
    btnOkPressed = false;
    btnCancelPressed = false;
}