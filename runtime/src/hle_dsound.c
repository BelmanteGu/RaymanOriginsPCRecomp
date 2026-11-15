/* DirectSound 8 for the game's audio (CRI middleware), mixed with SDL3.
 *
 * What it does: IDirectSound8 creates sound buffers whose memory is guest memory (the game writes PCM through Lock);
 * an SDL3 audio stream callback (48 kHz stereo float) mixes every playing buffer with its frequency, volume
 * (hundredths of a dB), pan and looping, and moves its play cursor, which GetCurrentPosition reports (CRI streams by
 * writing ahead of that cursor). IDirectSoundNotify positions set their events when the cursor passes them.
 * What it does not do: 3D sound, effects, capture. 8- and 16-bit PCM, mono and stereo.
 */
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

#include "audio_dump.h"
#include "hle.h"

#define MAXBUF 256
typedef struct {
    int used, primary, playing, looping;
    uint32_t mem, size, freq, channels, bits, block;
    double pos;                         /* play position in frames (fractional while resampling) */
    float gain_l, gain_r;
    int32_t volume, pan;
    uint32_t guest;                     /* the guest object */
    uint32_t notify_pos[16], notify_ev[16], nnotify;
} Buf;

static Buf bufs[MAXBUF];
static pthread_mutex_t mix_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioStream *stream;
static uint32_t ds_obj;
int k32_set_event_handle(uint32_t h);

static Buf *buf_of(uint32_t g) {
    uint32_t i = R32(g + 8);
    return i < MAXBUF && bufs[i].used && bufs[i].guest == g ? &bufs[i] : NULL;
}

static void update_gain(Buf *b) {
    float v = b->volume <= -10000 ? 0.0f : powf(10.0f, (float)b->volume / 2000.0f);
    float l = 1.0f, r = 1.0f;
    if (b->pan < 0) r = powf(10.0f, (float)b->pan / 2000.0f);
    if (b->pan > 0) l = powf(10.0f, -(float)b->pan / 2000.0f);
    b->gain_l = v * l;
    b->gain_r = v * r;
}

static void SDLCALL mix(void *user, SDL_AudioStream *s, int additional, int total) {
    (void)user; (void)total;
    int frames = additional / 8;
    if (frames <= 0) return;
    static float out[2 * 8192];
    if (frames > 8192) frames = 8192;
    memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    pthread_mutex_lock(&mix_lock);
    for (int i = 0; i < MAXBUF; i++) {
        Buf *b = &bufs[i];
        if (!b->used || b->primary || !b->playing || !b->size || !b->block) continue;
        uint32_t nframes = b->size / b->block;
        double step = (double)b->freq / 48000.0;
        const uint8_t *m = HOST(b->mem);
        for (int f = 0; f < frames; f++) {
            uint32_t p = (uint32_t)b->pos;
            if (p >= nframes) {
                if (!b->looping) { b->playing = 0; b->pos = 0; break; }
                b->pos -= nframes; p = (uint32_t)b->pos;
            }
            float l, r;
            const uint8_t *x = m + p * b->block;
            if (b->bits == 16) {
                int16_t s0, s1;
                memcpy(&s0, x, 2);
                s1 = s0;
                if (b->channels > 1) memcpy(&s1, x + 2, 2);
                l = s0 / 32768.0f; r = s1 / 32768.0f;
            } else {
                l = (x[0] - 128) / 128.0f; r = b->channels > 1 ? (x[1] - 128) / 128.0f : l;
            }
            out[2 * f] += l * b->gain_l;
            out[2 * f + 1] += r * b->gain_r;
            double before = b->pos;
            b->pos += step;
            for (uint32_t n = 0; n < b->nnotify; n++) {          /* notification positions crossed */
                double np = (double)b->notify_pos[n] / b->block;
                if (b->notify_pos[n] != 0xFFFFFFFFu && before < np && b->pos >= np) k32_set_event_handle(b->notify_ev[n]);
            }
        }
    }
    pthread_mutex_unlock(&mix_lock);
    for (int f = 0; f < 2 * frames; f++) out[f] = out[f] > 1.0f ? 1.0f : out[f] < -1.0f ? -1.0f : out[f];
    audio_dump(AUDIO_DSOUND, out, frames);
    SDL_PutAudioStreamData(s, out, frames * 8);
}

static void audio_start(void) {
    if (stream) return;
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, 48000};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, mix, NULL);
    if (stream) SDL_ResumeAudioStreamDevice(stream);
    else fprintf(stderr, "usf: no audio device: %s\n", SDL_GetError());
}

/* ---- IDirectSoundBuffer8 ---- */
static uint32_t b_qi(CPU *c, uint32_t a);
static uint32_t b_addref(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
static uint32_t b_release(CPU *c, uint32_t a) {
    (void)c;
    Buf *b = buf_of(ARG(0));
    if (b) { pthread_mutex_lock(&mix_lock); b->used = 0; pthread_mutex_unlock(&mix_lock); }
    return 0;
}
static uint32_t b_getcaps(CPU *c, uint32_t a) {
    (void)c;
    Buf *b = buf_of(ARG(0));
    W32(ARG(1) + 4, 0xE0); W32(ARG(1) + 8, b ? b->size : 0);
    return 0;
}
static uint32_t b_getpos(CPU *c, uint32_t a) {
    (void)c;
    Buf *b = buf_of(ARG(0));
    if (!b) return 0x80004005u;
    pthread_mutex_lock(&mix_lock);
    uint32_t play = (uint32_t)b->pos * b->block, write = play + b->block * 960;   /* ~20 ms ahead */
    if (b->size) { play %= b->size; write %= b->size; }
    pthread_mutex_unlock(&mix_lock);
    if (ARG(1)) W32(ARG(1), play);
    if (ARG(2)) W32(ARG(2), write);
    return 0;
}
static uint32_t b_getformat(CPU *c, uint32_t a) {
    (void)c;
    Buf *b = buf_of(ARG(0));
    if (!b) return 0x80004005u;
    if (ARG(1) && ARG(2) >= 16) {
        W16(ARG(1), 1); W16(ARG(1) + 2, (uint16_t)b->channels); W32(ARG(1) + 4, b->freq); W32(ARG(1) + 8, b->freq * b->block);
        W16(ARG(1) + 12, (uint16_t)b->block); W16(ARG(1) + 14, (uint16_t)b->bits);
    }
    if (ARG(3)) W32(ARG(3), 18);
    return 0;
}
static uint32_t b_getvol(CPU *c, uint32_t a) { (void)c; Buf *b = buf_of(ARG(0)); W32(ARG(1), b ? (uint32_t)b->volume : 0); return 0; }
static uint32_t b_getpan(CPU *c, uint32_t a) { (void)c; Buf *b = buf_of(ARG(0)); W32(ARG(1), b ? (uint32_t)b->pan : 0); return 0; }
static uint32_t b_getfreq(CPU *c, uint32_t a) { (void)c; Buf *b = buf_of(ARG(0)); W32(ARG(1), b ? b->freq : 0); return 0; }
static uint32_t b_getstatus(CPU *c, uint32_t a) {
    (void)c;
    Buf *b = buf_of(ARG(0));
    W32(ARG(1), b && b->playing ? (b->looping ? 5u : 1u) : 0u);
    return 0;
}
static uint32_t b_lock(CPU *c, uint32_t a) {
    (void)c;
    Buf *b = buf_of(ARG(0));
    if (!b || !b->size) return 0x88780032u;                     /* DSERR_INVALIDCALL */
    uint32_t off = ARG(1) % b->size, n = ARG(2);
    if (ARG(7) & 2u) n = b->size;                               /* DSBLOCK_ENTIREBUFFER */
    if (ARG(7) & 1u) { uint32_t w; pthread_mutex_lock(&mix_lock); w = ((uint32_t)b->pos * b->block + b->block * 960) % b->size; pthread_mutex_unlock(&mix_lock); off = w; }
    if (n > b->size) n = b->size;
    uint32_t first = off + n <= b->size ? n : b->size - off;
    W32(ARG(3), b->mem + off); W32(ARG(4), first);
    if (ARG(5)) W32(ARG(5), first < n ? b->mem : 0);
    if (ARG(6)) W32(ARG(6), n - first);
    return 0;
}
static uint32_t b_play(CPU *c, uint32_t a) {
    (void)c;
    Buf *b = buf_of(ARG(0));
    if (!b) return 0x80004005u;
    audio_start();
    pthread_mutex_lock(&mix_lock);
    b->playing = 1;
    b->looping = (ARG(3) & 1u) != 0;
    pthread_mutex_unlock(&mix_lock);
    return 0;
}
static uint32_t b_setpos(CPU *c, uint32_t a) { (void)c; Buf *b = buf_of(ARG(0)); if (b && b->block) b->pos = (double)(ARG(1) / b->block); return 0; }
static uint32_t b_setformat(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
static uint32_t b_setvol(CPU *c, uint32_t a) { (void)c; Buf *b = buf_of(ARG(0)); if (b) { b->volume = (int32_t)ARG(1); update_gain(b); } return 0; }
static uint32_t b_setpan(CPU *c, uint32_t a) { (void)c; Buf *b = buf_of(ARG(0)); if (b) { b->pan = (int32_t)ARG(1); update_gain(b); } return 0; }
static uint32_t b_setfreq(CPU *c, uint32_t a) { (void)c; Buf *b = buf_of(ARG(0)); if (b && ARG(1)) b->freq = ARG(1); return 0; }
static uint32_t b_stop(CPU *c, uint32_t a) { (void)c; Buf *b = buf_of(ARG(0)); if (b) b->playing = 0; return 0; }
static uint32_t b_ok(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }

static HleFn vt_buf[24] = {b_qi, b_addref, b_release, b_getcaps, b_getpos, b_getformat, b_getvol, b_getpan, b_getfreq,
                           b_getstatus, b_ok, b_lock, b_play, b_setpos, b_setformat, b_setvol, b_setpan, b_setfreq,
                           b_stop, b_ok, b_ok, b_ok, b_ok, b_ok};
static const uint8_t na_buf[24] = {3, 1, 1, 2, 3, 4, 2, 2, 2, 2, 3, 8, 4, 2, 2, 2, 2, 2, 1, 5, 1, 4, 4, 5};

/* IDirectSoundNotify: positions + event handles */
static uint32_t n_set(CPU *c, uint32_t a) {
    (void)c;
    Buf *b = buf_of(R32(ARG(0) + 12));
    if (!b) return 0x80004005u;
    b->nnotify = ARG(1) < 16 ? ARG(1) : 16;
    for (uint32_t i = 0; i < b->nnotify; i++) { b->notify_pos[i] = R32(ARG(2) + 8 * i); b->notify_ev[i] = R32(ARG(2) + 8 * i + 4); }
    return 0;
}
static HleFn vt_notify[4] = {b_ok, b_addref, b_addref, n_set};
static const uint8_t na_notify[4] = {3, 1, 1, 3};
static uint32_t vt_buf_guest, vt_notify_guest;

static uint32_t b_qi(CPU *c, uint32_t a) {
    (void)c;
    if (R32(ARG(1)) == 0xb0210783u) {                            /* IID_IDirectSoundNotify */
        if (!vt_notify_guest) vt_notify_guest = R32(obj_new_n("IDirectSoundNotify", 4, vt_notify, na_notify, 16));
        uint32_t n = gcalloc(16);
        W32(n, vt_notify_guest);
        W32(n + 12, ARG(0));                                     /* the buffer it belongs to */
        W32(ARG(2), n);
        return 0;
    }
    W32(ARG(2), ARG(0));                                         /* IDirectSoundBuffer8 and friends: itself */
    return 0;
}

/* ---- IDirectSound8 ---- */
static uint32_t d_create_buffer(CPU *c, uint32_t a) {
    (void)c;
    uint32_t desc = ARG(1), flags = R32(desc + 4), size = R32(desc + 8), wfx = R32(desc + 16);
    int i = 1;
    while (i < MAXBUF && bufs[i].used) i++;
    if (i >= MAXBUF) return 0x8007000Eu;
    Buf *b = &bufs[i];
    memset(b, 0, sizeof *b);
    b->used = 1;
    b->primary = (flags & 1u) != 0;
    b->size = b->primary ? 0 : size;
    b->channels = wfx ? R16(wfx + 2) : 2;
    b->freq = wfx ? R32(wfx + 4) : 48000;
    b->bits = wfx ? R16(wfx + 14) : 16;
    b->block = wfx ? R16(wfx + 12) : 4;
    b->mem = b->size ? gcalloc(b->size) : 0;
    b->volume = 0;
    update_gain(b);
    if (!vt_buf_guest) vt_buf_guest = R32(obj_new_n("IDirectSoundBuffer8", 24, vt_buf, na_buf, 16));
    uint32_t g = gcalloc(16);
    W32(g, vt_buf_guest);
    W32(g + 8, (uint32_t)i);
    b->guest = g;
    W32(ARG(2), g);
    usf_log("DirectSound buffer %d: %u bytes, %u Hz, %u ch, %u bits%s\n", i, b->size, b->freq, b->channels, b->bits,
            b->primary ? " (primary)" : "");
    return 0;
}
static uint32_t d_getcaps(CPU *c, uint32_t a) { (void)c; memset(HOST(ARG(1) + 4), 0, 92); W32(ARG(1) + 4, 0xF5F); return 0; }
static uint32_t d_spk(CPU *c, uint32_t a) { (void)c; W32(ARG(1), 4); return 0; }   /* DSSPEAKER_STEREO */
static HleFn vt_ds[12] = {b_ok, b_addref, b_addref, d_create_buffer, d_getcaps, b_ok, b_ok, b_ok, d_spk, b_ok, b_ok, b_ok};
static const uint8_t na_ds[12] = {3, 1, 1, 4, 2, 3, 3, 1, 2, 2, 2, 2};

uint32_t x_DirectSoundCreate8(CPU *c, uint32_t a) {
    (void)c;
    if (!ds_obj) ds_obj = obj_new_n("IDirectSound8", 12, vt_ds, na_ds, 16);
    if (ARG(1)) W32(ARG(1), ds_obj);
    audio_start();
    return 0;
}
