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

static int hikvision_read_header(AVFormatContext *ctx)
{
    int ret = 0;
    HikvisionContext *priv = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    AVStream *vstream = NULL;
    AVCodec *vcodec = NULL;
    AVCodecParameters *vcodec_params = NULL;

    AVStream *astream = NULL;
    AVCodec *acodec = NULL;
    AVCodecParameters *acodec_params = NULL;

    priv->header.magic = avio_rl32(pb);
    priv->header.reserved_0 = avio_rl32(pb);
    priv->header.reserved_1 = avio_rl16(pb);
    priv->header.video_codec_id = avio_rl16(pb);
    priv->header.audio_codec_id = avio_rl16(pb);
    priv->header.reserved_2 = avio_rl16(pb);
    priv->header.some_audio_codec_related_value = avio_rl16(pb);

    if(avio_read(pb, priv->header.reserved_3, 82) != 82) {
        ret = AVERROR_BUFFER_TOO_SMALL;
        return ret;
    }

    switch (priv->header.video_codec_id)
    {
    case HIKVISION_VIDEO_CODEC_H264:
        vcodec = avcodec_find_decoder(AV_CODEC_ID_H264);
        vstream = avformat_new_stream(ctx, vcodec);
        // if (!vstream || !vcodec_params)
        //     ret = AVERROR_INVALIDDATA;
        break;

    case HIKVISION_VIDEO_CODEC_MPEG4:
        vcodec = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
        vstream = avformat_new_stream(ctx, vcodec);
        vcodec_params = vstream->codecpar;

        // if (!vstream || !vcodec_params)
        //     ret = AVERROR_INVALIDDATA;
        break;
    
    default:
        av_log(ctx, AV_LOG_ERROR, "Unknown Video Codec ID %i\n", priv->header.video_codec_id);
        ret = AVERROR_PATCHWELCOME;
        break;
    }
    

    if (ret)
        return ret;

    switch (priv->header.audio_codec_id)
    {
    case HIKVISION_AUDIO_CODEC_G711U:
        acodec = avcodec_find_decoder(AV_CODEC_ID_PCM_ALAW);
        astream = avformat_new_stream(ctx, acodec);
        acodec_params = astream->codecpar;

        // if (!astream || !acodec_params)
        //     ret = AVERROR_INVALIDDATA;
        // break;

    case HIKVISION_AUDIO_CODEC_NONE:
        ret = 0;
        break;
    
    default:
        av_log(ctx, AV_LOG_ERROR, "Unknown Audio Codec ID %i\n", priv->header.audio_codec_id);
        ret = AVERROR_PATCHWELCOME;
        break;
    }

    if (ret)
        return ret;

    return ret;
}

static int hikvision_read_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    int ret = 0;
    HikvisionContext *priv = ctx->priv_data;
    AVIOContext *pb = ctx->pb;

    unsigned char *pkt_data;
    int size;

    size = 7737;

    av_log(ctx, AV_LOG_DEBUG, "Size: %i\n", size);

    pkt_data = av_malloc(size);
    ret = avio_read(pb, pkt_data, size);

    if (ret != size)
        return AVERROR_INVALIDDATA;

    ret = av_packet_from_data(pkt, pkt_data, size);

    if (ret < 0)
    {
        av_log(ctx, AV_LOG_ERROR, "Error Building Packet\n");
        return AVERROR_INVALIDDATA;
    }

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

    if(p->buf[0] != 'I' || p->buf[1] != 'M' || p->buf[2] != 'K' || p->buf[3] != 'H')
        return 0;
    
    return AVPROBE_SCORE_MAX;
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
