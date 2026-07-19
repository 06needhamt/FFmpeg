/*
 * RTP packetizer for JPEG XS payload format (RFC 9134)
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
 * @brief JPEG XS (ISO/IEC 21122) RTP packetization code (RFC 9134)
 *
 * The packetizer operates in the codestream packetization mode
 * (K == 0) with sequential transmission (T == 1): each input packet
 * (one JPEG XS frame) forms a single packetization unit that is
 * fragmented into equally sized RTP packet payloads.
 *
 * RFC 9134 defines the packetization unit as the JPEG XS picture
 * segment, i.e. the codestream preceded by an ISO video support box
 * and color specification box (ISO/IEC 21122-3). If the input packet
 * already carries these boxes (e.g. when remuxing from a transport
 * that preserves them), they are transmitted as-is. FFmpeg's native
 * JPEG XS packets, however, are bare codestreams starting with the SOC
 * marker; synthesizing conformant VS/CS boxes would require stream
 * metadata (buffer model, timecodes, ...) defined in ISO/IEC 21122-3
 * that is not available here, so in that case the codestream is sent
 * without leading boxes and a warning is issued once. Receivers that
 * locate the codestream via the SOC marker (such as FFmpeg's own
 * depacketizer) interoperate either way.
 */

#include "libavutil/intreadwrite.h"

#include "libavcodec/jpegxs.h"

#include "avformat.h"
#include "rtpenc.h"

#define RTP_JPEGXS_PAYLOAD_HEADER_SIZE 4

void ff_rtp_send_jpegxs(AVFormatContext *s1, const uint8_t *buf, int size)
{
    RTPMuxContext *s = s1->priv_data;
    int max_chunk = s->max_payload_size - RTP_JPEGXS_PAYLOAD_HEADER_SIZE;
    unsigned int packet_index = 0;
    int f_counter;

    if (max_chunk <= 0) {
        av_log(s1, AV_LOG_ERROR,
               "RTP max payload size too small for JPEG XS\n");
        return;
    }

    if (size >= 2 && AV_RB16(buf) == JPEGXS_MARKER_SOC && !s->frame_count)
        av_log(s1, AV_LOG_WARNING,
               "JPEG XS input carries no ISO VS/CS boxes; sending the bare "
               "codestream. Strict RFC 9134 receivers may reject this "
               "stream.\n");

    s->timestamp = s->cur_timestamp;

    /* the Frame counter identifies the video frame number modulo 32
     * (RFC 9134, Section 4.3) */
    f_counter = s->frame_count & 0x1f;
    s->frame_count++;

    while (size > 0) {
        int chunk = FFMIN(max_chunk, size);
        int last  = chunk == size;
        /* in codestream packetization mode, the P counter counts the
         * packets within the packetization unit modulo 2048 and is
         * extended by the SEP counter when it overruns
         * (RFC 9134, Section 4.3) */
        int p_counter   = packet_index & 0x7ff;
        int sep_counter = (packet_index >> 11) & 0x7ff;
        uint8_t *ptr = s->buf;

        /*
         * write the payload header (RFC 9134, Section 4.3):
         *
         *     0                   1                   2                   3
         *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
         *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *    |T|K|L| I |F counter|     SEP counter     |     P counter       |
         *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *
         * T = 1 (sequential transmission), K = 0 (codestream
         * packetization mode), I = 00 (progressive; interlaced input
         * is not split into per-field picture segments here). The L
         * bit marks the last packet of the packetization unit and, in
         * codestream mode, matches the RTP marker bit.
         */
        *ptr++ = 0x80 | (last << 5) | (f_counter >> 2);
        *ptr++ = ((f_counter & 0x03) << 6) | (sep_counter >> 5);
        *ptr++ = ((sep_counter & 0x1f) << 3) | (p_counter >> 8);
        *ptr++ = p_counter & 0xff;

        memcpy(ptr, buf, chunk);

        ff_rtp_send_data(s1, s->buf,
                         RTP_JPEGXS_PAYLOAD_HEADER_SIZE + chunk, last);

        buf  += chunk;
        size -= chunk;
        packet_index++;
    }
}
