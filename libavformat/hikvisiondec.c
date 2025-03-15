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
    priv->header.field_12 = avio_rl16(pb);
    priv->header.field_14 = avio_rl16(pb);
    priv->header.field_16 = avio_rl16(pb);
    priv->header.field_18 = avio_rl16(pb);
    priv->header.field_20 = avio_rl16(pb);
    priv->header.field_22 = avio_rl16(pb);
    priv->header.field_24 = avio_rl32(pb);
    priv->header.field_28 = avio_rl32(pb);
    priv->header.field_32 = avio_rl32(pb);

    av_log(ctx, AV_LOG_INFO, "Version: %i\n", priv->header.version);
    av_log(ctx, AV_LOG_INFO, "Field_08: 0x%08X\n", priv->header.field_8);
    av_log(ctx, AV_LOG_INFO, "Field_12: 0x%04X\n", priv->header.field_12);
    av_log(ctx, AV_LOG_INFO, "Field_14: 0x%04X\n", priv->header.field_14);
    av_log(ctx, AV_LOG_INFO, "Field_16: 0x%04X\n", priv->header.field_16);
    av_log(ctx, AV_LOG_INFO, "Field_18: 0x%04X\n", priv->header.field_18);
    av_log(ctx, AV_LOG_INFO, "Field_20: 0x%04X\n", priv->header.field_20);
    av_log(ctx, AV_LOG_INFO, "Field_22: 0x%04X\n", priv->header.field_22);
    av_log(ctx, AV_LOG_INFO, "Field_24: 0x%08X\n", priv->header.field_24);
    av_log(ctx, AV_LOG_INFO, "Field_28: 0x%08X\n", priv->header.field_28);
    av_log(ctx, AV_LOG_INFO, "Field_32: 0x%08X\n", priv->header.field_32);

    ret = hikvision_get_resolution(ctx, priv->header.field_28 & 0xFFFF, priv->header.field_28 >> 16);

    return ret;
}

int hikvision_parse_mediainfo(AVFormatContext *ctx) {
    int ret = 0;
    HikvisionContext *priv = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    avio_seek(pb, 8, SEEK_SET);
    priv->header.media_info.field_8 = avio_rl16(pb);

    if(priv->header.media_info.field_8 != 1)
        return AVERROR_INVALIDDATA;

    priv->header.media_info.field_10 = avio_rl16(pb);
    priv->header.media_info.field_12 = avio_rl16(pb);
    priv->header.media_info.field_14 = avio_r8(pb);
    priv->header.media_info.field_15 = avio_r8(pb);
    priv->header.media_info.field_16 = avio_rl32(pb);
    priv->header.media_info.field_20 = avio_rl32(pb);

    av_log(ctx, AV_LOG_INFO, "Field_08: 0x%04X\n", priv->header.media_info.field_8);
    av_log(ctx, AV_LOG_INFO, "Field_10: 0x%04X\n", priv->header.media_info.field_10);
    av_log(ctx, AV_LOG_INFO, "Field_12: 0x%04X\n", priv->header.media_info.field_12);
    av_log(ctx, AV_LOG_INFO, "Field_14: 0x%02X\n", priv->header.media_info.field_14);
    av_log(ctx, AV_LOG_INFO, "Field_15: 0x%02X\n", priv->header.media_info.field_15);
    av_log(ctx, AV_LOG_INFO, "Field_16: 0x%08X\n", priv->header.media_info.field_16);
    av_log(ctx, AV_LOG_INFO, "Field_20: 0x%08X\n", priv->header.media_info.field_20);

    return ret;
}

int hikvision_get_resolution(AVFormatContext *ctx, int width, int height) {
    int ret = 0;
    HikvisionContext *priv = ctx->priv_data;

    switch (width)
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
        priv->header.resolution.width = width;
        priv->header.resolution.height = height;
        break;
    }

    av_log(ctx, AV_LOG_INFO, "Switcher: 0x%04X\n", width);
    av_log(ctx, AV_LOG_INFO, "Width: %i\n", priv->header.resolution.width);
    av_log(ctx, AV_LOG_INFO, "Height: %i\n", priv->header.resolution.height);
    
    return ret;
}

unsigned int hikvision_parse_group(AVFormatContext *ctx, int group_id) 
{
    unsigned int ret = 0;
    HikvisionContext *priv = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    short comparitor = -1;
    unsigned int int_val = -1;
    unsigned int read_val = -1;

    avio_skip(pb, 20);
    comparitor = avio_rl16(pb);
    
    av_log(ctx, AV_LOG_INFO, "Comparitor: 0x%04X\n", comparitor);
    
    if(group_id == 1 && comparitor == 0x1001) {
        if(group_id < 0x2f) {
            av_log(ctx, AV_LOG_INFO, "Need to parse group header\n");
            avio_seek(pb, -22, SEEK_CUR);
            ret = hikvision_parse_group_header(ctx);
            if(ret == AVERROR_INVALIDDATA)
                return 0xFFFF;
            else
                return 0x30;
        }
        else {
            ret = 0xFFFF;
            return ret;
        }
    }
    else if(group_id < 0x13) {
        av_log(ctx, AV_LOG_INFO, "Need to parse block header\n");
        ret = hikvision_parse_block_header(ctx);

        if(ret == 0)
            return AVERROR_INVALIDDATA;

        int_val = avio_r8(pb);
        if(int_val <= group_id - 0x14U) {
            av_log(ctx, AV_LOG_INFO, "Need block header\n");

            if(priv->header.version == 0) {
                ret = 0;
            }
            else if(priv->header.version != 1) {
                ret = 1;
            }
            
            avio_skip(pb, 15);
            read_val = avio_r8(pb);

            if(ret != 0) { 
                av_log(ctx, AV_LOG_INFO, "int_val was not 0\n");
                int_val += read_val + 20;
                return int_val;
            }
            else {
                int_val += read_val;
            }

            // TODO: Output Payload

            return read_val + 20;          
        }
    }

    return 0xFFFF;
}

int hikvision_parse_group_header(AVFormatContext *ctx) 
{
    int ret = 0;
    HikvisionContext *priv = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    priv->group_header.field_4 = avio_rl32(pb) - 0x1000;
    priv->group_header.field_8 = avio_rl32(pb);
    priv->group_header.field_12 = avio_rl32(pb);
    priv->group_header.field_16 = avio_rl32(pb);
    priv->group_header.field_20 = avio_rl32(pb);
    priv->group_header.field_24 = avio_rl32(pb);
    priv->group_header.field_28 = avio_rl32(pb);
    avio_skip(pb, 12);
    priv->group_header.field_44 = avio_rl32(pb);

    av_log(ctx, AV_LOG_INFO, "Field_04: 0x%08X\n", priv->group_header.field_4);
    av_log(ctx, AV_LOG_INFO, "Field_08: 0x%08X\n", priv->group_header.field_8);
    av_log(ctx, AV_LOG_INFO, "Field_12: 0x%08X\n", priv->group_header.field_12);
    av_log(ctx, AV_LOG_INFO, "Field_16: 0x%08X\n", priv->group_header.field_16);
    av_log(ctx, AV_LOG_INFO, "Field_20: 0x%08X\n", priv->group_header.field_20);
    av_log(ctx, AV_LOG_INFO, "Field_24: 0x%08X\n", priv->group_header.field_24);
    av_log(ctx, AV_LOG_INFO, "Field_28: 0x%08X\n", priv->group_header.field_28);
    av_log(ctx, AV_LOG_INFO, "Field_44: 0x%08X\n", priv->group_header.field_44);

    if((priv->group_header.field_12 == 0x1000 || 
     priv->group_header.field_12 == 0x1001) && 
     (priv->group_header.field_16 - 0x1000 < 7))
     {
        av_log(ctx, AV_LOG_INFO, "Hit if cond\n");
        return ret;
     }

    return AVERROR_INVALIDDATA;
}

int hikvision_parse_block_header(AVFormatContext *ctx) 
{
    int ret = 0;
    HikvisionContext *priv = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    unsigned int comparitor = 0;
    int block_value = 0;
    double fval = 0.0;
    short buffer_10 = 0;

    comparitor = avio_rl32(pb);
    if(comparitor < 0x1001)
        return ret;

    if(comparitor < 0x1003) {
        return AVERROR_PATCHWELCOME;
    }
    else {
        if(comparitor < 0x1005)
            return ret;

        block_value = priv->group_header.field_44;
        av_log(ctx, AV_LOG_INFO, "block_value: 0x%08X\n", block_value);

        priv->block_header.field_4 = (block_value >> 26) + 2000;
        priv->block_header.field_8 = block_value >> 22 & 0x0F;
        priv->block_header.field_12 = block_value >> 17 & 0x1F;
        priv->block_header.field_16 = block_value >> 12 & 0x1F;
        priv->block_header.field_20 = block_value & 0x3F;
        priv->block_header.field_24 = block_value >> 6 & 0x3F;

        avio_skip(pb, 8);
        priv->block_header.field_28 = avio_rl16(pb);

        fval = priv->group_header.field_28 - 0x1000;
        av_log(ctx, AV_LOG_INFO, "fval: %f\n", fval);

        if(fval < 0.0)
            fval += 4.294967e+09;

        if(priv->header.field_8 == 0x20020302) {
            av_log(ctx, AV_LOG_INFO, "Need to parse resolution\n");
            hikvision_get_resolution(ctx, priv->group_header.field_20, priv->header.field_18);
        }

        buffer_10 = avio_rl16(pb);
        if(buffer_10 < 0x1000)
            return 0;
    }

    return 1;
}

int hikvision_search_start_code(AVFormatContext *ctx, int group_id) 
{
    unsigned char counter = 0;
    unsigned char value = 0XFF;

    AVIOContext *pb = ctx->pb;

    
    if(group_id != 3) {
        do {
            value = avio_r8(pb) + counter;
            if(value == 1) {
                av_log(ctx, AV_LOG_INFO, "Start Code: %i\n", counter);
                return counter;
            }
            counter++;
        } while (counter < (group_id - 3));        
    }

    return -1;
}

int hikvision_get_stream(AVFormatContext *ctx) 
{
    int ret = 0;
    unsigned int return_code = 0;
    
    AVIOContext *pb = ctx->pb;

    int group_id = -1;

    avio_skip(pb, 4);
    group_id = avio_rl32(pb);

    av_log(ctx, AV_LOG_INFO, "Group ID: %i\n", group_id);

    do {
        return_code = hikvision_parse_group(ctx, group_id);

        av_log(ctx, AV_LOG_INFO, "Return code: %u\n", return_code);

        if(return_code == 0XFFFF)
            return 0;

        if((return_code + 2147483646U) < 2) {
            av_log(ctx, AV_LOG_INFO, "Searching for start code\n");
            group_id--;
            avio_skip(pb, 1);
            ret = hikvision_search_start_code(ctx, group_id);

            if(ret == -1)
                return 0;

            avio_skip(pb, ret);
            group_id -= ret;
        }

        avio_skip(pb, return_code);
        group_id -= return_code;
        av_log(ctx, AV_LOG_INFO, "End of group loop\n");

    } while (true);
    
    return ret;

}

int hikvision_update_payload_info(AVFormatContext *ctx) 
{
    int ret = 0;
    HikvisionContext *priv = ctx->priv_data;

    short header_field_18 = priv->header.field_18;

    if(header_field_18 < 0x2001) {
        if(header_field_18 == 0 || 
        (header_field_18 >= 5 && header_field_18 != 0x100) || 
        header_field_18 < 0x1013)
            return AVERROR_INVALIDDATA;
    }

    return ret;
}

static int hikvision_read_header(AVFormatContext *ctx)
{
    int ret = 0;
    HikvisionContext *priv = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    AVStream *vstream = NULL;
    AVCodec *vcodec = NULL;
    AVCodecParameters *vcodec_params = NULL;
    FFStream *sti;
    priv->header.magic = avio_rl32(pb);

    switch (priv->header.magic)
    {
    case HIKVISION_ORIGINAL_MAGIC:
        av_log(ctx, AV_LOG_INFO, "File is an original\n");
        priv->header.version = 1;
        hikvision_parse_file_header(ctx);
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

    avio_seek(pb, 108, SEEK_SET);

    av_log(ctx, AV_LOG_DEBUG, "Demuxing H264 Video Stream\n");

    vcodec = avcodec_find_decoder(AV_CODEC_ID_H264);
    vstream = avformat_new_stream(ctx, vcodec);
    vcodec_params = vstream->codecpar;

    if (!vstream || !vcodec_params)
    {
        ret = AVERROR_INVALIDDATA;
    }
    
    sti = ffstream(vstream);

    vstream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vstream->codecpar->codec_id = AV_CODEC_ID_H264;
    sti->need_parsing = AVSTREAM_PARSE_FULL_RAW;

    //vstream->avg_frame_rate = 30.0;
    avpriv_set_pts_info(vstream, 64, 1, 1200000);

    //ret = hikvision_get_stream(ctx);
    
    //if(ret != 0)
       // return AVERROR_INVALIDDATA;

    return ret;
}

static int hikvision_read_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    HikvisionContext *raw = ctx->priv_data;
    int ret, size;

    size = 1024;

    if ((ret = av_new_packet(pkt, size)) < 0)
        return ret;

    pkt->pos = avio_tell(ctx->pb);
    pkt->stream_index = 0;
    ret = avio_read_partial(ctx->pb, pkt->data, size);
    if (ret < 0) {
        av_packet_unref(pkt);
        return ret;
    }
    av_shrink_packet(pkt, ret);
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
