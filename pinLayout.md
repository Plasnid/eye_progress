# Pin Connections — Adafruit QT Py ESP32-S3 to GC9A01A Round TFT

Wiring for the cat-eye animation sketch on an Adafruit QT Py ESP32-S3, using
hardware SPI for the display.

| QT Py ESP32-S3 pin | GPIO | GC9A01A module pin | Notes |
|---|---|---|---|
| 3V | — | VCC | 3.3V power |
| GND | — | GND | Ground |
| SCK | GPIO36 | SCK (SCL) | Hardware SPI clock — default pin, not set in code |
| MOSI | GPIO35 | SDA (MOSI) | Hardware SPI data — default pin, not set in code |
| A0 | GPIO18 | CS | `TFT_CS` in the sketch |
| A1 | GPIO17 | DC (RS) | `TFT_DC` in the sketch |
| A2 | GPIO9 | RES (RST) | `TFT_RST` in the sketch |
| 3V | — | BLK (backlight) | Tie directly to 3V for an always-on backlight |

Notes:

- MISO (GPIO37) is not used — the GC9A01A is a write-only display and has no data-out line.
- A3 (GPIO8), RX (GPIO16), TX (GPIO5), and the STEMMA QT SDA/SCL pins are left free for other peripherals.
- If you want backlight dimming instead of always-on, wire BLK to a free GPIO (e.g. A3) and drive it with PWM instead of tying it to 3V; the current sketch doesn't include backlight control code.