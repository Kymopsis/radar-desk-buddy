# Level 1: desk pet (no server)

What you get: the figure follows you, animated eyes when you are across the room,
time and weather when you are at the desk, and it hides when nobody is around.

## 1. Wire and flash

Follow [wiring.md](wiring.md) and [flashing.md](flashing.md).

## 2. First boot

The screen shows a QR code with the hotspot name `DeskBuddy-XXXX` and its
password. The password is random and different on every device.

1. Scan the QR code with your phone camera to join the hotspot.
2. The setup page opens by itself. If it does not, open `http://mod.config`
   (or `http://192.168.4.1`).
3. Pick a language, color theme and screen orientation, then **Continue**.

## 3. Wi-Fi and city

1. **Wi-Fi**: choose your home network (2.4 GHz), enter the password, **Connect Wi-Fi**.
2. **City**: type the city name only, for example `Berlin`, `Austin` or `Shenzhen`,
   then **Save city**. The device looks it up and shows the place it found.
   The time zone comes from the same lookup, so daylight saving time is handled.

The hotspot stays on, so you can come back to the setup page any time. To see
its password again, pat the head while the clock is showing.

## 4. Try it

| You | Desk Buddy |
|---|---|
| Walk into the room (up to about 5 m) | The figure rises, eyes appear and follow you |
| Come within about 0.5 m | Clock and weather |
| Leave | After about 10 s the figure hides and the screen dims, then turns off |

## 5. Tune

On the setup page under **Distance** you can change the near/far distances,
how long it waits before hiding, how fast the figure turns and how far.
**Restore defaults** brings back the factory values.

## Troubleshooting

- **Red "Radar offline" screen**: no data from the radar. Check TX/RX are crossed
  (ESP32 TX → radar RX), the radar has 3.3 V, and GND is shared.
- **Clock shows no time**: the device is not on your home Wi-Fi yet, or the
  network blocks NTP.
- **City not found**: use the English name without the country.

More in the [FAQ](faq.md).
