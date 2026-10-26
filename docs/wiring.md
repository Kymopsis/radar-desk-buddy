# Wiring

No soldering needed if your radar kit, round screen and AMS1117 module come with
pin headers. Signals use Dupont jumper wires; 5 V and GND use lever wire
connectors as a small power bus.

## Power

```
USB-C PD charger (≥ 20 W)
 └─ PD trigger board, set to 5 V
     ├─ lever connector "5V bus" ──┬─ USB-C male lead ─▶ ESP32-S3 board USB-C port
     │                             ├─ servo 1 red, servo 2 red (+ 470–1000 µF cap to GND)
     │                             └─ AMS1117 VIN ─▶ AMS1117 3.3 V out ─▶ radar 3V3
     └─ lever connector "GND bus" ── every GND (ESP32, servos, AMS1117, radar, screen, touch)
```

Rules that matter:

- **Feed the ESP32 board through its USB-C port**, not through the 5V pin on the
  header. On many boards the header pin bypasses the input path and the screen
  flickers or Wi-Fi drops.
- **Set the PD trigger to 5 V before connecting anything.** 9 V or 12 V will
  destroy the servos and the board.
- **Give the radar its own 3.3 V** from the AMS1117 module. Do not power it from
  the ESP32's 3V3 pin. Clean power improves angle accuracy.
- Run a separate GND wire from each part back to the GND bus (star ground). Servo
  current spikes should not flow through the radar's ground wire.

## Signals

| ESP32-S3 GPIO | Connects to | Notes |
|---|---|---|
| 9 | Screen DC | |
| 10 | Screen CS | |
| 11 | Screen SDA (MOSI) | |
| 12 | Screen SCL (SCLK) | |
| 13 | Screen RES (RST) | |
| 14 | Screen BLK | Backlight, active high |
| 3V3 / GND | Screen VCC / GND | |
| 5 | Pan servo signal (orange/yellow) | Turns the figure left/right |
| 7 | Lift servo signal (orange/yellow) | Raises/hides the figure |
| 8 | TTP223 SIG | Touch pad on the figure's head |
| 3V3 / GND | TTP223 VCC / GND | |
| 17 (TX) | Radar **RX** | UART, the ESP32 talks to the radar |
| 18 (RX) | Radar **TX** | UART, radar data to the ESP32 |
| 4 | Radar **RST** | Optional, active low |
| 16 | Radar **TX2 / OUT** | Optional |

Do not use GPIO 35, 36, 37 on N16R8 boards: they belong to the octal PSRAM.

Check the pin labels printed on your radar kit. If your kit has two 3V3 pins
and two GND pins, connect both of each.

## Mounting

- The radar looks forward from its antenna side. Plastic in front of it is fine;
  metal, foil or a phone right in front of it is not.
- Mount the radar on the base, not on the moving figure, so the measured angle
  does not move with the servo.
- If the figure turns away from you instead of toward you, use **Flip pan** on the
  setup page (section "Servo calibration").
