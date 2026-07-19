/*
 * RTP depacketizer for T.140 real-time text payload format (RFC 4103)
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
 * @brief T.140 real-time text RTP depacketization (RFC 4103)
 *
 * The payload consists of raw UTF-8 encoded T.140 code elements with a
 * fixed 1000 Hz clock. Packets containing no payload may be sent to
 * keep NAT bindings alive or immediately after an idle period and
 * produce no output. The fmtp "cps" parameter (maximum characters per
 * second, RFC 4103 Section 6) is parsed and stored for informational
 * purposes; it constrains transmitters, not receivers.
 */

#include "libavutil/avstring.h"

#include "avformat.h"
#include "internal.h"
#include "rtpdec_formats.h"

struct PayloadContext {
    int cps; /**< maximum characters per second, 0 if unspecified */
};

static av_cold int t140_init(AVFormatContext *ctx, int st_index,
                             PayloadContext *data)
{
    if (st_index < 0)
        return 0;

    /* RFC 4103 Section 6: the clock frequency is fixed at 1000 Hz. */
    avpriv_set_pts_info(ctx->streams[st_index], 32, 1, 1000);

    return 0;
}

static int t140_parse_fmtp(AVFormatContext *s, AVStream *stream,
                           PayloadContext *data,
                           const char *attr, const char *value)
{
    if (!strcmp(attr, "cps")) {
        data->cps = atoi(value);
        av_log(s, AV_LOG_DEBUG, "T.140 maximum character rate: %d cps\n",
               data->cps);
    }
    return 0;
}

static int t140_parse_sdp_line(AVFormatContext *ctx, int st_index,
                               PayloadContext *data, const char *line)
{
    const char *p;

    if (st_index < 0)
        return 0;

    if (av_strstart(line, "fmtp:", &p))
        return ff_parse_fmtp(ctx, ctx->streams[st_index], data, p,
                             t140_parse_fmtp);

    return 0;
}

static int t140_handle_packet(AVFormatContext *ctx, PayloadContext *data,
                              AVStream *st, AVPacket *pkt,
                              uint32_t *timestamp, const uint8_t *buf, int len,
                              uint16_t seq, int flags)
{
    int ret;

    /* Empty packets are valid: they are sent in the absence of text to
     * keep NAT bindings open, or carry only redundancy headers when
     * redundancy is in use. They produce no output. */
    if (!len)
        return AVERROR(EAGAIN);

    if ((ret = av_new_packet(pkt, len)) < 0)
        return ret;

    memcpy(pkt->data, buf, len);
    pkt->stream_index = st->index;

    return 0;
}

const RTPDynamicProtocolHandler ff_t140_dynamic_handler = { /* RFC 4103 */
    .enc_name         = "t140",
    .codec_type       = AVMEDIA_TYPE_SUBTITLE,
    .codec_id         = AV_CODEC_ID_TEXT,
    .priv_data_size   = sizeof(PayloadContext),
    .init             = t140_init,
    .parse_sdp_a_line = t140_parse_sdp_line,
    .parse_packet     = t140_handle_packet,
};
