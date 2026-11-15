/* Movies: the game's WMV/WMA files decoded with FFmpeg (libavformat/libavcodec), for the DirectShow layer.
 *
 * What it does: movie_add() opens a file of the current movie (the game renders the picture (.wmv) and the
 * language's soundtrack (.wma) into one filter graph); movie_run() starts a clock and one decoding thread per file.
 * The video thread converts each frame to BGRA (libswscale) and publishes it when the clock reaches its time; the
 * audio thread converts to 48 kHz stereo float (libswresample) and queues it on its own SDL3 audio stream.
 * movie_position()/movie_duration() answer IMediaSeeking (100 ns units); movie_frame() gives the newest picture to
 * the presenter. movie_stop() ends everything (the game's Stop, or a skip).
 * What it does not do: seeking, pause, more than one movie at a time.
 */
#ifdef USF_NO_FFMPEG                       /* builds without FFmpeg (Android for now): every movie ends at once */
#include "movie.h"
int movie_add(const char *p) { (void)p; return -1; }
void movie_run(void) {}
void movie_stop(void) {}
int movie_active(void) { return 0; }
int movie_loaded(void) { return 0; }
int movie_finished(void) { return 1; }
int64_t movie_duration(void) { return 1; }
int64_t movie_position(void) { return 1; }
void movie_set_volume(int v) { (void)v; }
int movie_frame(uint8_t *d, int a, int b, int *w, int *h, unsigned *s) { (void)d; (void)a; (void)b; (void)w; (void)h; (void)s; return 0; }
#else
#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "audio_dump.h"
#include "movie.h"

typedef struct {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    int stream, video;
    pthread_t th;
    int started;
} Track;

static Track tracks[2];
static int ntracks;
static atomic_int running, stop_req;
static double t_start;                         /* seconds, CLOCK_MONOTONIC, at movie_run */
static int64_t duration_us;
static float volume = 1.0f;
static SDL_AudioStream *astream;

static pthread_mutex_t fmu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *frame_buf;                     /* the newest published picture, BGRA */
static int frame_w, frame_h;
static unsigned frame_seq;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static double clock_s(void) { return running ? now_s() - t_start : 0.0; }

/* one stream of a file as a track: the video if want_video and there is one, else the audio */
static int open_track(const char *path, int want_video) {
    if (ntracks >= 2) return -1;
    Track *t = &tracks[ntracks];
    memset(t, 0, sizeof *t);
    if (avformat_open_input(&t->fmt, path, NULL, NULL) < 0) return -1;
    if (avformat_find_stream_info(t->fmt, NULL) < 0) { avformat_close_input(&t->fmt); return -1; }
    int vs = want_video ? av_find_best_stream(t->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0) : -1;
    int as = av_find_best_stream(t->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    t->video = vs >= 0;
    t->stream = vs >= 0 ? vs : as;
    if (t->stream < 0) { avformat_close_input(&t->fmt); return -1; }
    const AVCodecParameters *par = t->fmt->streams[t->stream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    t->dec = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!t->dec || avcodec_parameters_to_context(t->dec, par) < 0 || avcodec_open2(t->dec, codec, NULL) < 0) {
        fprintf(stderr, "usf: movie %s: no decoder\n", path);
        avcodec_free_context(&t->dec); avformat_close_input(&t->fmt);
        return -1;
    }
    if (t->fmt->duration > duration_us) duration_us = t->fmt->duration;
    fprintf(stderr, "usf: movie %s: %s %s, %.1f s\n", path, t->video ? "video" : "audio", codec->name, t->fmt->duration / 1e6);
    ntracks++;
    return 0;
}

static int has_audio_track(void) {
    for (int i = 0; i < ntracks; i++) if (!tracks[i].video) return 1;
    return 0;
}

/* The picture file brings its own sound; a separate soundtrack (the game asks for movie_<lang>.wma) replaces it. */
int movie_add(const char *path) {
    struct stat st;
    if (stat(path, &st)) return -1;
    size_t n = strlen(path);
    if (n > 4 && !strcasecmp(path + n - 4, ".wma"))                   /* a soundtrack replaces the .wmv's own sound */
        for (int i = 0; i < ntracks; i++)
            if (!tracks[i].video) {
                avcodec_free_context(&tracks[i].dec); avformat_close_input(&tracks[i].fmt);
                tracks[i] = tracks[--ntracks];
                break;
            }
    if (open_track(path, 1)) return -1;
    if (tracks[ntracks - 1].video && !has_audio_track()) open_track(path, 0);   /* the sound inside the .wmv */
    return 0;
}

static void publish(AVFrame *f, struct SwsContext **sws) {
    *sws = sws_getCachedContext(*sws, f->width, f->height, f->format, f->width, f->height, AV_PIX_FMT_BGRA,
                                SWS_BILINEAR, NULL, NULL, NULL);
    pthread_mutex_lock(&fmu);
    if (frame_w != f->width || frame_h != f->height) {
        free(frame_buf);
        frame_buf = malloc((size_t)f->width * f->height * 4);
        frame_w = f->width; frame_h = f->height;
    }
    uint8_t *dst[1] = {frame_buf};
    int ls[1] = {f->width * 4};
    sws_scale(*sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, ls);
    frame_seq++;
    pthread_mutex_unlock(&fmu);
}

static void audio_out(AVFrame *f, SwrContext **swr) {
    if (!*swr) {
        AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(swr, &out, AV_SAMPLE_FMT_FLT, 48000, &f->ch_layout, f->format, f->sample_rate, 0, NULL);
        swr_init(*swr);
    }
    int n = (int)av_rescale_rnd(swr_get_delay(*swr, f->sample_rate) + f->nb_samples, 48000, f->sample_rate, AV_ROUND_UP);
    float *buf = malloc((size_t)n * 2 * sizeof(float));
    uint8_t *out[1] = {(uint8_t *)buf};
    int got = swr_convert(*swr, out, n, (const uint8_t **)f->extended_data, f->nb_samples);
    for (int i = 0; i < got * 2; i++) buf[i] *= volume;
    if (got > 0) audio_dump(AUDIO_MOVIE, buf, got);
    if (got > 0 && astream) SDL_PutAudioStreamData(astream, buf, got * 2 * (int)sizeof(float));
    free(buf);
}

static void *decode_thread(void *arg) {
    Track *t = arg;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    struct SwsContext *sws = NULL;
    SwrContext *swr = NULL;
    AVRational tb = t->fmt->streams[t->stream]->time_base;
    while (!stop_req && av_read_frame(t->fmt, pkt) >= 0) {
        if (pkt->stream_index != t->stream) { av_packet_unref(pkt); continue; }
        avcodec_send_packet(t->dec, pkt);
        av_packet_unref(pkt);
        while (!stop_req && avcodec_receive_frame(t->dec, f) == 0) {
            if (t->video) {
                double pts = f->best_effort_timestamp == AV_NOPTS_VALUE ? clock_s() : f->best_effort_timestamp * av_q2d(tb);
                while (!stop_req && clock_s() < pts) SDL_Delay(2);       /* show it at its time */
                if (!stop_req) publish(f, &sws);
            } else {
                while (!stop_req && astream && SDL_GetAudioStreamQueued(astream) > 48000 * 8 / 2) SDL_Delay(5);
                if (!stop_req) audio_out(f, &swr);
            }
        }
    }
    sws_freeContext(sws);
    swr_free(&swr);
    av_frame_free(&f);
    av_packet_free(&pkt);
    return NULL;
}

void movie_run(void) {
    if (!ntracks || running) return;
    stop_req = 0;
    for (int i = 0; i < ntracks; i++)
        if (!tracks[i].video && !astream) {
            SDL_InitSubSystem(SDL_INIT_AUDIO);
            SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, 48000};
            astream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
            if (astream) SDL_ResumeAudioStreamDevice(astream);
        }
    t_start = now_s();
    running = 1;
    for (int i = 0; i < ntracks; i++) tracks[i].started = pthread_create(&tracks[i].th, NULL, decode_thread, &tracks[i]) == 0;
}

void movie_stop(void) {
    stop_req = 1;
    for (int i = 0; i < ntracks; i++) {
        if (tracks[i].started) pthread_join(tracks[i].th, NULL);
        avcodec_free_context(&tracks[i].dec);
        avformat_close_input(&tracks[i].fmt);
    }
    ntracks = 0;
    running = 0;
    duration_us = 0;
    if (astream) { SDL_DestroyAudioStream(astream); astream = NULL; }
    pthread_mutex_lock(&fmu);
    free(frame_buf);
    frame_buf = NULL; frame_w = frame_h = 0;
    pthread_mutex_unlock(&fmu);
}

int movie_active(void) { return running; }
int movie_loaded(void) { return ntracks > 0; }
int64_t movie_duration(void) { return duration_us * 10; }
int64_t movie_position(void) {
    int64_t p = (int64_t)(clock_s() * 1e7);
    return p > duration_us * 10 ? duration_us * 10 : p;
}
int movie_finished(void) { return running && clock_s() * 1e6 >= duration_us; }
void movie_set_volume(int hundredths_db) {
    volume = hundredths_db <= -10000 ? 0.0f : SDL_powf(10.0f, hundredths_db / 2000.0f);
}

int movie_frame(uint8_t *dst, int max_w, int max_h, int *w, int *h, unsigned *seq) {
    pthread_mutex_lock(&fmu);
    int ok = frame_buf && frame_seq != *seq && frame_w <= max_w && frame_h <= max_h;
    if (ok) {
        memcpy(dst, frame_buf, (size_t)frame_w * frame_h * 4);
        *w = frame_w; *h = frame_h; *seq = frame_seq;
    }
    pthread_mutex_unlock(&fmu);
    return ok;
}
#endif
