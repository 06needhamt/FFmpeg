/*
 * Hikvision DVR demuxer
 * Copyright (c) 2020 Tom Needham
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

#ifndef AVFORMAT_HIKVISION_H
#define AVFORMAT_HIKVISION_H

#include "avformat.h"
#include "avio_internal.h"

#define HIKVISION_VIDEO_CODEC_H264 0x0100
#define HIKVISION_VIDEO_CODEC_MPEG4 0x0003

#define HIKVISION_AUDIO_CODEC_NONE 0x7110
#define HIKVISION_AUDIO_CODEC_G711U 0x7111

typedef struct HikvisionHeader {
    unsigned int magic;
    unsigned int reserved_0;
    unsigned short reserved_1;
    unsigned short video_codec_id;
    unsigned short audio_codec_id;
    unsigned short reserved_2;
    unsigned short some_audio_codec_related_value;
    unsigned char reserved_3[92];
} HikvisionHeader;

typedef struct HikvisionContext {
    HikvisionHeader header;
} HikvisionContext; 

#endif
