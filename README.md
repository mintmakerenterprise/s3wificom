# S3wificom
Referencing <a href="https://github.com/SuwitNaynan/m5wificom-source">M5wificom-source code by SuwitNaynan</a><br>
Port in for <a href="https://github.com/Xinyuan-LilyGO/T-Display-S3"> Lilygo T-Display-S3</a>
Compile code with Arduino IDE <br>

Clone the t_display_s3 file and open t_display_s3.ino on Arduino IDE
install all include library before compile.


## Hardware setting
<h4>How to change pin interface</h4>

Source (t_display_s3.ino)

    int INPUT_PIN = 3;
    int OUTPUT_PIN = 1;


![T-Display S3 Circuit](images/t%20display%20s3%20circuit.png)


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


## Setting up wifi and wificom details
1. Boot up main menu
2. Wifi setup
3. Start hotspot
4. Connect to hotspot 
5. Type ip address on browser to access to form 
6. Input your wificom detail and home wifi for connection


### Main Menu

![S3 Main Menu](images/S3%20main%20menu.JPG)

### WiFi Setup

![WiFi Setup](images/wifi%20setup.jpg)


### Hotspot Setup

![Hotspot Setup](images/hotspot%20setup.JPG)

### Hotspot Input Page

![Hotspot Input Page](images/hotspot%20for%20input.png)




<hr>
*credit for*<br>
Main reference <a href="https://github.com/SuwitNaynan/m5wificom-source">M5wificom-source by SuwitNaynan</a><br>
base library form <a href="https://wificom.dev/">BrassBolt@wificom</a> , <a href="https://github.com/dmcomm">BladeSabre@DMComm</a>,  <a href="https://gist.github.com/arcao">Martin Sloup@StringStream</a>, Arduino<br>
base digirom form <a href="https://humulos.com/">humulos</a>, <a href="https://www.youtube.com/@joushiikuta/videos">jyoshiikuta</a>, Feanan<br>

