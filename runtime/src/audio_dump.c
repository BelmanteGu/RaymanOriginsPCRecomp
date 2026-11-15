/* USF_AUDIO_DUMP=dir: what each audio source sends to the sound device, as raw 48 kHz stereo float files
 * (<dir>/game.f32 for XAudio2, movie.f32 for movie soundtracks, dsound.f32 for DirectSound), for checking the audio without ears
 * (tools/audio_report.py turns them into WAV files, levels and a picture).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio_dump.h"

static FILE *files[3];
static int tried[3];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

void audio_dump(int src, const float *stereo, int frames) {
    const char *dir = getenv("USF_AUDIO_DUMP");
    if (!dir || src < 0 || src > 2 || frames <= 0) return;
    pthread_mutex_lock(&mu);
    if (!tried[src]) {
        char p[600];
        static const char *names[3] = {"game", "movie", "dsound"};
        snprintf(p, sizeof p, "%s/%s.f32", dir, names[src]);
        files[src] = fopen(p, "wb");
        tried[src] = 1;
    }
    if (files[src]) { fwrite(stereo, sizeof(float) * 2, (size_t)frames, files[src]); fflush(files[src]); }
    pthread_mutex_unlock(&mu);
}
