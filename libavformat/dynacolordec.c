/*
 * Dynacolor MVC Demuxer
 * Copyright (c) 2020-2021 Tom Needham
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

int ff_dyna_read_file_header(AVFormatContext *ctx, AVIOContext *pb, unsigned char *pes_data, DynacolorPesHeader *pes,
                               unsigned int *size, time_t *time, DynacolorHeader *header, unsigned int *basicIdx_H, unsigned int *basicIdx_L)
{
    int ret = 0;

    *(basicIdx_H) = avio_rl32(pb);

    header->Basic.Header1 = *(basicIdx_H)&0xFF;
    header->Basic.Header2 = *(basicIdx_H) >> 8 & 0xFF;
    header->Basic.reserved = *(basicIdx_H) >> 16 & 0x0F;
    header->Basic.Source = *(basicIdx_H) >> 20 & 0x01;
    header->Basic.WidthBase = *(basicIdx_H) >> 21 & 0x03;
    header->Basic.Reserved0 = *(basicIdx_H) >> 23 & 0x01;
    header->Basic.Channel = *(basicIdx_H) >> 24 & 0x02;
    header->Basic.StreamType = *(basicIdx_H) >> 26 & 0x02;
    header->Basic.QI = *(basicIdx_H) >> 28 & 0x0F;

    *size = avio_rl32(pb);
    header->Basic.Size = *size;

    *time = (time_t)avio_rl32(pb);
    header->Basic.Time = ((unsigned int)*time);

    *(basicIdx_L) = avio_rl32(pb);

    header->Basic.NTSC_PAL = *(basicIdx_L)&0x01;
    header->Basic.ImgMode = *(basicIdx_L) >> 1 & 0x01;
    header->Basic.Audio = *(basicIdx_L) >> 2 & 0x01;
    header->Basic.DST = *(basicIdx_L) >> 3 & 0x01;
    header->Basic.Covert = *(basicIdx_L) >> 4 & 0x01;
    header->Basic.Vloss = *(basicIdx_L) >> 5 & 0x01;
    header->Basic.AlarmIn = *(basicIdx_L) >> 6 & 0x01;
    header->Basic.Motion = *(basicIdx_L) >> 7 & 0x01;
    header->Basic.ExtraEnable = *(basicIdx_L) >> 8 & 0x01;
    header->Basic.EventEnable = *(basicIdx_L) >> 9 & 0x01;
    header->Basic.PPS = *(basicIdx_L) >> 10 & 0x40;
    header->Basic.Type = *(basicIdx_L) >> 16 & 0x08;
    header->Basic.Channel = *(basicIdx_L) >> 19 & 0x20;
    header->Basic.Chksum = *(basicIdx_L) >> 24 & 0xFF;

    if (header->Basic.ExtraEnable)
    {
        // File has DynacolorExtraIdx header so parse it
        unsigned int start;
        unsigned char end;
        char checksum;

        start = avio_rl32(pb);

        header->Extra.IsDST_S_W = start & 0x01;
        header->Extra.DST_Minutes = start >> 1 & 0xFF;
        header->Extra.OverSpeed = start >> 9 & 0x01;
        header->Extra.SkipPicCnt = start >> 10 & 0x20;
        header->Extra.Exception = start >> 15 & 0x01;
        header->Extra.SuperExtraSize = start >> 16 & 0xFFFF;

        header->Extra.Serno = avio_rl32(pb);
        header->Extra.AlmIn[0] = (unsigned char)avio_r8(pb);
        header->Extra.AlmIn[1] = (unsigned char)avio_r8(pb);
        header->Extra.Vloss[0] = (unsigned char)avio_r8(pb);
        header->Extra.Vloss[1] = (unsigned char)avio_r8(pb);
        header->Extra.Motion[0] = (unsigned char)avio_r8(pb);
        header->Extra.Motion[1] = (unsigned char)avio_r8(pb);

        end = (unsigned char)avio_r8(pb);

        header->Extra.IsDVRRec = end & 0x03;
        header->Extra.TimeZone = (end >> 2) & 0x40;

        checksum = (char)avio_r8(pb);

        header->Basic.Chksum = checksum & 0xFF;

        header->SuperExtra.type = (int)avio_rl32(pb);
        header->SuperExtra.remain_size = (int)avio_rl32(pb);

        if (avio_read(pb, header->SuperExtra.title, DYNACOLOR_SUPEREXTRA_TITLE_SIZE) != DYNACOLOR_SUPEREXTRA_TITLE_SIZE)
            return AVERROR_INVALIDDATA;

        if (avio_read(pb, header->SuperExtra.extra_data, DYNACOLOR_SUPEREXTRA_EXTRA_DATA_SIZE) != DYNACOLOR_SUPEREXTRA_EXTRA_DATA_SIZE)
            return AVERROR_INVALIDDATA;
    }
    else
    {
        // File has no DynacolorExtraIdx header so skip it
        av_log(ctx, AV_LOG_DEBUG, "Skipping DynacolorExtraIdx and DynacolorSuperExtraIdx Header\n");
        avio_skip(pb, DYNACOLOR_EXTRA_SIZE + DYNACOLOR_SUPEREXTRA_SIZE);

        memset(&header->Extra, 0xFF, DYNACOLOR_EXTRA_SIZE);
        memset(&header->SuperExtra, 0xFF, DYNACOLOR_SUPEREXTRA_SIZE);
    }

    if (avio_read(pb, pes_data, DYNACOLOR_PES_HEADER_SIZE) != DYNACOLOR_PES_HEADER_SIZE)
        return AVERROR_INVALIDDATA;

    return ret;
}

int ff_dyna_get_stream_format(AVFormatContext *ctx, DynacolorHeader *header)
{
    DynacolorContext *priv = ctx->priv_data;
    int ret = 0;

    // Try And Build Header for H264 Format
    av_log(ctx, AV_LOG_DEBUG, "Validating H264 PES Header\n");
    ret = ff_dyna_check_pes_packet_header(ctx, DYNACOLOR_H264_FORMAT_PREFIX);

    if (!ret)
    {
        // Format was not H264 or H265 so try And Build Header for MPEG4 Format
        av_log(ctx, AV_LOG_DEBUG, "Validating MPEG4 PES Header\n");
        ret = ff_dyna_check_pes_packet_header(ctx, DYNACOLOR_MPEG4_FORMAT_PREFIX);

        if (!ret)
        {
            // Format was not H264 or H265 so try And Build Header for MPEG4 Format
            av_log(ctx, AV_LOG_DEBUG, "Validating MPEG4 PES Header\n");
            ret = ff_dyna_check_pes_packet_header(ctx, DYNACOLOR_MPEG4_FORMAT_PREFIX);

            if (!ret)
            {
                // Format was not MPEG4, H264, or H265 so try And Build Header for JPEG Format
                av_log(ctx, AV_LOG_DEBUG, "Validating JPEG PES Header\n");
                ret = ff_dyna_check_pes_packet_header(ctx, DYNACOLOR_JPEG_FORMAT_PREFIX);

                if (!ret)
                {
                    // Format was not MPEG4, H264, H265 or JPEG so try And Build Header for AUDIO Format
                    av_log(ctx, AV_LOG_DEBUG, "Validating AUDIO PES Header\n");
                    ret = ff_dyna_check_pes_packet_header(ctx, DYNACOLOR_AUDIO_FORMAT_PREFIX);

                    if (!ret)
                    {
                        avpriv_report_missing_feature(ctx, "Format %02X,", priv->pes_header.format_id);
                        return AVERROR_PATCHWELCOME;
                    }
                    else
                    {
                        av_log(ctx, AV_LOG_DEBUG, "Successfully Validated AUDIO PES Header\n");
                        return DYNACOLOR_JPEG_FORMAT_PREFIX;
                    }
                }
                else
                {
                    av_log(ctx, AV_LOG_DEBUG, "Successfully Validated JPEG PES Header\n");
                    return DYNACOLOR_JPEG_FORMAT_PREFIX;
                }
            }
            else
            {
                av_log(ctx, AV_LOG_DEBUG, "Successfully Validated MPEG4 PES Header\n");
                return DYNACOLOR_MPEG4_FORMAT_PREFIX;
            }
        }
        else 
        {
            av_log(ctx, AV_LOG_DEBUG, "Successfully Validated H265 PES Header\n");
            return DYNACOLOR_H265_FORMAT_PREFIX;
        }
    }
    else
    {
        av_log(ctx, AV_LOG_DEBUG, "Successfully Validated H264 PES Header\n");
        return DYNACOLOR_H264_FORMAT_PREFIX;
    }

    return 0;
}

int ff_dyna_read_pes_header(AVFormatContext *ctx, uint8_t *pes_data)
{
    DynacolorContext *priv = ctx->priv_data;

    priv->pes_header.start_code0 = pes_data[0];
    priv->pes_header.start_code1 = pes_data[1];
    priv->pes_header.start_code2 = pes_data[2];
    priv->pes_header.format_id = pes_data[3] & 0x0F;
    priv->pes_header.channel_id = pes_data[3] >> 8 & 0x0F;

    priv->pes_header.unused_0 = MKTAG(pes_data[4], pes_data[5], pes_data[6], pes_data[7]);
    priv->pes_header.unused_1 = MKTAG(pes_data[8], pes_data[9], pes_data[10], pes_data[11]);
    priv->pes_header.unused_2 = MKTAG(pes_data[12], pes_data[13], pes_data[14], pes_data[15]);
    priv->pes_header.unused_3 = MKTAG(pes_data[16], pes_data[17], pes_data[18], pes_data[19]);
    priv->pes_header.unused_4 = MKTAG(pes_data[20], pes_data[21], pes_data[22], pes_data[23]);

    priv->pes_header.size_bit7to0 = pes_data[24];
    priv->pes_header.size_bit10to8 = pes_data[25];
    priv->pes_header.size_bit14to11 = pes_data[26];
    priv->pes_header.size_bit21to15 = pes_data[27];
    priv->pes_header.size_marker0 = pes_data[28] & 0x01;
    priv->pes_header.size_marker1 = pes_data[29] & 0x01;
    priv->pes_header.picture_type = pes_data[30];
    priv->pes_header.is_interleaved = pes_data[31] & 0x01;
    priv->pes_header.field_id = pes_data[31] >> 1 & 0x03;
    priv->pes_header.unused_5 = 0x00;

    return 0;
}

int ff_dyna_check_pes_packet_header(AVFormatContext *ctx, int format_prefix)
{
    DynacolorContext *dyna = ctx->priv_data;
    DynacolorPesHeader *pes = &dyna->pes_header;

    return pes->format_id == format_prefix;
}

static int dyna_read_probe(const AVProbeData *p)
{
    int magic = MKTAG(p->buf[0], p->buf[1], p->buf[2], p->buf[3]);

    if (magic == 0x20103240)
        return AVPROBE_SCORE_MAX;

    return 0;
}

static int dyna_read_header(AVFormatContext *ctx)
{
    int ret = 0;
    DynacolorContext *priv = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    AVStream *vstream = NULL;
    AVCodec *vcodec = NULL;
    AVCodecParameters *vcodec_params = NULL;

    AVStream *astream = NULL;
    AVCodec *acodec = NULL;
    AVCodecParameters *acodec_params = NULL;

    time_t time;
    unsigned char *pes_data;
    DynacolorPesHeader *pes;
    unsigned int size;

    unsigned int basicIdx_H, basicIdx_L;
    unsigned int stream_format;

    pes_data = av_malloc_array(DYNACOLOR_PES_HEADER_SIZE, 1);
    pes = av_malloc(sizeof(DynacolorPesHeader));

    if (ret)
    {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    ret = ff_dyna_read_file_header(ctx, pb, pes_data, pes, &size, &time,
                                     &priv->header, &basicIdx_H, &basicIdx_L);

    ret = ff_dyna_read_pes_header(ctx, pes_data);
    avio_seek(pb, -DYNACOLOR_PES_HEADER_SIZE, SEEK_CUR); // Move Back to before the header so it can be read again in dyna_read_packet

    if (ret)
    {
        av_log(ctx, AV_LOG_ERROR, "Failed to Build PES Header\n");
        return AVERROR_INVALIDDATA;
    }
    else
    {
        stream_format = ff_dyna_get_stream_format(ctx, &priv->header);
        if (!stream_format)
        {
            avpriv_report_missing_feature(ctx, "Format %02X,", stream_format);
            return AVERROR_PATCHWELCOME;
        }
        else if (stream_format == AVERROR_INVALIDDATA)
        {
            av_log(ctx, AV_LOG_ERROR, "Could not Determine Stream Format\n");
            return AVERROR_INVALIDDATA;
        }
        av_log(ctx, AV_LOG_DEBUG, "File Has 0x%02X Stream Format\n", stream_format);
    }


    if (priv->pes_header.format_id == DYNACOLOR_JPEG_FORMAT_PREFIX)
    {
        av_log(ctx, AV_LOG_DEBUG, "Demuxing JPEG Video Stream\n");

        vcodec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        vstream = avformat_new_stream(ctx, vcodec);
        vcodec_params = vstream->codecpar;

        if (!vstream || !vcodec_params)
        {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
    }
    else if (priv->pes_header.format_id == DYNACOLOR_H264_FORMAT_PREFIX)
    {
        av_log(ctx, AV_LOG_DEBUG, "Demuxing H264 Video Stream\n");

        vcodec = avcodec_find_decoder(AV_CODEC_ID_H264);
        vstream = avformat_new_stream(ctx, vcodec);
        vcodec_params = vstream->codecpar;

        if (!vstream || !vcodec_params)
        {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
    }
    else if (priv->pes_header.format_id == DYNACOLOR_H265_FORMAT_PREFIX)
    {
        av_log(ctx, AV_LOG_DEBUG, "Demuxing H265 Video Stream\n");

        vcodec = avcodec_find_decoder(AV_CODEC_ID_H265);
        vstream = avformat_new_stream(ctx, vcodec);
        vcodec_params = vstream->codecpar;

        if (!vstream || !vcodec_params)
        {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
    }
    else if (priv->pes_header.format_id == DYNACOLOR_MPEG4_FORMAT_PREFIX)
    {
        av_log(ctx, AV_LOG_DEBUG, "Demuxing MPEG4 Video Stream\n");

        vcodec = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
        vstream = avformat_new_stream(ctx, vcodec);
        vcodec_params = vstream->codecpar;

        if (!vstream || !vcodec_params)
        {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
    }
    else if (priv->pes_header.format_id == DYNACOLOR_AUDIO_FORMAT_PREFIX)
    {
        av_log(ctx, AV_LOG_DEBUG, "Demuxing Audio Stream\n");

        // NOTE: No codec is specified here since,
        //       There are no samples of this format
        //       That contain audio to use as a baseline
        //       So we just allow any audio codec to be demuxed
        astream = avformat_new_stream(ctx, NULL);
        acodec_params = astream->codecpar;

        if (!astream || !acodec_params)
        {
            ret = AVERROR_INVALIDDATA;
            goto end;
        }
    }

end:
    av_free(pes_data);
    av_free(pes);

    return ret;
}

static int dyna_read_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    int ret = 0;
    DynacolorContext *priv = ctx->priv_data;
    AVIOContext *pb = ctx->pb;
    DynacolorPesHeader *pes;

    unsigned char *pes_data;
    unsigned char *pkt_data;
    unsigned int size;

    pes_data = av_malloc_array(DYNACOLOR_PES_HEADER_SIZE, 1);
    pes = av_malloc(sizeof(DynacolorPesHeader));

    ret = ff_dyna_read_pes_header(ctx, pes_data);

    size = (priv->pes_header.size_bit7to0 & 0xFF) | 
    ((priv->pes_header.size_bit10to8 & 0x04) << 15) | 
    ((priv->pes_header.size_bit14to11 & 0x08) << 11) | 
    ((priv->pes_header.size_bit21to15 & 0x7F) << 8);

    av_log(ctx, AV_LOG_DEBUG, "Size = %i\n", size);

    pkt_data = av_malloc(size);
    ret = avio_read(pb, pkt_data, size);

    if (ret != size)
        goto end;

    ret = av_packet_from_data(pkt, pkt_data, size);

    if (ret < 0)
    {
        av_log(ctx, AV_LOG_ERROR, "Error Building Packet\n");
        goto end;
    }

end:
    av_free(pes_data);
    av_free(pes);

    return ret;
}

static int dyna_read_close(AVFormatContext *ctx)
{
    return 0;
}

static int dyna_read_seek(AVFormatContext *ctx, int stream_index,
                          int64_t timestamp, int flags)
{
    DynacolorContext *priv = ctx->priv_data;
    unsigned int size = 0;
    int64_t ret = 0L;

    size = (priv->pes_header.size_bit7to0 & 0xFF) |
    ((priv->pes_header.size_bit10to8 & 0x04) << 15) |
    ((priv->pes_header.size_bit14to11 & 0x08) << 11) |
    ((priv->pes_header.size_bit21to15 & 0x7F) << 8);

    ret = avio_seek(ctx->pb, size, SEEK_SET);

    if (ret < 0)
        return ret;

    return 0;
}

AVInputFormat ff_dynacolor_demuxer = {
    .name = "dynacolor",
    .long_name = NULL_IF_CONFIG_SMALL("Dynacolor MVC"),
    .priv_data_size = sizeof(DynacolorContext),
    .read_probe = dyna_read_probe,
    .read_header = dyna_read_header,
    .read_packet = dyna_read_packet,
    .read_close = dyna_read_close,
    .read_seek = dyna_read_seek,
    .extensions = "dyna,dyn",
};
