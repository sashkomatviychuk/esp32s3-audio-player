# Audio pipeline: from file to sound

[← Back to README](../README.md)

How a file on the SD card becomes sound: reading, decoding MP3 / WAV, volume, and sending the samples
to the PCM5102 DAC over I2S. Written in plain words; where a fact comes from a format specification
rather than from this project, the source is named (see [Sources](#sources)).

## The big picture

```
 SD card file ──► decoder ──► PCM samples ──► volume ──► I2S (DMA) ──► PCM5102 ──► analog out
 (.mp3 / .wav)   (wav / mp3)  (int16, L/R)   (gain)     (3 wires)       (DAC)
```

Everything between the file and the I2S driver is done by one task, `audio_task`
([audio_task.c](../src/audio_task.c)), in a loop of **one block at a time**:

1. `decoder.read()` - read a piece of the file and turn it into samples
2. `pcm5102_write()` - apply volume and hand the samples to the I2S driver
3. repeat until the track ends

Two ideas make this simple:

- **PCM is the common language.** Whatever the file is, the decoder outputs the same thing: a stream of
  16-bit numbers, left, right, left, right... The DAC and the rest of the player never know whether
  the source was MP3 or WAV.
- **The DAC sets the pace.** The I2S hardware consumes samples at a fixed speed (e.g. 44 100 per
  second). `pcm5102_write()` blocks when its buffer is full, so the loop automatically runs exactly as
  fast as the sound is played. There is no timer for pacing.

## What is PCM?

Sound is air pressure changing over time. Digital audio measures that pressure many times per second:

- **Sample rate** - how many measurements per second (44 100 Hz for CDs).
- **Bit depth** - how precisely each measurement is stored (here: 16 bits, a signed number from
  -32768 to 32767).
- **Channels** - 2 for stereo. The samples are *interleaved*: `L0 R0 L1 R1 L2 R2 ...`. One
  left+right pair is called a **frame**.

This raw list of numbers is called **PCM** (pulse-code modulation). One second of 44.1 kHz stereo
16-bit audio is 44 100 frames x 2 channels x 2 bytes = **176 400 bytes** (the "176 KB/s" in the README).
A DAC converts such numbers into a voltage.

## WAV: almost no decoding

*Source: the RIFF / WAVE format (Microsoft / IBM, 1991), widely documented, e.g. in the Microsoft
"Multimedia Programming Interface" docs and on soundfile.sapp.org.*

A WAV file is PCM with a small header in front, so "decoding" is only reading bytes. The file is a
list of **chunks**, each `4-byte name + 4-byte size + data`:

```
"RIFF" size "WAVE"
  "fmt "  size  → format tag, channels, sample rate, byte rate, bits per sample
  "data"  size  → the PCM samples
  (other chunks like "LIST" may appear between them and are skipped)
```

All numbers in the file are **little-endian** (low byte first), which is also how the ESP32 stores
them in memory. That is why the samples can be read straight into the output buffer.

What the code does ([wav_info.c](../src/wav_info.c), [wav_decoder.c](../src/wav_decoder.c)):

1. **Parse the header** (`wav_get_info`). Check `RIFF`/`WAVE`, walk the chunks (an odd-sized chunk is
   followed by one padding byte, per the RIFF spec), read `fmt ` and find `data`. Only plain 16-bit
   PCM, 1-2 channels, 8-48 kHz is accepted; anything else is refused with a log message.
2. **Duration** = `data size / (sample rate x channels x 2 bytes)`. Some encoders write a data size
   of 0 or `0xFFFFFFFF` when streaming; the code then uses "the rest of the file".
3. **Play**: every `read()` takes up to 2048 bytes from the file straight into the output buffer.
   A mono file is expanded to stereo by copying each sample to both channels
   (`audio_decoder_mono_to_stereo`).

Cost: no CPU, but a lot of data - 176 KB/s from the SD card.

## MP3: the real decoder

*Sources: ISO/IEC 11172-3 (MPEG-1 Audio) and ISO/IEC 13818-3 (MPEG-2 Audio) for the frame format;
ID3v2 specification (id3.org) for tags; Xing/LAME header description for exact length.*

MP3 is **lossy compression**: the encoder throws away sounds that the human ear barely notices and
packs the rest tightly. A 128 kbps MP3 is about 11 times smaller than the same sound as WAV. The price
is that playing it needs real computation.

### File structure

```
[ID3v2 tag: title, artist, cover...]  [frame][frame][frame] ... [frame]  [ID3v1 tag]
```

An MP3 is a sequence of independent-looking **frames**. Each frame holds **1152 samples** per channel
(MPEG-1; 576 for MPEG-2) - about 26 ms at 44.1 kHz - and starts with a 4-byte header:

| Header field | What it tells |
|---|---|
| sync word (11 bits of `1`) | "a frame starts here" |
| version, layer | MPEG-1/2/2.5, Layer III (the "3" in MP3) |
| bitrate index | how many kbit/s -> how many bytes the frame has |
| sample-rate index | 44.1 / 48 / 32 kHz (halved for MPEG-2) |
| channel mode | stereo / joint stereo / dual / mono |

A frame is about `144 x bitrate / sample rate` bytes: 128 kbps at 44.1 kHz gives ~417 bytes.

### Step 1: read the header ([mp3_info.c](../src/mp3_info.c))

Before playing, the code finds out what the file is:

1. **Skip the ID3v2 tag.** It starts with `ID3` and stores its size in 4 "syncsafe" bytes (7 useful
   bits each, so the tag can never be mistaken for a sync word). The audio starts right after it.
2. **Find the first valid frame header** and read sample rate, channels and bitrate from it. Layer I/II
   and broken headers are rejected.
3. **Duration.** If the first frame is a **Xing/Info** frame (written by LAME for VBR files), it holds
   the total frame count, so duration = `frames x samples per frame / sample rate` - exact. Otherwise
   it is estimated as `file size / bitrate`, which is only right for constant bitrate (CBR).

### Step 2: decode with Helix ([mp3_decoder.c](../src/mp3_decoder.c))

The actual maths (Huffman decoding, requantization, stereo processing, IMDCT, polyphase filterbank -
all defined in ISO 11172-3) is done by the **Helix** fixed-point decoder, in the component
`chmorgan/esp-libhelix-mp3`. "Fixed-point" means it uses integers only, so it runs fast on the
ESP32-S3 without a floating-point unit. This project does not reimplement that - it **feeds Helix and
collects the result**:

```
SD card ─(2 KB+ reads)─► input buffer (4096 B) ─► find sync word ─► MP3Decode() ─► 1152 stereo frames
```

One `read()` call decodes **one frame**:

1. **Keep the buffer filled.** Helix needs a whole frame plus extra bytes before it, so the code keeps
   at least `MAINBUF_SIZE` bytes available and refills from the SD card in big chunks (one read per
   several frames, not one per frame). The unread tail is moved to the start of the buffer first.
2. **Find the sync word** (`MP3FindSyncWord`) - the start of the next frame.
3. **Decode** (`MP3Decode`) - Helix returns up to 1152 stereo samples and moves the input pointer past
   the frame.
4. **Handle the result:**

| Result | Meaning | Action |
|---|---|---|
| OK | frame decoded | return the samples (mono is duplicated to stereo) |
| `MAINDATA_UNDERFLOW` | normal for the first frame(s) | skip the silent output, continue |
| `INDATA_UNDERFLOW` | buffer ends inside the frame | refill and retry; at the end of the file - stop |
| other error | corrupt data | skip a byte or more, look for the next sync word |

If 100 frames in a row fail, the file is given up as corrupt.

### Why the first frame can be silent: the bit reservoir

MP3 frames do not have to fit their own bytes: the encoder may store some of a frame's data in the
**unused space of earlier frames** (the "bit reservoir", from the Layer III design in ISO 11172-3). So
frame N may need bytes from frame N-1. When playback starts at the very beginning of a file, the first
frame refers to data that does not exist, and Helix reports `MAINDATA_UNDERFLOW`. It is expected and
harmless; the code counts those frames and moves on.

### How fast is it?

Decoding one frame takes about 7 ms on the ESP32-S3 while the frame plays for 24-26 ms, so the CPU is
busy about a third of the time. At the end of a track the decoder logs average / maximum decode time
and the frame duration (`mp3_decoder: Decoded N frames ...`), so this can be checked on real hardware.

## Common output: audio_decoder

Both decoders hide behind one small interface ([audio_decoder.h](../include/audio_decoder.h)):

```c
audio_decoder_open(path, ...)   // pick WAV or MP3 by file extension, parse the header
decoder.read(out, ...)          // fill `out` with interleaved stereo int16, return the frame count
audio_decoder_close(...)
```

`read()` returns 0 frames with `ESP_OK` at the end of the track. The buffer is always
`1152 x 2` samples, the size of the biggest MP3 frame; a WAV block simply fills less of it. This is
why a new format only needs one new `*_decoder.c` file.

Every SD card access inside a decoder takes `spi_mutex`, because the SD monitor task also uses the
card.

## Volume and mute ([pcm5102.c](../src/pcm5102.c))

The DAC chip has no volume control, so volume is applied in software **before** the samples are sent:

```c
sample = (sample * gain) >> 15;      // gain is a "Q15" number: 32768 means 1.0
```

- The gain comes from the 0-100 volume through a **quadratic curve**: volume 100 -> 1.0, volume 50 ->
  0.25. Human hearing is roughly logarithmic, so this feels more even than a straight line.
- **Mute** is gain 0.
- At full volume the loop is skipped, so the samples pass through unchanged.

## I2S: how samples reach the DAC

*Sources: Philips "I2S bus specification" (1986, revised 1996) for the signals; TI PCM5102A datasheet
for how the chip uses them; ESP-IDF `i2s_std` driver documentation for the ESP32 side.*

I2S (Inter-IC Sound) is a 3-wire serial link made for audio:

| Wire | Pin on the DAC | Job |
|---|---|---|
| **BCK** (bit clock, GPIO 6) | BCK | one pulse per bit |
| **LRCK / WS** (word select, GPIO 4) | LRCK | `0` = left sample, `1` = right sample; one period = one frame |
| **DIN / DOUT** (data, GPIO 5) | DIN | the bits, most significant first |

For 44.1 kHz stereo 16-bit:

- LRCK = **44 100 Hz** (one frame per period)
- BCK = 44 100 x 2 channels x 16 bits = **1.4112 MHz**

The ESP32-S3 is the **master**: it generates both clocks, and the DAC is a listener. The PCM5102 has
no separate master-clock input here (`SCK` is tied to GND), so it generates its internal clock from
BCK with a built-in PLL - this is a feature described in its datasheet, and the reason for the wiring
notes in [hardware.md](hardware.md#wiring-notes).

### DMA: why the CPU does not push every bit

The CPU does not toggle the pins. The I2S peripheral reads the samples from memory by **DMA** (direct
memory access) and clocks them out by itself:

- The driver is configured with **8 buffers x 480 frames = 3840 frames**, about **87 ms** of audio.
- `i2s_channel_write()` copies the samples into these buffers and waits if they are full - this is the
  "DAC sets the pace" behaviour described above.
- The 87 ms is a safety cushion: while the task reads the SD card (which is slow) and decodes the next
  frame, the DMA still has audio to play, so there are no gaps.
- If nothing is written, `auto_clear` sends silence instead of repeating the last buffer (no buzzing).

### Different sample rates

The I2S clock is set to the file's sample rate when a track starts (`pcm5102_set_sample_rate`). The
channel is stopped, the clock is reconfigured and the channel is started again. This also discards the
previous track's audio, which is still in the DMA buffers. If an MP3 changes its rate in the middle of
the stream (rare), the same function is called again.

## Time and progress

The elapsed time is **not** read from a clock. After each block the task adds
`frames x 1 000 000 / sample rate` microseconds: the length of the audio that was just sent. Since the
DAC consumes samples at exactly that rate, this tracks real playback (to within the DMA buffer, ~87 ms).
Every 500 ms the value is published to `player_state`, from where the display reads it.

## Where each step lives

| Step | Code |
|---|---|
| Playback loop, volume/mute commands, progress | [audio_task.c](../src/audio_task.c) |
| Choosing the decoder, common interface, mono -> stereo | [audio_decoder.c](../src/audio_decoder.c) |
| WAV header, WAV reading | [wav_info.c](../src/wav_info.c), [wav_decoder.c](../src/wav_decoder.c) |
| MP3 header, ID3v2, Xing duration | [mp3_info.c](../src/mp3_info.c) |
| MP3 frame decoding (Helix wrapper) | [mp3_decoder.c](../src/mp3_decoder.c) |
| Volume, I2S, DMA | [pcm5102.c](../src/pcm5102.c) |

## Sources

- **WAV**: RIFF / WAVE file format (Microsoft and IBM). `WAVEFORMATEX` / `WAVEFORMATEXTENSIBLE`
  structures are described in Microsoft's documentation.
- **MP3**: ISO/IEC 11172-3 (MPEG-1 Audio, Layer III) and ISO/IEC 13818-3 (MPEG-2 Audio). The standards
  are paid documents; free explanations of the frame header are widely available (e.g. the
  "MPEG Audio Frame Header" pages).
- **ID3v2**: id3.org, "ID3 tag version 2.4.0 - Main Structure" (syncsafe integers).
- **Xing / Info header**: not part of ISO; a de-facto convention of the Xing encoder, continued by LAME.
- **Helix MP3 decoder**: RealNetworks / Helix community fixed-point decoder; here the ESP-IDF port
  [`chmorgan/esp-libhelix-mp3`](https://components.espressif.com/components/chmorgan/esp-libhelix-mp3).
- **I2S**: "I2S bus specification", Philips Semiconductors (1986, revised June 5, 1996).
- **PCM5102A**: [Texas Instruments datasheet](https://www.ti.com/lit/ds/symlink/pcm5102a.pdf).
- **ESP32 side**: ESP-IDF "I2S" API reference (`driver/i2s_std.h`).

Parts that come from this project's own code (buffer sizes, error handling, volume curve, timing
numbers) are not part of any format specification.
