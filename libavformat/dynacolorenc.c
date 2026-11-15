/*
 * Dynacolor MVC Muxer
 * Copyright (c) 2021 Tom Needham
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

#include <time.h>
#include "avformat.h"
#include "internal.h"
#include "dynacolor.h"
#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/timecode.h"
#include "libavutil/avassert.h"

int ff_dyna_write_file_header(AVFormatContext *ctx, AVIOContext *pb,
                                DynacolorHeader *header, unsigned int *size, time_t *time) 
{
    int ret = 0;
    DynacolorContext *context = ctx->priv_data;
    AVStream *stream;

    header->Basic.Header1 = 0x40;
    header->Basic.Header2 = 0x32;
    header->Basic.reserved = 0x00;
    header->Basic.Source = 0x01;
    header->Basic.WidthBase = 0x00;
    header->Basic.Reserved0 = 0x00;
    header->Basic.Channel_Ext = 0x00;

    if(ctx->nb_streams > 1) {
        av_log(ctx, AV_LOG_ERROR, "Dynacolor Format only Supports One Stream\n");
        return AVERROR_INVALIDDATA;
    }

    stream = ctx->streams[0];

    if(stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        header->Basic.StreamType = 0x00;
        header->Basic.Audio = 0x00;
    }
    else if(stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        header->Basic.StreamType = 0x00;
        header->Basic.Audio = 0x01;
    }
    else {
        av_log(ctx, AV_LOG_ERROR, "Unknown Stream Type %i", (int) stream->codecpar->codec_type);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    header->Basic.QI = 0x02;
    header->Basic.Size = *size;
    header->Basic.Time = *time;
    header->Basic.NTSC_PAL = 0x01;
    header->Basic.DST = 0x00;
    header->Basic.Covert = 0x00;
    header->Basic.Vloss = 0x00;
    header->Basic.AlarmIn = 0x00;
    header->Basic.Motion = 0x00;
    header->Basic.ExtraEnable = 0x01;
    header->Basic.EventEnable = 0x00;
    header->Basic.PPS = 0x00;
    header->Basic.Type = 0x00;
    header->Basic.Channel = 0x00;
    header->Basic.Chksum = 0x11;

    header->Extra.DST_Minutes = 0x00;
    header->Extra.IsDST_S_W = 0x00;
    header->Extra.OverSpeed = 0x00;
    header->Extra.SkipPicCnt = 0x00;
    header->Extra.Exception = 0x00;
    header->Extra.SuperExtraSize = DYNACOLOR_SUPEREXTRA_SIZE;
    header->Extra.Serno = 0x0504;
    header->Extra.AlmIn[0] = 0x00;
    header->Extra.AlmIn[1] = 0x00;
    header->Extra.Vloss[0] = 0x00;
    header->Extra.Vloss[1] = 0x00;
    header->Extra.Motion[0] = 0x00;
    header->Extra.Motion[1] = 0x00;
    header->Extra.IsDVRRec = 0x01;
    header->Extra.TimeZone = 0x00;
    header->Extra.Chksum = 0x00;

    header->SuperExtra.type = 0x01;
    header->SuperExtra.remain_size = 0x06;
    
    memset(header->SuperExtra.title, 0xFF, DYNACOLOR_SUPEREXTRA_TITLE_SIZE);
    memset(header->SuperExtra.extra_data, 0xFF, DYNACOLOR_SUPEREXTRA_EXTRA_DATA_SIZE);

    context->header = *header;

    avio_write(pb, (unsigned char*)header, sizeof(DynacolorHeader));

end:
    return ret;
}

int ff_dyna_write_pes_header(AVFormatContext *ctx, AVIOContext* pb,
                                DynacolorPesHeader *pes, unsigned int size)
{    
    DynacolorContext *context = ctx->priv_data;
    int ret = 0;
    char* buffer = av_malloc(sizeof(DynacolorPesHeader));
    AVStream *stream = ctx->streams[0];

    pes->start_code0 = 0x50;
    pes->start_code1 = 0xF1;
    pes->start_code2 = 0x63;
    
    if(context->header.Basic.Audio){
        pes->format_id = DYNACOLOR_AUDIO_FORMAT_PREFIX;
    }
    else {
        switch (stream->codecpar->codec_id)
        {
            case AV_CODEC_ID_JPEG2000:
            case AV_CODEC_ID_JPEGLS:
            case AV_CODEC_ID_LJPEG:
            case AV_CODEC_ID_MJPEG:
            case AV_CODEC_ID_MJPEGB:
                pes->format_id = DYNACOLOR_JPEG_FORMAT_PREFIX;
                break;
            case AV_CODEC_ID_H264:
                pes->format_id = DYNACOLOR_H264_FORMAT_PREFIX;
                break;
            case AV_CODEC_ID_H265:
                pes->format_id = DYNACOLOR_H265_FORMAT_PREFIX; // NOTE: Not Officially supported just trying it out
                break;
            case AV_CODEC_ID_MPEG4:
                pes->format_id = DYNACOLOR_MPEG4_FORMAT_PREFIX;
                break;
            default:
                ret = AVERROR_MUXER_NOT_FOUND;
                goto end;
        }
    }

    pes->channel_id = 0x00;
    pes->unused_0 = MKTAG(0x00, 0x00, 0x00, 0x00);
    pes->unused_1 = MKTAG(0x00, 0x00, 0x00, 0x00);
    pes->unused_2 = MKTAG(0x00, 0x00, 0x00, 0x00);
    pes->unused_3 = MKTAG(0x00, 0x00, 0x00, 0x00);
    pes->unused_4 = MKTAG(0x00, 0x00, 0x00, 0x00);
    pes->size_bit7to0 = size & 0xFF;
    pes->size_bit10to8 = (size >> 15) & 0x04;
    pes->size_bit14to11 = (size >> 11) & 0x08;
    pes->size_bit21to15 = (size >> 8) & 0x7F;
    pes->size_marker0 = 0x00;
    pes->size_marker1 = 0x01;
    pes->picture_type = 0x48; // TODO: Work out if this changes with frame type
    pes->is_interleaved = 0x00; // TODO: When is this ever not 0?
    pes->field_id = 0x01;
    pes->unused_5 = 0x00;

    avio_write(pb, (unsigned char*) pes, sizeof(DynacolorPesHeader));

end:
    av_free(buffer);
    return ret;
}

static int dyna_init(AVFormatContext *ctx) 
{
    int ret = 0;

    av_log(ctx, AV_LOG_DEBUG, "Init Called\n");
    goto end;

end:
    return ret;
}

static int dyna_write_header(AVFormatContext *ctx)
{
    int ret = 0;
    DynacolorContext *priv = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    time_t time;
    unsigned int size;

    ret = ff_dyna_write_file_header(ctx, pb, &priv->header, &size, &time);

    if(ret != 0){
        av_log(ctx, AV_LOG_ERROR, "Error Writing File Header\n");
        goto end;
    }

    ret = ff_dyna_write_pes_header(ctx, pb, &priv->pes_header, size);

    if(ret != 0){
        av_log(ctx, AV_LOG_ERROR, "Error Writing PES Header\n");
        goto end;
    }

end:
    return ret;
}

static int dyna_write_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    int ret = 0;
    DynacolorContext *priv = ctx->priv_data;
    AVIOContext *pb = ctx->pb;
    int size = FFABS(((priv->pes_header.size_bit7to0 & 0xFF) |
        ((priv->pes_header.size_bit10to8 & 0x04) << 15) |
        ((priv->pes_header.size_bit14to11 & 0x08) << 11) |
        ((priv->pes_header.size_bit21to15 & 0x7F) << 8)) - 32);

    avio_write(pb, pkt->data, size);

    ret = ff_dyna_write_pes_header(ctx, pb, &priv->pes_header, size);

    if(ret != 0){
        av_log(ctx, AV_LOG_ERROR, "Error Writing PES Header\n");
        goto end;
    }
    
end:
    return ret;
}

static void dyna_deinit(AVFormatContext *ctx)
{
    av_log(ctx, AV_LOG_DEBUG, "Deinit Called\n");
}

AVOutputFormat ff_dynacolor_muxer = {
    .name           = "dynacolor",
    .long_name      = NULL_IF_CONFIG_SMALL("Dynacolor MVC"),
    .priv_data_size = sizeof(DynacolorContext),
    .audio_codec       = AV_CODEC_ID_AAC,
    .video_codec       = CONFIG_LIBX264_ENCODER ?
                         AV_CODEC_ID_H264 : AV_CODEC_ID_MPEG4,
    .extensions     = "dyna,dyn",
    .init           = dyna_init,
    .write_header   = dyna_write_header,
    .write_packet   = dyna_write_packet,
    .deinit         = dyna_deinit,
};
