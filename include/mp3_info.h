#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  uint32_t duration_sec;  // estimated length, 0 if unknown
  uint32_t bitrate_bps;   // bitrate of the first frame, 0 if unknown
  uint32_t audio_start;   // offset of the first audio byte (after an ID3v2 tag)
  uint32_t sample_rate;   // Hz, from the first frame header, 0 if no frame was found
  uint16_t channels;      // 1 (mono) or 2 (any stereo mode), 0 if no frame was found
} mp3_info_t;

/**
 * @brief Reads the first MPEG audio frame header of an MP3 file and derives
 *        its bitrate and length.
 *
 * Skips an ID3v2 tag, finds the first Layer III frame and computes
 * duration = audio bytes * 8 / bitrate. Exact for CBR files and for VBR files
 * with a Xing/Info tag, an estimate for VBR without one. Also reports the
 * sample rate and channel count of that frame, which the decoder needs before
 * the first frame is decoded. The file position is restored to the start
 * (fseek) before returning — the caller seeks to audio_start itself.
 *
 * Locking: reads from the SD card but takes no mutex — the caller must hold
 * spi_mutex (or be the only SD user at that moment).
 *
 * @param f        Open file, positioned anywhere
 * @param file_size Total file size in bytes
 * @param[out] out Result; all fields are 0 if the header could not be parsed
 *                 (sample_rate/channels are 0 only if no frame was found)
 */
void mp3_get_info(FILE* f, size_t file_size, mp3_info_t* out);
