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

/* USF_RSLOG=1 (analysis): which fixed-function features the game's draws use (DXVK builds every pixel shader with
 * fog, alpha test, point sprites, clip planes and sRGB writes behind specialization constants; a driver that does not
 * fold them pays for them all). Every 5 s: of the draws, how many had each one on, and how many had no pixel shader. */
static uint32_t rs[256], rs_draws, rs_on[6], rs_noshader;
static double rs_t0;
static int rs_log = -1;
void rslog_state(uint32_t state, uint32_t value) { if (state < 256) rs[state] = value; }
void rslog_draw(uint32_t ps) {
    if (rs_log < 0) rs_log = getenv("USF_RSLOG") != NULL;
    if (!rs_log) return;
    static const uint32_t st[6] = {28, 15, 156, 152, 194, 27};   /* fog, alpha test, point sprite, clip, sRGB, blend */
    rs_draws++;
    for (int i = 0; i < 6; i++) if (rs[st[i]]) rs_on[i]++;
    if (!ps) rs_noshader++;
    double now = ms_now();
    if (!rs_t0) rs_t0 = now;
    if (now - rs_t0 < 5000) return;
    fprintf(stderr, "usf: rslog: %u draws: fog %u, alpha test %u (func %u ref %u), point sprite %u, clip planes %u, sRGB write %u, blend %u, no pixel shader %u\n",
            rs_draws, rs_on[0], rs_on[1], rs[25], rs[24], rs_on[2], rs_on[3], rs_on[4], rs_on[5], rs_noshader);
    rs_draws = rs_noshader = 0;
    for (int i = 0; i < 6; i++) rs_on[i] = 0;
    rs_t0 = now;
}

/* USF_ALPHAKILL=1 (experiment, weak mobile GPUs): pixels that change nothing never reach the render backend. On the
 * Adreno 610 the render backend is the limit (KGSL counters: busy 99.8%, the shaders stalled on it; ~11 layers a
 * pixel on the title screen). With blending SRCALPHA -> INVSRCALPHA or ONE (no separate alpha blend), a pixel of
 * alpha 0 leaves the target as it was, so D3D9's own alpha test (alpha > 0) is turned on for those draws and DXVK
 * discards them in the shader: the same picture. Needs the alpha test in DXVK's shaders (DXVK_LEAN_FF=0). */
void alphakill_draw(void *nat) {
    static int on = -1, set;
    if (on < 0) on = getenv("USF_ALPHAKILL") && *getenv("USF_ALPHAKILL") == '1';
    if (!on) return;
    int want = rs[27] && rs[19] == 5 && (rs[20] == 6 || rs[20] == 2) && !rs[206] && !rs[15];
    if (want == set) return;
    if (want) {
        dxb_method(nat, 57)(nat, 24, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);      /* ALPHAREF 0 */
        dxb_method(nat, 57)(nat, 25, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0);      /* ALPHAFUNC GREATER */
    }
    dxb_method(nat, 57)(nat, 15, (uint64_t)want, 0, 0, 0, 0, 0, 0, 0, 0, 0);   /* ALPHATESTENABLE */
    set = want;
}
