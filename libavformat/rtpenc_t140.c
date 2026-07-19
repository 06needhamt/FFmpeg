/*
 * RTP packetizer for T.140 real-time text payload format (RFC 4103)
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
 * @brief T.140 real-time text RTP packetization (RFC 4103)
 *
 * The payload is the raw UTF-8 encoded T.140 code elements of one text
 * packet, with a fixed 1000 Hz clock. The marker bit is set on the
 * first packet transmitted after an idle period (RFC 4103 Section 4).
 * Oversized input is fragmented on UTF-8 sequence boundaries so that
 * multi-byte characters are never split across packets.
 */

#include "avformat.h"
#include "rtpenc.h"

/* RFC 4103 Section 5.1: the recommended buffering interval is 300 ms;
 * a transmission gap longer than one buffering interval constitutes an
 * idle period. With the fixed 1000 Hz clock, ticks equal milliseconds. */
#define RTP_T140_IDLE_THRESHOLD 300

/* Return the largest prefix of at most max_len bytes that does not end
 * inside a UTF-8 multi-byte sequence. */
static int t140_split_pos(const uint8_t *buf, int max_len)
{
    int i = max_len;

    while (i > 0 && (buf[i] & 0xc0) == 0x80)
        i--;

    /* All of the prefix consisted of continuation bytes: the input is
     * not valid UTF-8, fall back to a plain byte split. */
    return i > 0 ? i : max_len;
}

void ff_rtp_send_t140(AVFormatContext *s1, const uint8_t *buf, int size)
{
    RTPMuxContext *s = s1->priv_data;
    int first = 1;

    s->timestamp = s->cur_timestamp;

    while (size > 0) {
        int len    = size;
        int marker = 0;

        if (len > s->max_payload_size)
            len = t140_split_pos(buf, s->max_payload_size);

        if (first) {
            /* RFC 4103 Section 4: the M bit is set in the first packet
             * after an idle period. */
            marker = !s->t140_started ||
                     s->cur_timestamp - s->t140_last_timestamp >
                         RTP_T140_IDLE_THRESHOLD;
            first  = 0;
        }

        ff_rtp_send_data(s1, buf, len, marker);

        buf  += len;
        size -= len;
    }

    s->t140_started        = 1;
    s->t140_last_timestamp = s->cur_timestamp;
}
