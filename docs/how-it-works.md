# How it works

## Radar to angle

The 60 GHz radar sends frames over UART (115200 baud by default; the firmware
also tries 921600, 460800 and 1382400 if it hears nothing). Frames use the
TinyFrame envelope:

```
SOF 0x01 | ID u16 BE | LEN u16 BE | TYPE u16 BE | header checksum | payload | payload checksum
```

Type `0x0A04` carries the target list. Each target has x (left), y (forward)
and, on 3D firmware, z (up) in metres. The firmware picks the nearest target,
keeps the same track id while it is visible, and computes:

- azimuth = atan2(x, y), 0° = straight ahead, positive = left
- distance = √(x² + y²)

The decoder lives in `firmware/components/radar_link/radar_protocol.c` and has a
host test in `radar_link/test/`.

## Motion

`motion` turns the azimuth into a pan angle with a slew limit (default 90°/s)
and a soft limit (default ±80°). The lift servo raises the figure after a short
confirmation (default 300 ms) and hides it after nobody has been seen for a
while (default 10 s). All of these are on the setup page.

## Faces

| Zone | Default distance | Face |
|---|---|---|
| Near | ≤ 0.5 m (leave at 0.6 m) | Clock + weather, or "new message" |
| Far | up to 5 m | Eyes that follow the same angle as the pan servo |
| Nobody | — | Sleep, backlight off after about 12 s |

## Time and weather

After Wi-Fi connects, the clock syncs over NTP. The city you typed is turned
into coordinates with the Open-Meteo geocoding API once, then current weather
is fetched every 15 minutes from the Open-Meteo forecast API. That response also
carries the local UTC offset, which sets the clock's time zone.

## Telegram

The ESP32 long-polls `getUpdates` on the Telegram Bot API over HTTPS with
certificate checking. It replies with `sendMessage`. No webhook and no server.
