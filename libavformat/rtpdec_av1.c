/*
 * RTP parser for AV1 Payload Format (v1.0)
 * Copyright (c) 2021 Thomas Needham <06needhamt@gmail.com>
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

#include "libavutil/intreadwrite.h"

#include "avio_internal.h"
#include "rtpdec_formats.h"


#define RTP_AV1_DESC_REQUIRED_SIZE 1

typedef struct AV1PayloadContext {
    AVIOContext *buf;
    uint32_t     timestamp;
} AV1PayloadContext;

static av_cold int av1_init(AVFormatContext *ctx, int s,
                            AV1PayloadContext *data)
{
    av_log(ctx, AV_LOG_WARNING,
           "RTP/AV1 support is still experimental\n");

    return 0;
}

static int av1_handle_packet(AVFormatContext *ctx, AV1PayloadContext *rtp_av1_ctx,
                             AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                             const uint8_t *buf, int len, uint16_t seq,
                             int flags)
{
    unsigned short marker_1, marker_2;
    unsigned short extensions_length;
    unsigned char id, header_length;

    /* drop data of previous packets in case of non-continuous (lossy) packet stream */
    if (rtp_av1_ctx->buf && rtp_av1_ctx->timestamp != *timestamp)
        ffio_free_dyn_buf(&rtp_av1_ctx->buf);

    /* sanity check for size of input packet: 1 byte payload at least */
    if (len < RTP_AV1_DESC_REQUIRED_SIZE + 1) {
        av_log(ctx, AV_LOG_ERROR, "Too short RTP/AV1 packet, got %d bytes\n", len);
        return AVERROR_INVALIDDATA;
    }

    marker_1 = !!(buf[0] | (buf[1] & 0xE0));
    marker_2 = !!(buf[1] & 0x1F);
    buf += 2;

    extensions_length = AV_RB16(buf);

    if(marker_1 != 0x100)
        return AVERROR_INVALIDDATA;

    if(marker_2 != 0x00)
        return AVERROR_INVALIDDATA;
}

static void av1_close_context(AV1PayloadContext *av1) 
{
    ffio_free_dyn_buf(&av1->buf);
}

const RTPDynamicProtocolHandler ff_av1_dynamic_handler = {
    .enc_name         = "AV1",
    .codec_type       = AVMEDIA_TYPE_VIDEO,
    .codec_id         = AV_CODEC_ID_AV1,
    .priv_data_size   = sizeof(AV1PayloadContext),
    .init             = av1_init,
    .close            = av1_close_context,
    .parse_packet     = av1_handle_packet
};
