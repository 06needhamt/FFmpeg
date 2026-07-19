/*
 * RTP packetizer for TTML timed text payload format (RFC 8759)
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
 * @brief TTML RTP packetization (RFC 8759)
 *
 * Each access unit carries exactly one complete TTML document. A
 * document is preceded by the 4 byte payload header (16 reserved bits
 * that must be zero followed by a 16 bit length field) and may be
 * fragmented over several RTP packets, all of which share the RTP
 * timestamp of the document. The marker bit is set on the last packet
 * of a document (RFC 8759 Section 4.1).
 *
 * Streams produced by the libavcodec TTML encoder consist of paragraph
 * fragments rather than complete documents; those are wrapped into a
 * minimal standalone document per access unit, with media times
 * expressed relative to the RTP timestamp as specified by the
 * "#rtp-relative-media-time" feature extension (RFC 8759 Section 11).
 */

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "rtpenc.h"
#include "ttmlenc.h"

#define RTP_TTML_PAYLOAD_HEADER_SIZE 4

static void ttml_write_time(AVBPrint *bp, const char tag[], int64_t millisec)
{
    int64_t sec, min, hour;
    sec = millisec / 1000;
    millisec -= 1000 * sec;
    min = sec / 60;
    sec -= 60 * min;
    hour = min / 60;
    min -= 60 * hour;

    av_bprintf(bp, "%s=\"%02"PRId64":%02"PRId64":%02"PRId64".%03"PRId64"\"",
               tag, hour, min, sec, millisec);
}

/* Extract the <tt/> element parameters and pre-body elements from the
 * extradata written by the libavcodec TTML encoder. Mirrors
 * ttml_set_header_values_from_extradata() in ttmlenc.c. */
static int ttml_get_extradata_strings(const AVCodecParameters *par,
                                      const char **tt_element_params,
                                      const char **pre_body_elements)
{
    size_t additional_data_size =
        par->extradata_size - TTMLENC_EXTRADATA_SIGNATURE_SIZE;
    char *value =
        (char *)par->extradata + TTMLENC_EXTRADATA_SIGNATURE_SIZE;
    size_t value_size = av_strnlen(value, additional_data_size);

    if (!additional_data_size) {
        /* old extradata format without header parameters */
        *tt_element_params = TTML_DEFAULT_NAMESPACING;
        *pre_body_elements = "";
        return 0;
    }

    if (value_size == additional_data_size || value[value_size] != '\0')
        return AVERROR_INVALIDDATA;

    *tt_element_params = value;

    additional_data_size -= value_size + 1;
    value += value_size + 1;
    if (!additional_data_size)
        return AVERROR_INVALIDDATA;

    value_size = av_strnlen(value, additional_data_size);
    if (value_size == additional_data_size || value[value_size] != '\0')
        return AVERROR_INVALIDDATA;

    *pre_body_elements = value;

    return 0;
}

static int ttml_wrap_paragraph(AVFormatContext *s1, AVBPrint *doc,
                               const uint8_t *buf, int size,
                               int64_t duration_ms)
{
    const AVStream *st = s1->streams[0];
    const AVDictionaryEntry *lang = av_dict_get(st->metadata, "language",
                                                NULL, 0);
    const char *printed_lang = (lang && lang->value) ? lang->value : "";
    const char *tt_element_params, *pre_body_elements;
    int ret;

    ret = ttml_get_extradata_strings(st->codecpar, &tt_element_params,
                                     &pre_body_elements);
    if (ret < 0) {
        av_log(s1, AV_LOG_ERROR,
               "Failed to parse TTML header values from extradata: %s!\n",
               av_err2str(ret));
        return ret;
    }

    av_bprintf(doc,
               "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
               "<tt\n"
               "%s"
               "  xml:lang=\"%s\">\n"
               "%s"
               "  <body>\n"
               "    <div>\n"
               "      <p\n",
               tt_element_params, printed_lang, pre_body_elements);
    /* The epoch of the media times in the document is the time
     * corresponding to the RTP timestamp of its packets (RFC 8759
     * Section 6), so the paragraph always begins at zero. */
    av_bprintf(doc, "        ");
    ttml_write_time(doc, "begin", 0);
    if (duration_ms > 0) {
        av_bprintf(doc, "\n        ");
        ttml_write_time(doc, "end", duration_ms);
    }
    av_bprintf(doc, ">");
    av_bprint_append_data(doc, (const char *)buf, size);
    av_bprintf(doc, "</p>\n"
                    "    </div>\n"
                    "  </body>\n"
                    "</tt>\n");

    if (!av_bprint_is_complete(doc))
        return AVERROR(ENOMEM);

    return 0;
}

static void rtp_send_ttml_document(AVFormatContext *s1, const uint8_t *buf,
                                   int size)
{
    RTPMuxContext *s = s1->priv_data;
    /* The per-packet length field is 16 bits wide, limiting a single
     * fragment to 65535 bytes of User Data Words; larger documents are
     * carried through fragmentation. */
    int max_chunk = FFMIN(s->max_payload_size - RTP_TTML_PAYLOAD_HEADER_SIZE,
                          0xffff);

    if (max_chunk <= 0) {
        av_log(s1, AV_LOG_ERROR,
               "RTP max payload size %d too small for TTML\n",
               s->max_payload_size);
        return;
    }

    while (size > 0) {
        int chunk = FFMIN(size, max_chunk);
        int last  = chunk == size;

        AV_WB16(s->buf, 0); /* reserved, must be zero */
        AV_WB16(s->buf + 2, chunk);
        memcpy(s->buf + RTP_TTML_PAYLOAD_HEADER_SIZE, buf, chunk);

        ff_rtp_send_data(s1, s->buf, chunk + RTP_TTML_PAYLOAD_HEADER_SIZE,
                         last);

        buf  += chunk;
        size -= chunk;
    }
}

void ff_rtp_send_ttml(AVFormatContext *s1, const uint8_t *buf, int size,
                      int64_t duration)
{
    RTPMuxContext *s = s1->priv_data;

    if (size <= 0)
        return;

    /* All packets of a document carry the timestamp of the document
     * (RFC 8759 Section 4.1). */
    s->timestamp = s->cur_timestamp;

    if (ff_is_ttml_stream_paragraph_based(s1->streams[0]->codecpar)) {
        AVBPrint doc;
        int64_t duration_ms = av_rescale_q(duration,
                                           s1->streams[0]->time_base,
                                           (AVRational){ 1, 1000 });

        av_bprint_init(&doc, 0, AV_BPRINT_SIZE_UNLIMITED);
        if (ttml_wrap_paragraph(s1, &doc, buf, size, duration_ms) < 0) {
            av_bprint_finalize(&doc, NULL);
            return;
        }
        rtp_send_ttml_document(s1, (const uint8_t *)doc.str, doc.len);
        av_bprint_finalize(&doc, NULL);
    } else {
        rtp_send_ttml_document(s1, buf, size);
    }
}
