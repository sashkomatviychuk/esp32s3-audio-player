# Development

[← Back to README](../README.md)

## Toolchain

| Tool | Version used |
|---|---|
| PlatformIO Core | 6.2.0 |
| Platform `espressif32` | 7.0.1 |
| ESP-IDF | 6.0.1 (see [`dependencies.lock`](../dependencies.lock)) |
| `chmorgan/esp-libhelix-mp3` | 1.0.3 |
| `k0i05/esp_ssd1306` | see [`dependencies.lock`](../dependencies.lock) |

Components are fetched by the ESP-IDF component manager on the first build
([`src/idf_component.yml`](../src/idf_component.yml)); `dependencies.lock` pins their versions.

## Build, flash, monitor

```bash
pio run                  # build
pio run -t upload        # build and flash
pio device monitor       # serial log, 115200 baud
pio run -t menuconfig    # debug options, see usage.md
```

The console runs over the built-in USB-Serial-JTAG, so use the board's USB port.

## Host tests

The playback state machine has no RTOS or I/O calls, so its transition table is checked on a PC:

```bash
cc -std=c11 -Wall -Wextra -Iinclude tools/host_tests/test_player_fsm.c src/player_fsm.c -o test_player_fsm
./test_player_fsm
```

## Code style

Google-based, 2 spaces, 100 columns: [`.clang-format`](../.clang-format), [`.clang-tidy`](../.clang-tidy).
Public headers in [`include/`](../include) document each module's contract, including locking behavior.

## References

- [ESP32-S3 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf) and
  [technical reference manual](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)
- [ESP-IDF programming guide (ESP32-S3)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/) and its
  [NimBLE API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/bluetooth/nimble/index.html)
- [PCM5102A datasheet](https://www.ti.com/lit/ds/symlink/pcm5102a.pdf)
- [SSD1306 datasheet](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf)
- Components: [esp-libhelix-mp3](https://components.espressif.com/components/chmorgan/esp-libhelix-mp3),
  [esp_ssd1306](https://components.espressif.com/components/k0i05/esp_ssd1306)
