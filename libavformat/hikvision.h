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

#include <stdbool.h>

#include "avformat.h"
#include "avio_internal.h"

#define HIKVISION_ORIGINAL_MAGIC MKTAG('4', 'H', 'K', 'H')
#define HIKVISION_HIKCONVERT_MAGIC MKTAG('I', 'M', 'K', 'H')

typedef struct HikvisionResolution {
    int padding;
    int width;
    int height;
} HikvisionResolution;

typedef struct HikvisionMediaInfo {
    short field_8;
    short field_10;
    short field_12;
    unsigned char field_14;
    unsigned char field_15;
    int field_16;
    int field_20;
} HikvisionMediaInfo;

typedef struct HikvisionHeader {
    int magic;
    int version;
    int field_8;
    short field_18;
    short field_20;
    short field_22;
    int field_24;
    int field_28;
    int field_32;
    HikvisionMediaInfo media_info;
    HikvisionResolution resolution;
} HikvisionHeader;

typedef struct HikvisionGroupHeader
{
    int field_4;
    int field_8;
    int field_12;
    int field_16;
    int field_20;
    int field_24;
    int field_28;
    int field_44;

} HikvisionGroupHeader;

typedef struct HikvisionBlockHeader
{
    int field_4;
    int field_8;
    int field_12;
    int field_16;
    int field_20;
    int field_24;
    int field_28;
} HikvisionBlockHeader;

typedef struct HikvisionContext {
    HikvisionHeader header;
    HikvisionGroupHeader group_header;
    HikvisionBlockHeader block_header;
} HikvisionContext; 


int hikvision_parse_file_header(AVFormatContext *ctx);
int hikvision_parse_mediainfo(AVFormatContext *ctx);
int hikvision_get_resolution(AVFormatContext *ctx, int width, int height);
unsigned int hikvision_parse_group(AVFormatContext *ctx, int group_id);
int hikvision_parse_group_header(AVFormatContext *ctx);
int hikvision_parse_block_header(AVFormatContext *ctx);
int hikvision_search_start_code(AVFormatContext *ctx, int group_id);
int hikvision_get_stream(AVFormatContext *ctx);
int hikvision_update_payload_info(AVFormatContext *ctx);

#endif
