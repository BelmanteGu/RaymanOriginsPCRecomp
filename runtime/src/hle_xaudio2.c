/* XAudio2 2.7 for the game's sound (the CRI middleware creates CLSID_XAudio2 through CoCreateInstance), mixed by
 * our own engine thread into an SDL3 audio stream.
 *
 * What it does: IXAudio2 creates a stereo mastering voice, submix voices (kept only for their volume) and source
 * voices. A source voice queues XAUDIO2_BUFFERs whose PCM stays in guest memory (8/16-bit or float, any rate and
 * channel count); GetState reports the queue and the samples played, which is how CRI paces its streams. As the real
 * XAudio2, an engine thread runs a processing pass every 10 ms: OnVoiceProcessingPassStart, the mix (frequency
 * ratio, volume, channel volumes, output matrix or a default down-mix to stereo), then OnBufferStart / OnBufferEnd /
 * OnLoopEnd / OnStreamEnd and OnVoiceProcessingPassEnd, and the engine callbacks around the pass. Callbacks are game
 * code: the engine thread has its own guest CPU and stack, and calls them with the voice lock released.
 * What it does not do: effects (reverb...), filters, xWMA/ADPCM buffers, more than stereo output.
 */
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "audio_dump.h"
#include "hle.h"

#define MAXV 256
#define MAXQ 64
#define RATE 48000
#define PASS 480                                     /* frames per processing pass: 10 ms */

typedef struct { uint32_t flags, bytes, data, play_begin, play_len, loop_begin, loop_len, loop_count, ctx; } XBuf;
typedef struct {
    int used, kind;                                  /* 0 source, 1 submix, 2 master */
    uint32_t guest, callback, flags;
    uint32_t ch, rate, bits, is_float, block;
    uint32_t adpcm, spb;                             /* MS ADPCM (tag 2): samples per block */
    uint32_t dec_data, dec_block;                    /* the block decoded in dec[] (guest data, index + 1) */
    int16_t dec[8 * 2048];
    XBuf q[MAXQ];
    int qhead, qn, started_cur;
    double pos;                                      /* frame position inside the current buffer */
    uint32_t loops_done;
    uint64_t played;
    int running;
    float volume, ratio, chvol[8], matrix[8][2];
    int has_matrix;
    int send;                                        /* index of the submix it sends to, or -1 */
} Voice;

static Voice voices[MAXV];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t engine_obj, vt_source, vt_submix, vt_master, engine_cb;
static SDL_AudioStream *stream;
static pthread_t engine_th;
static int engine_up;

static float fl(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static Voice *vof(uint32_t g) {
    uint32_t i = R32(g + 8);
    return i < MAXV && voices[i].used && voices[i].guest == g ? &voices[i] : NULL;
}

/* ---- events gathered under the lock and delivered without it ---- */
typedef struct { uint32_t cb, slot, arg, nargs; } Ev;
static Ev evs[4096];
static int nev;
static void ev(uint32_t cb, uint32_t slot, uint32_t arg, uint32_t nargs) {
    if (cb && nev < 4096) evs[nev++] = (Ev){cb, slot, arg, nargs};
}
static void deliver(CPU *c) {
    static Ev local[4096];
    pthread_mutex_lock(&mu);
    int n = nev;
    memcpy(local, evs, sizeof(Ev) * (size_t)n);
    nev = 0;
    pthread_mutex_unlock(&mu);
    for (int i = 0; i < n; i++) {
        uint32_t fn = R32(R32(local[i].cb) + 4u * local[i].slot);
        if (local[i].nargs) guest_call(c, fn, CONV_STDCALL, 2, local[i].cb, local[i].arg);
        else guest_call(c, fn, CONV_STDCALL, 1, local[i].cb);
    }
}

/* MS ADPCM: one block decoded at a time (header per channel: predictor, delta, sample1, sample2; then 4-bit codes,
 * high nibble first, channels interleaved) */
static void adpcm_block(Voice *v, const uint8_t *b) {
    static const int coef[7][2] = {{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}};
    static const int adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
    uint32_t nch = v->ch, spb = v->spb > 2048 ? 2048 : v->spb;
    int c1[8], c2[8], delta[8], s1[8], s2[8];
    for (uint32_t c = 0; c < nch; c++) {
        int pi = b[c] < 7 ? b[c] : 0;
        c1[c] = coef[pi][0]; c2[c] = coef[pi][1];
        delta[c] = (int16_t)(b[nch + 2 * c] | b[nch + 2 * c + 1] << 8);
        s1[c] = (int16_t)(b[3 * nch + 2 * c] | b[3 * nch + 2 * c + 1] << 8);
        s2[c] = (int16_t)(b[5 * nch + 2 * c] | b[5 * nch + 2 * c + 1] << 8);
        v->dec[c] = (int16_t)s2[c];
        v->dec[nch + c] = (int16_t)s1[c];
    }
    const uint8_t *p = b + 7 * nch;
    uint32_t n = 0, total = (spb - 2) * nch;
    for (uint32_t k = 0; k < total; k++) {
        int nib = (k & 1) ? (p[n++] & 15) : (p[n] >> 4), c = (int)(k % nch);
        int sn = nib >= 8 ? nib - 16 : nib;
        int pred = (s1[c] * c1[c] + s2[c] * c2[c]) / 256 + sn * delta[c];
        if (pred > 32767) pred = 32767;
        if (pred < -32768) pred = -32768;
        s2[c] = s1[c]; s1[c] = pred;
        delta[c] = adapt[nib] * delta[c] / 256;
        if (delta[c] < 16) delta[c] = 16;
        v->dec[(2 + k / nch) * nch + (uint32_t)c] = (int16_t)pred;
    }
}
static uint32_t frames_of(const Voice *v, uint32_t bytes) { return v->adpcm ? bytes / v->block * v->spb : bytes / v->block; }

static float sample(Voice *v, const uint8_t *m, uint32_t frame, uint32_t ch) {
    if (v->adpcm) {
        uint32_t blk = frame / v->spb, data = (uint32_t)(m - usf_mem);
        if (v->dec_data != data || v->dec_block != blk + 1) { adpcm_block(v, m + (size_t)blk * v->block); v->dec_data = data; v->dec_block = blk + 1; }
        return v->dec[(frame % v->spb) * v->ch + ch] / 32768.0f;
    }
    const uint8_t *x = m + (size_t)frame * v->block + (size_t)ch * (v->bits / 8);
    if (v->is_float) { float f; memcpy(&f, x, 4); return f; }
    if (v->bits == 16) { int16_t s; memcpy(&s, x, 2); return s / 32768.0f; }
    if (v->bits == 8) return (x[0] - 128) / 128.0f;
    if (v->bits == 32) { int32_t s; memcpy(&s, x, 4); return s / 2147483648.0f; }
    return 0.0f;
}

/* the gain of input channel ch to the left/right output */
static void gains(const Voice *v, uint32_t ch, float *l, float *r) {
    if (v->has_matrix) { *l = v->matrix[ch][0]; *r = v->matrix[ch][1]; }
    else if (v->ch == 1) { *l = *r = 1.0f; }
    else if (v->ch == 2) { *l = ch == 0; *r = ch == 1; }
    else {                                           /* 5.1 / 7.1: FL FR C LFE BL BR (SL SR) */
        static const float L[8] = {1, 0, 0.707f, 0, 0.707f, 0, 0.707f, 0}, R[8] = {0, 1, 0.707f, 0, 0, 0.707f, 0, 0.707f};
        *l = ch < 8 ? L[ch] : 0; *r = ch < 8 ? R[ch] : 0;
    }
    float g = v->volume * (ch < 8 ? v->chvol[ch] : 1.0f);
    if (v->send >= 0 && voices[v->send].used) g *= voices[v->send].volume;
    *l *= g; *r *= g;
}

static void mix_voice(Voice *v, float *out, int frames) {
    double step = (double)v->rate * v->ratio / RATE;
    float gl[8], gr[8];
    for (uint32_t c = 0; c < v->ch && c < 8; c++) gains(v, c, &gl[c], &gr[c]);
    for (int f = 0; f < frames && v->qn; f++) {
        XBuf *b = &v->q[v->qhead];
        if (!v->started_cur) { v->started_cur = 1; ev(v->callback, 3, b->ctx, 1); }        /* OnBufferStart */
        uint32_t total = frames_of(v, b->bytes);
        uint32_t begin = b->play_begin, end = b->play_len ? b->play_begin + b->play_len : total;
        if (end > total) end = total;
        uint32_t lend = b->loop_len ? b->loop_begin + b->loop_len : end;
        uint32_t p = begin + (uint32_t)v->pos;
        if (b->loop_count && v->loops_done < b->loop_count && p >= lend) {               /* back to the loop start */
            v->pos -= lend - b->loop_begin; v->loops_done += b->loop_count != 255;
            ev(v->callback, 5, b->ctx, 1);                                                 /* OnLoopEnd */
            p = begin + (uint32_t)v->pos;
        }
        if (p >= end) {                                                                    /* buffer done */
            ev(v->callback, 4, b->ctx, 1);                                                 /* OnBufferEnd */
            if (b->flags & 0x40u) ev(v->callback, 2, 0, 0);                                /* OnStreamEnd */
            v->qhead = (v->qhead + 1) % MAXQ; v->qn--;
            v->pos = 0; v->loops_done = 0; v->started_cur = 0;
            f--;
            continue;
        }
        const uint8_t *m = HOST(b->data);
        for (uint32_t c = 0; c < v->ch && c < 8; c++) {
            float s = sample(v, m, p, c);
            out[2 * f] += s * gl[c];
            out[2 * f + 1] += s * gr[c];
        }
        double before = v->pos;
        v->pos += step;
        v->played += (uint64_t)((uint32_t)v->pos - (uint32_t)before);
    }
}

static CPU *engine_cpu;
CPU *cpu_guest_thread_context(uint32_t stack);

static void *engine_main(void *arg) {
    (void)arg;
    static float out[2 * PASS];
    double next = SDL_GetTicksNS() / 1e9;
    for (;;) {
        /* pass start: engine and voice callbacks (they may submit buffers) */
        pthread_mutex_lock(&mu);
        ev(engine_cb, 0, 0, 0);                                                            /* OnProcessingPassStart */
        for (int i = 0; i < MAXV; i++)
            if (voices[i].used && voices[i].kind == 0 && voices[i].running)
                ev(voices[i].callback, 0, PASS * voices[i].block, 1);                      /* OnVoiceProcessingPassStart */
        pthread_mutex_unlock(&mu);
        deliver(engine_cpu);
        /* the mix */
        memset(out, 0, sizeof out);
        pthread_mutex_lock(&mu);
        float master = 1.0f;
        for (int i = 0; i < MAXV; i++) {
            if (!voices[i].used) continue;
            if (voices[i].kind == 2) master = voices[i].volume;
            if (voices[i].kind == 0 && voices[i].running) mix_voice(&voices[i], out, PASS);
        }
        for (int i = 0; i < MAXV; i++)
            if (voices[i].used && voices[i].kind == 0 && voices[i].running) ev(voices[i].callback, 1, 0, 0);   /* PassEnd */
        ev(engine_cb, 1, 0, 0);                                                            /* OnProcessingPassEnd */
        pthread_mutex_unlock(&mu);
        deliver(engine_cpu);
        for (int i = 0; i < 2 * PASS; i++) { float s = out[i] * master; out[i] = s > 1 ? 1 : s < -1 ? -1 : s; }
        audio_dump(AUDIO_GAME, out, PASS);
        if (getenv("USF_XAUDIO2_LOG")) {                                                  /* every 5 s */
            static int pass_n;
            if (++pass_n % 500 == 0) {
                int nv = 0, nrun = 0, nq = 0; uint64_t played = 0;
                pthread_mutex_lock(&mu);
                for (int i = 0; i < MAXV; i++) if (voices[i].used && voices[i].kind == 0) {
                    nv++; nrun += voices[i].running; nq += voices[i].qn; played += voices[i].played; }
                pthread_mutex_unlock(&mu);
                fprintf(stderr, "usf: xaudio2: %d source voices, %d running, %d buffers queued, %llu samples played, cb %08x\n",
                        nv, nrun, nq, (unsigned long long)played, engine_cb);
            }
        }
        if (stream) SDL_PutAudioStreamData(stream, out, (int)sizeof out);
        /* pace: one pass per 10 ms of real time, a little ahead of the device */
        next += (double)PASS / RATE;
        double now = SDL_GetTicksNS() / 1e9;
        if (next > now) SDL_DelayNS((Uint64)((next - now) * 1e9));
        else if (now - next > 0.2) next = now;                                             /* fell behind: resync */
    }
    return NULL;
}

static void engine_start(void) {
    if (engine_up) return;
    engine_up = 1;
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, RATE};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (stream) SDL_ResumeAudioStreamDevice(stream);
    else fprintf(stderr, "usf: XAudio2: no audio device: %s\n", SDL_GetError());
    engine_cpu = cpu_guest_thread_context(0x100000u);
    pthread_create(&engine_th, NULL, engine_main, NULL);
    fprintf(stderr, "usf: XAudio2 engine up (stereo, 48 kHz)\n");
}

/* ---- voices ---- */
static int new_voice(int kind, uint32_t vt) {
    for (int i = 0; i < MAXV; i++)
        if (!voices[i].used) {
            Voice *v = &voices[i];
            memset(v, 0, sizeof *v);
            v->used = 1; v->kind = kind; v->volume = 1.0f; v->ratio = 1.0f; v->send = -1;
            for (int c = 0; c < 8; c++) v->chvol[c] = 1.0f;
            v->guest = gcalloc(16);
            W32(v->guest, vt);
            W32(v->guest + 8, (uint32_t)i);
            return i;
        }
    return -1;
}

static uint32_t ok(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
static uint32_t one(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
static uint32_t qi(CPU *c, uint32_t a) { (void)c; W32(ARG(2), ARG(0)); return 0; }

/* IXAudio2 */
static uint32_t x_count(CPU *c, uint32_t a) { (void)c; W32(ARG(1), 1); return 0; }
static uint32_t x_details(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(2);
    memset(HOST(p), 0, 1068);
    const char *n = "Default";
    for (int i = 0; n[i]; i++) { W16(p + 2u * (uint32_t)i, (uint8_t)n[i]); W16(p + 512 + 2u * (uint32_t)i, (uint8_t)n[i]); }
    W32(p + 1024, 0xF);                                            /* GlobalDefaultDevice */
    uint32_t w = p + 1028;                                         /* WAVEFORMATEXTENSIBLE: stereo float 48 kHz */
    W16(w, 0xFFFE); W16(w + 2, 2); W32(w + 4, RATE); W32(w + 8, RATE * 8); W16(w + 12, 8); W16(w + 14, 32); W16(w + 16, 22);
    W16(w + 18, 32); W32(w + 20, 3);
    static const uint8_t ieee[16] = {3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
    memcpy(HOST(w + 24), ieee, 16);
    return 0;
}
static uint32_t x_register(CPU *c, uint32_t a) { (void)c; engine_cb = ARG(1); return 0; }
static uint32_t x_unregister(CPU *c, uint32_t a) { (void)c; if (engine_cb == ARG(1)) engine_cb = 0; return 0; }

static void read_format(Voice *v, uint32_t fmt) {
    uint16_t tag = R16(fmt);
    v->ch = R16(fmt + 2); v->rate = R32(fmt + 4); v->block = R16(fmt + 12); v->bits = R16(fmt + 14);
    v->is_float = tag == 3 || (tag == 0xFFFE && R16(fmt + 16) >= 22 && R32(fmt + 24) == 3);
    v->adpcm = tag == 2;
    if (v->adpcm) { v->spb = R16(fmt + 18); v->dec_block = 0; if (!v->spb || v->spb > 2048) v->spb = 2048; }
    if (tag != 1 && tag != 2 && tag != 3 && tag != 0xFFFE) fprintf(stderr, "usf: XAudio2: format tag %u not mixed\n", tag);
    if (!v->block) v->block = v->ch * v->bits / 8;
    if (v->ch > 8) v->ch = 8;
}
static int send_target(uint32_t sends) {               /* XAUDIO2_VOICE_SENDS {count, pSends -> {flags, pOutputVoice}} */
    if (!sends || !R32(sends)) return -1;
    Voice *t = vof(R32(R32(sends + 4) + 4));
    return t && t->kind == 1 ? (int)(t - voices) : -1;
}
static uint32_t x_source(CPU *c, uint32_t a) {
    (void)c;
    if (getenv("USF_XAUDIO2_LOG")) fprintf(stderr, "usf: xaudio2: CreateSourceVoice fmt tag %u ch %u rate %u bits %u\n",
                                           R16(ARG(2)), R16(ARG(2) + 2), R32(ARG(2) + 4), R16(ARG(2) + 14));
    pthread_mutex_lock(&mu);
    int i = new_voice(0, vt_source);
    if (i >= 0) {
        read_format(&voices[i], ARG(2));
        voices[i].flags = ARG(3); voices[i].callback = ARG(5); voices[i].send = send_target(ARG(6));
        W32(ARG(1), voices[i].guest);
    }
    pthread_mutex_unlock(&mu);
    return i >= 0 ? 0 : 0x8007000Eu;
}
static uint32_t x_submix(CPU *c, uint32_t a) {
    (void)c;
    pthread_mutex_lock(&mu);
    int i = new_voice(1, vt_submix);
    if (i >= 0) { voices[i].ch = ARG(2); voices[i].rate = ARG(3); W32(ARG(1), voices[i].guest); }
    pthread_mutex_unlock(&mu);
    return i >= 0 ? 0 : 0x8007000Eu;
}
static uint32_t x_master(CPU *c, uint32_t a) {
    (void)c;
    engine_start();
    pthread_mutex_lock(&mu);
    int i = new_voice(2, vt_master);
    if (i >= 0) { voices[i].ch = 2; voices[i].rate = RATE; W32(ARG(1), voices[i].guest); }
    pthread_mutex_unlock(&mu);
    return i >= 0 ? 0 : 0x8007000Eu;
}
static uint32_t x_perf(CPU *c, uint32_t a) { (void)c; memset(HOST(ARG(1)), 0, 64); return 0; }

/* IXAudio2Voice / IXAudio2SourceVoice */
static uint32_t v_details(CPU *c, uint32_t a) {
    (void)c;
    Voice *v = vof(ARG(0));
    if (v) { W32(ARG(1), v->flags); W32(ARG(1) + 4, v->ch); W32(ARG(1) + 8, v->rate); }
    return 0;
}
static uint32_t v_outputs(CPU *c, uint32_t a) { (void)c; Voice *v = vof(ARG(0)); if (v) v->send = send_target(ARG(1)); return 0; }
static uint32_t v_setvol(CPU *c, uint32_t a) { (void)c; Voice *v = vof(ARG(0)); if (v) v->volume = fl(ARG(1)); return 0; }
static uint32_t v_getvol(CPU *c, uint32_t a) { (void)c; Voice *v = vof(ARG(0)); float f = v ? v->volume : 1; memcpy(HOST(ARG(1)), &f, 4); return 0; }
static uint32_t v_setch(CPU *c, uint32_t a) {
    (void)c;
    Voice *v = vof(ARG(0));
    for (uint32_t i = 0; v && i < ARG(1) && i < 8; i++) v->chvol[i] = fl(R32(ARG(2) + 4 * i));
    return 0;
}
static uint32_t v_getch(CPU *c, uint32_t a) {
    (void)c;
    Voice *v = vof(ARG(0));
    for (uint32_t i = 0; v && i < ARG(1) && i < 8; i++) memcpy(HOST(ARG(2) + 4 * i), &v->chvol[i], 4);
    return 0;
}
static uint32_t v_matrix(CPU *c, uint32_t a) {          /* (dest, src channels, dest channels, matrix[dst][src]) */
    (void)c;
    Voice *v = vof(ARG(0));
    uint32_t sc = ARG(2), dc = ARG(3);
    if (!v || !ARG(4) || sc > 8 || dc < 1) return 0;
    for (uint32_t s = 0; s < sc; s++) {
        v->matrix[s][0] = fl(R32(ARG(4) + 4 * (0 * sc + s)));
        v->matrix[s][1] = dc > 1 ? fl(R32(ARG(4) + 4 * (1 * sc + s))) : v->matrix[s][0];
    }
    v->has_matrix = 1;
    return 0;
}
static uint32_t v_destroy(CPU *c, uint32_t a) {
    (void)c;
    pthread_mutex_lock(&mu);
    Voice *v = vof(ARG(0));
    if (v) v->used = 0;
    pthread_mutex_unlock(&mu);
    return 0;
}
static uint32_t v_start(CPU *c, uint32_t a) { (void)c; Voice *v = vof(ARG(0)); if (v) v->running = 1; return 0; }
static uint32_t v_stop(CPU *c, uint32_t a) { (void)c; Voice *v = vof(ARG(0)); if (v) v->running = 0; return 0; }
static uint32_t v_submit(CPU *c, uint32_t a) {
    (void)c;
    pthread_mutex_lock(&mu);
    Voice *v = vof(ARG(0));
    uint32_t r = 0;
    if (!v || v->qn >= MAXQ) r = 0x88960001u;                                      /* XAUDIO2_E_INVALID_CALL */
    else {
        uint32_t b = ARG(1);
        v->q[(v->qhead + v->qn) % MAXQ] = (XBuf){R32(b), R32(b + 4), R32(b + 8), R32(b + 12), R32(b + 16), R32(b + 20),
                                                 R32(b + 24), R32(b + 28), R32(b + 32)};
        v->qn++;
        if (getenv("USF_XAUDIO2_LOG")) {                       /* the loudest sample of every 200th buffer */
            static int nsub;
            if (++nsub % 200 == 0) {
                float pk = 0; uint32_t bytes = R32(b + 4), d = R32(b + 8);
                for (uint32_t k = 0; k + 4 <= bytes; k += 4) { float f = fl(R32(d + k)); if (f < 0) f = -f; if (f > pk && f < 1e6f) pk = f; }
                fprintf(stderr, "usf: xaudio2: buffer %u bytes at %08x flags %x begin %u len %u, peak %.4f (as float)\n",
                        bytes, d, R32(b), R32(b + 12), R32(b + 16), pk);
            }
        }
    }
    pthread_mutex_unlock(&mu);
    return r;
}
static uint32_t v_flush(CPU *c, uint32_t a) {           /* drops the queued buffers (the playing one stays if running) */
    (void)c;
    pthread_mutex_lock(&mu);
    Voice *v = vof(ARG(0));
    if (v) {
        int keep = v->running && v->started_cur && v->qn ? 1 : 0;
        for (int i = keep; i < v->qn; i++) ev(v->callback, 4, v->q[(v->qhead + i) % MAXQ].ctx, 1);
        v->qn = keep;
        if (!keep) { v->pos = 0; v->started_cur = 0; v->loops_done = 0; }
    }
    pthread_mutex_unlock(&mu);
    return 0;
}
static uint32_t v_exitloop(CPU *c, uint32_t a) {
    (void)c;
    Voice *v = vof(ARG(0));
    if (v && v->qn) v->q[v->qhead].loop_count = v->loops_done;
    return 0;
}
static uint32_t v_state(CPU *c, uint32_t a) {           /* XAUDIO2_VOICE_STATE {context, queued, u64 samples played} */
    (void)c;
    pthread_mutex_lock(&mu);
    Voice *v = vof(ARG(0));
    W32(ARG(1), v && v->qn ? v->q[v->qhead].ctx : 0);
    W32(ARG(1) + 4, v ? (uint32_t)v->qn : 0);
    W64(ARG(1) + 8, v ? v->played : 0);
    pthread_mutex_unlock(&mu);
    return 0;
}
static uint32_t v_setratio(CPU *c, uint32_t a) { (void)c; Voice *v = vof(ARG(0)); if (v) v->ratio = fl(ARG(1)); return 0; }
static uint32_t v_getratio(CPU *c, uint32_t a) { (void)c; Voice *v = vof(ARG(0)); float f = v ? v->ratio : 1; memcpy(HOST(ARG(1)), &f, 4); return 0; }
static uint32_t v_setrate(CPU *c, uint32_t a) { (void)c; Voice *v = vof(ARG(0)); if (v) v->rate = ARG(1); return 0; }

static HleFn t_x2[16] = {qi, one, one, x_count, x_details, ok, x_register, x_unregister, x_source, x_submix, x_master,
                         ok, ok, ok, x_perf, ok};
static const uint8_t n_x2[16] = {3, 1, 1, 2, 3, 3, 2, 2, 8, 8, 7, 1, 1, 2, 2, 3};
static HleFn t_v[29] = {v_details, v_outputs, ok, ok, ok, ok, ok, ok, ok, ok, ok, ok, v_setvol, v_getvol, v_setch,
                        v_getch, v_matrix, ok, v_destroy, v_start, v_stop, v_submit, v_flush, ok, v_exitloop, v_state,
                        v_setratio, v_getratio, v_setrate};
static const uint8_t n_v[29] = {2, 2, 2, 3, 3, 3, 5, 4, 3, 2, 4, 3, 3, 2, 4, 3, 6, 5, 1, 3, 3, 3, 1, 1, 2, 2, 3, 2, 2};

uint32_t xaudio2_create(uint32_t clsid, uint32_t out);
uint32_t xaudio2_create(uint32_t clsid, uint32_t out) {
    if (R32(clsid) != 0x5a508685u && R32(clsid) != 0xdb05ea35u) return 0x80040154u;   /* XAudio2 2.7 (and debug) */
    if (getenv("USF_NO_XAUDIO2")) return 0x80040154u;   /* for tests: no sound device */
    if (!engine_obj) {
        engine_obj = obj_new_n("IXAudio2", 16, t_x2, n_x2, 16);
        vt_source = R32(obj_new_n("IXAudio2SourceVoice", 29, t_v, n_v, 16));
        vt_submix = R32(obj_new_n("IXAudio2SubmixVoice", 19, t_v, n_v, 16));
        vt_master = R32(obj_new_n("IXAudio2MasteringVoice", 19, t_v, n_v, 16));
    }
    W32(out, engine_obj);
    return 0;
}
