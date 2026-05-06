#pragma once
#include <Arduino.h>

// Basic color aliases used by the original sketch.
#ifndef BLACK
#define BLACK 0x0000
#endif
#ifndef WHITE
#define WHITE 0xFFFF
#endif
#ifndef RED
#define RED 0xF800
#endif
#ifndef GREEN
#define GREEN 0x07E0
#endif
#ifndef TFT_BLACK
#define TFT_BLACK BLACK
#endif

// TTGO / LilyGO T-Display S3 pins from the provided factory example.
static constexpr int TDISPLAY_PIN_BUTTON_1 = 0;
static constexpr int TDISPLAY_PIN_BUTTON_2 = 14;
static constexpr int TDISPLAY_PIN_POWER_ON = 15;
static constexpr int TDISPLAY_PIN_BAT_VOLT = 4;
static constexpr int TDISPLAY_PIN_LCD_BL   = 38;

struct DummyM5Config {};
namespace M5 {
  inline DummyM5Config config() { return {}; }
}

class CompatButton {
public:
  explicit CompatButton(int pin = -1) : pin_(pin) {}

  void begin() {
    if (pin_ >= 0) {
      pinMode(pin_, INPUT_PULLUP);
    }
    state_ = rawPressed();
    last_state_ = state_;
    last_change_ = millis();
    press_start_ = state_ ? millis() : 0;
    pressed_for_fired_ = false;
    was_pressed_latch_ = false;
  }

  void update() {
    bool now = rawPressed();
    unsigned long now_ms = millis();
    if (now != state_) {
      state_ = now;
      last_change_ = now_ms;
      if (state_) {
        press_start_ = now_ms;
        pressed_for_fired_ = false;
      } else if (last_state_) {
        was_pressed_latch_ = true;
        pressed_for_fired_ = false;
      }
    }
    last_state_ = state_;
  }

  bool wasPressed() {
    bool out = was_pressed_latch_;
    was_pressed_latch_ = false;
    return out;
  }

  bool pressedFor(uint32_t ms) {
    if (state_ && !pressed_for_fired_ && (millis() - press_start_ >= ms)) {
      pressed_for_fired_ = true;
      return true;
    }
    return false;
  }

private:
  bool rawPressed() const {
    return pin_ >= 0 ? digitalRead(pin_) == LOW : false;
  }

  int pin_;
  bool state_ = false;
  bool last_state_ = false;
  bool was_pressed_latch_ = false;
  bool pressed_for_fired_ = false;
  unsigned long last_change_ = 0;
  unsigned long press_start_ = 0;
};

class CompatDisplay {
public:
  void begin() {
    pinMode(TDISPLAY_PIN_LCD_BL, OUTPUT);
    setBrightness(16);
  }

  void setBrightness(uint8_t value) {
    // T-Display-S3 backlight controller is stepped. For serial-first build,
    // just keep the backlight on/off so the screen doesn't stay dark.
    digitalWrite(TDISPLAY_PIN_LCD_BL, value ? HIGH : LOW);
  }

  void fillScreen(uint16_t) {}
  void setSwapBytes(bool) {}
  void setRotation(uint8_t) {}
  void setTextWrap(bool) {}
  template <typename T> void loadFont(T) {}
  void setCursor(int32_t, int32_t) {}
  void setTextSize(uint8_t) {}
  void setTextColor(uint16_t, uint16_t = BLACK) {}
  void fillRect(int32_t, int32_t, int32_t, int32_t, uint16_t) {}
  void drawString(const char*, int32_t, int32_t) {}
  void drawString(const String&, int32_t, int32_t) {}
  void drawBitmap(int32_t, int32_t, const unsigned char*, int32_t, int32_t, uint16_t) {}
  void drawBitmap(int32_t, int32_t, const unsigned short*, int32_t, int32_t, uint16_t) {}
  void pushImage(int32_t, int32_t, int32_t, int32_t, const uint16_t*) {}
  void print(const String& s) { Serial.print(s); }
  void print(const char* s) { Serial.print(s); }
  void print(char c) { Serial.print(c); }
  void print(int v) { Serial.print(v); }
  void println(const String& s) { Serial.println(s); }
};

class CompatSpeaker {
public:
  void tone(uint32_t, uint32_t) {}
};

class CompatPower {
public:
  void begin() {
    pinMode(TDISPLAY_PIN_POWER_ON, OUTPUT);
    digitalWrite(TDISPLAY_PIN_POWER_ON, HIGH);
    analogReadResolution(12);
  }

  int getBatteryVoltage() {
    uint32_t raw = analogRead(TDISPLAY_PIN_BAT_VOLT);
    uint32_t mv = (raw * 2UL * 3300UL) / 4095UL;
    return static_cast<int>(mv);
  }

  void powerOff() {
    Serial.println("Power off requested. Entering restart loop instead on serial-first ESP32-S3 build.");
    delay(200);
    ESP.restart();
  }
};

class CompatStickCP2 {
public:
  CompatDisplay Display;
  CompatDisplay& Lcd = Display;
  CompatPower Power;
  CompatSpeaker Speaker;
  CompatButton BtnA{TDISPLAY_PIN_BUTTON_1};
  CompatButton BtnB{TDISPLAY_PIN_BUTTON_2};
  CompatButton BtnPWR{TDISPLAY_PIN_BUTTON_2}; // no dedicated power button on this build

  void begin(const DummyM5Config&) {
    Power.begin();
    Display.begin();
    BtnA.begin();
    BtnB.begin();
    BtnPWR.begin();
  }

  void update() {
    BtnA.update();
    BtnB.update();
    BtnPWR.update();
  }
};

inline CompatStickCP2 StickCP2;
