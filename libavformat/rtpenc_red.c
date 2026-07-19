/*
 * RTP packetization for T.140 text with RED redundancy
 * (RFC 4103, RFC 2198, RFC 9071)
 * Copyright (c) 2026 Thomas Needham
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
 * @brief RTP muxer for T.140 real-time text (RFC 4103), optionally
 *        encapsulated in RED (RFC 2198) carrying the last N T.140
 *        payloads as same-codec redundancy ("text/red").
 * @author Thomas Needham
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "rtpenc.h"

/* Marker is set on the first packet sent after an idle period
 * (RFC 4103 section 5.2); clock rate is 1000 Hz. */
#define T140_IDLE_THRESHOLD 300

#define RED_HDR_SIZE      4
#define RED_MAX_TS_OFFSET 0x3fff
#define RED_MAX_BLOCK_LEN 0x3ff

static void t140_red_push(RTPMuxContext *s, const uint8_t *buf, int len,
                          uint32_t ts)
{
    int gens = s->t140_red;

    if (len > RED_MAX_BLOCK_LEN)
        return; /* too large to ever be carried as a redundant block */

    if (s->t140_red_count == gens) {
        av_freep(&s->t140_red_buf[0]);
        memmove(&s->t140_red_buf[0], &s->t140_red_buf[1],
                (gens - 1) * sizeof(s->t140_red_buf[0]));
        memmove(&s->t140_red_len[0], &s->t140_red_len[1],
                (gens - 1) * sizeof(s->t140_red_len[0]));
        memmove(&s->t140_red_ts[0], &s->t140_red_ts[1],
                (gens - 1) * sizeof(s->t140_red_ts[0]));
        s->t140_red_count--;
    }
    s->t140_red_buf[s->t140_red_count] = av_memdup(buf, FFMAX(len, 1));
    if (!s->t140_red_buf[s->t140_red_count])
        return;
    s->t140_red_len[s->t140_red_count] = len;
    s->t140_red_ts[s->t140_red_count]  = ts;
    s->t140_red_count++;
}

/**
 * Find the largest prefix of buf, at most max_len bytes, that does not
 * split a UTF-8 multi-byte sequence (T.140 text is UTF-8; RFC 4103
 * section 6 requires fragmentation on character boundaries).
 */
static int t140_utf8_boundary(const uint8_t *buf, int size, int max_len)
{
    int len = FFMIN(size, max_len);

    if (len == size)
        return len;
    while (len > 0 && (buf[len] & 0xc0) == 0x80)
        len--;
    return len > 0 ? len : FFMIN(size, max_len);
}

static void t140_send_fragment(AVFormatContext *s1, const uint8_t *buf,
                               int len, int marker)
{
    RTPMuxContext *s = s1->priv_data;
    uint8_t *out = s->buf;
    int include[FF_RTP_T140_RED_MAX_GEN];
    int i;

    if (s->t140_red > 0) {
        /* decide once which stored generations are carried, so the
         * header chain and the data area always agree */
        for (i = 0; i < s->t140_red_count; i++) {
            uint32_t off = s->timestamp - s->t140_red_ts[i];
            include[i] = off <= RED_MAX_TS_OFFSET;
        }
        /* RED block header chain, oldest generation first */
        for (i = 0; i < s->t140_red_count; i++) {
            uint32_t off = s->timestamp - s->t140_red_ts[i];
            if (!include[i])
                continue;
            out[0] = 0x80 | s->t140_red_pt;
            AV_WB24(out + 1, (off << 10) | s->t140_red_len[i]);
            out += RED_HDR_SIZE;
        }
        *out++ = s->t140_red_pt; /* final header, F=0 */
        /* redundant data, matching the header chain */
        for (i = 0; i < s->t140_red_count; i++) {
            if (!include[i])
                continue;
            memcpy(out, s->t140_red_buf[i], s->t140_red_len[i]);
            out += s->t140_red_len[i];
        }
    }
    memcpy(out, buf, len);
    out += len;

    ff_rtp_send_data(s1, s->buf, out - s->buf, marker);

    if (s->t140_red > 0)
        t140_red_push(s, buf, len, s->timestamp);
}

void ff_rtp_send_t140(AVFormatContext *s1, const uint8_t *buf, int size)
{
    RTPMuxContext *s = s1->priv_data;
    int marker;
    int max_frag = s->max_payload_size;

    /* size fragments so that a full window of redundant generations
     * of the same size always fits alongside the primary data */
    if (s->t140_red > 0)
        max_frag = (s->max_payload_size - 1 -
                    s->t140_red * RED_HDR_SIZE) / (s->t140_red + 1);
    max_frag = FFMIN(max_frag, RED_MAX_BLOCK_LEN);
    if (max_frag < 1)
        max_frag = 1;

    marker = !s->t140_started ||
             (int32_t)(s->cur_timestamp - s->t140_last_timestamp) >
                 T140_IDLE_THRESHOLD;
    s->t140_started        = 1;
    s->t140_last_timestamp = s->cur_timestamp;

    s->timestamp = s->cur_timestamp;
    if (!size) {
        /* empty T.140 block, e.g. keep-alive */
        t140_send_fragment(s1, buf, 0, marker);
        return;
    }
    while (size > 0) {
        int len = t140_utf8_boundary(buf, size, max_frag);
        t140_send_fragment(s1, buf, len, marker);
        marker = 0;
        buf  += len;
        size -= len;
    }
}
