# Hardware

[← Back to README](../README.md)

## Parts

| Part | Role |
|---|---|
| ESP32-S3-DevKitM-1 | MCU |
| PCM5102 I2S DAC module | Audio output (line level) |
| microSD SPI module | Track storage |
| SSD1306 128x64 OLED (I2C) | UI |
| EC11 rotary encoder with push switch | Volume / mute |
| 3 push buttons | Play/Pause, Next, Prev |

## Pin map

| Signal | GPIO |
|---|---|
| Button Play/Pause | 2 |
| Button Next | 1 |
| Button Prev | 42 |
| Encoder A / B | 21 / 47 |
| Encoder switch (mute) | 8 |
| SD SPI2 MISO / SCLK / MOSI / CS | 11 / 12 / 13 / 14 |
| PCM5102 BCK / DIN / LRCK | 6 / 5 / 4 |
| Display SDA / SCL (I2C0) | 17 / 18 |

## Schematic

![Schematic](images/schematic.png)

KiCad schematic, 5 V from USB-C regulated to 3.3 V. Vector version: [schematic.pdf](schematic.pdf).

## Wiring notes

- **PCM5102 control pins must not float.** Tie SCK to GND and set FLT = L, DEMP = L, FMT = L,
  XSMT = H (solder the bridges on the back of the module or wire the pins). Otherwise there is no
  sound or only noise, even though I2S runs correctly.
- Buttons and the encoder use the internal pull-ups and connect to GND. The display needs 4.7 kOhm
  I2C pull-ups if the module has none.
- The PCM5102 output is line level with no headphone amplifier. Use a headphone amp, active speakers
  or a line input; low-impedance earbuds distort on loud tracks.
- The SD card runs on its own SPI2 bus at 4 MHz. Keep the wires short: on a breadboard a loose
  contact can still drop the card.

## SD card

Format the card as **FAT32** and put `.mp3` / `.wav` files in the root directory. Up to 64 tracks are
listed; subfolders and hidden files such as macOS `._*` are ignored.
