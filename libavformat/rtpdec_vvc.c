/*
 * RTP parser for VVC/H.266 payload format (RFC 9328)
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
 * @brief VVC/H.266 RTP depacketization code (RFC 9328)
 *
 * The de-packetization process turns the three RFC 9328 payload
 * structures (single NAL unit packets, aggregation packets with
 * Type == 28 and fragmentation units with Type == 29) back into an
 * Annex B NAL unit stream that is handed to the VVC parser/decoder.
 */

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "internal.h"
#include "rtpdec.h"
#include "rtpdec_formats.h"

#define RTP_VVC_PAYLOAD_HEADER_SIZE       2
#define RTP_VVC_FU_HEADER_SIZE            1
#define RTP_VVC_DONL_FIELD_SIZE           2
#define RTP_VVC_AP_NALU_LENGTH_FIELD_SIZE 2

/* RFC 9328, Section 4.3.2 / 4.3.3 */
#define RTP_VVC_AP_NAL_TYPE               28
#define RTP_VVC_FU_NAL_TYPE               29

/* Highest NAL unit type that may be passed to the decoder
 * (RFC 9328, Section 6: types 0..27 may be passed on, NAL-unit-like
 * structures 28..31 must not). */
#define VVC_MAX_REAL_NAL_UNIT_TYPE        27

/* SDP out-of-band signaling data */
struct PayloadContext {
    int using_donl_field;
    int profile_id;
    int tier_flag;
    int level_id;
    uint8_t *dci, *vps, *sps, *pps, *sei;
    int dci_size, vps_size, sps_size, pps_size, sei_size;
};

static const uint8_t start_sequence[] = { 0x00, 0x00, 0x00, 0x01 };

static av_cold int vvc_sdp_parse_fmtp_config(AVFormatContext *s,
                                             AVStream *stream,
                                             PayloadContext *vvc_data,
                                             const char *attr, const char *value)
{
    /* profile-id: 0-127, default 1 (Main 10) */
    if (!strcmp(attr, "profile-id")) {
        vvc_data->profile_id = atoi(value);
        av_log(s, AV_LOG_TRACE, "SDP: found profile-id: %d\n",
               vvc_data->profile_id);
    }

    /* tier-flag: 0-1, default 0 */
    if (!strcmp(attr, "tier-flag")) {
        vvc_data->tier_flag = atoi(value);
        av_log(s, AV_LOG_TRACE, "SDP: found tier-flag: %d\n",
               vvc_data->tier_flag);
    }

    /* level-id: 0-255, default 51 (level 3.1) */
    if (!strcmp(attr, "level-id")) {
        vvc_data->level_id = atoi(value);
        av_log(s, AV_LOG_TRACE, "SDP: found level-id: %d\n",
               vvc_data->level_id);
    }

    /* sub-profile-id: comma-separated list of base64 without padding */
    /* interop-constraints: [base64] */
    /* sprop-sublayer-id: 0-6, highest allowed value of TID, default: 6 */
    /* sprop-ols-id: 0-256 */
    /* recv-sublayer-id: 0-6 */
    /* recv-ols-id: 0-256 */
    /* max-recv-level-id: 0-255 */

    /* sprop-dci: [base64]
     * sprop-vps: comma-separated list of [base64]
     * sprop-sps: comma-separated list of [base64]
     * sprop-pps: comma-separated list of [base64]
     * sprop-sei: comma-separated list of [base64] */
    if (!strcmp(attr, "sprop-dci") || !strcmp(attr, "sprop-vps") ||
        !strcmp(attr, "sprop-sps") || !strcmp(attr, "sprop-pps") ||
        !strcmp(attr, "sprop-sei")) {
        uint8_t **data_ptr = NULL;
        int *size_ptr = NULL;
        if (!strcmp(attr, "sprop-dci")) {
            data_ptr = &vvc_data->dci;
            size_ptr = &vvc_data->dci_size;
        } else if (!strcmp(attr, "sprop-vps")) {
            data_ptr = &vvc_data->vps;
            size_ptr = &vvc_data->vps_size;
        } else if (!strcmp(attr, "sprop-sps")) {
            data_ptr = &vvc_data->sps;
            size_ptr = &vvc_data->sps_size;
        } else if (!strcmp(attr, "sprop-pps")) {
            data_ptr = &vvc_data->pps;
            size_ptr = &vvc_data->pps_size;
        } else if (!strcmp(attr, "sprop-sei")) {
            data_ptr = &vvc_data->sei;
            size_ptr = &vvc_data->sei_size;
        } else
            av_assert0(0);

        ff_h264_parse_sprop_parameter_sets(s, data_ptr, size_ptr, value);
    }

    /* max-lsr, max-fps */

    /* sprop-max-don-diff: 0-32767
     *
     * When greater than 0, the DONL field is present in single NAL unit
     * packets, in the first aggregation unit of APs, and in FUs with the
     * S bit equal to 1 (RFC 9328, Sections 4.3.1-4.3.3). */
    if (!strcmp(attr, "sprop-max-don-diff")) {
        if (atoi(value) > 0)
            vvc_data->using_donl_field = 1;
        av_log(s, AV_LOG_TRACE,
               "Found sprop-max-don-diff in SDP, DON field usage is: %d\n",
               vvc_data->using_donl_field);
    }

    /* sprop-depack-buf-bytes: 0-4294967295 */
    /* depack-buf-cap: 1-4294967295 */

    return 0;
}

static av_cold int vvc_parse_sdp_line(AVFormatContext *ctx, int st_index,
                                      PayloadContext *vvc_data, const char *line)
{
    AVStream *current_stream;
    AVCodecParameters *par;
    const char *sdp_line_ptr = line;

    if (st_index < 0)
        return 0;

    current_stream = ctx->streams[st_index];
    par = current_stream->codecpar;

    if (av_strstart(sdp_line_ptr, "framesize:", &sdp_line_ptr)) {
        ff_h264_parse_framesize(par, sdp_line_ptr);
    } else if (av_strstart(sdp_line_ptr, "fmtp:", &sdp_line_ptr)) {
        int ret = ff_parse_fmtp(ctx, current_stream, vvc_data, sdp_line_ptr,
                                vvc_sdp_parse_fmtp_config);
        if (vvc_data->dci_size || vvc_data->vps_size || vvc_data->sps_size ||
            vvc_data->pps_size || vvc_data->sei_size) {
            par->extradata_size = vvc_data->dci_size + vvc_data->vps_size +
                                  vvc_data->sps_size + vvc_data->pps_size +
                                  vvc_data->sei_size;
            if ((ret = ff_alloc_extradata(par, par->extradata_size)) >= 0) {
                int pos = 0;
                memcpy(par->extradata + pos, vvc_data->dci, vvc_data->dci_size);
                pos += vvc_data->dci_size;
                memcpy(par->extradata + pos, vvc_data->vps, vvc_data->vps_size);
                pos += vvc_data->vps_size;
                memcpy(par->extradata + pos, vvc_data->sps, vvc_data->sps_size);
                pos += vvc_data->sps_size;
                memcpy(par->extradata + pos, vvc_data->pps, vvc_data->pps_size);
                pos += vvc_data->pps_size;
                memcpy(par->extradata + pos, vvc_data->sei, vvc_data->sei_size);
            }

            av_freep(&vvc_data->dci);
            av_freep(&vvc_data->vps);
            av_freep(&vvc_data->sps);
            av_freep(&vvc_data->pps);
            av_freep(&vvc_data->sei);
            vvc_data->dci_size = 0;
            vvc_data->vps_size = 0;
            vvc_data->sps_size = 0;
            vvc_data->pps_size = 0;
            vvc_data->sei_size = 0;
        }
        return ret;
    }

    return 0;
}

static int vvc_handle_packet(AVFormatContext *ctx, PayloadContext *rtp_vvc_ctx,
                             AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                             const uint8_t *buf, int len, uint16_t seq,
                             int flags)
{
    const uint8_t *rtp_pl = buf;
    int tid, lid, nal_type;
    int first_fragment, last_fragment, fu_type;
    uint8_t new_nal_header[2];
    int res = 0;

    /* sanity check for size of input packet: 1 byte payload at least */
    if (len < RTP_VVC_PAYLOAD_HEADER_SIZE + 1) {
        av_log(ctx, AV_LOG_ERROR, "Too short RTP/VVC packet, got %d bytes\n",
               len);
        return AVERROR_INVALIDDATA;
    }

    /*
     * decode the VVC payload header according to Section 4.2 of RFC 9328,
     * which shares its structure with the NAL unit header (Section 1.1.4):
     *
     *    0                   1
     *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
     *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *   |F|Z|  LayerID  |  Type   | TID |
     *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *
     *      Forbidden zero (F): 1 bit
     *      Reserved zero (Z): 1 bit
     *      NUH layer ID (LayerID): 6 bits
     *      NAL unit type (Type): 5 bits
     *      NUH temporal ID plus 1 (TID): 3 bits
     */
    lid      =  buf[0] & 0x3f;
    nal_type = (buf[1] >> 3) & 0x1f;
    tid      =  buf[1] & 0x07;

    /* A TID value of 0 is illegal (RFC 9328, Section 1.1.4) */
    if (!tid) {
        av_log(ctx, AV_LOG_ERROR, "Illegal temporal ID in RTP/VVC packet\n");
        return AVERROR_INVALIDDATA;
    }

    av_log(ctx, AV_LOG_TRACE,
           "RTP/VVC packet: type %d, LayerId %d, TID %d, %d bytes\n",
           nal_type, lid, tid, len);

    switch (nal_type) {
    /* aggregation packet (AP) - with two or more NAL units */
    case RTP_VVC_AP_NAL_TYPE:
        /* pass the VVC payload header */
        buf += RTP_VVC_PAYLOAD_HEADER_SIZE;
        len -= RTP_VVC_PAYLOAD_HEADER_SIZE;

        /* pass the DONL field of the first aggregation unit; unlike
         * RFC 7798, subsequent aggregation units carry no DOND field in
         * RFC 9328 (Section 4.3.2), hence start_skip is always 0 */
        if (rtp_vvc_ctx->using_donl_field) {
            buf += RTP_VVC_DONL_FIELD_SIZE;
            len -= RTP_VVC_DONL_FIELD_SIZE;
        }

        res = ff_h264_handle_aggregated_packet(ctx, rtp_vvc_ctx, pkt, buf, len,
                                               0, NULL, 0);
        if (res < 0)
            return res;
        break;
    /* fragmentation unit (FU) */
    case RTP_VVC_FU_NAL_TYPE:
        /* pass the VVC payload header */
        buf += RTP_VVC_PAYLOAD_HEADER_SIZE;
        len -= RTP_VVC_PAYLOAD_HEADER_SIZE;

        /*
         *    decode the FU header (RFC 9328, Section 4.3.3)
         *
         *     0 1 2 3 4 5 6 7
         *    +-+-+-+-+-+-+-+-+
         *    |S|E|P|  FuType |
         *    +---------------+
         *
         *       Start fragment (S): 1 bit
         *       End fragment (E): 1 bit
         *       Last FU of the last VCL NAL unit of the picture (P): 1 bit
         *       FuType: 5 bits
         */
        first_fragment = buf[0] & 0x80;
        last_fragment  = buf[0] & 0x40;
        fu_type        = buf[0] & 0x1f;

        /* pass the VVC FU header */
        buf += RTP_VVC_FU_HEADER_SIZE;
        len -= RTP_VVC_FU_HEADER_SIZE;

        /* pass the DONL field, present only in the first FU of a
         * fragmented NAL unit (RFC 9328, Section 4.3.3) */
        if (rtp_vvc_ctx->using_donl_field && first_fragment) {
            buf += RTP_VVC_DONL_FIELD_SIZE;
            len -= RTP_VVC_DONL_FIELD_SIZE;
        }

        av_log(ctx, AV_LOG_TRACE, " FU type %d with %d bytes\n", fu_type, len);

        /* sanity check for size of input packet: 1 byte payload at least;
         * an FU payload MUST NOT be empty (RFC 9328, Section 4.3.3) */
        if (len <= 0) {
            if (len < 0) {
                av_log(ctx, AV_LOG_ERROR,
                       "Too short RTP/VVC packet, got %d bytes of NAL unit type %d\n",
                       len, nal_type);
                return AVERROR_INVALIDDATA;
            } else {
                return AVERROR(EAGAIN);
            }
        }

        /* A non-fragmented NAL unit MUST NOT be transmitted in one FU
         * (RFC 9328, Section 4.3.3) */
        if (first_fragment && last_fragment) {
            av_log(ctx, AV_LOG_ERROR,
                   "Illegal combination of S and E bit in RTP/VVC packet\n");
            return AVERROR_INVALIDDATA;
        }

        /* reconstruct the NAL unit header of the fragmented NAL unit from
         * the F, LayerId and TID fields of the payload header and the
         * FuType field of the FU header (RFC 9328, Section 6) */
        new_nal_header[0] = rtp_pl[0];
        new_nal_header[1] = (fu_type << 3) | (rtp_pl[1] & 0x07);

        res = ff_h264_handle_frag_packet(pkt, buf, len, first_fragment,
                                         new_nal_header,
                                         sizeof(new_nal_header));

        break;
    /* NAL-unit-like structures with type values 30 and 31 are reserved
     * for future extensions and MUST NOT be passed to the decoder */
    case 30:
    case 31:
        avpriv_report_missing_feature(ctx,
                                      "RTP/VVC NAL unit type %d", nal_type);
        res = AVERROR_PATCHWELCOME;
        break;
    /* single NAL unit packet (RFC 9328, Section 4.3.1) */
    default:
        av_assert1(nal_type <= VVC_MAX_REAL_NAL_UNIT_TYPE);

        if (rtp_vvc_ctx->using_donl_field) {
            /* the conditional DONL field is located between the payload
             * header (which doubles as the NAL unit header) and the NAL
             * unit payload data and has to be removed */
            if (len < RTP_VVC_PAYLOAD_HEADER_SIZE + RTP_VVC_DONL_FIELD_SIZE + 1) {
                av_log(ctx, AV_LOG_ERROR,
                       "Too short RTP/VVC packet, got %d bytes\n", len);
                return AVERROR_INVALIDDATA;
            }
            if ((res = av_new_packet(pkt, sizeof(start_sequence) + len -
                                          RTP_VVC_DONL_FIELD_SIZE)) < 0)
                return res;
            memcpy(pkt->data, start_sequence, sizeof(start_sequence));
            memcpy(pkt->data + sizeof(start_sequence), buf,
                   RTP_VVC_PAYLOAD_HEADER_SIZE);
            memcpy(pkt->data + sizeof(start_sequence) +
                   RTP_VVC_PAYLOAD_HEADER_SIZE,
                   buf + RTP_VVC_PAYLOAD_HEADER_SIZE + RTP_VVC_DONL_FIELD_SIZE,
                   len - RTP_VVC_PAYLOAD_HEADER_SIZE - RTP_VVC_DONL_FIELD_SIZE);
        } else {
            /* create A/V packet: start sequence + NAL unit */
            if ((res = av_new_packet(pkt, sizeof(start_sequence) + len)) < 0)
                return res;
            memcpy(pkt->data, start_sequence, sizeof(start_sequence));
            memcpy(pkt->data + sizeof(start_sequence), buf, len);
        }

        break;
    }

    pkt->stream_index = st->index;

    return res;
}

static av_cold void vvc_close_context(PayloadContext *vvc_data)
{
    av_freep(&vvc_data->dci);
    av_freep(&vvc_data->vps);
    av_freep(&vvc_data->sps);
    av_freep(&vvc_data->pps);
    av_freep(&vvc_data->sei);
}

const RTPDynamicProtocolHandler ff_vvc_dynamic_handler = {
    .enc_name         = "H266",
    .codec_type       = AVMEDIA_TYPE_VIDEO,
    .codec_id         = AV_CODEC_ID_VVC,
    .need_parsing     = AVSTREAM_PARSE_FULL,
    .priv_data_size   = sizeof(PayloadContext),
    .parse_sdp_a_line = vvc_parse_sdp_line,
    .close            = vvc_close_context,
    .parse_packet     = vvc_handle_packet,
};
