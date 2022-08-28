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

#include "avformat.h"
#include "internal.h"
#include "hikvision.h"

int hikvision_parse_file_header(AVFormatContext *ctx) {
    int ret = 0;
    HikvisionContext *priv = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    avio_seek(pb, 8, SEEK_SET);
    priv->header.field_8 = avio_rl32(pb);
    avio_seek(pb, 18, SEEK_SET);
    priv->header.field_18 = avio_rl16(pb);
    priv->header.field_20 = avio_rl16(pb);
    priv->header.field_22 = avio_rl16(pb);
    priv->header.field_24 = avio_rl32(pb);
    priv->header.field_28 = avio_rl32(pb);
    priv->header.field_32 = avio_rl32(pb);

    av_log(ctx, AV_LOG_INFO, "Version: %i\n", priv->header.version);
    av_log(ctx, AV_LOG_INFO, "Field_08: %08X\n", priv->header.field_8);
    av_log(ctx, AV_LOG_INFO, "Field_18: %04X\n", priv->header.field_18);
    av_log(ctx, AV_LOG_INFO, "Field_20: %04X\n", priv->header.field_20);
    av_log(ctx, AV_LOG_INFO, "Field_22: %04X\n", priv->header.field_22);
    av_log(ctx, AV_LOG_INFO, "Field_24: %08X\n", priv->header.field_24);
    av_log(ctx, AV_LOG_INFO, "Field_28: %08X\n", priv->header.field_28);
    av_log(ctx, AV_LOG_INFO, "Field_32: %08X\n", priv->header.field_32);

    ret = hikvision_get_resolution(ctx);

    return ret;
}

int hikvision_parse_mediainfo(AVFormatContext *ctx) {
    int ret = 0;
    HikvisionContext *priv = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    return ret;
}

int hikvision_get_resolution(AVFormatContext *ctx) {
    int ret = 0;
    HikvisionContext *priv = ctx->priv_data;
    unsigned short switcher_width = (unsigned short) (priv->header.field_28 & 0xFFFF);
    unsigned short switcher_height = (unsigned short) (priv->header.field_28 >> 16);

    switch (switcher_width)
    {
    case 0x1001:
        priv->header.resolution.width = 352;
        priv->header.resolution.height = 240;
        break;
    case 0x1002:
        priv->header.resolution.width = 176;
        priv->header.resolution.height = 128;
        break;
    case 0x1003:
        priv->header.resolution.width = 704;
        priv->header.resolution.height = 480;
        break;
    case 0x1004:
        priv->header.resolution.width = 704;
        priv->header.resolution.height = 240;
        break;
    case 0x1005:
        priv->header.resolution.width = 96;
        priv->header.resolution.height = 64;
        break;
    case 0x1006:
        priv->header.resolution.width = 320;
        priv->header.resolution.height = 240;
        break;
    case 0x1007:
        priv->header.resolution.width = 160;
        priv->header.resolution.height = 128;
        break;
    case 0x1008:
        priv->header.resolution.width = 528;
        priv->header.resolution.height = 320;
        break;
    default:
        priv->header.resolution.width = switcher_width;
        priv->header.resolution.height = switcher_height;
        break;
    }

    av_log(ctx, AV_LOG_INFO, "Switcher: %04X\n", switcher_width);
    av_log(ctx, AV_LOG_INFO, "Width: %i\n", priv->header.resolution.width);
    av_log(ctx, AV_LOG_INFO, "Height: %i\n", priv->header.resolution.height);
    
    return ret;
} 

static int hikvision_read_header(AVFormatContext *ctx)
{
    int ret = 0;
    HikvisionContext *priv = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    priv->header.magic = avio_rl32(pb);

    switch (priv->header.magic)
    {
    case HIKVISION_ORIGINAL_MAGIC:
        av_log(ctx, AV_LOG_INFO, "File is an original\n");
        priv->header.version = 1;
        break;
    case HIKVISION_HIKCONVERT_MAGIC:
        av_log(ctx, AV_LOG_INFO, "File is a converted file\n");
        priv->header.version = 3;
        hikvision_parse_mediainfo(ctx);
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "Found Unknown Magic %d\n", priv->header.magic);
        return AVERROR_INVALIDDATA;
    }

    hikvision_parse_file_header(ctx);

    return ret;
}

static int hikvision_read_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    int ret = 0;

    return ret;
}

static int hikvision_read_seek(AVFormatContext *ctx, int stream_index,
                               int64_t timestamp, int flags)
{
    return 0;
}

static int hikvision_probe(const AVProbeData *p)
{
    if (p->buf_size < 100)
        return 0;

    if(p->buf[0] == '4' && p->buf[1] == 'H' && p->buf[2] == 'K' && p->buf[3] == 'H')
        return AVPROBE_SCORE_MAX;

    if(p->buf[0] == 'I' && p->buf[1] == 'M' && p->buf[2] == 'K' && p->buf[3] == 'H')
        return AVPROBE_SCORE_MAX;
    
    return 0;
}

static int hikvision_close(AVFormatContext *ctx)
{
    return 0;
}

AVInputFormat ff_hikvision_demuxer = {
    .name           = "hikvision",
    .long_name      = NULL_IF_CONFIG_SMALL("Hikvision DVR Demuxer"),
    .priv_data_size = sizeof(HikvisionContext),
    .read_probe     = hikvision_probe,
    .read_header    = hikvision_read_header,
    .read_packet    = hikvision_read_packet,
    .read_close     = hikvision_close,
    .read_seek      = hikvision_read_seek,
    .extensions     = "hik",
};
