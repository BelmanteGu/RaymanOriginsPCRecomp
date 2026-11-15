/* binkw32.dll: the game's Bink movies (intro, ending, attract demo), decoded with FFmpeg behind the same calls.
 *
 * What it does: the game drives Bink the way its player (0x7FF750..0x7FFAB8) expects:
 *   BinkOpen -> BinkGetFrameBuffersInfo (sizes of the Y, cR, cB planes) -> the game creates L8 textures, locks them
 *   and hands their memory over with BinkRegisterFrameBuffers -> every frame: BinkWait (0 = time for the next
 *   frame), BinkDoFrame (decode into the registered planes of set FrameNum), BinkShouldSkip / BinkNextFrame,
 *   BinkGetRects (> 0: a new picture, the game copies it to the textures its PS_MovieWIN32 shader reads) and
 *   BinkGetSummary (finished when TotalPlayedFrames reaches TotalFrames).
 * A decoding thread reads the file with libavformat: video frames (YUV 4:2:0) wait in a small queue, the soundtrack
 * is converted to 48 kHz stereo float and played on its own SDL audio stream. Time is the wall clock from the first
 * frame; a late game skips frames (BinkShouldSkip) instead of slowing the movie down.
 * What it does not do: Bink alpha planes (the game's movies have none), seeking, or more than one movie at a time.
 * USF_NO_FFMPEG builds (none at present) answer "no movie".
 */
#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "audio_dump.h"
#include "hle.h"

#define QN 6

typedef struct {
    uint32_t guest;                                  /* HBINK: Width, Height, Frames, FrameNum, ... (0x200 bytes) */
    AVFormatContext *fmt;
    AVCodecContext *vdec, *adec;
    int vs, as;
    SwrContext *swr;
    SDL_AudioStream *audio;
    uint32_t w, h, frames, rate_num, rate_den;
    uint8_t *q[QN];                                  /* decoded pictures: Y, then U, then V, tightly packed */
    int qhead, qn, eof;
    pthread_t th;
    pthread_mutex_t m;
    pthread_cond_t cv;
    int quit;
    double start;                                    /* wall time of frame 0 */
    uint32_t played, changed;
    uint32_t fbset;                                  /* guest BINKFRAMEBUFFERS registered by the game */
    float volume;
} Movie;

static Movie *cur;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void put_audio(Movie *mv, AVFrame *fr) {
    if (!mv->swr) {
        AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(&mv->swr, &out, AV_SAMPLE_FMT_FLT, 48000, &mv->adec->ch_layout, mv->adec->sample_fmt,
                            mv->adec->sample_rate, 0, NULL);
        if (!mv->swr || swr_init(mv->swr) < 0) return;
    }
    int n = (int)av_rescale_rnd(fr->nb_samples, 48000, mv->adec->sample_rate, AV_ROUND_UP) + 64;
    float *buf = malloc((size_t)n * 8);
    uint8_t *outp[1] = {(uint8_t *)buf};
    int got = swr_convert(mv->swr, outp, n, (const uint8_t **)fr->extended_data, fr->nb_samples);
    if (got > 0) {
        for (int i = 0; i < 2 * got; i++) buf[i] *= mv->volume;
        audio_dump(AUDIO_MOVIE, buf, got);
        if (mv->audio) SDL_PutAudioStreamData(mv->audio, buf, got * 8);
    }
    free(buf);
}

static void put_video(Movie *mv, AVFrame *fr) {
    pthread_mutex_lock(&mv->m);
    while (mv->qn == QN && !mv->quit) pthread_cond_wait(&mv->cv, &mv->m);
    if (mv->quit) { pthread_mutex_unlock(&mv->m); return; }
    int slot = (mv->qhead + mv->qn) % QN;
    pthread_mutex_unlock(&mv->m);
    uint8_t *d = mv->q[slot];
    uint32_t cw = (mv->w + 1) / 2, ch = (mv->h + 1) / 2;
    for (uint32_t y = 0; y < mv->h; y++) memcpy(d + y * mv->w, fr->data[0] + y * fr->linesize[0], mv->w);
    d += mv->w * mv->h;
    for (uint32_t y = 0; y < ch; y++) memcpy(d + y * cw, fr->data[1] + y * fr->linesize[1], cw);
    d += cw * ch;
    for (uint32_t y = 0; y < ch; y++) memcpy(d + y * cw, fr->data[2] + y * fr->linesize[2], cw);
    pthread_mutex_lock(&mv->m);
    mv->qn++;
    pthread_cond_broadcast(&mv->cv);
    pthread_mutex_unlock(&mv->m);
}

static void *decode_main(void *p) {
    Movie *mv = p;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    while (!mv->quit && av_read_frame(mv->fmt, pkt) >= 0) {
        AVCodecContext *dc = pkt->stream_index == mv->vs ? mv->vdec : pkt->stream_index == mv->as ? mv->adec : NULL;
        if (dc && avcodec_send_packet(dc, pkt) >= 0)
            while (!mv->quit && avcodec_receive_frame(dc, fr) >= 0) {
                if (dc == mv->vdec) put_video(mv, fr); else put_audio(mv, fr);
            }
        av_packet_unref(pkt);
    }
    pthread_mutex_lock(&mv->m);
    mv->eof = 1;
    pthread_cond_broadcast(&mv->cv);
    pthread_mutex_unlock(&mv->m);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    return NULL;
}

static AVCodecContext *open_codec(AVFormatContext *f, int s) {
    if (s < 0) return NULL;
    const AVCodec *codec = avcodec_find_decoder(f->streams[s]->codecpar->codec_id);
    AVCodecContext *c = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!c || avcodec_parameters_to_context(c, f->streams[s]->codecpar) < 0 || avcodec_open2(c, codec, NULL) < 0) {
        avcodec_free_context(&c);
        return NULL;
    }
    return c;
}

static void close_movie(Movie *mv) {
    pthread_mutex_lock(&mv->m);
    mv->quit = 1;
    pthread_cond_broadcast(&mv->cv);
    pthread_mutex_unlock(&mv->m);
    pthread_join(mv->th, NULL);
    if (mv->audio) SDL_DestroyAudioStream(mv->audio);
    swr_free(&mv->swr);
    avcodec_free_context(&mv->vdec);
    avcodec_free_context(&mv->adec);
    avformat_close_input(&mv->fmt);
    for (int i = 0; i < QN; i++) free(mv->q[i]);
    if (mv == cur) cur = NULL;
    free(mv);
}

static Movie *mv_of(uint32_t h) { return cur && cur->guest == h ? cur : NULL; }

/* BinkOpen(name, flags) */
static uint32_t bink_open(CPU *c, uint32_t a) {
    (void)c;
    char host[1200];
    win_path_host(STR(ARG(0)), 0, host, sizeof host);
    if (cur) close_movie(cur);
    Movie *mv = calloc(1, sizeof *mv);
    mv->volume = 1.0f;
    pthread_mutex_init(&mv->m, NULL);
    pthread_cond_init(&mv->cv, NULL);
    if (avformat_open_input(&mv->fmt, host, NULL, NULL) < 0 || avformat_find_stream_info(mv->fmt, NULL) < 0) {
        fprintf(stderr, "usf: BinkOpen(%s): cannot open %s\n", STR(ARG(0)), host);
        free(mv);
        return 0;
    }
    mv->vs = av_find_best_stream(mv->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    mv->as = av_find_best_stream(mv->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    mv->vdec = open_codec(mv->fmt, mv->vs);
    mv->adec = open_codec(mv->fmt, mv->as);
    if (!mv->vdec) { avformat_close_input(&mv->fmt); free(mv); return 0; }
    AVStream *st = mv->fmt->streams[mv->vs];
    mv->w = (uint32_t)mv->vdec->width;
    mv->h = (uint32_t)mv->vdec->height;
    mv->rate_num = (uint32_t)(st->avg_frame_rate.num ? st->avg_frame_rate.num : 30);
    mv->rate_den = (uint32_t)(st->avg_frame_rate.den ? st->avg_frame_rate.den : 1);
    mv->frames = st->nb_frames > 0 ? (uint32_t)st->nb_frames
                                   : (uint32_t)(mv->fmt->duration / 1e6 * mv->rate_num / mv->rate_den);
    size_t sz = (size_t)mv->w * mv->h + 2u * ((mv->w + 1) / 2) * ((mv->h + 1) / 2);
    for (int i = 0; i < QN; i++) mv->q[i] = malloc(sz);
    if (mv->adec) {
        SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, 48000};
        mv->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (mv->audio) SDL_ResumeAudioStreamDevice(mv->audio);
    }
    mv->guest = gcalloc(0x200);
    W32(mv->guest, mv->w); W32(mv->guest + 4, mv->h); W32(mv->guest + 8, mv->frames); W32(mv->guest + 12, 1);
    W32(mv->guest + 20, mv->rate_num); W32(mv->guest + 24, mv->rate_den);
    cur = mv;
    pthread_create(&mv->th, NULL, decode_main, mv);
    fprintf(stderr, "usf: BinkOpen(%s): %ux%u, %u frames at %.2f fps%s\n", STR(ARG(0)), mv->w, mv->h, mv->frames,
            (double)mv->rate_num / mv->rate_den, mv->adec ? ", with sound" : "");
    return mv->guest;
}

static uint32_t bink_close(CPU *c, uint32_t a) { (void)c; Movie *mv = mv_of(ARG(0)); if (mv) close_movie(mv); return 0; }

/* BinkGetFrameBuffersInfo(h, set): two sets of Y, cR, cB planes for the game to allocate */
static uint32_t bink_fb_info(CPU *c, uint32_t a) {
    (void)c;
    Movie *mv = mv_of(ARG(0));
    uint32_t s = ARG(1);
    memset(HOST(s), 0, 0x18 + 2 * 0x30);
    if (!mv) return 0;
    W32(s, 2); W32(s + 4, mv->w); W32(s + 8, mv->h); W32(s + 12, (mv->w + 1) / 2); W32(s + 16, (mv->h + 1) / 2);
    for (int f = 0; f < 2; f++) { uint32_t p = s + 0x18 + 0x30 * (uint32_t)f; W32(p, 1); W32(p + 12, 1); W32(p + 24, 1); }
    return 0;
}
static uint32_t bink_register(CPU *c, uint32_t a) { (void)c; Movie *mv = mv_of(ARG(0)); if (mv) mv->fbset = ARG(1); return 0; }

static uint32_t due(const Movie *mv) {                     /* the frame index the clock has reached */
    if (mv->start == 0.0) return 0;
    return (uint32_t)((now_s() - mv->start) * mv->rate_num / mv->rate_den);
}
/* BinkWait: 0 when a new frame is due and decoded */
static uint32_t bink_wait(CPU *c, uint32_t a) {
    (void)c;
    Movie *mv = mv_of(ARG(0));
    if (!mv) return 0;
    pthread_mutex_lock(&mv->m);
    int ready = mv->qn > 0 || mv->eof;
    pthread_mutex_unlock(&mv->m);
    if (!ready) return 1;
    return mv->played <= due(mv) ? 0 : 1;
}
static void copy_plane(uint32_t plane, const uint8_t *src, uint32_t w, uint32_t h) {
    uint32_t buf = R32(plane + 4), pitch = R32(plane + 8);
    if (!buf || !pitch) return;
    for (uint32_t y = 0; y < h; y++) memcpy(HOST(buf + y * pitch), src + (size_t)y * w, w < pitch ? w : pitch);
}
/* BinkDoFrame: the next picture into the registered planes of set FrameNum */
static uint32_t bink_do_frame(CPU *c, uint32_t a) {
    (void)c;
    Movie *mv = mv_of(ARG(0));
    if (!mv) return 0;
    pthread_mutex_lock(&mv->m);
    while (!mv->qn && !mv->eof) pthread_cond_wait(&mv->cv, &mv->m);
    if (!mv->qn) { pthread_mutex_unlock(&mv->m); mv->played = mv->frames; return 0; }   /* the file ended early */
    uint8_t *pic = mv->q[mv->qhead];
    pthread_mutex_unlock(&mv->m);
    if (mv->start == 0.0) mv->start = now_s();
    if (mv->fbset) {
        uint32_t set = mv->fbset + 0x18 + 0x30 * (R32(mv->fbset + 0x14) & 1u);
        uint32_t cw = (mv->w + 1) / 2, ch = (mv->h + 1) / 2;
        copy_plane(set, pic, mv->w, mv->h);
        copy_plane(set + 12, pic + (size_t)mv->w * mv->h + (size_t)cw * ch, cw, ch);   /* cR = V */
        copy_plane(set + 24, pic + (size_t)mv->w * mv->h, cw, ch);                      /* cB = U */
    }
    pthread_mutex_lock(&mv->m);
    mv->qhead = (mv->qhead + 1) % QN;
    mv->qn--;
    pthread_cond_broadcast(&mv->cv);
    pthread_mutex_unlock(&mv->m);
    mv->played++;
    mv->changed = 1;
    return 0;
}
static uint32_t bink_next(CPU *c, uint32_t a) {
    (void)c;
    Movie *mv = mv_of(ARG(0));
    if (mv) W32(mv->guest + 12, mv->played + 1);
    return 0;
}
static uint32_t bink_should_skip(CPU *c, uint32_t a) {          /* more than two frames behind the clock */
    (void)c;
    Movie *mv = mv_of(ARG(0));
    return mv && mv->played + 2 < due(mv) && mv->played < mv->frames ? 1 : 0;
}
static uint32_t bink_get_rects(CPU *c, uint32_t a) {
    (void)c;
    Movie *mv = mv_of(ARG(0));
    if (!mv || !mv->changed) return 0;
    mv->changed = 0;
    return 1;
}
/* BINKSUMMARY: Width, Height, TotalTime, FileFrameRate, FileFrameRateDiv, FrameRate, FrameRateDiv, TotalOpenTime,
 * TotalFrames, TotalPlayedFrames, ... */
static uint32_t bink_summary(CPU *c, uint32_t a) {
    (void)c;
    Movie *mv = mv_of(ARG(0));
    uint32_t s = ARG(1);
    memset(HOST(s), 0, 0x70);
    if (!mv) return 0;
    W32(s, mv->w); W32(s + 4, mv->h); W32(s + 12, mv->rate_num); W32(s + 16, mv->rate_den);
    W32(s + 20, mv->rate_num); W32(s + 24, mv->rate_den); W32(s + 32, mv->frames);
    W32(s + 36, mv->played >= mv->frames || (mv->eof && !mv->qn) ? mv->frames : mv->played);
    return 0;
}
static uint32_t bink_realtime(CPU *c, uint32_t a) {             /* (h, BINKREALTIME *, frames) */
    (void)c;
    Movie *mv = mv_of(ARG(0));
    uint32_t r = ARG(1);
    memset(HOST(r), 0, 0x30);
    if (!mv) return 0;
    W32(r, mv->played); W32(r + 4, mv->rate_num); W32(r + 8, mv->rate_den); W32(r + 12, mv->frames);
    return 0;
}
static uint32_t bink_volume(CPU *c, uint32_t a) {               /* (h, track, volume 0..32768) */
    (void)c;
    Movie *mv = mv_of(ARG(0));
    if (mv) mv->volume = (float)ARG(2) / 32768.0f;
    return 0;
}
static uint32_t ok1(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
static uint32_t zero(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }

#define B(name, n, fn) {"binkw32.dll", name, n, CONV_STDCALL, fn},
const HleImport hle_bink_table[] = {
    B("_BinkOpen@8", 2, bink_open)
    B("_BinkSetSoundSystem@8", 2, ok1)
    B("_BinkOpenDirectSound@4", 1, zero)
    B("_BinkClose@4", 1, bink_close)
    B("_BinkNextFrame@4", 1, bink_next)
    B("_BinkDoFrame@4", 1, bink_do_frame)
    B("_BinkWait@4", 1, bink_wait)
    B("_BinkShouldSkip@4", 1, bink_should_skip)
    B("_BinkGetRects@8", 2, bink_get_rects)
    B("_BinkGetRealtime@12", 3, bink_realtime)
    B("_BinkGetSummary@8", 2, bink_summary)
    B("_BinkRegisterFrameBuffers@8", 2, bink_register)
    B("_BinkGetFrameBuffersInfo@8", 2, bink_fb_info)
    B("_BinkSetWillLoop@8", 2, zero)
    B("_BinkSetVolume@12", 3, bink_volume)
    {NULL, NULL, 0, 0, NULL},
};
