# FAQ

**Does it use a camera or record anything?**
No. The radar measures position and movement as numbers. There is no camera and
no microphone.

**Does it work through the case?**
Yes, through plastic, wood or fabric. Metal and foil block the radar.

**Which radar kit do I need?**
The Kymopsis 60 GHz AiP radar development kit with the **Presence, wall mount**
firmware. It reports target positions over UART, which the ESP32 turns into an
angle and a distance. See the parts list in the [README](../README.md).

**Can I use a different ESP32?**
The firmware targets ESP32-S3 with 8 MB octal PSRAM (N16R8). Other S3 boards
need pin changes in `firmware/components/board/include/board_pins.h` and may run
out of memory without PSRAM.

**My Wi-Fi is 5 GHz only.**
The ESP32-S3 supports 2.4 GHz only. Most routers can enable a 2.4 GHz network.

**What is sent over the internet?**
Your city name (once) and coordinates to Open-Meteo, NTP time requests, and, if
you enable Level 2, your bot's traffic to Telegram. Nothing goes to Kymopsis.

**The figure turns the wrong way.**
Open the setup page, section "Servo calibration", and press **Flip pan**.

**I forgot the hotspot password.**
Walk up to the desk and pat the head (touch pad) while the clock is showing: the
setup QR code and password appear for 30 seconds (pat again to go back). Before
Wi-Fi is set up, they show whenever you come close.

**Can I buy the parts as a set?**
A bundle is planned on [kymopsis.com](https://kymopsis.com/shop/?utm_source=github&utm_medium=faq&utm_campaign=radar-desk-buddy).
