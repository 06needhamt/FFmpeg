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

#ifndef AVFORMAT_RTPDEC_FLEXFEC_H
#define AVFORMAT_RTPDEC_FLEXFEC_H

#include <stdint.h>

typedef struct FFFlexFECContext FFFlexFECContext;

/**
 * Allocate a FlexFEC receive context.  @p logctx is only used for
 * logging and may be NULL.
 */
FFFlexFECContext *ff_flexfec_alloc(void *logctx);

void ff_flexfec_free(FFFlexFECContext **pctx);

/**
 * Store a copy of a received source packet (complete RTP packet
 * including header) for later use in recovery.  Duplicates are
 * detected and ignored.
 */
int ff_flexfec_add_source(FFFlexFECContext *ctx, const uint8_t *buf, int len);

/**
 * Parse and store a FlexFEC repair packet (complete RTP packet
 * including header).  Supports the flexible mask variant (R=0, F=0),
 * the fixed L/D variant (R=0, F=1) and retransmissions (R=1, F=0).
 */
int ff_flexfec_add_repair(FFFlexFECContext *ctx, const uint8_t *buf, int len);

/**
 * Attempt to reconstruct the source packet with sequence number
 * @p seq for stream @p ssrc.
 *
 * @return 0 on success with *out (caller frees with av_free) and
 *         *outlen set; AVERROR(EEXIST) if the packet is already
 *         present; AVERROR(EAGAIN) if it cannot currently be
 *         recovered.
 */
int ff_flexfec_recover(FFFlexFECContext *ctx, uint16_t seq, uint32_t ssrc,
                       uint8_t **out, int *outlen);

#endif /* AVFORMAT_RTPDEC_FLEXFEC_H */
