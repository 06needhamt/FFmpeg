/*
 * Dynacolor MVC common definitions
 * Copyright (c) 2020-2026 Tom Needham
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVFORMAT_DYNACOLOR_H
#define AVFORMAT_DYNACOLOR_H

#include <stdint.h>

/*
 * A Dynacolor MVC file is a sequence of records, each made of:
 *
 *  - a 16 byte basic index:
 *      0       '@'
 *      1       device type
 *      2       bits 0-3: reserved, bit 4: source (0: DVR, 1: IP device),
 *              bits 5-6: width base (0: 720, 1: 704, 2: 640)
 *      3       bits 0-1: channel bits 5-6, bits 2-3: stream type,
 *              bits 4-7: quality index
 *      4-7     record size (PES header + payload), little-endian
 *      8-11    UNIX time in seconds, little-endian
 *      12      bit 0: 0: NTSC, 1: PAL, bit 1: 0: live, 1: playback,
 *              bit 2: audio, bit 3: DST, bit 4: covert, bit 5: video loss,
 *              bit 6: alarm in, bit 7: motion
 *      13      bit 0: extra index present, bit 1: event, bits 2-7: PPS index
 *      14      bits 0-2: picture type (1: I, 2: P, 3: B),
 *              bits 3-7: channel bits 0-4
 *      15      XOR of bytes 0-14
 *  - if bit 0 of byte 13 is set, a 16 byte extra index:
 *      0       DST minutes
 *      1       bit 0: summer/winter time, bit 1: over speed,
 *              bits 2-6: skipped picture count, bit 7: exception
 *      2-3     size of the following "super extra" data, little-endian
 *      4-7     serial number, little-endian
 *      8-13    alarm in, video loss and motion status bitmaps
 *      14      bits 0-1: DVR recording, bits 2-7: time zone
 *      15      XOR of bytes 0-14
 *    followed by the super extra data (camera title, GPS, text, ...).
 *  - a 32 byte MPEG PES style header:
 *      0-2     start code 0x000001
 *      3       bits 4-7: format, bits 0-3: channel bits 0-3
 *      4-23    unused by the format
 *      24-26   record size, with marker bits:
 *              byte 24: 1 << 7 | size bits 15-21
 *              byte 25: size bits 11-14 << 4 | 1 << 3 | size bits 8-10
 *              byte 26: size bits 0-7
 *      27      picture type
 *      28-31   field information
 *  - the payload, an elementary stream frame.
 */

#define DYNACOLOR_BASIC_IDX_SIZE  16
#define DYNACOLOR_EXTRA_IDX_SIZE  16
#define DYNACOLOR_PES_HEADER_SIZE 32

#define DYNACOLOR_MAX_RECORD_SIZE 0x3FFFFF /* 22 bit size field in the PES header */
#define DYNACOLOR_MAX_CHANNELS    128      /* 7 bit channel number */

#define DYNACOLOR_FORMAT_AUDIO 0xC
#define DYNACOLOR_FORMAT_JPEG  0xD
#define DYNACOLOR_FORMAT_MPEG4 0xE
#define DYNACOLOR_FORMAT_H264  0xF

#define DYNACOLOR_PICTURE_I 1
#define DYNACOLOR_PICTURE_P 2
#define DYNACOLOR_PICTURE_B 3

static inline uint8_t ff_dynacolor_checksum(const uint8_t *buf, int size)
{
    uint8_t sum = 0;
    for (int i = 0; i < size; i++)
        sum ^= buf[i];
    return sum;
}

#endif /* AVFORMAT_DYNACOLOR_H */
