# Pin Connections — Adafruit QT Py ESP32-S3 to GC9D01 160x160 Round TFT

Wiring for the cat-eye animation sketch on an Adafruit QT Py ESP32-S3, using
hardware SPI (via the Arduino_GFX library's `Arduino_ESP32SPI` bus) for the
display. Pin assignments here deliberately avoid **A2**, since it's reserved
for a LiIon/LiPoly Charger BFF Add-On's battery-voltage-monitor divider.

| QT Py ESP32-S3 pin | GPIO | GC9D01 module pin | Notes |
|---|---|---|---|
| 3V | — | VCC | 3.3V power |
| GND | — | GND | Ground |
| SCK | GPIO36 | SCK (SCL) | Hardware SPI clock — board default, passed to `Arduino_ESP32SPI` as `SCK` |
| MOSI | GPIO35 | SDA (MOSI) | Hardware SPI data — board default, passed to `Arduino_ESP32SPI` as `MOSI` |
| MISO | GPIO37 | (not connected) | Not used by this write-only display; only present because the bus constructor takes it |
| A0 | GPIO18 | CS | `TFT_CS` in the sketch |
| A1 | GPIO17 | DC (RS) | `TFT_DC` in the sketch |
| A3 | GPIO8 | RES (RST) | `TFT_RST` in the sketch — moved here from A2 |
| 3V | — | BLK (backlight) | Tie directly to 3V for an always-on backlight |

**A2 (GPIO9) is intentionally left unconnected and unused by this sketch.**
With a LiIon/LiPoly Charger BFF Add-On stacked underneath the QT Py, A2 is
wired on that board to a voltage divider used to detect USB vs. battery
power - reading or driving A2 for anything else would interfere with that
monitoring circuit (or give bogus readings from it).

Notes:

- A0 (GPIO18) doubles as `TFT_CS`, and A2 is reserved for the Charger BFF as
  described above, so the sketch seeds `randomSeed()` from **TX (GPIO5)**
  instead - it's the nearest pin not already claimed by the display, the
  Charger BFF, or (previously) the random-seed read itself.
- RX (GPIO16) and the STEMMA QT SDA/SCL pins are still free for other
  peripherals.
- If you want backlight dimming instead of always-on, wire BLK to a free GPIO and drive it with PWM instead of tying it to 3V; the current sketch doesn't include backlight control code.
- The GC9D01 is natively 160x160, matching the sketch's `SCREEN_DIM`.
- If a Charger BFF is *not* being used, A2 is free again and could be
  reclaimed (e.g. swap it back in for TFT_RST or the random seed) - but
  there's no need to, since the current A0/A1/A3/TX assignment works either way.
