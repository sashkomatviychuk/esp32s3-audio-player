# Results and lessons learned

[← Back to README](../README.md)

## Results

- Plays 44.1 kHz stereo WAV (176 KB/s from the SD card at 4 MHz) and MP3 stably, without SD CRC errors.
- MP3 decoding takes about 7 ms per 24 ms frame on the ESP32-S3, leaving CPU and stack headroom.
- Controls respond through three independent sources (buttons, encoder, BLE) into one command queue.

## What I learned

Most of the hard problems were hardware, not code:

- **Do not disable the check that annoys you.** Turning off SD data CRC hid real corruption; the errors
  were a speed and wiring problem. CRC stays on and the SD bus runs at a stable 4 MHz.
- **Exclude hardware causes first.** Separate SPI buses, pull-ups and shorter wires fixed more than any timing tweak.
- **Clean logs and no sound means read the datasheet.** The PCM5102 was silent while I2S clocked and
  data flowed, because its control pins were floating.
- **Split the source from the signal path.** "Distorted sound" came from the DAC's line-level output
  driving low-impedance earbuds, not from the decoder. Decode time, stack margin and a volume sweep proved it.
- **Replaced the VS1053.** The hardware MP3 decoder never produced sound (SCI worked, SDI did not), so
  the project moved to software decoding plus a PCM5102. The VS1053 driver is still in the git history.
- **Don't derive state from side effects.** An SD monitor that treated "mounted" as "published" showed
  "Insert SD card" with a card inserted; explicit flags fixed it.

## Known limitations

- MP3 is Layer III only, with no seeking; the duration of VBR files without a Xing header is an estimate
- WAV is 16-bit PCM only (1-2 channels, 8-48 kHz)
- Track list: SD root only, 64 files, Latin font (other characters render as `?`)
- No hardware card-detect: removal is noticed within about 2 s, and playback restarts from the beginning
  of the track after the card returns
- On breadboard wiring a loose contact can still drop the SD card

## Roadmap

- [ ] Seeking, FLAC / AAC
- [ ] Subfolders and a scrollable track list
- [ ] Custom PCB and enclosure with USB-C power
- [ ] Headphone amplifier stage
