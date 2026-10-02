#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/**
 * @brief Estimates the length of an MP3 file from its first frame header.
 *
 * Skips an ID3v2 tag, finds the first MPEG audio frame and computes
 * duration = audio bytes * 8 / bitrate. Exact for CBR files, an estimate for
 * VBR. The file position is restored to the start (rewind) before returning.
 *
 * Locking: reads from the SD card but takes no mutex — the caller must hold
 * spi_mutex (or be the only SD user at that moment).
 *
 * @param f        Open file, positioned anywhere
 * @param fileSize Total file size in bytes
 *
 * @return Duration in seconds, 0 if it could not be determined
 */
uint32_t mp3_get_duration_sec(FILE* f, size_t fileSize);
