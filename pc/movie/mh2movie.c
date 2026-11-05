/* mh2movie: the game's Sofdec movies through FFmpeg (see mh2movie.h).
 *
 * The movie is a byte range of a file (an entry of AFS02.AFS, in the
 * extracted files or inside the disc image), read through a custom
 * AVIOContext. libavformat's MPEG-PS demuxer recognises Sofdec streams
 * and their ADX audio; libavcodec decodes MPEG-2 video and ADX; libswscale
 * converts frames to RGBA and libswresample the audio to 48 kHz stereo. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "mh2movie.h"

#define OUT_RATE 48000

/* fopen of a UTF-8 path (the path is the host's, which is UTF-8 on every system): on Windows through the wide call, since
 * the C library's own would read it in the ANSI code page and lose the letters outside it. A path that is not valid UTF-8
 * is read in the ANSI code page. */
static FILE *open_utf8(const char *path)
{
#ifdef _WIN32
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(cp, flags, path, -1, NULL, 0);
    if (n <= 0) {
        cp = CP_ACP;
        flags = 0;
        n = MultiByteToWideChar(cp, flags, path, -1, NULL, 0);
    }
    if (n <= 0)
        return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    FILE *f = NULL;
    if (w && MultiByteToWideChar(cp, flags, path, -1, w, n) > 0)
        f = _wfopen(w, L"rb");
    free(w);
    return f;
#else
    return fopen(path, "rb");
#endif
}

struct mh2movie {
    FILE *f;
    uint64_t base, size, pos;
    AVIOContext *io;
    AVFormatContext *fmt;
    int vstream, astream;
    AVCodecContext *vdec, *adec;
    struct SwsContext *sws;
    SwrContext *swr;
    AVPacket *pkt;
    AVFrame *frame;
    int width, height;
    double last_pts;         /* the previous frame's time, for frames without one */
    int have_pts;
    int eof, vdrained, adrained;
    int16_t *audio;          /* queued output audio, interleaved stereo */
    int naudio, capaudio;    /* in frames */
};

static int io_read(void *opaque, uint8_t *buf, int n)
{
    mh2movie *m = opaque;
    if (m->pos >= m->size)
        return AVERROR_EOF;
    if ((uint64_t)n > m->size - m->pos)
        n = (int)(m->size - m->pos);
#ifdef _WIN32
    if (_fseeki64(m->f, (long long)(m->base + m->pos), SEEK_SET) != 0)
#else
    if (fseeko(m->f, (off_t)(m->base + m->pos), SEEK_SET) != 0)
#endif
        return AVERROR(EIO);
    size_t got = fread(buf, 1, (size_t)n, m->f);
    if (!got)
        return AVERROR_EOF;
    m->pos += got;
    return (int)got;
}

static int64_t io_seek(void *opaque, int64_t off, int whence)
{
    mh2movie *m = opaque;
    int64_t np;
    switch (whence & ~AVSEEK_FORCE) {
    case AVSEEK_SIZE: return (int64_t)m->size;
    case SEEK_SET: np = off; break;
    case SEEK_CUR: np = (int64_t)m->pos + off; break;
    case SEEK_END: np = (int64_t)m->size + off; break;
    default: return -1;
    }
    if (np < 0 || (uint64_t)np > m->size)
        return -1;
    m->pos = (uint64_t)np;
    return np;
}

static AVCodecContext *open_decoder(AVStream *st)
{
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec)
        return NULL;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (!c)
        return NULL;
    if (avcodec_parameters_to_context(c, st->codecpar) < 0 || avcodec_open2(c, codec, NULL) < 0) {
        avcodec_free_context(&c);
        return NULL;
    }
    c->pkt_timebase = st->time_base;
    return c;
}

MH2MOVIE_API int mh2movie_abi(void) { return MH2MOVIE_ABI_VERSION; }

MH2MOVIE_API void mh2movie_close(mh2movie *m)
{
    if (!m)
        return;
    avcodec_free_context(&m->vdec);
    avcodec_free_context(&m->adec);
    if (m->fmt)
        avformat_close_input(&m->fmt);
    if (m->io) {
        av_freep(&m->io->buffer);
        avio_context_free(&m->io);
    }
    sws_freeContext(m->sws);
    swr_free(&m->swr);
    av_packet_free(&m->pkt);
    av_frame_free(&m->frame);
    if (m->f)
        fclose(m->f);
    free(m->audio);
    free(m);
}

MH2MOVIE_API mh2movie *mh2movie_open(const char *path, uint64_t offset, uint64_t size, mh2movie_info *info,
                                     char *err, int errlen)
{
    mh2movie *m = calloc(1, sizeof(*m));
    if (!m) {
        snprintf(err, (size_t)errlen, "out of memory");
        return NULL;
    }
    m->vstream = m->astream = -1;
    m->base = offset;
    m->size = size;
    m->f = open_utf8(path);
    if (!m->f) {
        snprintf(err, (size_t)errlen, "cannot open %s", path);
        goto fail;
    }
    unsigned char *buf = av_malloc(64 * 1024);
    m->io = buf ? avio_alloc_context(buf, 64 * 1024, 0, m, io_read, NULL, io_seek) : NULL;
    m->fmt = avformat_alloc_context();
    if (!m->io || !m->fmt) {
        snprintf(err, (size_t)errlen, "out of memory");
        goto fail;
    }
    m->fmt->pb = m->io;
    if (avformat_open_input(&m->fmt, NULL, av_find_input_format("mpeg"), NULL) < 0) {
        m->fmt = NULL;   /* freed by avformat_open_input */
        snprintf(err, (size_t)errlen, "not an MPEG program stream");
        goto fail;
    }
    avformat_find_stream_info(m->fmt, NULL);
    m->vstream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    m->astream = av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (m->vstream < 0 || !(m->vdec = open_decoder(m->fmt->streams[m->vstream]))) {
        snprintf(err, (size_t)errlen, "no decodable video stream");
        goto fail;
    }
    if (m->astream >= 0 && !(m->adec = open_decoder(m->fmt->streams[m->astream])))
        m->astream = -1;   /* play without sound */
    m->pkt = av_packet_alloc();
    m->frame = av_frame_alloc();
    if (!m->pkt || !m->frame) {
        snprintf(err, (size_t)errlen, "out of memory");
        goto fail;
    }
    m->width = m->vdec->width;
    m->height = m->vdec->height;
    if (info) {
        AVStream *vs = m->fmt->streams[m->vstream];
        info->width = m->width;
        info->height = m->height;
        info->fps = vs->avg_frame_rate.den ? av_q2d(vs->avg_frame_rate) : 29.97;
        info->duration = m->fmt->duration > 0 ? (double)m->fmt->duration / AV_TIME_BASE : 0;
        info->has_audio = m->astream >= 0;
    }
    return m;
fail:
    mh2movie_close(m);
    return NULL;
}

static void queue_audio(mh2movie *m, const AVFrame *fr)
{
    if (!m->swr) {
        AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&m->swr, &out, AV_SAMPLE_FMT_S16, OUT_RATE, &fr->ch_layout, (enum AVSampleFormat)fr->format,
                                fr->sample_rate, 0, NULL) < 0 ||
            swr_init(m->swr) < 0) {
            swr_free(&m->swr);
            m->astream = -1;
            return;
        }
    }
    int room = swr_get_out_samples(m->swr, fr->nb_samples);
    if (m->naudio + room > m->capaudio) {
        int cap = (m->naudio + room) * 2;
        int16_t *p = realloc(m->audio, (size_t)cap * 4);
        if (!p)
            return;
        m->audio = p;
        m->capaudio = cap;
    }
    uint8_t *out = (uint8_t *)(m->audio + m->naudio * 2);
    int n = swr_convert(m->swr, &out, room, (const uint8_t **)fr->extended_data, fr->nb_samples);
    if (n > 0)
        m->naudio += n;
}

/* Try to take a decoded video frame; 1 if one was converted into rgba. */
static int take_video(mh2movie *m, uint8_t *rgba, double *pts)
{
    if (avcodec_receive_frame(m->vdec, m->frame) < 0)
        return 0;
    AVFrame *fr = m->frame;
    if (!m->sws || fr->width != m->width || fr->height != m->height) {
        sws_freeContext(m->sws);
        m->width = fr->width;
        m->height = fr->height;
        m->sws = sws_getContext(fr->width, fr->height, (enum AVPixelFormat)fr->format, m->width, m->height,
                                AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    }
    int ok = 0;
    if (m->sws) {
        uint8_t *dst[4] = {rgba, NULL, NULL, NULL};
        int stride[4] = {m->width * 4, 0, 0, 0};
        sws_scale(m->sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, stride);
        int64_t ts = fr->best_effort_timestamp;
        AVStream *vs = m->fmt->streams[m->vstream];
        if (ts != AV_NOPTS_VALUE) {
            *pts = (double)(ts - (vs->start_time != AV_NOPTS_VALUE ? vs->start_time : 0)) * av_q2d(vs->time_base);
        } else {
            /* The last frame of several movies comes out of the decoder's
             * flush without a time (tools/movie_check.py): one frame after
             * the previous. */
            double fps = vs->avg_frame_rate.num > 0 ? av_q2d(vs->avg_frame_rate) : 30000.0 / 1001.0;
            *pts = m->have_pts ? m->last_pts + 1.0 / fps : 0.0;
        }
        m->last_pts = *pts;
        m->have_pts = 1;
        ok = 1;
    }
    av_frame_unref(fr);
    return ok;
}

static void drain_audio(mh2movie *m)
{
    while (m->adec && avcodec_receive_frame(m->adec, m->frame) >= 0) {
        queue_audio(m, m->frame);
        av_frame_unref(m->frame);
    }
}

MH2MOVIE_API int mh2movie_next_frame(mh2movie *m, uint8_t *rgba, double *pts)
{
    for (;;) {
        if (take_video(m, rgba, pts))
            return 1;
        if (m->eof) {
            if (!m->vdrained) {   /* flush the decoders once */
                avcodec_send_packet(m->vdec, NULL);
                if (m->adec) {
                    avcodec_send_packet(m->adec, NULL);
                    drain_audio(m);
                }
                m->vdrained = 1;
                continue;
            }
            return 0;
        }
        int r = av_read_frame(m->fmt, m->pkt);
        if (r < 0) {
            m->eof = 1;
            continue;
        }
        if (m->pkt->stream_index == m->vstream) {
            avcodec_send_packet(m->vdec, m->pkt);
        } else if (m->pkt->stream_index == m->astream && m->adec) {
            if (avcodec_send_packet(m->adec, m->pkt) >= 0)
                drain_audio(m);
        }
        av_packet_unref(m->pkt);
    }
}

MH2MOVIE_API int mh2movie_audio(mh2movie *m, int16_t *out, int max_frames)
{
    int n = m->naudio < max_frames ? m->naudio : max_frames;
    if (n <= 0)
        return 0;
    memcpy(out, m->audio, (size_t)n * 4);
    memmove(m->audio, m->audio + n * 2, (size_t)(m->naudio - n) * 4);
    m->naudio -= n;
    return n;
}
