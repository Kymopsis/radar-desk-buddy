# Flashing the firmware

## Option A: build with ESP-IDF (works today)

1. Install [ESP-IDF v6.0](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html)
   (the VS Code extension is the easiest way on Windows).
2. Build and flash:

```bash
git clone https://github.com/Kymopsis/radar-desk-buddy.git
cd radar-desk-buddy/firmware
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

`PORT` is `COM5` (Windows), `/dev/ttyACM0` (Linux) or `/dev/cu.usbmodem*` (macOS).
Use the board's USB-C port marked **USB** for flashing and logs.

If the board is not detected: hold **BOOT**, tap **RST**, release **BOOT**, then
flash again.

## Option B: flash from the browser

A web flasher (Chrome or Edge, no install) will be linked here with the first
release.

## Start over

To erase all settings (Wi-Fi, city, Telegram token):

```bash
idf.py -p PORT erase-flash
idf.py -p PORT flash
```
