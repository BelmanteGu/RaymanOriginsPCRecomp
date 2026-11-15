/* Movies decoded with FFmpeg for the DirectShow layer (movie.c). Times in 100 ns units, as IMediaSeeking. */
#pragma once
#include <stdint.h>

int movie_add(const char *host_path);          /* a file of the movie being built (video .wmv or soundtrack .wma) */
void movie_run(void);                          /* start the clock and the decoding threads */
void movie_stop(void);                         /* stop and close everything */
int movie_active(void);                        /* running (after movie_run) */
int movie_loaded(void);                        /* files open (after RenderFile) */
int movie_finished(void);                      /* the clock reached the duration */
int64_t movie_duration(void);
int64_t movie_position(void);
void movie_set_volume(int hundredths_db);      /* IBasicAudio::put_Volume */
/* the newest picture if it changed since *seq and fits: BGRA copied to dst, its size in *w, *h */
int movie_frame(uint8_t *dst, int max_w, int max_h, int *w, int *h, unsigned *seq);
