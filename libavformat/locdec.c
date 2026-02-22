/*
 * Low Overhead Container (LOC) demuxer
 * draft-ietf-moq-loc-01 / LOCF file format
 *
 * Supports two on-disk variants:
 *
 *   LOCF v1  –  Single-frame demo format from libloc:
 *     [4]  'L','O','C','F'
 *     [1]  0x01
 *     [4]  ext_block_len  (uint32 BE)
 *     [8]  payload_len    (uint64 BE)
 *     [N]  LOC extension block
 *     [M]  LOC payload
 *
 *   LOCF v2  –  Multi-frame FFmpeg format (written by locenc.c):
 *     === File header (32 bytes) ===
 *     [4]  'L','O','C','F'
 *     [1]  0x02
 *     [1]  codec_type   0 = video, 1 = audio
 *     [4]  codec_id     uint32 BE  (AVCodecID value)
 *     [4]  tb_num       uint32 BE  (AVRational.num)
 *     [4]  tb_den       uint32 BE  (AVRational.den)
 *     [4]  dim0         uint32 BE  (width  for video, sample_rate for audio)
 *     [2]  dim1         uint16 BE  (height for video, channels    for audio)
 *     [4]  extra_len    uint32 BE
 *     [4]  reserved     (zero)
 *     === Extradata (extra_len bytes) ===
 *     === Frame records (until EOF) ===
 *     [8]  pts      int64  BE
 *     [8]  dts      int64  BE
 *     [4]  flags    uint32 BE  (AV_PKT_FLAG_*)
 *     [4]  ext_len  uint32 BE  (LOC extension block length)
 *     [4]  pay_len  uint32 BE  (LOC payload length)
 *     [N]  LOC extension block
 *     [M]  LOC payload (packet data returned to caller)
 *
 * Copyright (c) 2025 – MIT licence; see COPYING
 */

#include "libavutil/avassert.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "internal.h"

#include <loc/loc.h>

/* -------------------------------------------------------------------------
 * On-disk constants
 * ------------------------------------------------------------------------- */

#define LOCF_MAGIC          UINT32_C(0x4C4F4346)  /* 'LOCF' */
#define LOCF_VERSION_V1     0x01
#define LOCF_VERSION_V2     0x02

/* v2 fixed header size (before extradata) */
#define LOCF_V2_HDR_SIZE    32
/* v1 fixed header size */
#define LOCF_V1_HDR_SIZE    17  /* 4+1+4+8 */

/* v2 frame record header size (before ext + payload data) */
#define LOCF_V2_FRAME_HDR   28  /* 8+8+4+4+4 */

/* Sanity caps to avoid malloc bombs */
#define LOCF_MAX_EXTRA      (4 * 1024 * 1024)
#define LOCF_MAX_EXT_LEN    (1 * 1024 * 1024)
#define LOCF_MAX_PAY_LEN    (256 * 1024 * 1024)

/* v2 codec_type field values */
#define LOCF_CT_VIDEO   0
#define LOCF_CT_AUDIO   1

/* -------------------------------------------------------------------------
 * Private context
 * ------------------------------------------------------------------------- */

typedef struct LOCDemuxContext {
    int      version;      /* 1 or 2 */

    /* v1 single-frame state */
    int      v1_done;      /* non-zero once the single frame has been returned */
    uint64_t v1_pay_len;   /* payload length in bytes */

    /*
     * In v1, read_header parses the extension block to detect the codec.
     * Rather than seeking back (fragile on non-seekable streams), we store
     * the useful metadata here so read_packet can use it directly without
     * re-reading the extensions.  The extension bytes are consumed by
     * read_header and are NOT re-read by read_packet.
     */
    int      v1_ext_parsed;       /* non-zero if extensions were pre-parsed */
    int64_t  v1_pts;              /* AV_NOPTS_VALUE if no capture_ts found */
    int      v1_key;              /* non-zero if frame is a key frame */
} LOCDemuxContext;

/* -------------------------------------------------------------------------
 * Probe
 * ------------------------------------------------------------------------- */

static int loc_probe(const AVProbeData *p)
{
    if (p->buf_size < 5)
        return 0;
    if (AV_RB32(p->buf) != LOCF_MAGIC)
        return 0;
    if (p->buf[4] == LOCF_VERSION_V1 || p->buf[4] == LOCF_VERSION_V2)
        return AVPROBE_SCORE_MAX;
    return 0;
}

/* -------------------------------------------------------------------------
 * Helper: create and configure a stream from v2 header fields
 * ------------------------------------------------------------------------- */

static int loc_create_stream_v2(AVFormatContext *s,
                                uint8_t  codec_type,
                                uint32_t codec_id_raw,
                                uint32_t tb_num, uint32_t tb_den,
                                uint32_t dim0,   uint16_t dim1,
                                const uint8_t *extra, int extra_len)
{
    AVStream *st = avformat_new_stream(s, NULL);
    if (!st)
        return AVERROR(ENOMEM);

    st->codecpar->codec_type = (codec_type == LOCF_CT_AUDIO)
                                    ? AVMEDIA_TYPE_AUDIO
                                    : AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id   = (enum AVCodecID)codec_id_raw;

    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        st->codecpar->width  = (int)dim0;
        st->codecpar->height = (int)dim1;
    } else {
        st->codecpar->sample_rate = (int)dim0;
        st->codecpar->ch_layout.nb_channels = (int)dim1;
    }

    if (extra_len > 0 && extra) {
        st->codecpar->extradata = av_mallocz(extra_len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!st->codecpar->extradata)
            return AVERROR(ENOMEM);
        memcpy(st->codecpar->extradata, extra, extra_len);
        st->codecpar->extradata_size = extra_len;
    }

    avpriv_set_pts_info(st, 64,
                        tb_num ? tb_num : 1,
                        tb_den ? tb_den : 1000000);
    return 0;
}

/* -------------------------------------------------------------------------
 * read_header
 * ------------------------------------------------------------------------- */

static int loc_read_header(AVFormatContext *s)
{
    LOCDemuxContext *loc = s->priv_data;
    AVIOContext     *pb  = s->pb;

    /* Read magic + version (already probed, but re-read cleanly) */
    uint32_t magic   = avio_rb32(pb);
    uint8_t  version = avio_r8(pb);

    if (magic != LOCF_MAGIC) {
        av_log(s, AV_LOG_ERROR, "Not a LOCF file\n");
        return AVERROR_INVALIDDATA;
    }

    loc->version = version;

    if (version == LOCF_VERSION_V1) {
        /*
         * v1: single-frame demo format.
         * Parse the extension block here for codec detection.  We consume
         * the ext bytes and store the decoded metadata in the private
         * context so read_packet can use them without re-reading.
         */
        uint32_t ext_len = avio_rb32(pb);
        uint64_t pay_len = avio_rb64(pb);

        if (pay_len > LOCF_MAX_PAY_LEN) {
            av_log(s, AV_LOG_ERROR, "Implausible v1 payload length\n");
            return AVERROR_INVALIDDATA;
        }

        loc->v1_pay_len    = pay_len;
        loc->v1_done       = 0;
        loc->v1_pts        = AV_NOPTS_VALUE;
        loc->v1_key        = 1;  /* assume key unless extensions say otherwise */
        loc->v1_ext_parsed = 0;

        AVStream *st = avformat_new_stream(s, NULL);
        if (!st)
            return AVERROR(ENOMEM);

        st->codecpar->codec_type = AVMEDIA_TYPE_UNKNOWN;
        st->codecpar->codec_id   = AV_CODEC_ID_NONE;

        if (ext_len > 0 && ext_len <= LOCF_MAX_EXT_LEN) {
            uint8_t *extbuf = av_malloc(ext_len);
            if (!extbuf)
                return AVERROR(ENOMEM);

            int ret = avio_read(pb, extbuf, (int)ext_len);
            if (ret == (int)ext_len) {
                loc_object_t obj;
                loc_object_init(&obj);
                if (loc_decode_extensions(extbuf, ext_len, &obj) > 0) {
                    /* Capture timestamp → PTS (µs timebase) */
                    if (obj.has_capture_ts)
                        loc->v1_pts = (int64_t)obj.capture_ts_us;

                    /* Key-frame flag from video frame marking */
                    if (obj.has_video_frame_marking) {
                        loc->v1_key = !!(obj.video_frame_marking & LOC_VFM_INDEPENDENT);
                    }

                    /* Codec type inference */
                    if (obj.has_video_config || obj.has_video_frame_marking) {
                        st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
                        if (obj.has_video_config) {
                            st->codecpar->codec_id = AV_CODEC_ID_H264;
                            uint8_t *ed = av_mallocz(obj.video_config_len
                                                     + AV_INPUT_BUFFER_PADDING_SIZE);
                            if (ed) {
                                memcpy(ed, obj.video_config, obj.video_config_len);
                                st->codecpar->extradata      = ed;
                                st->codecpar->extradata_size = (int)obj.video_config_len;
                            }
                        }
                    } else if (obj.has_audio_level) {
                        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
                    }
                    loc->v1_ext_parsed = 1;
                }
            } else {
                av_log(s, AV_LOG_WARNING,
                       "Could not read v1 extension block (%d/%u)\n",
                       ret, ext_len);
                /* Continue – payload may still be readable */
            }
            av_free(extbuf);
        }

        avpriv_set_pts_info(st, 64, 1, 1000000); /* µs timebase */
        return 0;
    }

    if (version == LOCF_VERSION_V2) {
        if (LOCF_V2_HDR_SIZE < 6) {
            av_log(s, AV_LOG_ERROR, "Internal header size error\n");
            return AVERROR_BUG;
        }

        /* We already consumed 5 bytes (magic+ver); read the remaining 27 */
        uint8_t  codec_type   = avio_r8(pb);
        uint32_t codec_id_raw = avio_rb32(pb);
        uint32_t tb_num       = avio_rb32(pb);
        uint32_t tb_den       = avio_rb32(pb);
        uint32_t dim0         = avio_rb32(pb);
        uint16_t dim1         = avio_rb16(pb);
        uint32_t extra_len    = avio_rb32(pb);
        /* reserved */ avio_rb32(pb);

        if (extra_len > LOCF_MAX_EXTRA) {
            av_log(s, AV_LOG_ERROR, "Implausible extradata length %u\n", extra_len);
            return AVERROR_INVALIDDATA;
        }

        uint8_t *extra = NULL;
        if (extra_len > 0) {
            extra = av_malloc(extra_len);
            if (!extra)
                return AVERROR(ENOMEM);
            int ret = avio_read(pb, extra, extra_len);
            if (ret != (int)extra_len) {
                av_free(extra);
                return ret < 0 ? ret : AVERROR_EOF;
            }
        }

        int ret = loc_create_stream_v2(s, codec_type, codec_id_raw,
                                       tb_num, tb_den, dim0, dim1,
                                       extra, (int)extra_len);
        av_free(extra);
        return ret;
    }

    av_log(s, AV_LOG_ERROR, "Unknown LOCF version 0x%02x\n", version);
    return AVERROR_INVALIDDATA;
}

/* -------------------------------------------------------------------------
 * read_packet
 * ------------------------------------------------------------------------- */

static int loc_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    LOCDemuxContext *loc = s->priv_data;
    AVIOContext     *pb  = s->pb;

    if (loc->version == LOCF_VERSION_V1) {
        if (loc->v1_done)
            return AVERROR_EOF;

        uint64_t pay_len = loc->v1_pay_len;

        if (pay_len > LOCF_MAX_PAY_LEN)
            return AVERROR_INVALIDDATA;

        /* Allocate packet for the raw payload */
        int ret = av_new_packet(pkt, (int)pay_len);
        if (ret < 0)
            return ret;

        /* Apply pre-parsed extension metadata from read_header */
        pkt->pts          = loc->v1_pts;
        pkt->dts          = loc->v1_pts;
        pkt->stream_index = 0;
        if (loc->v1_key)
            pkt->flags |= AV_PKT_FLAG_KEY;

        /* Read payload */
        if (pay_len > 0) {
            ret = avio_read(pb, pkt->data, (int)pay_len);
            if (ret != (int)pay_len) {
                av_packet_unref(pkt);
                return ret < 0 ? ret : AVERROR_EOF;
            }
        }

        loc->v1_done = 1;
        return 0;
    }

    /* v2: read 28-byte frame header */
    uint8_t hdr[LOCF_V2_FRAME_HDR];
    int n = avio_read(pb, hdr, LOCF_V2_FRAME_HDR);
    if (n == 0 || n == AVERROR_EOF)
        return AVERROR_EOF;
    if (n < LOCF_V2_FRAME_HDR)
        return n < 0 ? n : AVERROR_EOF;

    int64_t  pts     = (int64_t)AV_RB64(hdr + 0);
    int64_t  dts     = (int64_t)AV_RB64(hdr + 8);
    uint32_t flags   = AV_RB32(hdr + 16);
    uint32_t ext_len = AV_RB32(hdr + 20);
    uint32_t pay_len = AV_RB32(hdr + 24);

    if (ext_len > LOCF_MAX_EXT_LEN || pay_len > LOCF_MAX_PAY_LEN) {
        av_log(s, AV_LOG_ERROR,
               "Implausible frame sizes ext=%u pay=%u\n", ext_len, pay_len);
        return AVERROR_INVALIDDATA;
    }

    /* Read LOC extension block */
    uint8_t *extbuf = NULL;
    if (ext_len > 0) {
        extbuf = av_malloc(ext_len);
        if (!extbuf)
            return AVERROR(ENOMEM);
        n = avio_read(pb, extbuf, (int)ext_len);
        if (n != (int)ext_len) {
            av_free(extbuf);
            return n < 0 ? n : AVERROR_EOF;
        }
    }

    /* Allocate packet for payload */
    int ret = av_new_packet(pkt, (int)pay_len);
    if (ret < 0) {
        av_free(extbuf);
        return ret;
    }

    /* Parse LOC extensions to enrich packet metadata */
    if (extbuf) {
        AVStream *st = s->streams[0];
        loc_object_t obj;
        loc_object_init(&obj);
        if (loc_decode_extensions(extbuf, ext_len, &obj) > 0) {
            /*
             * If the file-level pts is AV_NOPTS_VALUE (e.g. produced by a
             * non-FFmpeg writer), fall back to the capture_ts extension,
             * converting µs → stream timebase.
             */
            if (obj.has_capture_ts && pts == AV_NOPTS_VALUE) {
                pts = dts = av_rescale_q((int64_t)obj.capture_ts_us,
                                         (AVRational){1, 1000000},
                                         st->time_base);
            }
            /*
             * Audio level from the LOC extension (RFC 6464 §3).
             * There is no standard FFmpeg side-data type for per-packet
             * audio level; log it at trace level and discard for now.
             * A future patch could register AV_PKT_DATA_LOC_AUDIO_LEVEL.
             */
            if (obj.has_audio_level)
                av_log(s, AV_LOG_TRACE, "LOC audio level raw=0x%02x\n",
                       obj.audio_level_raw);
        }
        av_free(extbuf);
    }

    /* Read payload */
    if (pay_len > 0) {
        n = avio_read(pb, pkt->data, (int)pay_len);
        if (n != (int)pay_len) {
            av_packet_unref(pkt);
            return n < 0 ? n : AVERROR_EOF;
        }
    }

    pkt->stream_index = 0;
    pkt->pts          = pts;
    pkt->dts          = dts;
    pkt->flags        = (int)flags;

    return 0;
}

/* -------------------------------------------------------------------------
 * Format registration
 * ------------------------------------------------------------------------- */

const FFInputFormat ff_loc_demuxer = {
    .p.name           = "loc",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Low Overhead Container (LOC)"),
    .p.extensions     = "locf",
    .p.flags          = AVFMT_GENERIC_INDEX,
    .priv_data_size   = sizeof(LOCDemuxContext),
    .read_probe       = loc_probe,
    .read_header      = loc_read_header,
    .read_packet      = loc_read_packet,
};
