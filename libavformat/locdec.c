/*
 * Low Overhead Container (LOC) demuxer
 * draft-ietf-moq-loc-03 (also reads draft-ietf-moq-loc-02 property blocks)
 *
 * Codec identification
 * --------------------
 * draft-ietf-moq-loc deliberately delegates codec identification to the
 * MoQ catalog layer (WARP/MSF), so a LOC property block carries codec
 * *configuration* but no codec *identity*.  For LOC objects at rest there
 * is no catalog, so identity comes from, in priority order:
 *
 *   1. the libloc Codec ID property (0x21): an RFC 6381 / WebCodecs codec
 *      string such as "avc1.64001f", "av01.0.08M.08", "opus";
 *   2. for LOCF v3 files, the codec string recorded in the file header;
 *   3. lavf packet probing (AV_CODEC_ID_PROBE + request_probe), which
 *      identifies self-describing bitstreams (Annex-B H.264/HEVC, ADTS
 *      AAC, ...).  Per WebCodecs semantics a payload without out-of-band
 *      config is in self-describing form, so probing covers precisely the
 *      case where no Config property exists to consult.
 *
 * No codec is ever assumed from unrelated evidence: a Video Frame Marking
 * property does not make a stream H.264, or even video (libloc's demo
 * historically attached VFM to arbitrary payloads).  A stream whose codec
 * cannot be established is exposed for probing rather than mislabelled.
 *
 * LOC revision handling
 * ---------------------
 * draft-03 renamed "Header Extensions" to "Properties" and renumbered the
 * identifiers, so a property block cannot be parsed correctly without
 * knowing which revision produced it:
 *
 *   Property             draft-02 ID   draft-03 ID
 *   Timestamp                2            0x0A
 *   Timescale                -            0x08   (new in draft-03)
 *   Video Frame Marking      4 (varint)   0x09   (length-prefixed bytes)
 *   Audio Level              6            0x0C
 *   Video Config            13            0x0D   (unchanged)
 *   Audio Config             -            0x0F   (new in draft-03)
 *   Codec ID (libloc)       0x21          0x21   (identical either way)
 *
 * The revision is resolved per file: the explicit revision byte in the
 * LOCF v3 header when present, else loc_detect_version() on the property
 * block, else a per-variant fallback.
 *
 * Supported on-disk variants (all begin 'L','O','C','F' + version byte):
 *
 *   0x01  LOCF v1 - single-frame plaintext demo format from libloc:
 *     [4]  magic  [1] 0x01
 *     [4]  ext_block_len  (uint32 BE)
 *     [8]  payload_len    (uint64 BE)
 *     [N]  LOC property block
 *     [M]  LOC payload
 *
 *   0x02  libloc encrypted demo format (MOQ Secure Object).  The payload
 *     is AEAD ciphertext; FFmpeg has no access to the track key, so these
 *     files are cleanly rejected with a pointer to `loc_demo decode`.
 *     (Never assign 0x02 to another LOCF variant.)
 *
 *   0x03  LOCF v3 - multi-frame FFmpeg format (written by locenc.c):
 *     === File header (32 bytes) ===
 *     [4]  magic  [1] 0x03
 *     [1]  codec_type    0 = video, 1 = audio
 *     [1]  loc_version   LOC draft revision (2 or 3; 0 = unspecified)
 *     [1]  codec_str_len length of the codec string below (may be 0)
 *     [4]  tb_num        uint32 BE  (AVRational.num)
 *     [4]  tb_den        uint32 BE  (AVRational.den)
 *     [4]  dim0          uint32 BE  (width  for video, sample_rate for audio)
 *     [2]  dim1          uint16 BE  (height for video, channels    for audio)
 *     [4]  extra_len     uint32 BE
 *     [6]  reserved      (zero)
 *     === Codec string (codec_str_len bytes, RFC 6381 / WebCodecs) ===
 *     === Extradata (extra_len bytes) ===
 *     === Frame records (until EOF) ===
 *     [8]  pts      int64  BE
 *     [8]  dts      int64  BE
 *     [4]  flags    uint32 BE  (AV_PKT_FLAG_*)
 *     [4]  ext_len  uint32 BE  (LOC property block length)
 *     [4]  pay_len  uint32 BE  (LOC payload length)
 *     [N]  LOC property block
 *     [M]  LOC payload (packet data returned to caller)
 *
 * The codec string in the v3 header is an interchange identifier: unlike a
 * raw AVCodecID value it is stable across FFmpeg versions and meaningful
 * to non-FFmpeg readers.
 *
 * Copyright (c) 2025 - MIT licence; see COPYING
 */

#include "libavutil/avassert.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavcodec/codec_desc.h"
#include "libavcodec/codec_id.h"
#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "internal.h"

#include <loc/loc.h>

/* -------------------------------------------------------------------------
 * On-disk constants
 * ------------------------------------------------------------------------- */

#define LOCF_MAGIC          UINT32_C(0x4C4F4346)  /* 'LOCF' */
#define LOCF_VERSION_V1     0x01   /* libloc demo, plaintext single frame  */
#define LOCF_VERSION_ENC    0x02   /* libloc demo, encrypted secure object */
#define LOCF_VERSION_V3     0x03   /* FFmpeg multi-frame                   */

/* v3 fixed header size (before codec string + extradata) */
#define LOCF_V3_HDR_SIZE    32
/* v3 frame record header size (before ext + payload data) */
#define LOCF_V3_FRAME_HDR   28  /* 8+8+4+4+4 */

/* Sanity caps to avoid malloc bombs */
#define LOCF_MAX_EXTRA      (4 * 1024 * 1024)
#define LOCF_MAX_EXT_LEN    (1 * 1024 * 1024)
#define LOCF_MAX_PAY_LEN    (256 * 1024 * 1024)

/* v3 codec_type field values */
#define LOCF_CT_VIDEO   0
#define LOCF_CT_AUDIO   1

#define LOCF_LOC_VERSION_UNSPECIFIED  0

/* -------------------------------------------------------------------------
 * Private context
 * ------------------------------------------------------------------------- */

typedef struct LOCDemuxContext {
    int      version;      /* LOCF container version: 1 or 3 */

    loc_version_t declared_version;   /* from v3 header, or UNKNOWN */
    loc_version_t fallback_version;   /* used when detection is inconclusive */

    /* v1 single-frame state */
    int      v1_done;
    uint64_t v1_pay_len;
    int64_t  v1_pts;              /* AV_NOPTS_VALUE if no timestamp found */
    int      v1_key;
} LOCDemuxContext;

/* -------------------------------------------------------------------------
 * Codec string mapping  (RFC 6381 / WebCodecs registry)
 * ------------------------------------------------------------------------- */

/**
 * Map an RFC 6381 / WebCodecs codec string to an AVCodecID.
 *
 * Matches on the base token (up to the first '.').  Falls back to
 * avcodec_descriptor_get_by_name() so strings from FFmpeg's own name
 * vocabulary ("h264", "pcm_s16le", ...) written by an older muxer resolve
 * too.  Returns AV_CODEC_ID_NONE when the string is not recognised.
 */
static enum AVCodecID loc_codec_from_string(const char *str, size_t len)
{
    char base[64];
    size_t blen = 0;

    if (!str || !len)
        return AV_CODEC_ID_NONE;

    while (blen < len && blen < sizeof(base) - 1 && str[blen] != '.') {
        base[blen] = str[blen];
        blen++;
    }
    base[blen] = '\0';

    if (!strcmp(base, "avc1") || !strcmp(base, "avc3"))
        return AV_CODEC_ID_H264;
    if (!strcmp(base, "hev1") || !strcmp(base, "hvc1"))
        return AV_CODEC_ID_HEVC;
    if (!strcmp(base, "av01"))
        return AV_CODEC_ID_AV1;
    if (!strcmp(base, "vp09"))
        return AV_CODEC_ID_VP9;
    if (!strcmp(base, "vp08") || !strcmp(base, "vp8"))
        return AV_CODEC_ID_VP8;
    if (!strcmp(base, "vp9"))
        return AV_CODEC_ID_VP9;
    if (!strcmp(base, "opus") || !strcmp(base, "Opus"))
        return AV_CODEC_ID_OPUS;
    if (!strcmp(base, "flac") || !strcmp(base, "fLaC"))
        return AV_CODEC_ID_FLAC;
    if (!strcmp(base, "vorbis"))
        return AV_CODEC_ID_VORBIS;
    if (!strcmp(base, "mp4a")) {
        /* Object types 0x40.34 / 0x6B are MPEG-1/2 Layer III. */
        if (len >= blen + 3 &&
            (!strncmp(str + blen, ".40.34", FFMIN(len - blen, 6)) ||
             !strncmp(str + blen, ".6B",    FFMIN(len - blen, 3)) ||
             !strncmp(str + blen, ".6b",    FFMIN(len - blen, 3))))
            return AV_CODEC_ID_MP3;
        return AV_CODEC_ID_AAC;
    }

    /* FFmpeg codec-name vocabulary fallback ("h264", "aac", ...). */
    {
        const AVCodecDescriptor *desc = avcodec_descriptor_get_by_name(base);
        if (desc)
            return desc->id;
    }

    return AV_CODEC_ID_NONE;
}

/**
 * Media type implied by an RFC 6381 base token, for streams where only the
 * codec string is known.  Returns AVMEDIA_TYPE_UNKNOWN if unclassified.
 */
static enum AVMediaType loc_media_type_from_codec(enum AVCodecID id)
{
    const AVCodecDescriptor *desc = avcodec_descriptor_get(id);
    return desc ? desc->type : AVMEDIA_TYPE_UNKNOWN;
}

/* -------------------------------------------------------------------------
 * Probe
 * ------------------------------------------------------------------------- */

static int loc_probe(const AVProbeData *p)
{
    if (p->buf_size < 5)
        return 0;
    if (AV_RB32(p->buf) != LOCF_MAGIC)
        return 0;
    if (p->buf[4] == LOCF_VERSION_V1 ||
        p->buf[4] == LOCF_VERSION_ENC ||
        p->buf[4] == LOCF_VERSION_V3)
        return AVPROBE_SCORE_MAX;
    return 0;
}

/* -------------------------------------------------------------------------
 * LOC revision resolution
 * ------------------------------------------------------------------------- */

static loc_version_t loc_resolve_version(AVFormatContext *s,
                                         const LOCDemuxContext *loc,
                                         const uint8_t *ext, size_t ext_len)
{
    loc_version_t detected;

    if (loc->declared_version == LOC_VERSION_DRAFT_02 ||
        loc->declared_version == LOC_VERSION_DRAFT_03)
        return loc->declared_version;

    if (ext && ext_len) {
        detected = loc_detect_version(ext, ext_len);
        if (detected != LOC_VERSION_UNKNOWN) {
            av_log(s, AV_LOG_DEBUG, "LOC revision auto-detected as %s\n",
                   loc_version_name(detected));
            return detected;
        }
    }

    av_log(s, AV_LOG_DEBUG,
           "LOC revision indeterminate; assuming %s\n",
           loc_version_name(loc->fallback_version));
    return loc->fallback_version;
}

/**
 * Convert a decoded LOC Timestamp to the stream timebase, honouring a
 * draft-03 Timescale when present (else wall-clock microseconds).
 */
static int64_t loc_timestamp_to_stream_tb(AVFormatContext *s,
                                          const loc_object_t *obj,
                                          AVRational stream_tb)
{
    AVRational src_tb = (AVRational){ 1, 1000000 };

    if (obj->has_timescale) {
        if (obj->timescale == 0 || obj->timescale > INT_MAX) {
            av_log(s, AV_LOG_WARNING,
                   "Ignoring implausible LOC Timescale %" PRIu64 "\n",
                   obj->timescale);
        } else {
            src_tb = (AVRational){ 1, (int)obj->timescale };
        }
    }

    return av_rescale_q((int64_t)obj->capture_ts_us, src_tb, stream_tb);
}

/* -------------------------------------------------------------------------
 * Shared: apply codec evidence from a LOC object to a stream
 * ------------------------------------------------------------------------- */

/**
 * Establish codec_type / codec_id / extradata for @p st from a decoded LOC
 * object, using only actual evidence (Codec ID property, Config
 * properties).  When no evidence identifies the codec, the stream is set
 * up for lavf packet probing instead of being mislabelled.
 */
static int loc_apply_stream_evidence(AVFormatContext *s, AVStream *st,
                                     const loc_object_t *obj)
{
    AVCodecParameters *cp = st->codecpar;

    /* 1. Codec ID property (libloc 0x21): the authoritative identity. */
    if (obj->has_codec_id && obj->codec_id_len) {
        enum AVCodecID id = loc_codec_from_string(
            (const char *)obj->codec_id_str, obj->codec_id_len);
        if (id != AV_CODEC_ID_NONE) {
            cp->codec_id   = id;
            cp->codec_type = loc_media_type_from_codec(id);
            av_log(s, AV_LOG_VERBOSE, "LOC Codec ID '%.*s' -> %s\n",
                   (int)obj->codec_id_len, (const char *)obj->codec_id_str,
                   avcodec_get_name(id));
        } else {
            av_log(s, AV_LOG_WARNING,
                   "Unrecognised LOC Codec ID '%.*s'; will probe\n",
                   (int)obj->codec_id_len, (const char *)obj->codec_id_str);
        }
    }

    /* 2. Config properties: pin the media type and carry extradata.
     *    They do NOT identify the codec - config bytes are opaque here. */
    if (obj->has_video_config && obj->video_config_len) {
        if (cp->codec_type == AVMEDIA_TYPE_UNKNOWN)
            cp->codec_type = AVMEDIA_TYPE_VIDEO;
        if (!cp->extradata) {
            uint8_t *ed = av_mallocz(obj->video_config_len
                                     + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!ed)
                return AVERROR(ENOMEM);
            memcpy(ed, obj->video_config, obj->video_config_len);
            cp->extradata      = ed;
            cp->extradata_size = (int)obj->video_config_len;
        }
    } else if (obj->has_audio_config && obj->audio_config_len) {
        if (cp->codec_type == AVMEDIA_TYPE_UNKNOWN)
            cp->codec_type = AVMEDIA_TYPE_AUDIO;
        if (!cp->extradata) {
            uint8_t *ed = av_mallocz(obj->audio_config_len
                                     + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!ed)
                return AVERROR(ENOMEM);
            memcpy(ed, obj->audio_config, obj->audio_config_len);
            cp->extradata      = ed;
            cp->extradata_size = (int)obj->audio_config_len;
        }
    }

    /* 3. Nothing identified the codec: expose the payload to lavf's
     *    packet prober rather than guessing. */
    if (cp->codec_id == AV_CODEC_ID_NONE) {
        cp->codec_id = AV_CODEC_ID_PROBE;
        ffstream(st)->request_probe = AVPROBE_SCORE_STREAM_RETRY;
        av_log(s, AV_LOG_VERBOSE,
               "No codec identity in LOC object; probing packet data\n");
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * read_header
 * ------------------------------------------------------------------------- */

static int loc_read_header(AVFormatContext *s)
{
    LOCDemuxContext *loc = s->priv_data;
    AVIOContext     *pb  = s->pb;

    uint32_t magic   = avio_rb32(pb);
    uint8_t  version = avio_r8(pb);

    if (magic != LOCF_MAGIC) {
        av_log(s, AV_LOG_ERROR, "Not a LOCF file\n");
        return AVERROR_INVALIDDATA;
    }

    loc->version          = version;
    loc->declared_version = LOC_VERSION_UNKNOWN;

    if (version == LOCF_VERSION_ENC) {
        av_log(s, AV_LOG_ERROR,
               "LOCF version 0x02 is libloc's encrypted MOQ Secure Object "
               "format; the payload is AEAD ciphertext and cannot be "
               "demuxed without the track key. Decrypt it first with "
               "`loc_demo decode <file> <out> --decrypt <hex_key>`.\n");
        return AVERROR(ENOSYS);
    }

    if (version == LOCF_VERSION_V1) {
        /*
         * v1: single-frame demo format.  No recorded revision; libloc's
         * demo tracks the library default, which is also the fallback
         * when auto-detection is inconclusive.
         */
        uint32_t ext_len = avio_rb32(pb);
        uint64_t pay_len = avio_rb64(pb);

        loc->fallback_version = LOC_VERSION_DEFAULT;

        if (pay_len > LOCF_MAX_PAY_LEN) {
            av_log(s, AV_LOG_ERROR, "Implausible v1 payload length\n");
            return AVERROR_INVALIDDATA;
        }

        loc->v1_pay_len = pay_len;
        loc->v1_done    = 0;
        loc->v1_pts     = AV_NOPTS_VALUE;
        loc->v1_key     = 1;  /* single object: independent by construction */

        AVStream *st = avformat_new_stream(s, NULL);
        if (!st)
            return AVERROR(ENOMEM);

        st->codecpar->codec_type = AVMEDIA_TYPE_UNKNOWN;
        st->codecpar->codec_id   = AV_CODEC_ID_NONE;

        /* µs timebase; a Timescale property (draft-03) is rescaled into it. */
        avpriv_set_pts_info(st, 64, 1, 1000000);

        if (ext_len > 0 && ext_len <= LOCF_MAX_EXT_LEN) {
            uint8_t *extbuf = av_malloc(ext_len);
            int      ret;

            if (!extbuf)
                return AVERROR(ENOMEM);

            ret = avio_read(pb, extbuf, (int)ext_len);
            if (ret == (int)ext_len) {
                loc_object_t  obj;
                loc_version_t ver = loc_resolve_version(s, loc,
                                                        extbuf, ext_len);

                loc_object_init_version(&obj, ver);
                if (loc_decode_extensions(extbuf, ext_len, &obj) > 0) {
                    if (obj.has_capture_ts)
                        loc->v1_pts = loc_timestamp_to_stream_tb(
                                          s, &obj, st->time_base);
                    if (obj.has_video_frame_marking)
                        loc->v1_key = !!(obj.video_frame_marking
                                         & LOC_VFM_INDEPENDENT);

                    ret = loc_apply_stream_evidence(s, st, &obj);
                    if (ret < 0) {
                        av_free(extbuf);
                        return ret;
                    }
                }
            } else {
                av_log(s, AV_LOG_WARNING,
                       "Could not read v1 property block (%d/%u)\n",
                       ret, ext_len);
            }
            av_free(extbuf);
        }

        /* No property block at all: probe the payload. */
        if (st->codecpar->codec_id == AV_CODEC_ID_NONE) {
            st->codecpar->codec_id = AV_CODEC_ID_PROBE;
            ffstream(st)->request_probe = AVPROBE_SCORE_STREAM_RETRY;
        }

        return 0;
    }

    if (version == LOCF_VERSION_V3) {
        /* 5 bytes consumed (magic+ver); read the remaining 27. */
        uint8_t  codec_type    = avio_r8(pb);
        uint8_t  loc_ver       = avio_r8(pb);
        uint8_t  codec_str_len = avio_r8(pb);
        uint32_t tb_num        = avio_rb32(pb);
        uint32_t tb_den        = avio_rb32(pb);
        uint32_t dim0          = avio_rb32(pb);
        uint16_t dim1          = avio_rb16(pb);
        uint32_t extra_len     = avio_rb32(pb);
        char     codec_str[256];
        AVStream *st;
        int      ret;

        avio_rb32(pb);            /* reserved */
        avio_rb16(pb);            /* reserved */

        if (loc_ver == LOC_VERSION_DRAFT_02 ||
            loc_ver == LOC_VERSION_DRAFT_03) {
            loc->declared_version = (loc_version_t)loc_ver;
            av_log(s, AV_LOG_VERBOSE, "LOCF declares LOC revision %s\n",
                   loc_version_name(loc->declared_version));
        } else if (loc_ver != LOCF_LOC_VERSION_UNSPECIFIED) {
            av_log(s, AV_LOG_WARNING,
                   "Unknown LOC revision byte 0x%02x; will auto-detect\n",
                   loc_ver);
        }
        loc->fallback_version = LOC_VERSION_DRAFT_02;

        codec_str[0] = '\0';
        if (codec_str_len > 0) {
            ret = avio_read(pb, (unsigned char *)codec_str, codec_str_len);
            if (ret != codec_str_len)
                return ret < 0 ? ret : AVERROR_EOF;
            codec_str[codec_str_len] = '\0';
        }

        if (extra_len > LOCF_MAX_EXTRA) {
            av_log(s, AV_LOG_ERROR,
                   "Implausible extradata length %u\n", extra_len);
            return AVERROR_INVALIDDATA;
        }

        st = avformat_new_stream(s, NULL);
        if (!st)
            return AVERROR(ENOMEM);

        st->codecpar->codec_type = (codec_type == LOCF_CT_AUDIO)
                                       ? AVMEDIA_TYPE_AUDIO
                                       : AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id   = loc_codec_from_string(codec_str,
                                                         codec_str_len);
        if (st->codecpar->codec_id == AV_CODEC_ID_NONE) {
            if (codec_str_len)
                av_log(s, AV_LOG_WARNING,
                       "Unrecognised codec string '%s'; probing\n",
                       codec_str);
            st->codecpar->codec_id = AV_CODEC_ID_PROBE;
            ffstream(st)->request_probe = AVPROBE_SCORE_STREAM_RETRY;
        }

        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            st->codecpar->width  = (int)dim0;
            st->codecpar->height = (int)dim1;
        } else {
            st->codecpar->sample_rate = (int)dim0;
            st->codecpar->ch_layout.nb_channels = (int)dim1;
        }

        if (extra_len > 0) {
            uint8_t *extra = av_mallocz(extra_len
                                        + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!extra)
                return AVERROR(ENOMEM);
            ret = avio_read(pb, extra, extra_len);
            if (ret != (int)extra_len) {
                av_free(extra);
                return ret < 0 ? ret : AVERROR_EOF;
            }
            st->codecpar->extradata      = extra;
            st->codecpar->extradata_size = (int)extra_len;
        }

        avpriv_set_pts_info(st, 64,
                            tb_num ? tb_num : 1,
                            tb_den ? tb_den : 1000000);
        return 0;
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
        uint64_t pay_len = loc->v1_pay_len;
        int      ret;

        if (loc->v1_done)
            return AVERROR_EOF;

        ret = av_new_packet(pkt, (int)pay_len);
        if (ret < 0)
            return ret;

        pkt->pts          = loc->v1_pts;
        pkt->dts          = loc->v1_pts;
        pkt->stream_index = 0;
        if (loc->v1_key)
            pkt->flags |= AV_PKT_FLAG_KEY;

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

    /* v3: read 28-byte frame header */
    {
        uint8_t  hdr[LOCF_V3_FRAME_HDR];
        int64_t  pts, dts;
        uint32_t flags, ext_len, pay_len;
        uint8_t *extbuf = NULL;
        int      ret, n;

        n = avio_read(pb, hdr, LOCF_V3_FRAME_HDR);
        if (n == 0 || n == AVERROR_EOF)
            return AVERROR_EOF;
        if (n < LOCF_V3_FRAME_HDR)
            return n < 0 ? n : AVERROR_EOF;

        pts     = (int64_t)AV_RB64(hdr + 0);
        dts     = (int64_t)AV_RB64(hdr + 8);
        flags   = AV_RB32(hdr + 16);
        ext_len = AV_RB32(hdr + 20);
        pay_len = AV_RB32(hdr + 24);

        if (ext_len > LOCF_MAX_EXT_LEN || pay_len > LOCF_MAX_PAY_LEN) {
            av_log(s, AV_LOG_ERROR,
                   "Implausible frame sizes ext=%u pay=%u\n",
                   ext_len, pay_len);
            return AVERROR_INVALIDDATA;
        }

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

        ret = av_new_packet(pkt, (int)pay_len);
        if (ret < 0) {
            av_free(extbuf);
            return ret;
        }

        if (extbuf) {
            AVStream     *st  = s->streams[0];
            loc_object_t  obj;
            loc_version_t ver = loc_resolve_version(s, loc, extbuf, ext_len);

            loc_object_init_version(&obj, ver);
            if (loc_decode_extensions(extbuf, ext_len, &obj) > 0) {
                /* Non-FFmpeg writers may leave the record pts unset; the
                 * Timestamp property is the fallback. */
                if (obj.has_capture_ts && pts == AV_NOPTS_VALUE)
                    pts = dts = loc_timestamp_to_stream_tb(s, &obj,
                                                           st->time_base);
                if (obj.has_audio_level)
                    av_log(s, AV_LOG_TRACE, "LOC audio level raw=0x%02x\n",
                           obj.audio_level_raw);
            }
            av_free(extbuf);
        }

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
