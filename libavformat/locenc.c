/*
 * Low Overhead Container (LOC) muxer
 * draft-ietf-moq-loc-01 / LOCF v2 file format
 *
 * Writes a LOCF v2 multi-frame file.  Each AVPacket is wrapped in a LOC
 * object: the capture timestamp extension is set from pkt->pts, the video
 * frame marking extension is set based on AV_PKT_FLAG_KEY, and (for video)
 * the video_config extension carries a copy of the stream's extradata on
 * every key-frame.  The raw coded payload is stored verbatim.
 *
 * LOCF v2 File Layout:
 *   === File header (32 bytes) ===
 *   [4]  'L','O','C','F'
 *   [1]  0x02
 *   [1]  codec_type   0 = video, 1 = audio
 *   [4]  codec_id     uint32 BE  (AVCodecID)
 *   [4]  tb_num       uint32 BE
 *   [4]  tb_den       uint32 BE
 *   [4]  dim0         uint32 BE  (width / sample_rate)
 *   [2]  dim1         uint16 BE  (height / channels)
 *   [4]  extra_len    uint32 BE
 *   [4]  reserved (0x00000000)
 *   === Extradata (extra_len bytes) ===
 *   === Frame records (one per write_packet call) ===
 *   [8]  pts      int64  BE
 *   [8]  dts      int64  BE
 *   [4]  flags    uint32 BE
 *   [4]  ext_len  uint32 BE
 *   [4]  pay_len  uint32 BE
 *   [N]  LOC extension block
 *   [M]  LOC payload
 *
 * Copyright (c) 2025 – MIT licence; see COPYING
 */

#include "libavutil/avassert.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"
#include "mux.h"

#include <loc/loc.h>

/* -------------------------------------------------------------------------
 * On-disk constants (shared with locdec.c)
 * ------------------------------------------------------------------------- */

#define LOCF_MAGIC        UINT32_C(0x4C4F4346)
#define LOCF_VERSION_V2   0x02
#define LOCF_CT_VIDEO     0
#define LOCF_CT_AUDIO     1

/* Maximum size of a single encoded LOC object that we'll ever allocate */
#define LOCF_ENC_BUF_MAX  (16 * 1024 * 1024)

/* -------------------------------------------------------------------------
 * Private muxer context
 * ------------------------------------------------------------------------- */

typedef struct LOCMuxContext {
    int stream_idx;   /* index of the single stream we're muxing */
} LOCMuxContext;

/* -------------------------------------------------------------------------
 * write_header
 * ------------------------------------------------------------------------- */

static int loc_write_header(AVFormatContext *s)
{
    LOCMuxContext      *loc = s->priv_data;
    AVIOContext        *pb  = s->pb;

    if (s->nb_streams != 1) {
        av_log(s, AV_LOG_ERROR,
               "LOC muxer supports exactly one stream (got %d)\n",
               s->nb_streams);
        return AVERROR(EINVAL);
    }

    AVStream           *st  = s->streams[0];
    AVCodecParameters  *cp  = st->codecpar;

    uint8_t codec_type;
    switch (cp->codec_type) {
    case AVMEDIA_TYPE_VIDEO: codec_type = LOCF_CT_VIDEO; break;
    case AVMEDIA_TYPE_AUDIO: codec_type = LOCF_CT_AUDIO; break;
    default:
        av_log(s, AV_LOG_ERROR,
               "LOC muxer only supports video and audio streams\n");
        return AVERROR(EINVAL);
    }

    loc->stream_idx = 0;

    uint32_t dim0 = (codec_type == LOCF_CT_VIDEO)
                    ? (uint32_t)cp->width
                    : (uint32_t)cp->sample_rate;
    uint16_t dim1 = (codec_type == LOCF_CT_VIDEO)
                    ? (uint16_t)cp->height
                    : (uint16_t)cp->ch_layout.nb_channels;

    /*
     * Use the stream's time_base if it has been set by the caller; otherwise
     * default to 1/1000000 (µs) for audio and 1/90000 for video – both
     * common and unambiguous choices.
     */
    AVRational tb = st->time_base;
    if (!tb.num || !tb.den) {
        tb = (codec_type == LOCF_CT_VIDEO)
             ? (AVRational){1, 90000}
             : (AVRational){1, 1000000};
        avpriv_set_pts_info(st, 64, tb.num, tb.den);
    }

    uint32_t extra_len = cp->extradata_size > 0
                         ? (uint32_t)cp->extradata_size
                         : 0;

    /* === 32-byte file header === */
    avio_wb32(pb, LOCF_MAGIC);
    avio_w8  (pb, LOCF_VERSION_V2);
    avio_w8  (pb, codec_type);
    avio_wb32(pb, (uint32_t)cp->codec_id);
    avio_wb32(pb, (uint32_t)tb.num);
    avio_wb32(pb, (uint32_t)tb.den);
    avio_wb32(pb, dim0);
    avio_wb16(pb, dim1);
    avio_wb32(pb, extra_len);
    avio_wb32(pb, 0);   /* reserved */

    /* === Extradata === */
    if (extra_len > 0)
        avio_write(pb, cp->extradata, extra_len);

    return 0;
}

/* -------------------------------------------------------------------------
 * write_packet
 * ------------------------------------------------------------------------- */

static int loc_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIOContext       *pb = s->pb;
    AVStream          *st = s->streams[pkt->stream_index];
    AVCodecParameters *cp = st->codecpar;

    /* Build a loc_object_t for this frame */
    loc_object_t obj;
    loc_object_init(&obj);

    /* Capture timestamp from PTS (convert to µs since epoch).
     * We can only do a sensible conversion if the timebase is set. */
    if (pkt->pts != AV_NOPTS_VALUE && st->time_base.den) {
        obj.has_capture_ts = 1;
        obj.capture_ts_us  = (uint64_t)av_rescale_q(pkt->pts,
                                 st->time_base,
                                 (AVRational){1, 1000000});
    }

    /* Video-specific extensions */
    if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
        obj.has_video_frame_marking = 1;

        uint8_t vfm_flags = LOC_VFM_START_OF_FRAME | LOC_VFM_END_OF_FRAME;
        if (pkt->flags & AV_PKT_FLAG_KEY)
            vfm_flags |= LOC_VFM_INDEPENDENT;
        else
            vfm_flags |= LOC_VFM_DISCARDABLE;

        obj.video_frame_marking = loc_vfm_pack(vfm_flags, 0, 0, 0);

        /*
         * Attach video_config (ID=13) on key-frames that carry out-of-band
         * extradata.  For in-band SPS/PPS (Annex-B H.264, etc.) the
         * extradata might be empty, in which case we skip the extension.
         */
        if ((pkt->flags & AV_PKT_FLAG_KEY) &&
            cp->extradata && cp->extradata_size > 0) {
            obj.has_video_config   = 1;
            obj.video_config       = cp->extradata;
            obj.video_config_len   = (size_t)cp->extradata_size;
        }
    }

    /* Audio-specific extensions */
    if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
        /*
         * We don't have a per-packet audio level in the AVPacket model, so
         * emit the maximum silence level (0x7F) by default.  Downstream
         * could set this to a proper value via a custom side-data type.
         */
        obj.has_audio_level  = 1;
        obj.audio_level_raw  = LOC_AUDIO_LEVEL_SILENCE;
    }

    /* Payload = raw packet data */
    obj.payload     = pkt->data;
    obj.payload_len = (size_t)pkt->size;

    /* Compute total encoded LOC object size */
    ssize_t total_sz = loc_encoded_size(&obj);
    if (total_sz < 0) {
        av_log(s, AV_LOG_ERROR, "loc_encoded_size failed: %s\n",
               loc_strerror((int)total_sz));
        return AVERROR_EXTERNAL;
    }
    if ((size_t)total_sz > LOCF_ENC_BUF_MAX) {
        av_log(s, AV_LOG_ERROR, "LOC object too large (%zd bytes)\n",
               (ssize_t)total_sz);
        return AVERROR(ERANGE);
    }

    /* Encode the LOC object into a temporary buffer */
    uint8_t *locbuf = av_malloc((size_t)total_sz);
    if (!locbuf)
        return AVERROR(ENOMEM);

    ssize_t written = loc_encode(locbuf, (size_t)total_sz, &obj);
    if (written < 0 || written != total_sz) {
        av_log(s, AV_LOG_ERROR, "loc_encode failed: %s\n",
               loc_strerror((int)written));
        av_free(locbuf);
        return AVERROR_EXTERNAL;
    }

    /*
     * The extension block length is (total_size – payload_len).
     * The payload starts immediately after the extension block.
     */
    ssize_t ext_sz = loc_extensions_encoded_size(&obj);
    if (ext_sz < 0) {
        av_free(locbuf);
        return AVERROR_EXTERNAL;
    }
    uint32_t ext_len = (uint32_t)ext_sz;
    uint32_t pay_len = (uint32_t)pkt->size;

    /* === 28-byte frame record header === */
    avio_wb64(pb, (uint64_t)pkt->pts);
    avio_wb64(pb, (uint64_t)pkt->dts);
    avio_wb32(pb, (uint32_t)pkt->flags);
    avio_wb32(pb, ext_len);
    avio_wb32(pb, pay_len);

    /* === LOC object (extension block + payload) === */
    avio_write(pb, locbuf, (int)written);

    av_free(locbuf);
    return 0;
}

static int loc_write_trailer(AVFormatContext *s)
{
    /* No trailer needed for LOCF v2; EOF terminates the frame sequence. */
    return 0;
}

/* -------------------------------------------------------------------------
 * Format registration
 * ------------------------------------------------------------------------- */

const FFOutputFormat ff_loc_muxer = {
    .p.name           = "loc",
    .p.long_name      = NULL_IF_CONFIG_SMALL("Low Overhead Container (LOC)"),
    .p.mime_type      = "application/octet-stream",
    .p.extensions     = "locf",
    .p.audio_codec    = AV_CODEC_ID_OPUS,
    .p.video_codec    = AV_CODEC_ID_H264,
    .p.flags          = AVFMT_GLOBALHEADER | AVFMT_VARIABLE_FPS |
                        AVFMT_TS_NONSTRICT,
    .priv_data_size   = sizeof(LOCMuxContext),
    .write_header     = loc_write_header,
    .write_packet     = loc_write_packet,
    .write_trailer    = loc_write_trailer,
};
