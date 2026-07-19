/*
 * FlexFEC (RFC 8627) receive-side recovery for RTP
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
 * @brief FlexFEC (RFC 8627) source-packet ring buffer, repair packet
 *        parsing and single-loss XOR reconstruction (section 6.3).
 * @author Thomas Needham
 *
 * Prototype scope: one protected source SSRC per RTP session.  The
 * flexible mask variant (R=0, F=0), the fixed L/D row/column variant
 * (R=0, F=1) and single-packet retransmission (R=1, F=0) are
 * supported.  Iterative 2-D decoding falls out of repeated single-loss
 * recovery since recovered packets are fed back into the source ring.
 */

#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avformat.h"
#include "rtpdec_flexfec.h"

#define FLEXFEC_RING_SIZE   256  /* power of two, source packets kept */
#define FLEXFEC_MAX_REPAIR  32
#define FLEXFEC_MAX_COVER   110  /* largest bitmask (RFC 8627 4.2.2.1) */

typedef struct FlexFECSource {
    int      valid;
    uint16_t seq;
    uint8_t *buf;
    int      len;
} FlexFECSource;

typedef struct FlexFECRepair {
    int      valid;
    int      retransmission;      /* R=1 packet, payload is the source */
    uint16_t retrans_seq;
    int      nb_cover;
    uint16_t cover[FLEXFEC_MAX_COVER];
    uint8_t  hdr_recovery[2];     /* XOR of source octets 0-1 (V bits invalid) */
    uint16_t len_recovery;
    uint32_t ts_recovery;
    uint8_t *payload;             /* XOR of source octets after byte 12 */
    int      payload_len;
} FlexFECRepair;

struct FFFlexFECContext {
    void *logctx;
    FlexFECSource ring[FLEXFEC_RING_SIZE];
    FlexFECRepair repairs[FLEXFEC_MAX_REPAIR];
    int repair_idx;
    unsigned recovered;
};

FFFlexFECContext *ff_flexfec_alloc(void *logctx)
{
    FFFlexFECContext *ctx = av_mallocz(sizeof(*ctx));
    if (ctx)
        ctx->logctx = logctx;
    return ctx;
}

static void flexfec_clear_repair(FlexFECRepair *rep)
{
    av_freep(&rep->payload);
    rep->valid = 0;
}

void ff_flexfec_free(FFFlexFECContext **pctx)
{
    FFFlexFECContext *ctx = *pctx;
    int i;

    if (!ctx)
        return;
    for (i = 0; i < FLEXFEC_RING_SIZE; i++)
        av_freep(&ctx->ring[i].buf);
    for (i = 0; i < FLEXFEC_MAX_REPAIR; i++)
        flexfec_clear_repair(&ctx->repairs[i]);
    av_freep(pctx);
}

int ff_flexfec_add_source(FFFlexFECContext *ctx, const uint8_t *buf, int len)
{
    uint16_t seq;
    FlexFECSource *src;

    if (len < 12)
        return AVERROR_INVALIDDATA;
    seq = AV_RB16(buf + 2);
    src = &ctx->ring[seq % FLEXFEC_RING_SIZE];
    if (src->valid && src->seq == seq)
        return 0; /* duplicate */
    av_freep(&src->buf);
    src->buf = av_memdup(buf, len);
    if (!src->buf) {
        src->valid = 0;
        return AVERROR(ENOMEM);
    }
    src->len   = len;
    src->seq   = seq;
    src->valid = 1;
    return 0;
}

static int flexfec_source_present(FFFlexFECContext *ctx, uint16_t seq,
                                  FlexFECSource **out)
{
    FlexFECSource *src = &ctx->ring[seq % FLEXFEC_RING_SIZE];
    if (src->valid && src->seq == seq) {
        if (out)
            *out = src;
        return 1;
    }
    return 0;
}

int ff_flexfec_add_repair(FFFlexFECContext *ctx, const uint8_t *buf, int len)
{
    const uint8_t *p;
    int plen, cc, r, f, i;
    FlexFECRepair *rep;

    if (len < 12 + 12)
        return AVERROR_INVALIDDATA;

    cc   = buf[0] & 0x0f;
    p    = buf + 12 + 4 * cc; /* skip RTP header and protected-SSRC list */
    plen = len - 12 - 4 * cc;
    if (plen < 12)
        return AVERROR_INVALIDDATA;

    r = p[0] >> 7;
    f = (p[0] >> 6) & 1;
    if (r && f)
        return 0; /* reserved, MUST ignore */

    rep = &ctx->repairs[ctx->repair_idx];
    ctx->repair_idx = (ctx->repair_idx + 1) % FLEXFEC_MAX_REPAIR;
    flexfec_clear_repair(rep);

    rep->retransmission  = r;
    rep->hdr_recovery[0] = p[0];
    rep->hdr_recovery[1] = p[1];
    rep->ts_recovery     = AV_RB32(p + 4);

    if (r) {
        /* RFC 8627 4.2.2.3: layout matches the original source packet;
         * the sequence number sits where length recovery would be */
        rep->retrans_seq = AV_RB16(p + 2);
        rep->payload     = av_memdup(p + 12, FFMAX(plen - 12, 1));
        if (!rep->payload)
            return AVERROR(ENOMEM);
        rep->payload_len = plen - 12;
        rep->valid       = 1;
        return 0;
    }

    rep->len_recovery = AV_RB16(p + 2);

    if (!f) {
        /* flexible mask variant */
        uint16_t sn_base = AV_RB16(p + 8);
        int off = 10;
        uint64_t mask;

        if (plen < off + 2)
            return AVERROR_INVALIDDATA;
        /* Mask [0-14] */
        mask = AV_RB16(p + off) & 0x7fff;
        for (i = 0; i < 15; i++)
            if (mask & (1ULL << (14 - i)))
                rep->cover[rep->nb_cover++] = sn_base + i;
        if (AV_RB16(p + off) & 0x8000) {
            /* k=1: Mask [15-45] follows */
            off += 2;
            if (plen < off + 4)
                return AVERROR_INVALIDDATA;
            mask = AV_RB32(p + off) & 0x7fffffff;
            for (i = 0; i < 31; i++)
                if (mask & (1ULL << (30 - i)))
                    rep->cover[rep->nb_cover++] = sn_base + 15 + i;
            if (AV_RB32(p + off) & 0x80000000u) {
                /* k=1: Mask [46-109] follows */
                off += 4;
                if (plen < off + 8)
                    return AVERROR_INVALIDDATA;
                mask = AV_RB64(p + off);
                for (i = 0; i < 64; i++)
                    if (mask & (1ULL << (63 - i)))
                        rep->cover[rep->nb_cover++] = sn_base + 46 + i;
                off += 8;
            } else {
                off += 4;
            }
        } else {
            off += 2;
        }
        rep->payload_len = plen - off;
        rep->payload     = av_memdup(p + off, FFMAX(rep->payload_len, 1));
    } else {
        /* fixed L columns / D rows variant */
        uint16_t sn_base = AV_RB16(p + 8);
        int l, d;

        if (plen < 12)
            return AVERROR_INVALIDDATA;
        l = p[10];
        d = p[11];
        if (!l)
            return 0; /* L=0 reserved, MUST ignore */
        if (d <= 1) {
            /* row FEC: SN, SN+1, ..., SN+L-1 */
            for (i = 0; i < l && i < FLEXFEC_MAX_COVER; i++)
                rep->cover[rep->nb_cover++] = sn_base + i;
        } else {
            /* column FEC: SN, SN+L, ..., SN+(D-1)*L */
            for (i = 0; i < d && i < FLEXFEC_MAX_COVER; i++)
                rep->cover[rep->nb_cover++] = sn_base + i * l;
        }
        rep->payload_len = plen - 12;
        rep->payload     = av_memdup(p + 12, FFMAX(rep->payload_len, 1));
    }
    if (!rep->payload)
        return AVERROR(ENOMEM);
    if (rep->payload_len < 0) {
        flexfec_clear_repair(rep);
        return AVERROR_INVALIDDATA;
    }
    rep->valid = 1;
    return 0;
}

/**
 * Attempt to reconstruct the source packet with the given sequence
 * number (RFC 8627 sections 6.3.2 and 6.3.3).
 *
 * @return 0 on success with *out / *outlen set (caller frees), <0 if the
 *         packet cannot be recovered from the currently held data
 */
int ff_flexfec_recover(FFFlexFECContext *ctx, uint16_t seq, uint32_t ssrc,
                       uint8_t **out, int *outlen)
{
    int i, j;

    if (flexfec_source_present(ctx, seq, NULL))
        return AVERROR(EEXIST);

    for (i = 0; i < FLEXFEC_MAX_REPAIR; i++) {
        FlexFECRepair *rep = &ctx->repairs[i];
        uint8_t b0, b1, *pkt;
        uint16_t rec_len;
        uint32_t ts;
        int covers = 0, missing = 0;

        if (!rep->valid)
            continue;

        if (rep->retransmission) {
            if (rep->retrans_seq != seq)
                continue;
            pkt = av_malloc(12 + rep->payload_len);
            if (!pkt)
                return AVERROR(ENOMEM);
            pkt[0] = 0x80 | (rep->hdr_recovery[0] & 0x3f);
            pkt[1] = rep->hdr_recovery[1];
            AV_WB16(pkt + 2, seq);
            AV_WB32(pkt + 4, rep->ts_recovery);
            AV_WB32(pkt + 8, ssrc);
            memcpy(pkt + 12, rep->payload, rep->payload_len);
            *out    = pkt;
            *outlen = 12 + rep->payload_len;
            goto done;
        }

        for (j = 0; j < rep->nb_cover; j++) {
            if (rep->cover[j] == seq)
                covers = 1;
            else if (!flexfec_source_present(ctx, rep->cover[j], NULL))
                missing++;
        }
        if (!covers || missing)
            continue; /* XOR parity recovers exactly one loss */

        b0      = rep->hdr_recovery[0];
        b1      = rep->hdr_recovery[1];
        rec_len = rep->len_recovery;
        ts      = rep->ts_recovery;
        pkt     = av_malloc(12 + rep->payload_len);
        if (!pkt)
            return AVERROR(ENOMEM);
        memset(pkt + 12, 0, rep->payload_len);
        memcpy(pkt + 12, rep->payload, rep->payload_len);

        for (j = 0; j < rep->nb_cover; j++) {
            FlexFECSource *src;
            int k, srclen;
            if (rep->cover[j] == seq)
                continue;
            flexfec_source_present(ctx, rep->cover[j], &src);
            b0      ^= src->buf[0];
            b1      ^= src->buf[1];
            rec_len ^= (uint16_t)(src->len - 12);
            ts      ^= AV_RB32(src->buf + 4);
            srclen   = FFMIN(src->len - 12, rep->payload_len);
            for (k = 0; k < srclen; k++)
                pkt[12 + k] ^= src->buf[12 + k];
        }

        if (rec_len > rep->payload_len) {
            av_log(ctx->logctx, AV_LOG_WARNING,
                   "FlexFEC: implausible recovered length %d for seq %u\n",
                   rec_len, seq);
            av_free(pkt);
            continue;
        }

        pkt[0] = 0x80 | (b0 & 0x3f); /* force RTP version 2 */
        pkt[1] = b1;
        AV_WB16(pkt + 2, seq);
        AV_WB32(pkt + 4, ts);
        AV_WB32(pkt + 8, ssrc);

        *out    = pkt;
        *outlen = 12 + rec_len;
        goto done;
    }
    return AVERROR(EAGAIN);

done:
    /* feed the recovered packet back so chained (2-D) recovery works */
    ff_flexfec_add_source(ctx, *out, *outlen);
    ctx->recovered++;
    av_log(ctx->logctx, AV_LOG_VERBOSE,
           "FlexFEC: recovered source packet seq %u (%d bytes, %u total)\n",
           seq, *outlen, ctx->recovered);
    return 0;
}
