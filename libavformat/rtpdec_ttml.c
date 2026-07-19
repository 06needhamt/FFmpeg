/*
 * RTP depacketizer for TTML timed text payload format (RFC 8759)
 * Copyright (c) 2026
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

/**
 * @file
 * @brief TTML RTP depacketization (RFC 8759)
 *
 * Every packet starts with a 4 byte payload header: 16 reserved bits
 * (ignored on reception) and a 16 bit length field giving the number of
 * User Data Words bytes in the packet. A document may be fragmented
 * over several packets, all sharing the RTP timestamp of the document;
 * the marker bit signals the final packet of a document. Fragments are
 * reassembled and one complete TTML document is output per AVPacket.
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"
#include "rtpdec_formats.h"

#define RTP_TTML_PAYLOAD_HEADER_SIZE 4

struct PayloadContext {
    AVIOContext *doc;    /**< document being reassembled, or NULL   */
    uint32_t timestamp;  /**< RTP timestamp of the pending document */
    uint16_t prev_seq;   /**< sequence number of the last fragment  */
};

static av_cold int ttml_init(AVFormatContext *ctx, int st_index,
                             PayloadContext *data)
{
    AVStream *st;

    if (st_index < 0)
        return 0;

    st = ctx->streams[st_index];
    /* RFC 8759 announces TTML under m=application, which the generic
     * SDP parsing maps to AVMEDIA_TYPE_DATA. */
    st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    avpriv_set_pts_info(st, 32, 1, 90000);

    return 0;
}

static void ttml_drop_document(PayloadContext *data)
{
    uint8_t *p;

    if (data->doc) {
        avio_close_dyn_buf(data->doc, &p);
        av_free(p);
        data->doc = NULL;
    }
}

static void ttml_close_context(PayloadContext *data)
{
    ttml_drop_document(data);
}

static int ttml_handle_packet(AVFormatContext *ctx, PayloadContext *data,
                              AVStream *st, AVPacket *pkt,
                              uint32_t *timestamp, const uint8_t *buf, int len,
                              uint16_t seq, int flags)
{
    uint16_t length;
    int ret;

    if (len < RTP_TTML_PAYLOAD_HEADER_SIZE) {
        av_log(ctx, AV_LOG_ERROR, "Too short RTP/TTML packet\n");
        return AVERROR_INVALIDDATA;
    }

    /* 16 reserved bits (ignored on reception), then the length of the
     * User Data Words field in bytes. */
    length = AV_RB16(buf + 2);
    if (length > len - RTP_TTML_PAYLOAD_HEADER_SIZE) {
        av_log(ctx, AV_LOG_ERROR,
               "RTP/TTML length field %d larger than packet payload %d\n",
               length, len - RTP_TTML_PAYLOAD_HEADER_SIZE);
        return AVERROR_INVALIDDATA;
    }
    buf += RTP_TTML_PAYLOAD_HEADER_SIZE;
    len  = length;

    if (data->doc && (data->timestamp != *timestamp ||
                      seq != (uint16_t)(data->prev_seq + 1))) {
        /* Fragments were lost. If the current packet still belongs to
         * the dropped document (same timestamp), it cannot start a new
         * one either; documents are delimited by the marker bit only,
         * so anything up to the next marker is unusable. A packet with
         * a new timestamp is treated as the potential start of the next
         * document. */
        av_log(ctx, AV_LOG_WARNING,
               "Dropping partially received TTML document\n");
        ttml_drop_document(data);
        if (data->timestamp == *timestamp)
            return AVERROR(EAGAIN);
    }

    if (!data->doc) {
        if ((ret = avio_open_dyn_buf(&data->doc)) < 0)
            return ret;
        data->timestamp = *timestamp;
    }

    avio_write(data->doc, buf, len);
    data->prev_seq = seq;

    if (!(flags & RTP_FLAG_MARKER))
        return AVERROR(EAGAIN);

    *timestamp = data->timestamp;

    ret = ff_rtp_finalize_packet(pkt, &data->doc, st->index);
    if (ret < 0)
        return ret;
    pkt->flags |= AV_PKT_FLAG_KEY;

    return 0;
}

const RTPDynamicProtocolHandler ff_ttml_dynamic_handler = { /* RFC 8759 */
    .enc_name         = "ttml+xml",
    .codec_type       = AVMEDIA_TYPE_SUBTITLE,
    .codec_id         = AV_CODEC_ID_TTML,
    .priv_data_size   = sizeof(PayloadContext),
    .init             = ttml_init,
    .close            = ttml_close_context,
    .parse_packet     = ttml_handle_packet,
};

/* RFC 8759 Section 6.1.2 maps TTML to the "application" media type, so
 * the handler must also be discoverable for m=application streams. */
const RTPDynamicProtocolHandler ff_ttml_data_dynamic_handler = {
    .enc_name         = "ttml+xml",
    .codec_type       = AVMEDIA_TYPE_DATA,
    .codec_id         = AV_CODEC_ID_TTML,
    .priv_data_size   = sizeof(PayloadContext),
    .init             = ttml_init,
    .close            = ttml_close_context,
    .parse_packet     = ttml_handle_packet,
};
