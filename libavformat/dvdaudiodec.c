/*
 * DVD-Audio demuxer, powered by libaudvd
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

/*
 * See doc/demuxers.texi for a high-level overview.
 *
 * libaudvd parses the AUDIO_TS IFOs and hands out the AOB sectors of a
 * title in play order. They are MPEG-PS packs, demuxed by an inner mpegps
 * demuxer, which already tells 0xa0 LPCM from 0xa1 MLP. The AOB
 * timestamps restart at every track, so the inner demuxer is reopened at
 * each track boundary and the packets of each instance are rebased onto
 * the title timeline with that track's start.
 */

#include <inttypes.h>

#include <audvd/audvd.h>

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "avformat.h"
#include "avio_internal.h"
#include "demux.h"
#include "internal.h"

#define DVDAUDIO_BLOCK_SIZE        AUDVD_SECTOR_SIZE
#define DVDAUDIO_TIME_BASE_Q       (AVRational) { 1, AUDVD_PTS_HZ }

#define DVDAUDIO_SUBSTREAM_LPCM    0xa0
#define DVDAUDIO_SUBSTREAM_MLP     0xa1

/*
 * Downmix extradata of the pcm_dvda streams: the title set's 16 downmix
 * coefficient tables as raw gain codes, and whether the current track
 * allows them to be used. Parsed by libavcodec/pcm-dvda.c.
 */
#define DVDAUDIO_DM_MAGIC          MKBETAG('D', 'V', 'D', 'M')
#define DVDAUDIO_DM_VERSION        1
#define DVDAUDIO_DM_FLAG_VALID     0x01
#define DVDAUDIO_DM_FLAG_PROHIBITED 0x02
#define DVDAUDIO_DM_SIZE           (8 + AUDVD_MAX_DOWNMIX_TABLES * AUDVD_DOWNMIX_CHANNELS * 2)

typedef struct DVDAudioDemuxContext {
    const AVClass    *class;

    /* options */
    int               opt_title;
    int               opt_group;
    int               opt_track_start;
    int               opt_track_end;
    int               opt_ignore_amg;

    audvd_disc       *disc;
    audvd_reader     *reader;
    audvd_title       title;
    int               track_first, track_last;   /* 0-based, inclusive */
    int64_t           range_start_pts;

    /* inner MPEG-PS demuxer */
    AVFormatContext  *mpeg_ctx;
    uint8_t          *mpeg_buf;
    FFIOContext       mpeg_pb;
    int               subdemux_fresh;  /* no data read since (re)open */
    int               subdemux_reset;  /* reopen at the next EOF */
    audvd_pos         sub_pos;         /* track of the current instance */
    int               scrambled_track; /* last track reported scrambled, -1 */

    /* LPCM downmix extradata */
    uint8_t           dm_blob[DVDAUDIO_DM_SIZE];
    int               dm_sent_flags;   /* flags in the blob the decoder has */
    int               dm_pending;      /* attach new extradata to the next LPCM packet */
} DVDAudioDemuxContext;

static void dvdaudio_libaudvd_log(void *opaque, audvd_log_level level, const char *msg)
{
    AVFormatContext *s = opaque;
    int lavu_level;

    switch (level) {
    case AUDVD_LOG_ERROR:   lavu_level = AV_LOG_ERROR;   break;
    case AUDVD_LOG_WARNING: lavu_level = AV_LOG_WARNING; break;
    case AUDVD_LOG_INFO:    lavu_level = AV_LOG_VERBOSE; break;
    default:                lavu_level = AV_LOG_DEBUG;   break;
    }
    av_log(s, lavu_level, "libaudvd: %s\n", msg);
}

static int dvdaudio_error(int err)
{
    switch (err) {
    case AUDVD_ERR_NOMEM:       return AVERROR(ENOMEM);
    case AUDVD_ERR_EOF:         return AVERROR_EOF;
    case AUDVD_ERR_RANGE:       return AVERROR(EINVAL);
    case AUDVD_ERR_NOTFOUND:    return AVERROR(ENOENT);
    case AUDVD_ERR_INVALID:     return AVERROR_INVALIDDATA;
    case AUDVD_ERR_UNSUPPORTED: return AVERROR_PATCHWELCOME;
    default:                    return AVERROR(EIO);
    }
}

static int dvdaudio_dm_flags(const audvd_track *tr)
{
    return (tr->downmix_valid      ? DVDAUDIO_DM_FLAG_VALID      : 0) |
           (tr->downmix_prohibited ? DVDAUDIO_DM_FLAG_PROHIBITED : 0);
}

static int dvdaudio_select_title(AVFormatContext *s)
{
    DVDAudioDemuxContext *c = s->priv_data;
    int nb = audvd_num_titles(c->disc);

    for (int i = 0; i < nb; i++) {
        audvd_title t;

        if (audvd_get_title(c->disc, i, &t) < 0)
            continue;
        if (c->opt_title ? t.number != c->opt_title
                         : !t.audio_only || (c->opt_group && t.group != c->opt_group))
            continue;

        if (!t.audio_only) {
            av_log(s, AV_LOG_ERROR, "Title %d is linked to video (AVTT), "
                                    "which is not supported\n", t.number);
            return AVERROR_PATCHWELCOME;
        }
        if (!t.nb_tracks) {
            av_log(s, AV_LOG_ERROR, "Title %d has no usable tracks\n", t.number);
            return AVERROR_INVALIDDATA;
        }
        c->title = t;
        return 0;
    }

    if (c->opt_title)
        av_log(s, AV_LOG_ERROR, "Title %d not found\n", c->opt_title);
    else if (c->opt_group)
        av_log(s, AV_LOG_ERROR, "No audio-only title in group %d\n", c->opt_group);
    else
        av_log(s, AV_LOG_ERROR, "No audio-only title found\n");
    return AVERROR(EINVAL);
}

static uint64_t dvdaudio_speaker_mask(const audvd_channel_layout *l)
{
    static const uint64_t speaker_to_av[] = {
        [AUDVD_SPK_L]   = AV_CH_FRONT_LEFT,
        [AUDVD_SPK_R]   = AV_CH_FRONT_RIGHT,
        [AUDVD_SPK_C]   = AV_CH_FRONT_CENTER,
        [AUDVD_SPK_LFE] = AV_CH_LOW_FREQUENCY,
        [AUDVD_SPK_S]   = AV_CH_BACK_CENTER,
        [AUDVD_SPK_LS]  = AV_CH_BACK_LEFT,
        [AUDVD_SPK_RS]  = AV_CH_BACK_RIGHT,
    };
    uint64_t mask = 0;

    for (int i = 0; i < l->nb_channels; i++)
        mask |= speaker_to_av[l->speakers[i]];
    return mask;
}

static void dvdaudio_build_dm_blob(AVFormatContext *s, int flags)
{
    DVDAudioDemuxContext *c = s->priv_data;
    uint8_t *p = c->dm_blob;

    memset(p, 0, sizeof(c->dm_blob));
    AV_WB32(p, DVDAUDIO_DM_MAGIC);
    p[4] = DVDAUDIO_DM_VERSION;
    p[5] = flags;
    p += 8;
    for (int t = 0; t < AUDVD_MAX_DOWNMIX_TABLES; t++) {
        audvd_downmix dm;

        if (audvd_get_downmix(c->disc, c->title.ats, t, &dm) < 0)
            memset(&dm, 0xff, sizeof(dm));  /* all muted: never selected */
        for (int ch = 0; ch < AUDVD_DOWNMIX_CHANNELS; ch++) {
            *p++ = dm.gain_code[ch][0];
            *p++ = dm.gain_code[ch][1];
        }
    }
}

static int dvdaudio_streams_setup(AVFormatContext *s)
{
    DVDAudioDemuxContext *c = s->priv_data;
    int64_t duration = 0;

    for (int k = c->track_first; k <= c->track_last; k++) {
        audvd_track tr;
        audvd_get_track(c->disc, c->title.index, k, &tr);
        duration += tr.length_pts;
    }

    for (int k = c->track_first; k <= c->track_last; k++) {
        audvd_track tr;
        AVStream *st = NULL;
        int id, ret;

        audvd_get_track(c->disc, c->title.index, k, &tr);
        if (tr.attr.codec == AUDVD_CODEC_UNKNOWN) {
            av_log(s, AV_LOG_WARNING, "Track %d: unknown audio coding mode %#x\n",
                   tr.number, tr.attr.raw[0]);
            continue;
        }
        id = tr.attr.codec == AUDVD_CODEC_MLP ? DVDAUDIO_SUBSTREAM_MLP
                                               : DVDAUDIO_SUBSTREAM_LPCM;
        for (int i = 0; i < s->nb_streams; i++)
            if (s->streams[i]->id == id)
                st = s->streams[i];

        if (st) {
            if (st->codecpar->sample_rate != tr.attr.group_rate[0] ||
                st->codecpar->ch_layout.nb_channels != tr.attr.layout.nb_channels)
                av_log(s, AV_LOG_VERBOSE, "Track %d: stream parameters change to "
                       "%d Hz, %d channels\n", tr.number, tr.attr.group_rate[0],
                       tr.attr.layout.nb_channels);
            continue;
        }

        st = avformat_new_stream(s, NULL);
        if (!st)
            return AVERROR(ENOMEM);

        st->id                  = id;
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->codec_id  = tr.attr.codec == AUDVD_CODEC_MLP ? AV_CODEC_ID_MLP
                                                                    : AV_CODEC_ID_PCM_DVDA;
        st->codecpar->sample_rate         = tr.attr.group_rate[0];
        st->codecpar->bits_per_raw_sample = FFMAX(tr.attr.group_bits[0],
                                                  tr.attr.group_bits[1]);
        av_channel_layout_uninit(&st->codecpar->ch_layout);
        av_channel_layout_from_mask(&st->codecpar->ch_layout,
                                    dvdaudio_speaker_mask(&tr.attr.layout));
        st->duration = duration;
        /* lets probing finish when the substream only starts late in the title */
        st->codecpar->format = st->codecpar->bits_per_raw_sample > 16 ? AV_SAMPLE_FMT_S32
                                                                      : AV_SAMPLE_FMT_S16;

        if (id == DVDAUDIO_SUBSTREAM_LPCM) {
            c->dm_sent_flags = dvdaudio_dm_flags(&tr);
            dvdaudio_build_dm_blob(s, c->dm_sent_flags);
            if ((ret = ff_alloc_extradata(st->codecpar, sizeof(c->dm_blob))) < 0)
                return ret;
            memcpy(st->codecpar->extradata, c->dm_blob, sizeof(c->dm_blob));
        }

        avpriv_set_pts_info(st, 64, DVDAUDIO_TIME_BASE_Q.num, DVDAUDIO_TIME_BASE_Q.den);
    }

    if (!s->nb_streams) {
        av_log(s, AV_LOG_ERROR, "No supported audio streams in title %d\n",
               c->title.number);
        return AVERROR_INVALIDDATA;
    }
    return 0;
}

static int dvdaudio_chapters_setup(AVFormatContext *s)
{
    DVDAudioDemuxContext *c = s->priv_data;

    for (int k = c->track_first; k <= c->track_last; k++) {
        audvd_track tr;
        AVChapter *ch;
        int64_t start;

        audvd_get_track(c->disc, c->title.index, k, &tr);
        start = tr.start_pts - c->range_start_pts;
        ch = avpriv_new_chapter(s, tr.number, DVDAUDIO_TIME_BASE_Q,
                                start, start + tr.length_pts, NULL);
        if (!ch)
            return AVERROR(ENOMEM);

        av_dict_set_int(&ch->metadata, "track", tr.number, 0);
        if (tr.attr.codec == AUDVD_CODEC_LPCM) {
            if (tr.downmix_prohibited)
                av_dict_set(&ch->metadata, "downmix", "prohibited", 0);
            else if (tr.downmix_valid)
                av_dict_set_int(&ch->metadata, "downmix_table", tr.downmix_table, 0);
        }
    }
    return 0;
}

/* Reject discs whose first sector is CPPM-scrambled, before any demuxing. */
static int dvdaudio_check_cppm(AVFormatContext *s)
{
    DVDAudioDemuxContext *c = s->priv_data;
    uint8_t buf[DVDAUDIO_BLOCK_SIZE];
    audvd_track tr;
    int ret;

    audvd_get_track(c->disc, c->title.index, c->track_first, &tr);
    ret = audvd_read_aob_sectors(c->disc, c->title.ats, tr.first_sector, 1, buf);
    if (ret != 1) {
        av_log(s, AV_LOG_WARNING, "Cannot read the AOB data of track %d (%s); "
               "only the disc structure will be available\n", tr.number,
               ret < 0 ? audvd_strerror(ret) : "no data");
        return 0;
    }
    if (audvd_pack_is_scrambled(buf)) {
        av_log(s, AV_LOG_ERROR, "Title %d is CPPM-protected; this demuxer does not "
               "decrypt it\n", c->title.number);
        return AVERROR_PATCHWELCOME;
    }
    return 0;
}

/*
 * The AVIO read callback of the inner demuxer. A track boundary (or any
 * timestamp discontinuity) ends the data of the current instance, so that
 * it is reopened for the next track.
 */
static int dvdaudio_subdemux_read_data(void *opaque, uint8_t *buf, int buf_size)
{
    AVFormatContext *s = opaque;
    DVDAudioDemuxContext *c = s->priv_data;
    int max = buf_size / DVDAUDIO_BLOCK_SIZE;
    int ret;

    for (;;) {
        audvd_pos pos;

        ret = audvd_reader_tell(c->reader, &pos);
        if (ret < 0)
            goto subdemux_eof;

        if (!c->subdemux_fresh && (pos.flags & AUDVD_POS_DISCONTINUITY)) {
            c->subdemux_reset = 1;
            ret = AUDVD_ERR_EOF;
            goto subdemux_eof;
        }

        ret = audvd_reader_read(c->reader, buf, max, &pos);
        if (ret < 0)
            goto subdemux_eof;

        if (c->subdemux_fresh) {
            c->sub_pos        = pos;
            c->subdemux_fresh = 0;
        }

        if (pos.flags & AUDVD_POS_ENCRYPTED) {
            if (c->scrambled_track != pos.track) {
                av_log(s, AV_LOG_ERROR, "Track %d: skipping CPPM-scrambled sectors "
                       "from %"PRIu32"; decryption is not supported\n",
                       pos.track + 1, pos.sector);
                c->scrambled_track = pos.track;
            }
            continue;
        }

        return ret * DVDAUDIO_BLOCK_SIZE;
    }

subdemux_eof:
    ret = dvdaudio_error(ret);
    c->mpeg_pb.pub.eof_reached = 1;
    c->mpeg_pb.pub.error       = ret;
    c->mpeg_pb.pub.read_packet = NULL;
    c->mpeg_pb.pub.buf_end     = c->mpeg_pb.pub.buf_ptr = c->mpeg_pb.pub.buffer;

    return ret;
}

static void dvdaudio_subdemux_close(AVFormatContext *s)
{
    DVDAudioDemuxContext *c = s->priv_data;

    av_freep(&c->mpeg_pb.pub.buffer);
    avformat_close_input(&c->mpeg_ctx);
}

static int dvdaudio_subdemux_open(AVFormatContext *s)
{
    DVDAudioDemuxContext *c = s->priv_data;
    extern const FFInputFormat ff_mpegps_demuxer;
    int ret;

    if (!(c->mpeg_buf = av_mallocz(DVDAUDIO_BLOCK_SIZE)))
        return AVERROR(ENOMEM);

    ffio_init_context(&c->mpeg_pb, c->mpeg_buf, DVDAUDIO_BLOCK_SIZE, 0, s,
                      dvdaudio_subdemux_read_data, NULL, NULL);
    c->mpeg_pb.pub.seekable = 0;

    if (!(c->mpeg_ctx = avformat_alloc_context()))
        return AVERROR(ENOMEM);

    if ((ret = ff_copy_whiteblacklists(c->mpeg_ctx, s)) < 0) {
        avformat_free_context(c->mpeg_ctx);
        c->mpeg_ctx = NULL;

        return ret;
    }

    c->subdemux_fresh  = 1;
    c->subdemux_reset  = 0;

    c->mpeg_ctx->flags = AVFMT_FLAG_CUSTOM_IO;
    c->mpeg_ctx->ctx_flags |= AVFMTCTX_UNSEEKABLE;
    c->mpeg_ctx->probesize = 0;
    c->mpeg_ctx->max_analyze_duration = 0;
    c->mpeg_ctx->interrupt_callback = s->interrupt_callback;
    c->mpeg_ctx->pb = &c->mpeg_pb.pub;
    c->mpeg_ctx->io_open = NULL;

    return avformat_open_input(&c->mpeg_ctx, "", &ff_mpegps_demuxer.p, NULL);
}

static int dvdaudio_subdemux_reset(AVFormatContext *s)
{
    av_log(s, AV_LOG_VERBOSE, "Resetting sub-demuxer\n");

    dvdaudio_subdemux_close(s);
    return dvdaudio_subdemux_open(s);
}

static int dvdaudio_read_header(AVFormatContext *s)
{
    DVDAudioDemuxContext *c = s->priv_data;
    audvd_open_opts opts = { dvdaudio_libaudvd_log, s,
                             c->opt_ignore_amg ? AUDVD_OPEN_IGNORE_AMG : 0 };
    audvd_track tr;
    int ret;

    c->scrambled_track = -1;

    if ((ret = audvd_open(s->url, &opts, &c->disc)) < 0) {
        av_log(s, AV_LOG_ERROR, "Unable to open the DVD-Audio zone at %s: %s\n",
               s->url, audvd_strerror(ret));
        return dvdaudio_error(ret);
    }

    if ((ret = dvdaudio_select_title(s)) < 0)
        return ret;

    c->track_first = c->opt_track_start - 1;
    c->track_last  = c->opt_track_end ? c->opt_track_end - 1 : c->title.nb_tracks - 1;
    if (c->track_first >= c->title.nb_tracks || c->track_last >= c->title.nb_tracks ||
        c->track_last < c->track_first) {
        av_log(s, AV_LOG_ERROR, "Track range %d-%d is not within the %d tracks of "
               "title %d\n", c->opt_track_start, c->track_last + 1,
               c->title.nb_tracks, c->title.number);
        return AVERROR(EINVAL);
    }
    audvd_get_track(c->disc, c->title.index, c->track_first, &tr);
    c->range_start_pts = tr.start_pts;

    if ((ret = audvd_reader_open(c->disc, c->title.index, &c->reader)) < 0 ||
        (ret = audvd_reader_set_tracks(c->reader, c->track_first, c->track_last)) < 0)
        return dvdaudio_error(ret);

    if ((ret = dvdaudio_check_cppm(s)) < 0)
        return ret;

    av_dict_set_int(&s->metadata, "dvda_title", c->title.number, 0);
    av_dict_set_int(&s->metadata, "dvda_group", c->title.group, 0);
    av_dict_set_int(&s->metadata, "dvda_ats", c->title.ats, 0);

    if ((ret = dvdaudio_streams_setup(s)) < 0 ||
        (ret = dvdaudio_chapters_setup(s)) < 0)
        return ret;

    return dvdaudio_subdemux_open(s);
}

/*
 * After a (re)open of the inner demuxer: queue new downmix extradata if
 * the track now being read has different downmix flags.
 */
static void dvdaudio_update_dm_flags(AVFormatContext *s)
{
    DVDAudioDemuxContext *c = s->priv_data;
    audvd_track tr;

    if (c->subdemux_fresh ||
        audvd_get_track(c->disc, c->title.index, c->sub_pos.track, &tr) < 0 ||
        tr.attr.codec != AUDVD_CODEC_LPCM || dvdaudio_dm_flags(&tr) == c->dm_sent_flags)
        return;
    c->dm_sent_flags = dvdaudio_dm_flags(&tr);
    c->dm_pending    = 1;
}

static int dvdaudio_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    DVDAudioDemuxContext *c = s->priv_data;
    AVStream *st_subdemux, *st = NULL;
    int ret;

    ret = av_read_frame(c->mpeg_ctx, pkt);
    if (ret < 0) {
        if (c->subdemux_reset && ret == AVERROR_EOF) {
            if ((ret = dvdaudio_subdemux_reset(s)) < 0)
                return ret;
            dvdaudio_update_dm_flags(s); /* the downmix flags are per track */
            return FFERROR_REDO;
        }

        return ret;
    }

    st_subdemux = c->mpeg_ctx->streams[pkt->stream_index];
    for (int i = 0; i < s->nb_streams; i++) {
        if (s->streams[i]->id == st_subdemux->id) {
            st = s->streams[i];
            break;
        }
    }
    if (!st) {
        av_log(s, AV_LOG_DEBUG, "Discarding packet of unexpected substream %#x\n",
               st_subdemux->id);
        av_packet_unref(pkt);
        return FFERROR_REDO;
    }
    pkt->stream_index = st->index;

    if (pkt->pts != AV_NOPTS_VALUE)
        pkt->pts = audvd_pos_title_pts(&c->sub_pos, pkt->pts) - c->range_start_pts;
    if (pkt->dts != AV_NOPTS_VALUE)
        pkt->dts = audvd_pos_title_pts(&c->sub_pos, pkt->dts) - c->range_start_pts;
    pkt->pos = -1;

    if (c->dm_pending && st->id == DVDAUDIO_SUBSTREAM_LPCM) {
        uint8_t *sd;

        c->dm_blob[5] = c->dm_sent_flags;
        sd = av_packet_new_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, sizeof(c->dm_blob));
        if (!sd) {
            av_packet_unref(pkt);
            return AVERROR(ENOMEM);
        }
        memcpy(sd, c->dm_blob, sizeof(c->dm_blob));
        c->dm_pending = 0;
    }

    av_log(s, AV_LOG_TRACE, "st=%d track=%d pts=%" PRId64 " dts=%" PRId64 "\n",
           pkt->stream_index, c->sub_pos.track + 1, pkt->pts, pkt->dts);

    return 0;
}

static int dvdaudio_close(AVFormatContext *s)
{
    DVDAudioDemuxContext *c = s->priv_data;

    dvdaudio_subdemux_close(s);
    audvd_reader_close(c->reader);
    c->reader = NULL;
    audvd_close(c->disc);
    c->disc = NULL;

    return 0;
}

static int dvdaudio_read_seek(AVFormatContext *s, int stream_index,
                              int64_t timestamp, int flags)
{
    DVDAudioDemuxContext *c = s->priv_data;
    uint64_t landed;
    int ret;

    if (flags & AVSEEK_FLAG_BYTE)
        return AVERROR(ENOSYS);

    if (stream_index >= 0)
        timestamp = av_rescale_q(timestamp, s->streams[stream_index]->time_base,
                                 DVDAUDIO_TIME_BASE_Q);
    else
        timestamp = av_rescale_q(timestamp, AV_TIME_BASE_Q, DVDAUDIO_TIME_BASE_Q);
    if (timestamp < 0)
        timestamp = 0;

    ret = audvd_reader_seek_pts(c->reader, c->range_start_pts + timestamp, &landed);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Seeking to %" PRId64 " failed: %s\n",
               timestamp, audvd_strerror(ret));
        return dvdaudio_error(ret);
    }

    if ((ret = dvdaudio_subdemux_reset(s)) < 0)
        return ret;
    dvdaudio_update_dm_flags(s);

    av_log(s, AV_LOG_DEBUG, "seeking: requested=%" PRId64 " landed=%" PRId64 "\n",
           timestamp, (int64_t)landed - c->range_start_pts);

    return 0;
}

#define OFFSET(x) offsetof(DVDAudioDemuxContext, x)
static const AVOption dvdaudio_options[] = {
    {"group",       "group of the title to pick automatically (0=any)", OFFSET(opt_group),       AV_OPT_TYPE_INT,  { .i64=0 }, 0, 9,  AV_OPT_FLAG_DECODING_PARAM },
    {"ignore_amg",  "ignore AUDIO_TS.IFO, use the title set IFOs",      OFFSET(opt_ignore_amg),  AV_OPT_TYPE_BOOL, { .i64=0 }, 0, 1,  AV_OPT_FLAG_DECODING_PARAM },
    {"title",       "disc title number (0=first audio-only title)",     OFFSET(opt_title),       AV_OPT_TYPE_INT,  { .i64=0 }, 0, 99, AV_OPT_FLAG_DECODING_PARAM },
    {"track_end",   "last track number (0=end of the title)",          OFFSET(opt_track_end),   AV_OPT_TYPE_INT,  { .i64=0 }, 0, 99, AV_OPT_FLAG_DECODING_PARAM },
    {"track_start", "first track number",                               OFFSET(opt_track_start), AV_OPT_TYPE_INT,  { .i64=1 }, 1, 99, AV_OPT_FLAG_DECODING_PARAM },
    {NULL}
};

static const AVClass dvdaudio_class = {
    .class_name = "DVD-Audio demuxer",
    .item_name  = av_default_item_name,
    .option     = dvdaudio_options,
    .version    = LIBAVUTIL_VERSION_INT
};

const FFInputFormat ff_dvdaudio_demuxer = {
    .p.name         = "dvdaudio",
    .p.long_name    = NULL_IF_CONFIG_SMALL("DVD-Audio"),
    .p.priv_class   = &dvdaudio_class,
    .p.flags        = AVFMT_SHOW_IDS | AVFMT_TS_DISCONT   | AVFMT_SEEK_TO_PTS |
                      AVFMT_NOFILE   | AVFMT_NO_BYTE_SEEK | AVFMT_NOGENSEARCH | AVFMT_NOBINSEARCH,
    .priv_data_size = sizeof(DVDAudioDemuxContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_close     = dvdaudio_close,
    .read_header    = dvdaudio_read_header,
    .read_packet    = dvdaudio_read_packet,
    .read_seek      = dvdaudio_read_seek
};
