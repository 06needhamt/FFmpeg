/*
 * RTP redundant audio/text depacketization (RFC 2198)
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
 * @brief RTP support for the RED payload format (RFC 2198)
 * @author Thomas Needham
 *
 * Depacketizes RED-encapsulated payloads and dispatches the primary
 * encoding to the depacketizer registered for the embedded payload
 * type.  When a sequence number discontinuity is detected, redundant
 * blocks from the current packet that were not previously delivered
 * are emitted (oldest first) ahead of the primary block, providing
 * loss concealment for same-codec redundancy such as text/red
 * (RFC 4103 section 4).
 */

#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "rtp.h"
#include "rtpdec.h"
#include "rtpdec_formats.h"

#define RED_MAX_MAPS    8   /* payload type -> encoding name mappings */
#define RED_MAX_BLOCKS  8   /* pending blocks awaiting emission */
#define RED_HDR_SIZE    4   /* size of a non-final RED block header */

struct RedBlock {
    uint8_t *data;
    int      len;
    uint32_t timestamp;
    uint16_t seq;
    int      flags;
};

struct PayloadContext {
    /* fmtp "pt/pt/.../pt" list; the last entry describes the primary */
    int fmtp_pts[RED_MAX_MAPS];
    int nb_fmtp_pts;

    /* rtpmap lines seen for secondary payload types on this media */
    struct {
        int  pt;
        char enc_name[32];
    } maps[RED_MAX_MAPS];
    int nb_maps;

    /* resolved sub-depacketizer for the primary encoding */
    const RTPDynamicProtocolHandler *sub_handler;
    PayloadContext *sub_ctx;
    int sub_pt;                    /* payload type sub_handler is bound to */

    /* blocks pending emission (gap-fill redundancy, then primary) */
    struct RedBlock blocks[RED_MAX_BLOCKS];
    int nb_blocks;
    int cur_block;
    int draining_sub;              /* sub-handler has more packets queued */

    /* continuity state for gap detection */
    uint16_t next_seq;
    int      seq_valid;
    uint32_t last_ts;
    int      ts_valid;
};

static void red_free_blocks(PayloadContext *red)
{
    int i;
    for (i = 0; i < red->nb_blocks; i++)
        av_freep(&red->blocks[i].data);
    red->nb_blocks = red->cur_block = 0;
}

static void red_close_context(PayloadContext *red)
{
    red_free_blocks(red);
    if (red->sub_handler && red->sub_handler->close)
        red->sub_handler->close(red->sub_ctx);
    av_freep(&red->sub_ctx);
}

static int red_parse_fmtp_pts(AVFormatContext *ctx, PayloadContext *red,
                              const char *p)
{
    red->nb_fmtp_pts = 0;
    while (*p && red->nb_fmtp_pts < RED_MAX_MAPS) {
        char *end;
        long pt = strtol(p, &end, 10);
        if (end == p || pt < 0 || pt > 127)
            break;
        red->fmtp_pts[red->nb_fmtp_pts++] = pt;
        p = end;
        if (*p == '/')
            p++;
        else
            break;
    }
    if (!red->nb_fmtp_pts) {
        av_log(ctx, AV_LOG_WARNING,
               "RED: could not parse fmtp payload type list\n");
        return AVERROR_INVALIDDATA;
    }
    return 0;
}

static int red_parse_sdp_line(AVFormatContext *ctx, int st_index,
                              PayloadContext *red, const char *line)
{
    const char *p;

    if (st_index < 0)
        return 0;

    if (av_strstart(line, "fmtp:", &p)) {
        /* skip the payload type number */
        while (*p && *p != ' ')
            p++;
        while (*p == ' ')
            p++;
        return red_parse_fmtp_pts(ctx, red, p);
    } else if (av_strstart(line, "rtpmap:", &p)) {
        /* rtpmap for a secondary payload type, forwarded by the SDP
         * parser; remember the pt -> encoding name mapping */
        char *end;
        long pt = strtol(p, &end, 10);
        int i;
        if (end == p || pt < 0 || pt > 127)
            return 0;
        p = end;
        while (*p == ' ')
            p++;
        for (i = 0; i < red->nb_maps; i++)
            if (red->maps[i].pt == pt)
                return 0;
        if (red->nb_maps < RED_MAX_MAPS) {
            size_t n = strcspn(p, "/ ");
            if (n >= sizeof(red->maps[0].enc_name))
                n = sizeof(red->maps[0].enc_name) - 1;
            memcpy(red->maps[red->nb_maps].enc_name, p, n);
            red->maps[red->nb_maps].enc_name[n] = '\0';
            red->maps[red->nb_maps].pt = pt;
            red->nb_maps++;
        }
    }
    return 0;
}

/**
 * Bind the sub-depacketizer for the primary payload type carried in
 * the final RED block header.  Resolution order: rtpmap encoding name
 * recorded from the SDP, static payload type table, and finally a
 * media-type default (t140 for text streams).
 */
static int red_resolve_sub_handler(AVFormatContext *ctx, PayloadContext *red,
                                   AVStream *st, int pt)
{
    const RTPDynamicProtocolHandler *handler = NULL;
    const char *enc_name = NULL;
    int i, ret;

    if (red->sub_pt == pt)
        return 0;
    if (red->sub_handler) {
        /* primary encoding changed mid-stream; not supported */
        av_log(ctx, AV_LOG_WARNING,
               "RED: primary payload type changed from %d to %d, ignoring\n",
               red->sub_pt, pt);
        return AVERROR_PATCHWELCOME;
    }

    for (i = 0; i < red->nb_maps; i++)
        if (red->maps[i].pt == pt)
            enc_name = red->maps[i].enc_name;

    if (!enc_name && pt < RTP_PT_PRIVATE)
        enc_name = ff_rtp_enc_name(pt);
    if ((!enc_name || !*enc_name) &&
        st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
        enc_name = "t140"; /* RFC 4103 text/red default */

    if (enc_name && *enc_name)
        handler = ff_rtp_handler_find_by_name(enc_name,
                                              st->codecpar->codec_type);

    if (handler) {
        PayloadContext *sub_ctx = NULL;
        if (handler->priv_data_size) {
            sub_ctx = av_mallocz(handler->priv_data_size);
            if (!sub_ctx)
                return AVERROR(ENOMEM);
        }
        if (handler->init) {
            ret = handler->init(ctx, st->index, sub_ctx);
            if (ret < 0) {
                if (handler->close)
                    handler->close(sub_ctx);
                av_freep(&sub_ctx);
                return ret;
            }
        }
        red->sub_handler = handler;
        red->sub_ctx     = sub_ctx;
        if (st->codecpar->codec_id == AV_CODEC_ID_NONE)
            st->codecpar->codec_id = handler->codec_id;
        av_log(ctx, AV_LOG_VERBOSE, "RED: primary pt %d -> %s\n",
               pt, handler->enc_name);
    } else if (pt < RTP_PT_PRIVATE &&
               st->codecpar->codec_id == AV_CODEC_ID_NONE) {
        /* static payload type without a custom depacketizer */
        ff_rtp_get_codec_info(st->codecpar, pt);
    }
    red->sub_pt = pt;
    return 0;
}

/**
 * Emit one pending block through the sub-depacketizer (or directly if
 * the primary encoding needs no custom depacketization).
 *
 * @return <0 no packet produced, 0 packet produced, 1 packet produced
 *         and more output is pending
 */
static int red_emit_next(AVFormatContext *ctx, PayloadContext *red,
                         AVStream *st, AVPacket *pkt, uint32_t *timestamp)
{
    int rv;

    /* first drain any packets queued inside the sub-depacketizer */
    if (red->draining_sub) {
        rv = red->sub_handler->parse_packet(ctx, red->sub_ctx, st, pkt,
                                            timestamp, NULL, 0, 0, 0);
        if (rv == 1)
            return 1;
        red->draining_sub = 0;
        if (rv == 0)
            return red->cur_block < red->nb_blocks ? 1 : 0;
        /* fall through to the next block on error */
    }

    while (red->cur_block < red->nb_blocks) {
        struct RedBlock *blk = &red->blocks[red->cur_block++];

        *timestamp = blk->timestamp;
        if (red->sub_handler && red->sub_handler->parse_packet) {
            rv = red->sub_handler->parse_packet(ctx, red->sub_ctx, st, pkt,
                                                timestamp, blk->data,
                                                blk->len, blk->seq,
                                                blk->flags);
        } else {
            rv = av_new_packet(pkt, blk->len);
            if (rv >= 0) {
                memcpy(pkt->data, blk->data, blk->len);
                pkt->stream_index = st->index;
            }
        }
        av_freep(&blk->data);

        if (rv == 1) {
            red->draining_sub = 1;
            return 1;
        }
        if (rv == 0)
            return red->cur_block < red->nb_blocks ? 1 : 0;
        /* rv < 0: block consumed without output, try the next one */
    }

    red->nb_blocks = red->cur_block = 0;
    return -1;
}

static int red_queue_block(AVFormatContext *ctx, PayloadContext *red,
                           const uint8_t *data, int len, uint32_t timestamp,
                           uint16_t seq, int flags)
{
    struct RedBlock *blk;

    if (red->nb_blocks >= RED_MAX_BLOCKS) {
        av_log(ctx, AV_LOG_WARNING, "RED: too many pending blocks\n");
        return AVERROR(ENOBUFS);
    }
    blk = &red->blocks[red->nb_blocks];
    blk->data = av_memdup(data, FFMAX(len, 1));
    if (!blk->data)
        return AVERROR(ENOMEM);
    blk->len       = len;
    blk->timestamp = timestamp;
    blk->seq       = seq;
    blk->flags     = flags;
    red->nb_blocks++;
    return 0;
}

static int red_parse_packet(AVFormatContext *ctx, PayloadContext *red,
                            AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                            const uint8_t *buf, int len, uint16_t seq,
                            int flags)
{
    struct {
        int pt;
        int ts_offset;
        int len;
    } hdrs[RED_MAX_MAPS];
    int nb_hdrs = 0, primary_pt, hdr_len = 0, i, ret;
    const uint8_t *data;
    int gap;

    if (!buf) /* return the next queued packet */
        return red_emit_next(ctx, red, st, pkt, timestamp);

    if (red->nb_blocks) {
        av_log(ctx, AV_LOG_WARNING,
               "RED: dropping %d undelivered blocks\n",
               red->nb_blocks - red->cur_block);
        red_free_blocks(red);
    }
    red->draining_sub = 0;

    /* walk the RED block header chain (RFC 2198 section 3) */
    while (hdr_len < len && (buf[hdr_len] & 0x80)) {
        if (hdr_len + RED_HDR_SIZE >= len)
            return AVERROR_INVALIDDATA;
        if (nb_hdrs >= RED_MAX_MAPS) {
            av_log(ctx, AV_LOG_WARNING, "RED: too many redundant blocks\n");
            return AVERROR_INVALIDDATA;
        }
        hdrs[nb_hdrs].pt        =  buf[hdr_len] & 0x7f;
        hdrs[nb_hdrs].ts_offset = (AV_RB24(buf + hdr_len + 1) >> 10) & 0x3fff;
        hdrs[nb_hdrs].len       =  AV_RB16(buf + hdr_len + 2) & 0x3ff;
        nb_hdrs++;
        hdr_len += RED_HDR_SIZE;
    }
    if (hdr_len >= len)
        return AVERROR_INVALIDDATA;
    primary_pt = buf[hdr_len] & 0x7f;
    hdr_len++;

    data = buf + hdr_len;
    len -= hdr_len;

    ret = red_resolve_sub_handler(ctx, red, st, primary_pt);
    if (ret < 0 && ret != AVERROR_PATCHWELCOME)
        return ret;

    gap = red->seq_valid && seq != red->next_seq;

    /* queue redundant blocks that plug a detected gap, oldest first */
    for (i = 0; i < nb_hdrs; i++) {
        uint32_t block_ts = *timestamp - hdrs[i].ts_offset;
        if (hdrs[i].len > len)
            return AVERROR_INVALIDDATA;
        if (gap && hdrs[i].pt == primary_pt && hdrs[i].len > 0 &&
            (!red->ts_valid || (int32_t)(block_ts - red->last_ts) > 0)) {
            uint16_t red_seq = seq - (nb_hdrs - i);
            ret = red_queue_block(ctx, red, data, hdrs[i].len, block_ts,
                                  red_seq, 0);
            if (ret < 0)
                return ret;
            av_log(ctx, AV_LOG_VERBOSE,
                   "RED: recovered lost block seq %u ts %u from redundancy\n",
                   red_seq, block_ts);
        }
        data += hdrs[i].len;
        len  -= hdrs[i].len;
    }

    /* primary block: whatever remains after the redundant data */
    if (len < 0)
        return AVERROR_INVALIDDATA;
    ret = red_queue_block(ctx, red, data, len, *timestamp, seq, flags);
    if (ret < 0)
        return ret;

    red->next_seq  = seq + 1;
    red->seq_valid = 1;
    red->last_ts   = *timestamp;
    red->ts_valid  = 1;

    return red_emit_next(ctx, red, st, pkt, timestamp);
}

const RTPDynamicProtocolHandler ff_red_text_dynamic_handler = {
    .enc_name         = "red",
    .codec_type       = AVMEDIA_TYPE_SUBTITLE,
    .codec_id         = AV_CODEC_ID_TEXT,
    .priv_data_size   = sizeof(PayloadContext),
    .parse_sdp_a_line = red_parse_sdp_line,
    .close            = red_close_context,
    .parse_packet     = red_parse_packet,
};

const RTPDynamicProtocolHandler ff_red_audio_dynamic_handler = {
    .enc_name         = "red",
    .codec_type       = AVMEDIA_TYPE_AUDIO,
    .codec_id         = AV_CODEC_ID_NONE,
    .priv_data_size   = sizeof(PayloadContext),
    .parse_sdp_a_line = red_parse_sdp_line,
    .close            = red_close_context,
    .parse_packet     = red_parse_packet,
};
