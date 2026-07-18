/*
 * RTP packetizer for VVC/H.266 payload format (RFC 9328)
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
 * @brief VVC/H.266 RTP packetization code (RFC 9328)
 *
 * Each input packet (one access unit) is split into NAL units. Small
 * NAL units are aggregated into Aggregation Packets (Type == 28), NAL
 * units that fit into the payload on their own are sent as single NAL
 * unit packets and NAL units exceeding the maximum payload size are
 * fragmented into Fragmentation Units (Type == 29). The RTP marker bit
 * is set on the last packet of an access unit. DONL fields are never
 * produced (transmission order equals decoding order, i.e.
 * sprop-max-don-diff == 0).
 */

#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "nal.h"
#include "rtpenc.h"

#define RTP_VVC_PAYLOAD_HEADER_SIZE       2
#define RTP_VVC_FU_HEADER_SIZE            1
#define RTP_VVC_AP_NALU_LENGTH_FIELD_SIZE 2

/* RFC 9328, Section 4.3.2 / 4.3.3 */
#define RTP_VVC_AP_NAL_TYPE               28
#define RTP_VVC_FU_NAL_TYPE               29

/* NAL unit types 0..11 are VCL NAL units (Table 5 of H.266) */
#define VVC_NAL_TYPE_IS_VCL(t)            ((t) <= 11)

static void flush_vvc_buffered(AVFormatContext *s1, int last)
{
    RTPMuxContext *s = s1->priv_data;
    if (s->buf_ptr != s->buf) {
        // If we're only sending one single NAL unit, send it as such, skip
        // the AP framing (2 bytes payload header + 2 bytes NALU size)
        if (s->buffered_nals == 1) {
            ff_rtp_send_data(s1, s->buf +
                             RTP_VVC_PAYLOAD_HEADER_SIZE +
                             RTP_VVC_AP_NALU_LENGTH_FIELD_SIZE,
                             s->buf_ptr - s->buf -
                             RTP_VVC_PAYLOAD_HEADER_SIZE -
                             RTP_VVC_AP_NALU_LENGTH_FIELD_SIZE, last);
        } else
            ff_rtp_send_data(s1, s->buf, s->buf_ptr - s->buf, last);
    }
    s->buf_ptr = s->buf;
    s->buffered_nals = 0;
}

static void vvc_nal_send(AVFormatContext *s1, const uint8_t *buf, int size,
                         int last)
{
    RTPMuxContext *s = s1->priv_data;
    int nal_type, nal_lid, nal_tid, nal_fbit;

    if (size < RTP_VVC_PAYLOAD_HEADER_SIZE) {
        av_log(s1, AV_LOG_ERROR, "Too short VVC NAL unit (%d bytes)\n", size);
        return;
    }

    /*
     * decode the NAL unit header (RFC 9328, Section 1.1.4):
     *
     *    0                   1
     *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
     *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *   |F|Z|  LayerID  |  Type   | TID |
     *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     */
    nal_fbit =  buf[0] & 0x80;
    nal_lid  =  buf[0] & 0x3f;
    nal_type = (buf[1] >> 3) & 0x1f;
    nal_tid  =  buf[1] & 0x07;

    av_log(s1, AV_LOG_DEBUG, "Sending NAL type %d of len %d M=%d\n",
           nal_type, size, last);

    if (!nal_tid) {
        av_log(s1, AV_LOG_ERROR,
               "Illegal temporal ID 0 in VVC NAL unit header\n");
        return;
    }

    if (size <= s->max_payload_size) {
        int buffered_size = s->buf_ptr - s->buf;

        // Flush buffered NAL units if the current unit doesn't fit
        if (buffered_size + RTP_VVC_AP_NALU_LENGTH_FIELD_SIZE + size >
            s->max_payload_size) {
            flush_vvc_buffered(s1, 0);
            buffered_size = 0;
        }
        // If the NAL unit fits including the framing (2 bytes length plus,
        // for the first unit, the 2 byte AP payload header), write the unit
        // to the buffer as an AP packet, otherwise flush and send as a
        // single NAL unit packet.
        if (buffered_size + RTP_VVC_AP_NALU_LENGTH_FIELD_SIZE +
            RTP_VVC_PAYLOAD_HEADER_SIZE + size <= s->max_payload_size) {
            if (buffered_size == 0) {
                /*
                 * initialize the AP payload header (RFC 9328,
                 * Section 4.3.2):
                 *
                 *   F       = 0 unless the F bit of any aggregated NAL
                 *             unit is 1
                 *   Z       = 0
                 *   LayerId = lowest LayerId of all aggregated NAL units
                 *   Type    = 28 (aggregation packet (AP))
                 *   TID     = lowest TID of all aggregated NAL units
                 */
                *s->buf_ptr++ = nal_fbit | nal_lid;
                *s->buf_ptr++ = (RTP_VVC_AP_NAL_TYPE << 3) | nal_tid;
            } else {
                /* update F, LayerId and TID for the new aggregated unit */
                s->buf[0] = ((s->buf[0] | nal_fbit) & 0xc0) |
                            FFMIN(s->buf[0] & 0x3f, nal_lid);
                s->buf[1] = (RTP_VVC_AP_NAL_TYPE << 3) |
                            FFMIN(s->buf[1] & 0x07, nal_tid);
            }
            AV_WB16(s->buf_ptr, size);
            s->buf_ptr += RTP_VVC_AP_NALU_LENGTH_FIELD_SIZE;
            memcpy(s->buf_ptr, buf, size);
            s->buf_ptr += size;
            s->buffered_nals++;
        } else {
            flush_vvc_buffered(s1, 0);
            ff_rtp_send_data(s1, buf, size, last);
        }
    } else {
        int header_size = RTP_VVC_PAYLOAD_HEADER_SIZE + RTP_VVC_FU_HEADER_SIZE;

        flush_vvc_buffered(s1, 0);

        av_log(s1, AV_LOG_DEBUG, "NAL size %d > %d\n", size,
               s->max_payload_size);

        /*
         * create the FU payload header (RFC 9328, Section 4.3.3): the
         * F, LayerId and TID fields MUST be equal to the corresponding
         * fields of the fragmented NAL unit, the Type field MUST be
         * equal to 29
         */
        s->buf[0] = nal_fbit | nal_lid;
        s->buf[1] = (RTP_VVC_FU_NAL_TYPE << 3) | nal_tid;

        /*
         *     create the FU header
         *
         *     0 1 2 3 4 5 6 7
         *    +-+-+-+-+-+-+-+-+
         *    |S|E|P|  FuType |
         *    +---------------+
         *
         *       S      = 1 for the first fragment, 0 otherwise
         *       E      = 1 for the last fragment, 0 otherwise
         *       P      = 1 for the last FU of the last VCL NAL unit of
         *                a coded picture, 0 otherwise
         *       FuType = NAL unit type of the fragmented NAL unit
         */
        s->buf[2] = 0x80 | nal_type;

        /* the NAL unit header of the fragmented NAL unit is not included
         * in the FU payload, but conveyed through the payload header and
         * the FU header instead */
        buf  += RTP_VVC_PAYLOAD_HEADER_SIZE;
        size -= RTP_VVC_PAYLOAD_HEADER_SIZE;

        while (size + header_size > s->max_payload_size) {
            memcpy(&s->buf[header_size], buf,
                   s->max_payload_size - header_size);
            ff_rtp_send_data(s1, s->buf, s->max_payload_size, 0);
            buf  += s->max_payload_size - header_size;
            size -= s->max_payload_size - header_size;
            /* clear the S bit for all fragments but the first */
            s->buf[2] &= ~0x80;
        }
        /* set the E bit: mark as last fragment */
        s->buf[2] |= 0x40;
        /* set the P bit if this is the last FU of the last VCL NAL unit
         * of the coded picture */
        if (last && VVC_NAL_TYPE_IS_VCL(nal_type))
            s->buf[2] |= 0x20;
        memcpy(&s->buf[header_size], buf, size);
        ff_rtp_send_data(s1, s->buf, size + header_size, last);
    }
}

void ff_rtp_send_vvc(AVFormatContext *s1, const uint8_t *buf1, int size)
{
    const uint8_t *r, *end = buf1 + size;
    RTPMuxContext *s = s1->priv_data;

    s->timestamp = s->cur_timestamp;
    s->buf_ptr   = s->buf;
    if (s->nal_length_size)
        r = ff_nal_mp4_find_startcode(buf1, end, s->nal_length_size) ? buf1 : end;
    else
        r = ff_nal_find_startcode(buf1, end);
    while (r < end) {
        const uint8_t *r1;

        if (s->nal_length_size) {
            r1 = ff_nal_mp4_find_startcode(r, end, s->nal_length_size);
            if (!r1)
                r1 = end;
            // Check that the last unit is not truncated
            if (r1 - r < s->nal_length_size)
                break;
            r += s->nal_length_size;
        } else {
            while (!*(r++));
            r1 = ff_nal_find_startcode(r, end);
        }
        vvc_nal_send(s1, r, r1 - r, r1 == end);
        r = r1;
    }
    flush_vvc_buffered(s1, 1);
}
