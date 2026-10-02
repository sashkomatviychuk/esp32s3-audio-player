#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  uint32_t duration_sec;  // estimated length, 0 if unknown
  uint32_t bitrate_bps;   // bitrate of the first frame, 0 if unknown
  uint32_t audio_start;   // offset of the first audio byte (after an ID3v2 tag)
} mp3_info_t;

/**
 * @brief Reads the first MPEG audio frame header of an MP3 file and derives
 *        its bitrate and length.
 *
 * Skips an ID3v2 tag, finds the first Layer III frame and computes
 * duration = audio bytes * 8 / bitrate. Exact for CBR files, an estimate for
 * VBR. The elapsed time of a stream can be derived the same way:
 * (bytes sent - audio_start) * 8 / bitrate_bps. The file position is
 * restored to the start (rewind) before returning.
 *
 * Locking: reads from the SD card but takes no mutex — the caller must hold
 * spi_mutex (or be the only SD user at that moment).
 *
 * @param f        Open file, positioned anywhere
 * @param fileSize Total file size in bytes
 * @param[out] out Result; all fields are 0 if the header could not be parsed
 */
void mp3_get_info(FILE* f, size_t fileSize, mp3_info_t* out);
