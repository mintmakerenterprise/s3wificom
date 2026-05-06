ESP32-S3 SERIAL-FIRST PORT NOTES
================================

Target board:
- Arduino IDE board: ESP32S3 Dev Module
- Hardware basis: LilyGO / TTGO T-Display S3 pinout from the supplied factory.zip

Pins used in this port:
- DMComm TX/output: GPIO 1
- DMComm RX/input : GPIO 3
- Battery sense   : GPIO 4
- Button A        : GPIO 0
- Button B        : GPIO 14
- Power enable    : GPIO 15
- LCD backlight   : GPIO 38

What changed:
- Removed hard dependency on M5StickCPlus2 library.
- Added TDisplayS3Compat.h to emulate the minimum M5 API that the sketch expects.
- Kept this build serial-first: the display API is stubbed, so the sketch compiles without M5 display libraries.
- Battery voltage is read from GPIO 4 using the same divider formula seen in the supplied TTGO factory example.
- Serial speed changed to 115200.
- DMComm pins changed from M5 values to GPIO1/GPIO3.
- ADC resolution changed to 12-bit for ESP32-S3.

Important warning:
- This build assumes your DMComm external interface is resistor-divider based, because that is what you specified.
- If the Digivice still shows wrong idle/high/low waveform behavior, that is likely a hardware-interface limitation, not just firmware.
- GPIO1 is kept because you explicitly requested it, but it is not the safest GPIO on ESP32-S3 for sensitive signaling.

Recommended Arduino libraries (same functional family as original sketch):
- ESPAsyncWebServer
- AsyncTCP
- PubSubClient
- ArduinoJson
- ImprovWiFiLibrary
- DMComm

What this build is and is not:
- It IS a first-pass ESP32-S3 port for testing serial, WiFi, MQTT, EEPROM config, battery read, and DMComm pin mapping.
- It is NOT yet a polished native T-Display-S3 graphical port.
