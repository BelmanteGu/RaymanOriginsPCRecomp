/* USF_PS_ROTATE=<frame> (analysis): what each pixel shader costs a frame, in one run on the phone.
 *
 * What it does: from presented frame <frame> on, windows of 90 frames alternate: one as the game draws it, one with
 * the draws of one pixel shader left out (the next shader in the list of those drawn so far), and so on round the
 * list. Each pair is logged: "psrot: <hash> <draws a frame>: <ms a frame without it> (with it <ms>)": what leaving
 * it out saved. The scene moves meanwhile (play it back with USF_REPLAY), so read several rounds.
 * What it does not do: anything without USF_PS_ROTATE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "d3d9.h"
#include "dxvk_bridge.h"

#define WIN 90
#define MAXPS 128
static int on = -1;
static uint32_t start;
static struct { uint32_t hash, draws; } ps[MAXPS];
static int nps, target = -1, window, k;       /* window: frames in the current one; k: the next shader to leave out */
static double t_win, base_ms;
static uint32_t cur_hash;

static double ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static int enabled(void) {
    if (on < 0) { const char *e = getenv("USF_PS_ROTATE"); on = e != NULL; start = e ? (uint32_t)atoi(e) : 0; }
    return on;
}
/* SetPixelShader: the shader of the next draws (its content hash) */
void psrot_set(uint32_t hash) { if (enabled()) cur_hash = hash; }
/* a draw: 1 if it is to be left out */
int psrot_draw(void) {
    if (!enabled() || g_dev.frames < start) return 0;
    int i = 0;
    while (i < nps && ps[i].hash != cur_hash) i++;
    if (i == nps && nps < MAXPS) { ps[nps].hash = cur_hash; ps[nps].draws = 0; nps++; }
    if (i < MAXPS) ps[i].draws++;
    return target >= 0 && target < nps && ps[target].hash == cur_hash;
}
/* a presented frame */
void psrot_frame(void) {
    if (!enabled() || g_dev.frames < start) return;
    double now = ms_now();
    if (!t_win) { t_win = now; window = 0; return; }
    if (++window < WIN) return;
    double ms = (now - t_win) / WIN;
    if (target < 0) base_ms = ms;                          /* a window as drawn: the base for the next */
    else {
        fprintf(stderr, "usf: psrot: %08x %5.1f draws/frame: %5.1f ms without it (with it %5.1f): saves %+5.1f ms\n",
                ps[target].hash, ps[target].draws / (double)WIN, ms, base_ms, base_ms - ms);
        for (int i = 0; i < nps; i++) ps[i].draws = 0;
    }
    if (target < 0 && nps) { target = k % nps; k++; } else target = -1;
    t_win = now;
    window = 0;
}
