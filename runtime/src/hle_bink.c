/* binkw32.dll: the Bink movie calls (intro, ending, attract demo), bound by their decorated stdcall names.
 *
 * What it does for now: tells the game that no movie can be opened (BinkOpen returns NULL), which the game treats as
 * "skip this movie". The other calls are accepted so nothing breaks if one is reached anyway.
 * What it does not do yet: play the movies. binkw32.dll is x86 code from RAD and cannot run here; the plan is FFmpeg
 * (which decodes Bink video and audio) behind these same calls, like the USFIV-PC movie player (movie.c).
 */
#include "hle.h"

static uint32_t bink_open(CPU *c, uint32_t a) {
    (void)c;
    fprintf(stderr, "usf: BinkOpen(%s, %x): movies are not played yet, skipped\n", STR(ARG(0)), ARG(1));
    return 0;
}
static uint32_t ok1(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
static uint32_t zero(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }

#define B(name, n, fn) {"binkw32.dll", name, n, CONV_STDCALL, fn},
const HleImport hle_bink_table[] = {
    B("_BinkOpen@8", 2, bink_open)
    B("_BinkSetSoundSystem@8", 2, ok1)
    B("_BinkOpenDirectSound@4", 1, zero)
    B("_BinkClose@4", 1, zero)
    B("_BinkNextFrame@4", 1, zero)
    B("_BinkDoFrame@4", 1, zero)
    B("_BinkWait@4", 1, zero)
    B("_BinkShouldSkip@4", 1, zero)
    B("_BinkGetRects@8", 2, zero)
    B("_BinkGetRealtime@12", 3, zero)
    B("_BinkGetSummary@8", 2, zero)
    B("_BinkRegisterFrameBuffers@8", 2, zero)
    B("_BinkGetFrameBuffersInfo@8", 2, zero)
    B("_BinkSetWillLoop@8", 2, zero)
    B("_BinkSetVolume@12", 3, zero)
    {NULL, NULL, 0, 0, NULL},
};
