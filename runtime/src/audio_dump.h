/* USF_AUDIO_DUMP (audio_dump.c): raw 48 kHz stereo float of each audio source. */
#pragma once
enum { AUDIO_GAME = 0, AUDIO_MOVIE = 1, AUDIO_DSOUND = 2 };   /* XAudio2, movies, DirectSound */
void audio_dump(int src, const float *stereo, int frames);
