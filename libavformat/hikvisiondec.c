/*
 * Hikvision DVR demuxer
 * Copyright (c) 2020 Tom Needham
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

#include "avformat.h"
#include "internal.h"
#include "hikvision.h"

static int hikvision_close(AVFormatContext *s)
{
    return 0;
}

static int hikvision_read_header(AVFormatContext *s)
{
    return 0;
}

static int hikvision_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    return 0;
}

static int hikvision_read_seek(AVFormatContext *s, int stream_index,
                               int64_t timestamp, int flags)
{
    return 0;
}

static int hikvision_probe(const AVProbeData *p)
{
    return 0;
}


AVInputFormat ff_hikvision_demuxer = {
    .name           = "hikvision",
    .long_name      = NULL_IF_CONFIG_SMALL("Hikvision DVR Demuxer"),
    .priv_data_size = sizeof(HikvisionContext),
    .read_probe     = hikvision_probe,
    .read_header    = hikvision_read_header,
    .read_packet    = hikvision_read_packet,
    .read_close     = hikvision_close,
    .read_seek      = hikvision_read_seek,
    .extensions     = "hik",
};
