# Usage

[← Back to README](../README.md)

## Controls

| Control | Action |
|---|---|
| Play/Pause button | Play / pause (replays the current track when stopped) |
| Next / Prev buttons | Next / previous track |
| Encoder rotation | Volume, 5 % per detent |
| Encoder click | Mute / restore the previous volume |

The display shows the track list, elapsed / total time and the volume. When the SD card is removed it
shows a warning screen; after the card returns it is re-scanned and the current track restarts from
the beginning.

## BLE control

The device advertises as `MP3 Player` with one custom service
(`5f1c2a40-8b3e-4d7a-9c61-2e4f0a7b3d15`) and one write characteristic
(`5f1c2a41-8b3e-4d7a-9c61-2e4f0a7b3d15`, WRITE / WRITE_NO_RSP, no pairing). Write a single ASCII byte
with any generic BLE app (nRF Connect, LightBlue):

| Byte | Command |
|---|---|
| `P` | Play / pause |
| `N` / `B` | Next / previous track |
| `+` / `-` | Volume up / down |
| `M` | Toggle mute |

## Configuration (menuconfig)

`pio run -t menuconfig` -> *MP3 player: audio debug* (defined in [`src/Kconfig.projbuild`](../src/Kconfig.projbuild)):

| Option | Default | Effect |
|---|---|---|
| `AUDIO_DEBUG_MODE` | off | Always play one fixed file, ignore the track list |
| `AUDIO_DEBUG_FILENAME` | `demo_audio.wav` | File used in debug mode (e.g. `demo_audio.mp3`) |
| `AUDIO_DEBUG_MP3_INFO_LOG` | off | Log bitrate, sample rate and duration of every MP3 |
| `AUDIO_DEBUG_SPI_MASTER_LOG_*` | INFO | `spi_master` log level |

In debug mode the header shows a `D` marker and the highlighted row shows the played file name.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| No sound, log looks clean, I2S runs | PCM5102 control pins floating (SCK, FLT, DEMP, FMT, XSMT) - see [hardware](hardware.md#wiring-notes) |
| Distorted sound on loud tracks | Low-impedance earbuds on a line-level output; use an amplifier or lower the volume |
| "Insert SD card" with a card inserted | Card is not FAT32, loose wiring, or the card was not detected; check the log for `sd_card` errors |
| SD errors such as `data CRC failed` | Wiring too long or loose; keep SPI wires short |
| Track missing from the list | Not in the root directory, not `.mp3` / `.wav`, hidden `._*` file, or more than 64 files |
| Title shows `?` | Only the Latin font is supported |
| Blank display | Check SDA/SCL (17 / 18) and the I2C pull-ups; the player works without a display, the log shows `display_task_init failed` |
| Upload fails or no serial log | Use the board's USB port (built-in USB-Serial-JTAG), 115200 baud |
