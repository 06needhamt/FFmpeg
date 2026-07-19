/*
 * RTP packetizer for APV payload format (draft-ietf-avtcore-rtp-apv-01)
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
 * @brief APV (Advanced Professional Video, RFC 9924) RTP packetization
 *        code (draft-ietf-avtcore-rtp-apv-01)
 *
 * NOTE: this implements revision -01 of the (not yet finalized) IETF
 * AVTCORE working group draft. The payload header layout or semantics
 * may still change in later revisions.
 *
 * The packetizer operates in the simple mode (OM == 01b): the access
 * unit, with its 32-bit au_size field prepended as required by
 * Section 5.1 of the draft, is fragmented at arbitrary byte positions.
 * Input packets are expected in the framing produced by the APV
 * demuxer and encoder wrappers, i.e. the 'aPv1' signature followed by
 * size-prefixed PBUs, without the au_size field (which is added here).
 *
 * Note that, per Section 5.4 of the draft, the RTP marker bit is set
 * on the *first* packet of each access unit, unlike most video payload
 * formats which mark the last one.
 */

#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "rtpenc.h"

#define RTP_APV_PAYLOAD_HEADER_SIZE 3
#define RTP_APV_AU_SIZE_FIELD_SIZE  4

/* draft-ietf-avtcore-rtp-apv-01, Section 5.5 */
#define RTP_APV_OM_SIMPLE           1

#define RTP_APV_PT_MIDDLE           0
#define RTP_APV_PT_LAST             1
#define RTP_APV_PT_FIRST            2

void ff_rtp_send_apv(AVFormatContext *s1, const uint8_t *buf, int size)
{
    RTPMuxContext *s = s1->priv_data;
    int max_chunk = s->max_payload_size - RTP_APV_PAYLOAD_HEADER_SIZE;
    int64_t total = (int64_t)size + RTP_APV_AU_SIZE_FIELD_SIZE;
    int64_t nb_packets, remaining;
    int first = 1;

    if (max_chunk <= RTP_APV_AU_SIZE_FIELD_SIZE) {
        av_log(s1, AV_LOG_ERROR, "RTP max payload size too small for APV\n");
        return;
    }

    s->timestamp = s->cur_timestamp;

    nb_packets = (total + max_chunk - 1) / max_chunk;
    if (nb_packets - 1 > UINT16_MAX) {
        /* the Fragment Counter is a 16-bit field */
        av_log(s1, AV_LOG_ERROR,
               "APV AU of %d bytes needs too many RTP packets\n", size);
        return;
    }
    remaining = nb_packets;

    while (remaining > 0) {
        uint8_t *ptr = s->buf;
        int pt, chunk;

        remaining--;

        if (first && remaining == 0)
            /* a single packet carrying the entire AU MUST set PT to 01b
             * (draft-ietf-avtcore-rtp-apv-01, Section 5.5) */
            pt = RTP_APV_PT_LAST;
        else if (first)
            pt = RTP_APV_PT_FIRST;
        else if (remaining == 0)
            pt = RTP_APV_PT_LAST;
        else
            pt = RTP_APV_PT_MIDDLE;

        /*
         * write the payload header (draft-ietf-avtcore-rtp-apv-01,
         * Section 5.5):
         *
         *     0                   1                   2
         *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3
         *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *    |V=0|OM |PT |H|S|     FRAGMENT COUNTER (FC)     |
         *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *
         * V = 0, OM = simple mode, H = 0 (no repeated frame header),
         * S = 0 (no static frame header signaling). The FC holds the
         * number of remaining payloads of this AU, reaching 0 on the
         * last one.
         */
        *ptr++ = (RTP_APV_OM_SIMPLE << 4) | (pt << 2);
        AV_WB16(ptr, (uint16_t)remaining);
        ptr += 2;

        if (first) {
            /* the AU is transmitted with its 32-bit au_size field
             * prepended, aligned with the start of the first payload
             * (draft-ietf-avtcore-rtp-apv-01, Section 5.1) */
            AV_WB32(ptr, size);
            ptr += RTP_APV_AU_SIZE_FIELD_SIZE;
            chunk = FFMIN(max_chunk - RTP_APV_AU_SIZE_FIELD_SIZE, size);
        } else {
            chunk = FFMIN(max_chunk, size);
        }

        memcpy(ptr, buf, chunk);
        ptr  += chunk;
        buf  += chunk;
        size -= chunk;

        /* the RTP marker bit is set on the first packet of the AU
         * (draft-ietf-avtcore-rtp-apv-01, Section 5.4) */
        ff_rtp_send_data(s1, s->buf, ptr - s->buf, first);
        first = 0;
    }
}
