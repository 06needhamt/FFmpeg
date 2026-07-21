/*
 * Low Overhead Container (LOC) muxer
 * draft-ietf-moq-loc-03 / LOCF v3 file format
 *
 * Writes a LOCF v3 multi-frame file.  Each AVPacket is wrapped in a LOC
 * object: the Timestamp property is set from pkt->pts (with a draft-03
 * Timescale so media time is carried natively), the Video Frame Marking
 * property tracks AV_PKT_FLAG_KEY, and Video/Audio Config properties carry
 * the stream's extradata.  The raw coded payload is stored verbatim.
 *
 * Codec identity
 * --------------
 * draft-ietf-moq-loc delegates codec identification to the MoQ catalog
 * layer, so LOC objects carry configuration but no codec identity.  A file
 * has no catalog, so this muxer records the identity twice:
 *
 *   - in the LOCF v3 header, as an RFC 6381 / WebCodecs codec string,
 *     which (unlike a raw AVCodecID value) is stable across FFmpeg
 *     versions and meaningful to non-FFmpeg readers; and
 *   - as the libloc Codec ID property (0x21) inside LOC objects that also
 *     carry a Config property, so an object extracted from the file and
 *     placed on a MoQ track remains self-describing.
 *
 * The LOCF version is 0x03: libloc's demo format already uses 0x02 for its
 * *encrypted* MOQ Secure Object variant, whose header layout is different.
 * 0x02 must never be reused for another LOCF variant.
 *
 * LOCF v3 File Layout:
 *   === File header (32 bytes) ===
 *   [4]  'L','O','C','F'
 *   [1]  0x03
 *   [1]  codec_type    0 = video, 1 = audio
 *   [1]  loc_version   LOC container draft revision (2 or 3)
 *   [1]  codec_str_len
 *   [4]  tb_num        uint32 BE
 *   [4]  tb_den        uint32 BE
 *   [4]  dim0          uint32 BE  (width / sample_rate)
 *   [2]  dim1          uint16 BE  (height / channels)
 *   [4]  extra_len     uint32 BE
 *   [6]  reserved      (zero)
 *   === Codec string (codec_str_len bytes) ===
 *   === Extradata (extra_len bytes) ===
 *   === Frame records (one per write_packet call) ===
 *   [8]  pts      int64  BE
 *   [8]  dts      int64  BE
 *   [4]  flags    uint32 BE
 *   [4]  ext_len  uint32 BE
 *   [4]  pay_len  uint32 BE
 *   [N]  LOC property block
 *   [M]  LOC payload
 *
 * LOC revision handling
 * ---------------------
 * The revision (draft-02 or draft-03) is selected with the "loc_version"
 * option (default: draft-03, the latest implemented by libloc) and is
 * recorded in the file header so the demuxer never has to guess.  libloc
 * renders the correct property IDs and Video Frame Marking wire shape from
 * loc_object_t::version.  The Codec ID property (0x21) collides with
 * neither revision's registry and is emitted under both.
 *
 * Copyright (c) 2025 - MIT licence; see COPYING
 */

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavcodec/codec_id.h"
#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"
#include "mux.h"

#include <loc/loc.h>

/* -------------------------------------------------------------------------
 * On-disk constants (shared with locdec.c)
 * ------------------------------------------------------------------------- */

#define LOCF_MAGIC        UINT32_C(0x4C4F4346)
#define LOCF_VERSION_V3   0x03
#define LOCF_CT_VIDEO     0
#define LOCF_CT_AUDIO     1

/* Maximum size of a single encoded LOC object that we'll ever allocate */
#define LOCF_ENC_BUF_MAX  (16 * 1024 * 1024)

/* -------------------------------------------------------------------------
 * Private muxer context
 * ------------------------------------------------------------------------- */

typedef struct LOCMuxContext {
    const AVClass *class;

    /* LOC container draft revision to write (loc_version_t values) */
    int loc_version;

    /* RFC 6381 / WebCodecs codec string for the stream, NUL-terminated. */
    char codec_str[64];

    /*
     * Timebase in which the LOC Timestamp property is expressed and the
     * matching Timescale value.  Under draft-03 this tracks the stream
     * timebase where possible; draft-02 has no Timescale property, so it
     * is always µs there.
     */
    AVRational ts_tb;
    uint64_t   timescale;

    /* Audio Config + Codec ID are emitted once, on the first packet. */
    int audio_props_sent;
} LOCMuxContext;

/* -------------------------------------------------------------------------
 * Codec string mapping  (RFC 6381 / WebCodecs registry)
 * ------------------------------------------------------------------------- */

/**
 * Produce an RFC 6381 / WebCodecs codec string for @p id.
 *
 * Falls back to avcodec_get_name() - FFmpeg's stable codec-name
 * vocabulary - for codecs without a well-known registry string; the
 * demuxer resolves that vocabulary via avcodec_descriptor_get_by_name().
 */
static const char *loc_codec_to_string(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264:   return "avc1";
    case AV_CODEC_ID_HEVC:   return "hev1";
    case AV_CODEC_ID_AV1:    return "av01";
    case AV_CODEC_ID_VP9:    return "vp09";
    case AV_CODEC_ID_VP8:    return "vp8";
    case AV_CODEC_ID_OPUS:   return "opus";
    case AV_CODEC_ID_AAC:    return "mp4a.40.2";
    case AV_CODEC_ID_MP3:    return "mp4a.40.34";
    case AV_CODEC_ID_FLAC:   return "flac";
    case AV_CODEC_ID_VORBIS: return "vorbis";
    default:                 return avcodec_get_name(id);
    }
}

/* -------------------------------------------------------------------------
 * write_header
 * ------------------------------------------------------------------------- */

static int loc_write_header(AVFormatContext *s)
{
    LOCMuxContext      *loc = s->priv_data;
    AVIOContext        *pb  = s->pb;
    AVStream           *st;
    AVCodecParameters  *cp;
    uint8_t             codec_type;
    uint32_t            dim0, extra_len;
    uint16_t            dim1;
    size_t              codec_str_len;
    AVRational          tb;

    if (s->nb_streams != 1) {
        av_log(s, AV_LOG_ERROR,
               "LOC muxer supports exactly one stream (got %d)\n",
               s->nb_streams);
        return AVERROR(EINVAL);
    }

    st = s->streams[0];
    cp = st->codecpar;

    switch (cp->codec_type) {
    case AVMEDIA_TYPE_VIDEO: codec_type = LOCF_CT_VIDEO; break;
    case AVMEDIA_TYPE_AUDIO: codec_type = LOCF_CT_AUDIO; break;
    default:
        av_log(s, AV_LOG_ERROR,
               "LOC muxer only supports video and audio streams\n");
        return AVERROR(EINVAL);
    }

    loc->audio_props_sent = 0;

    av_strlcpy(loc->codec_str, loc_codec_to_string(cp->codec_id),
               sizeof(loc->codec_str));
    codec_str_len = strlen(loc->codec_str);

    dim0 = (codec_type == LOCF_CT_VIDEO)
           ? (uint32_t)cp->width
           : (uint32_t)cp->sample_rate;
    dim1 = (codec_type == LOCF_CT_VIDEO)
           ? (uint16_t)cp->height
           : (uint16_t)cp->ch_layout.nb_channels;

    /*
     * Use the stream's time_base if set; otherwise default to 1/90000 for
     * video and 1/1000000 for audio - both common and unambiguous.
     */
    tb = st->time_base;
    if (!tb.num || !tb.den) {
        tb = (codec_type == LOCF_CT_VIDEO)
             ? (AVRational){1, 90000}
             : (AVRational){1, 1000000};
        avpriv_set_pts_info(st, 64, tb.num, tb.den);
        tb = st->time_base;
    }

    /*
     * Pick the Timescale.  draft-03 carries media time directly, so a
     * 1/N timebase maps onto Timescale=N with the pts passed through
     * unchanged.  Anything else (or draft-02, which has no Timescale
     * property) is rescaled to microseconds.
     */
    if (loc->loc_version == LOC_VERSION_DRAFT_03 && tb.num == 1 && tb.den > 0) {
        loc->timescale = (uint64_t)tb.den;
        loc->ts_tb     = tb;
    } else {
        loc->timescale = 1000000;
        loc->ts_tb     = (AVRational){1, 1000000};
    }

    av_log(s, AV_LOG_VERBOSE,
           "Writing LOC objects as %s, codec '%s', timescale %" PRIu64 "\n",
           loc_version_name((loc_version_t)loc->loc_version),
           loc->codec_str, loc->timescale);

    extra_len = cp->extradata_size > 0 ? (uint32_t)cp->extradata_size : 0;

    /* === 32-byte file header === */
    avio_wb32(pb, LOCF_MAGIC);
    avio_w8  (pb, LOCF_VERSION_V3);
    avio_w8  (pb, codec_type);
    avio_w8  (pb, (uint8_t)loc->loc_version);
    avio_w8  (pb, (uint8_t)codec_str_len);
    avio_wb32(pb, (uint32_t)tb.num);
    avio_wb32(pb, (uint32_t)tb.den);
    avio_wb32(pb, dim0);
    avio_wb16(pb, dim1);
    avio_wb32(pb, extra_len);
    avio_wb32(pb, 0);              /* reserved */
    avio_wb16(pb, 0);              /* reserved */

    /* === Codec string === */
    if (codec_str_len > 0)
        avio_write(pb, (const unsigned char *)loc->codec_str,
                   (int)codec_str_len);

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
    LOCMuxContext     *loc = s->priv_data;
    AVIOContext       *pb  = s->pb;
    AVStream          *st  = s->streams[pkt->stream_index];
    AVCodecParameters *cp  = st->codecpar;
    loc_object_t       obj;
    uint8_t           *locbuf;
    ssize_t            total_sz, written, ext_sz;
    uint32_t           ext_len, pay_len;
    int                self_describing = 0;

    loc_object_init_version(&obj, (loc_version_t)loc->loc_version);

    /*
     * Timestamp.  Under draft-03 with a Timescale this is media time in
     * timescale units; under draft-02 it is wall-clock µs since the epoch.
     */
    if (pkt->pts != AV_NOPTS_VALUE && st->time_base.den) {
        obj.has_capture_ts = 1;
        obj.capture_ts_us  = (uint64_t)av_rescale_q(pkt->pts,
                                                    st->time_base,
                                                    loc->ts_tb);
        if (loc->loc_version == LOC_VERSION_DRAFT_03) {
            obj.has_timescale = 1;
            obj.timescale     = loc->timescale;
        }
    }

    /* Video-specific properties */
    if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
        uint8_t vfm_flags = LOC_VFM_START_OF_FRAME | LOC_VFM_END_OF_FRAME;

        obj.has_video_frame_marking = 1;
        if (pkt->flags & AV_PKT_FLAG_KEY)
            vfm_flags |= LOC_VFM_INDEPENDENT;
        else
            vfm_flags |= LOC_VFM_DISCARDABLE;

        obj.video_frame_marking = loc_vfm_pack(vfm_flags, 0, 0, 0);
        /* 0 = derive on-wire length (draft-03 short form when LID and
         * TL0PICIDX are zero; varint under draft-02). */
        obj.video_frame_marking_len = 0;

        /*
         * Key frames with out-of-band extradata get the Config property.
         * Such objects are made fully self-describing with the Codec ID
         * property as well, so a LOC object extracted from this file and
         * placed on a MoQ track without a catalog remains identifiable.
         */
        if ((pkt->flags & AV_PKT_FLAG_KEY) &&
            cp->extradata && cp->extradata_size > 0) {
            obj.has_video_config = 1;
            obj.video_config     = cp->extradata;
            obj.video_config_len = (size_t)cp->extradata_size;
            self_describing      = 1;
        }
    }

    /* Audio-specific properties */
    if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
        /*
         * No per-packet audio level exists in the AVPacket model; emit the
         * silence level (0x7F) by default.
         */
        obj.has_audio_level = 1;
        obj.audio_level_raw = LOC_AUDIO_LEVEL_SILENCE;

        /*
         * Audio Config (draft-03) + Codec ID once, on the first packet.
         * Unlike video, every audio packet is typically flagged as a key
         * frame, so attaching them to each one would bloat the stream.
         */
        if (!loc->audio_props_sent) {
            if (loc->loc_version == LOC_VERSION_DRAFT_03 &&
                cp->extradata && cp->extradata_size > 0) {
                obj.has_audio_config = 1;
                obj.audio_config     = cp->extradata;
                obj.audio_config_len = (size_t)cp->extradata_size;
            }
            self_describing       = 1;
            loc->audio_props_sent = 1;
        }
    }

    /* Codec ID property (libloc 0x21, valid under both revisions). */
    if (self_describing && loc->codec_str[0] != '\0') {
        obj.has_codec_id = 1;
        obj.codec_id_str = (const uint8_t *)loc->codec_str;
        obj.codec_id_len = strlen(loc->codec_str);
    }

    /* Payload = raw packet data */
    obj.payload     = pkt->data;
    obj.payload_len = (size_t)pkt->size;

    total_sz = loc_encoded_size(&obj);
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

    locbuf = av_malloc((size_t)total_sz);
    if (!locbuf)
        return AVERROR(ENOMEM);

    written = loc_encode(locbuf, (size_t)total_sz, &obj);
    if (written < 0 || written != total_sz) {
        av_log(s, AV_LOG_ERROR, "loc_encode failed: %s\n",
               loc_strerror((int)written));
        av_free(locbuf);
        return AVERROR_EXTERNAL;
    }

    ext_sz = loc_extensions_encoded_size(&obj);
    if (ext_sz < 0) {
        av_log(s, AV_LOG_ERROR, "loc_extensions_encoded_size failed: %s\n",
               loc_strerror((int)ext_sz));
        av_free(locbuf);
        return AVERROR_EXTERNAL;
    }
    ext_len = (uint32_t)ext_sz;
    pay_len = (uint32_t)pkt->size;

    /* === 28-byte frame record header === */
    avio_wb64(pb, (uint64_t)pkt->pts);
    avio_wb64(pb, (uint64_t)pkt->dts);
    avio_wb32(pb, (uint32_t)pkt->flags);
    avio_wb32(pb, ext_len);
    avio_wb32(pb, pay_len);

    /* === LOC object (property block + payload) === */
    avio_write(pb, locbuf, (int)written);

    av_free(locbuf);
    return 0;
}

static int loc_write_trailer(AVFormatContext *s)
{
    /* No trailer needed for LOCF v3; EOF terminates the frame sequence. */
    return 0;
}

/* -------------------------------------------------------------------------
 * Options
 * ------------------------------------------------------------------------- */

#define OFFSET(x) offsetof(LOCMuxContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM

static const AVOption loc_options[] = {
    { "loc_version", "LOC container draft revision to write",
      OFFSET(loc_version), AV_OPT_TYPE_INT,
      { .i64 = LOC_VERSION_LATEST },
      LOC_VERSION_DRAFT_02, LOC_VERSION_DRAFT_03, ENC, .unit = "loc_version" },
    { "draft02", "draft-ietf-moq-loc-02 (legacy property numbering)",
      0, AV_OPT_TYPE_CONST, { .i64 = LOC_VERSION_DRAFT_02 },
      0, 0, ENC, .unit = "loc_version" },
    { "draft03", "draft-ietf-moq-loc-03 (default)",
      0, AV_OPT_TYPE_CONST, { .i64 = LOC_VERSION_DRAFT_03 },
      0, 0, ENC, .unit = "loc_version" },
    { NULL },
};

static const AVClass loc_muxer_class = {
    .class_name = "LOC muxer",
    .item_name  = av_default_item_name,
    .option     = loc_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

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
    .p.priv_class     = &loc_muxer_class,
    .priv_data_size   = sizeof(LOCMuxContext),
    .write_header     = loc_write_header,
    .write_packet     = loc_write_packet,
    .write_trailer    = loc_write_trailer,
};
