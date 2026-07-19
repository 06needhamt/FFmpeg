/*
 * RTP parser for APV payload format (draft-ietf-avtcore-rtp-apv-01)
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
 * @brief APV (Advanced Professional Video, RFC 9924) RTP
 *        depacketization code (draft-ietf-avtcore-rtp-apv-01)
 *
 * NOTE: this implements revision -01 of the (not yet finalized) IETF
 * AVTCORE working group draft. The payload header layout or semantics
 * may still change in later revisions.
 *
 * On the wire, each access unit is carried with its 32-bit au_size
 * field prepended (raw bitstream framing per Appendix A of RFC 9924)
 * and may be fragmented over multiple RTP packets. The RTP marker bit
 * is - unusually - set on the *first* packet of each AU. Reassembly is
 * therefore driven by the au_size field read from the first payload of
 * an AU: bytes are accumulated until the announced AU size has been
 * reached, which works for both the simple and the low-delay operation
 * mode without mode-specific logic. The au_size field itself is
 * stripped, so the emitted packets carry exactly the framing produced
 * by the APV demuxer and expected by the APV decoder (the 'aPv1'
 * signature followed by size-prefixed PBUs).
 */

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "rtpdec.h"
#include "rtpdec_formats.h"

#define RTP_APV_PAYLOAD_HEADER_SIZE 3
#define RTP_APV_AU_SIZE_FIELD_SIZE  4

/* draft-ietf-avtcore-rtp-apv-01, Section 5.5 */
#define RTP_APV_OM_SIMPLE           1
#define RTP_APV_OM_LOW_DELAY        2

/* maximum tolerated AU size, matching the APV raw demuxer's bound */
#define RTP_APV_MAX_AU_SIZE         (1 << 26)

struct PayloadContext {
    int profile_id;
    int level_id;
    int band_id;

    /* access unit reassembly state */
    AVIOContext *au_buf;
    uint32_t au_size;       ///< announced size of the AU being reassembled
    uint32_t au_received;   ///< bytes accumulated so far
    uint32_t timestamp;     ///< RTP timestamp of the AU being reassembled
    uint16_t expected_seq;  ///< sequence number of the previous packet
    int seq_valid;
};

static av_cold int apv_sdp_parse_fmtp_config(AVFormatContext *s,
                                             AVStream *stream,
                                             PayloadContext *apv_data,
                                             const char *attr, const char *value)
{
    /* profile-id: default 33 (Baseline profile)
     * level-id:   default 153 (level 5.1)
     * band-id:    default 0
     * (draft-ietf-avtcore-rtp-apv-01, Section 6.1.1) */
    if (!strcmp(attr, "profile-id")) {
        apv_data->profile_id = atoi(value);
        av_log(s, AV_LOG_TRACE, "SDP: found profile-id: %d\n",
               apv_data->profile_id);
    } else if (!strcmp(attr, "level-id")) {
        apv_data->level_id = atoi(value);
        av_log(s, AV_LOG_TRACE, "SDP: found level-id: %d\n",
               apv_data->level_id);
    } else if (!strcmp(attr, "band-id")) {
        apv_data->band_id = atoi(value);
        av_log(s, AV_LOG_TRACE, "SDP: found band-id: %d\n",
               apv_data->band_id);
    }

    return 0;
}

static av_cold int apv_parse_sdp_line(AVFormatContext *ctx, int st_index,
                                      PayloadContext *apv_data, const char *line)
{
    const char *sdp_line_ptr = line;

    if (st_index < 0)
        return 0;

    if (av_strstart(sdp_line_ptr, "fmtp:", &sdp_line_ptr))
        return ff_parse_fmtp(ctx, ctx->streams[st_index], apv_data,
                             sdp_line_ptr, apv_sdp_parse_fmtp_config);

    return 0;
}

static void apv_reset_au_buffer(PayloadContext *rtp_apv_ctx)
{
    uint8_t *dummy;

    if (rtp_apv_ctx->au_buf) {
        avio_close_dyn_buf(rtp_apv_ctx->au_buf, &dummy);
        av_free(dummy);
        rtp_apv_ctx->au_buf = NULL;
    }
    rtp_apv_ctx->au_size     = 0;
    rtp_apv_ctx->au_received = 0;
}

static int apv_handle_packet(AVFormatContext *ctx, PayloadContext *rtp_apv_ctx,
                             AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                             const uint8_t *buf, int len, uint16_t seq,
                             int flags)
{
    int version, om, res;

    if (len < RTP_APV_PAYLOAD_HEADER_SIZE + 1) {
        av_log(ctx, AV_LOG_ERROR, "Too short RTP/APV packet, got %d bytes\n",
               len);
        return AVERROR_INVALIDDATA;
    }

    /*
     * decode the payload header (draft-ietf-avtcore-rtp-apv-01,
     * Section 5.5):
     *
     *     0                   1                   2
     *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3
     *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *    |V=0|OM |PT |H|S|     FRAGMENT COUNTER (FC)     |
     *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *
     * The PT, H and S fields as well as the FC are not needed for
     * reassembly here, which is driven by the au_size field instead
     * (see the file level comment); they are decoded for tracing only.
     */
    version = (buf[0] >> 6) & 0x03;
    om      = (buf[0] >> 4) & 0x03;

    if (version != 0) {
        avpriv_report_missing_feature(ctx, "RTP/APV payload header version %d",
                                      version);
        return AVERROR_PATCHWELCOME;
    }

    if (om != RTP_APV_OM_SIMPLE && om != RTP_APV_OM_LOW_DELAY) {
        av_log(ctx, AV_LOG_ERROR, "Reserved RTP/APV operation mode %d\n", om);
        return AVERROR_INVALIDDATA;
    }

    av_log(ctx, AV_LOG_TRACE,
           "RTP/APV packet: OM %d, PT %d, FC %d, M %d, %d bytes\n",
           om, (buf[0] >> 2) & 0x03, AV_RB16(buf + 1),
           !!(flags & RTP_FLAG_MARKER), len);

    buf += RTP_APV_PAYLOAD_HEADER_SIZE;
    len -= RTP_APV_PAYLOAD_HEADER_SIZE;

    /* the RTP marker bit is set on the packet carrying the first byte
     * of the au_size field of an AU (draft-ietf-avtcore-rtp-apv-01,
     * Section 5.4) */
    if (flags & RTP_FLAG_MARKER) {
        uint32_t au_size;

        if (rtp_apv_ctx->au_buf) {
            av_log(ctx, AV_LOG_WARNING,
                   "Discarding incomplete APV access unit\n");
            apv_reset_au_buffer(rtp_apv_ctx);
        }

        if (len < RTP_APV_AU_SIZE_FIELD_SIZE + 1) {
            av_log(ctx, AV_LOG_ERROR,
                   "Too short first RTP/APV packet of an AU (%d bytes)\n",
                   len);
            return AVERROR_INVALIDDATA;
        }

        /* the first payload of an AU starts with the 32-bit au_size
         * field, which is stripped from the output */
        au_size = AV_RB32(buf);
        buf += RTP_APV_AU_SIZE_FIELD_SIZE;
        len -= RTP_APV_AU_SIZE_FIELD_SIZE;

        if (au_size < 24 || au_size > RTP_APV_MAX_AU_SIZE) {
            av_log(ctx, AV_LOG_ERROR, "Invalid APV AU size %"PRIu32"\n",
                   au_size);
            return AVERROR_INVALIDDATA;
        }

        if ((res = avio_open_dyn_buf(&rtp_apv_ctx->au_buf)) < 0)
            return res;
        rtp_apv_ctx->au_size     = au_size;
        rtp_apv_ctx->au_received = 0;
        rtp_apv_ctx->timestamp   = *timestamp;
    } else {
        if (!rtp_apv_ctx->au_buf) {
            /* resynchronization after a loss or at startup; the loss
             * itself was already reported */
            av_log(ctx, AV_LOG_TRACE,
                   "Dropping RTP/APV packet without preceding AU start\n");
            return AVERROR(EAGAIN);
        }
        if (rtp_apv_ctx->seq_valid &&
            seq != (uint16_t)(rtp_apv_ctx->expected_seq + 1)) {
            av_log(ctx, AV_LOG_WARNING,
                   "Lost RTP packet within an APV AU, dropping the AU\n");
            apv_reset_au_buffer(rtp_apv_ctx);
            return AVERROR(EAGAIN);
        }
        if (rtp_apv_ctx->timestamp != *timestamp) {
            av_log(ctx, AV_LOG_WARNING,
                   "RTP/APV timestamp changed within an AU, dropping the AU\n");
            apv_reset_au_buffer(rtp_apv_ctx);
            return AVERROR(EAGAIN);
        }
    }

    rtp_apv_ctx->expected_seq = seq;
    rtp_apv_ctx->seq_valid    = 1;

    if (rtp_apv_ctx->au_received + (uint32_t)len > rtp_apv_ctx->au_size) {
        av_log(ctx, AV_LOG_ERROR,
               "RTP/APV payload exceeds the announced AU size "
               "(%"PRIu32" + %d > %"PRIu32")\n",
               rtp_apv_ctx->au_received, len, rtp_apv_ctx->au_size);
        apv_reset_au_buffer(rtp_apv_ctx);
        return AVERROR_INVALIDDATA;
    }

    avio_write(rtp_apv_ctx->au_buf, buf, len);
    rtp_apv_ctx->au_received += len;

    if (rtp_apv_ctx->au_received == rtp_apv_ctx->au_size) {
        res = ff_rtp_finalize_packet(pkt, &rtp_apv_ctx->au_buf, st->index);
        rtp_apv_ctx->au_size     = 0;
        rtp_apv_ctx->au_received = 0;
        if (res < 0)
            return res;
        /* APV is intra-only; every access unit is a sync point */
        pkt->flags |= AV_PKT_FLAG_KEY;
        return 0;
    }

    return AVERROR(EAGAIN);
}

static av_cold void apv_close_context(PayloadContext *apv_data)
{
    apv_reset_au_buffer(apv_data);
}

const RTPDynamicProtocolHandler ff_apv_dynamic_handler = {
    .enc_name         = "apv",
    .codec_type       = AVMEDIA_TYPE_VIDEO,
    .codec_id         = AV_CODEC_ID_APV,
    .need_parsing     = AVSTREAM_PARSE_HEADERS,
    .priv_data_size   = sizeof(PayloadContext),
    .parse_sdp_a_line = apv_parse_sdp_line,
    .close            = apv_close_context,
    .parse_packet     = apv_handle_packet,
};
