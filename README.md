# Radar Desk Buddy

**An ESP32-S3 desk companion that sees you with a 60 GHz mmWave radar.**
Its eyes and its little figure turn to follow you around the room. Walk up to
the desk and the round screen switches to the time and the local weather.
Send it a message from Telegram and it is waiting for you when you come back.

No soldering: everything connects with Dupont jumper wires and lever wire
connectors. Wi-Fi setup happens in your phone's browser, served by the ESP32
itself.

> Status: firmware compiles with ESP-IDF v6.0.2 and runs on the prototype.
> The 3D-printed enclosure (v3.2: 8 STL parts, print one of each) is in [`enclosure/`](enclosure/).

## Highlights

- **Radar follow.** A 60 GHz radar measures where you are (angle and distance).
  The pan servo turns the figure toward you, even in the dark and without a camera.
- **Two zones.** Far away: animated eyes that look at you. Close to the desk:
  clock and weather. Nobody around: the figure hides and the screen sleeps.
- **Eyes and figure move together.** The pupils on the screen follow the same
  angle as the servo, so the whole thing reads as one character.
- **Browser setup.** The ESP32 opens a `DeskBuddy-XXXX` hotspot. Scan the QR code
  on the screen, pick your Wi-Fi, type your city, done. No app to install.
- **No soldering.** Dupont wires for signals, lever wire connectors for 5 V and
  ground.

## Pick your level

| Level | You get | You need | Guide |
|---|---|---|---|
| **1. Desk pet** | Radar follow, eyes, rise/hide, local time and weather | The parts below, 30–60 minutes | [docs/level-1-desk-pet.md](docs/level-1-desk-pet.md) |
| **2. Telegram** | Level 1 + messages from your phone on the screen, `/wave`, `/peek` | A free Telegram bot (5 minutes) | [docs/level-2-telegram.md](docs/level-2-telegram.md) |
| 3. Own server | Pairing two buddies, photos, presence sharing | A small server you run yourself | Coming later |

Levels 1 and 2 need **no server and no account with us**. The ESP32 talks
directly to Open-Meteo (weather) and Telegram.

## Parts

| Part | Notes | Buy |
|---|---|---|
| 60 GHz AiP radar development kit | Choose the **Presence, wall mount** firmware (3D presence with target positions over UART). Comes with a lead to female Dupont plugs | [Kymopsis](https://kymopsis.com/product/60ghz-aip-gesture-radar-development-kit/?attribute_pre-flashed-firmware=Presence%2C+wall+mount&utm_source=github&utm_medium=readme&utm_campaign=radar-desk-buddy) |
| ESP32-S3 board, N16R8, dual USB-C | 16 MB flash + 8 MB PSRAM is required | [Kymopsis](https://kymopsis.com/product/esp32-s3-n16r8-development-board/?utm_source=github&utm_medium=readme&utm_campaign=radar-desk-buddy) |
| 1.28" round TFT, GC9A01, 240×240 | SPI | [Kymopsis](https://kymopsis.com/product/1-28-inch-round-tft-display-gc9a01-240x240/?utm_source=github&utm_medium=readme&utm_campaign=radar-desk-buddy) |
| 2 × micro servo (SG90 or MG90S) | Pan + lift | [SG90](https://kymopsis.com/product/sg90-micro-servo-9g/?utm_source=github&utm_medium=readme&utm_campaign=radar-desk-buddy) · [MG90S](https://kymopsis.com/product/mg90s-metal-gear-micro-servo-9g/?utm_source=github&utm_medium=readme&utm_campaign=radar-desk-buddy) |
| TTP223 touch sensor | "Pat the head" to open a message | [Kymopsis](https://kymopsis.com/product/ttp223-capacitive-touch-sensor-module/?utm_source=github&utm_medium=readme&utm_campaign=radar-desk-buddy) |
| AMS1117-3.3 V module | Clean 3.3 V rail for the radar | [Kymopsis](https://kymopsis.com/product/ams1117-3-3v-ldo-power-module/?utm_source=github&utm_medium=readme&utm_campaign=radar-desk-buddy) |
| Dupont jumper wires | M-F and F-F | [Kymopsis](https://kymopsis.com/product/dupont-jumper-wire-kit-120pcs/?utm_source=github&utm_medium=readme&utm_campaign=radar-desk-buddy) |
| Lever wire connectors | 5 V and GND bus | [Kymopsis](https://kymopsis.com/product/lever-wire-connector-assortment-2-3-5-port/?utm_source=github&utm_medium=readme&utm_campaign=radar-desk-buddy) |
| USB-C PD trigger board set to 5 V, PD charger ≥ 20 W | Power input | Any |
| Short USB-C lead (male) | 5 V into the ESP32 board's USB-C | Any |
| 470–1000 µF capacitor (≥ 10 V) | Across the servo supply | Any |

A ready-to-build bundle with these parts is planned on
[kymopsis.com](https://kymopsis.com/shop/?utm_source=github&utm_medium=readme&utm_campaign=radar-desk-buddy).

## Quick start

1. Wire it up: [docs/wiring.md](docs/wiring.md).
2. Flash the firmware: [docs/flashing.md](docs/flashing.md).
3. Power on. The screen shows a QR code. Scan it to join `DeskBuddy-XXXX`
   (the password is on the screen and is different on every device).
4. Your phone opens the setup page (or go to `http://mod.config`). Choose a
   language, join your home Wi-Fi, type your city.
5. Walk around the room. The figure follows you; come close to see the time
   and weather.

## How it works

```
60 GHz radar ──UART──▶ ESP32-S3 ──SPI──▶ round screen (eyes / clock / message)
                          │  ├─PWM──▶ pan servo  (look at you)
                          │  └─PWM──▶ lift servo (rise / hide)
                          ├─ Wi-Fi ─▶ NTP, Open-Meteo (time, weather)
                          └─ Wi-Fi ─▶ Telegram Bot API (messages, optional)
```

The radar reports targets as x/y coordinates. The firmware converts the
nearest one to an angle and a distance, smooths it, drives the pan servo and
the on-screen pupils with the same angle, and picks the face from the distance
band (near / far / nobody). See [docs/how-it-works.md](docs/how-it-works.md).

## Repository layout

```
firmware/    ESP-IDF project (C)
docs/        Wiring, flashing, level guides, FAQ
enclosure/   3D-printable case and mechanism (8 STL files)
```

## Help and questions

- Check the [FAQ](docs/faq.md).
- Open an [issue](../../issues/new/choose) with your board, radar kit, and
  serial log.
- For volume or custom radar work: [kymopsis.com/b2b](https://kymopsis.com/b2b/?utm_source=github&utm_medium=readme&utm_campaign=radar-desk-buddy).

## License

Code: [MIT](LICENSE). Documentation and enclosure files: [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
Screen fonts are generated from Noto Sans CJK and Fredoka (SIL Open Font License 1.1).

The radar module ships with its own pre-built firmware; this repository only
contains the ESP32 side and the documented UART protocol it reads.
