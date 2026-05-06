# S3wificom
Referencing <a href="https://github.com/SuwitNaynan/m5wificom-source">M5wificom-source code by SuwitNaynan</a><br>
Port in for <a href="https://github.com/Xinyuan-LilyGO/T-Display-S3"> Lilygo T-Display-S3</a>
Compile code with Arduino IDE <br>

Clone the t_display_s3 file and open t_display_s3.ino on Arduino IDE
install all include library before compile.

<h4>How to change pin interface</h4>

Source (t_display_s3.ino)

    int INPUT_PIN = 3;
    int OUTPUT_PIN = 1;


![T-Display S3 Circuit](t%20display%20s3%20circuit.png)


Ensure you select these parameter during ino upload.

## Arduino IDE Settings

| Arduino IDE Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| Port | Your port |
| USB CDC On Boot | Enable |
| CPU Frequency | 240MHz (WiFi) |
| Core Debug Level | None |
| USB DFU On Boot | Disable |
| Erase All Flash Before Sketch Upload | Disable |
| Events Run On | Core 1 |
| Flash Mode | QIO 80MHz |
| Flash Size | 16MB (128Mb) |
| Arduino Runs On | Core 1 |
| USB Firmware MSC On Boot | Disable |
| Partition Scheme | 16M Flash (3M APP / 9.9MB FATFS) |
| PSRAM | OPI PSRAM |
| Upload Mode | UART0 / Hardware CDC |
| Upload Speed | 921600 |
| USB Mode | CDC and JTAG |

For complete upload steps, refer <a href="https://github.com/Xinyuan-LilyGO/T-Display-S3"> Arduino IDE Manual installation</a>

<hr>
*credit for*<br>
Main reference <a href="https://github.com/SuwitNaynan/m5wificom-source">M5wificom-source by SuwitNaynan</a><br>
base library form <a href="https://wificom.dev/">BrassBolt@wificom</a> , <a href="https://github.com/dmcomm">BladeSabre@DMComm</a>,  <a href="https://gist.github.com/arcao">Martin Sloup@StringStream</a>, Arduino<br>
base drigirom form <a href="https://humulos.com/">humulos</a>, <a href="https://www.youtube.com/@joushiikuta/videos">jyoshiikuta</a>, Feanan<br>

