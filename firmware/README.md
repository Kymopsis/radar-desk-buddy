# Radar Desk Buddy firmware

ESP-IDF project for the ESP32-S3-N16R8 (16 MB flash, 8 MB octal PSRAM).
Tested to compile with **ESP-IDF v6.0.2**.

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Plug the USB cable into the board's **USB** (native) port for flashing and logs;
the console is USB-Serial-JTAG.

## Components

| Component | What it does |
|---|---|
| `board` | Pin map (`board_pins.h`) |
| `display` | GC9A01 round LCD over SPI |
| `ui` | Faces (eyes, info, message, setup QR), distance bands, i18n (10 languages) |
| `radar_link` | UART reader for the 60 GHz radar (TinyFrame frames, baud auto-detect) |
| `motion` | Pan/lift follow logic and presets (rise, hide, peek, wave, look around) |
| `servo` | LEDC 50 Hz PWM for the two micro servos |
| `softap_portal` | Setup Wi-Fi hotspot + captive web page at `http://mod.config` |
| `dns_server` | Captive DNS for the setup hotspot |
| `device_cfg` | Settings in NVS (Wi-Fi, city, Telegram token, tuning) |
| `net_time` | Level 1: SNTP clock, city geocoding and weather from Open-Meteo |
| `telegram_link` | Level 2: Telegram Bot API long polling, messages on the screen |
| `mqtt_link` | Level 3 (optional): your own MQTT server. Idle unless a server URL is set |

`components/radar_link/test/` has a host-side test for the radar frame decoder:

```bash
cd components/radar_link/test
gcc -std=c11 -Wall -Wextra -Werror -I../include ../radar_protocol.c test_radar_protocol.c -lm -o radar_protocol_test
./radar_protocol_test
```

## Network services used

| Service | Why | Sent |
|---|---|---|
| `pool.ntp.org`, `time.google.com`, `time.cloudflare.com` | Clock | NTP request |
| `geocoding-api.open-meteo.com` | Turn your city name into coordinates | City name |
| `api.open-meteo.com` | Current weather and UTC offset | Coordinates (2–3 decimals) |
| `api.telegram.org` | Only if you add a bot token | Your bot token, chat id, message text |

Nothing is sent to Kymopsis.
