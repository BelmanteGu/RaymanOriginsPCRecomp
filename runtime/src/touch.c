/* On-screen controls and the port's own settings, drawn over the game (the touch layer).
 *
 * What it does: in play only the controls show: a floating stick on the left half (it appears where the thumb lands),
 * jump (A), attack (X), back (B) and a run toggle on the right, and a pause button at the top right. Fingers are
 * tracked individually; the result is a set of pad bits (the same ones USF_INPUT uses, see hle_xinput.c) that the
 * game reads through its keyboard (GetKeyState) and its DirectInput pad. Pause sends Start, which opens the game's
 * own pause menu; while it is open, the port's settings appear at the bottom of the screen, also by touch:
 * on-screen controls on/off, their size and opacity, and 60 / 120 fps. They are kept in <data>/port_settings.txt.
 * The layer is drawn on the CPU into an RGBA picture (shapes and a built-in 5x7 font) that vk_overlay.c blends over
 * the frame. On Android it is on by default; on the PC with RAYMAN_TOUCH=1 (the mouse is a finger).
 * What it does not do: put entries into the game's own option screens (the game's own Options keep its settings,
 * resolution included); haptics.
 */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

enum { B_UP = 0x0001, B_DOWN = 0x0002, B_LEFT = 0x0004, B_RIGHT = 0x0008, B_START = 0x0400 /* pause: Esc + Start */, B_A = 0x1000,
       B_B = 0x2000, B_X = 0x4000, B_RUN = 0x0200 };

typedef struct { int on, size, opacity, fps120; } Settings;   /* size 0..2, opacity 0..2 */
static Settings set = {1, 1, 1, 0};
static int active;                                     /* the layer exists (Android, or RAYMAN_TOUCH on the PC) */
static int paused, run_on, dirty = 1;
static uint16_t held;                                  /* pad bits from fingers this frame */
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

typedef struct { long id; float x, y, x0, y0; int role; } Finger;   /* role: 0 none, 1 stick, 2+ button index */
static Finger fingers[10];

typedef struct { float x, y, r; uint16_t bit; const char *label; int toggle; } Button;
static Button buttons[] = {
    {0.89f, 0.72f, 0.075f, B_A, "A", 0},        /* jump */
    {0.79f, 0.84f, 0.065f, B_X, "X", 0},        /* attack */
    {0.80f, 0.58f, 0.055f, B_B, "B", 0},        /* back / attack */
    {0.94f, 0.47f, 0.050f, B_RUN, "RUN", 1},    /* run toggle */
    {0.955f, 0.08f, 0.040f, B_START, "II", 0},  /* pause */
};
#define NB (int)(sizeof buttons / sizeof buttons[0])
static float aspect = 16.0f / 9.0f;                     /* of the screen: circles stay round */

/* ---- settings ---- */
static void path(char *out, size_t n) { snprintf(out, n, "%s/port_settings.txt", g_data_dir); }
static void save(void) {
    char p[1100];
    path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "touch=%d\nsize=%d\nopacity=%d\nfps120=%d\n", set.on, set.size, set.opacity, set.fps120); fclose(f); }
}
static void load(void) {
    char p[1100], line[128];
    path(p, sizeof p);
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "touch=%d", &v) == 1) set.on = v;
        else if (sscanf(line, "size=%d", &v) == 1) set.size = v % 3;
        else if (sscanf(line, "opacity=%d", &v) == 1) set.opacity = v % 3;
        else if (sscanf(line, "fps120=%d", &v) == 1) set.fps120 = v;
    }
    if (f) fclose(f);
}
void touch_init(void);
void touch_init(void) {
#ifdef __ANDROID__
    active = !getenv("RAYMAN_TOUCH") || atoi(getenv("RAYMAN_TOUCH"));
#else
    active = getenv("RAYMAN_TOUCH") && atoi(getenv("RAYMAN_TOUCH"));
#endif
    load();
    if (set.fps120 && !getenv("RAYMAN_FPS")) setenv("RAYMAN_FPS", "120", 1);
}
int touch_active(void);
int touch_active(void) { return active; }

/* ---- the settings bar shown while the game is paused ---- */
typedef struct { float x0, y0, x1, y1; } Rect;
static Rect setting_rect(int i) { return (Rect){0.06f + 0.225f * (float)i, 0.86f, 0.06f + 0.225f * (float)i + 0.205f, 0.96f}; }
static void setting_text(int i, char *out, size_t n) {
    static const char *lv[3] = {"S", "M", "L"}, *op[3] = {"25%", "50%", "75%"};
    switch (i) {
    case 0: snprintf(out, n, "CONTROLS: %s", set.on ? "ON" : "OFF"); break;
    case 1: snprintf(out, n, "SIZE: %s", lv[set.size]); break;
    case 2: snprintf(out, n, "OPACITY: %s", op[set.opacity]); break;
    default: snprintf(out, n, "FPS: %s", set.fps120 ? "120" : "60"); break;
    }
}
static void setting_tap(int i) {
    if (i == 0) set.on = !set.on;
    else if (i == 1) set.size = (set.size + 1) % 3;
    else if (i == 2) set.opacity = (set.opacity + 1) % 3;
    else {
        set.fps120 = !set.fps120;
        setenv("RAYMAN_FPS", set.fps120 ? "120" : "60", 1);   /* the pacer and the engine rates follow (d3d9_device.c) */
    }
    save();
}

/* ---- input ---- */
static float scale(void) { return set.size == 0 ? 0.8f : set.size == 2 ? 1.25f : 1.0f; }
static int hit_button(float x, float y) {
    for (int i = 0; i < NB; i++) {
        if (!set.on && buttons[i].bit != B_START) continue;
        float r = buttons[i].r * scale() * (buttons[i].bit == B_START ? 1.0f : 1.25f);   /* a forgiving target */
        float dx = (x - buttons[i].x) * aspect, dy = y - buttons[i].y;
        if (dx * dx + dy * dy <= r * r) return i;
    }
    return -1;
}
static void recompute(void) {
    uint16_t b = run_on ? B_RUN : 0;
    for (int k = 0; k < 10; k++) {
        Finger *f = &fingers[k];
        if (!f->id) continue;
        if (f->role == 1) {                                   /* stick: direction from where the thumb landed */
            float dx = (f->x - f->x0) * aspect, dy = f->y - f->y0, d = 0.035f;
            if (dx > d) b |= B_RIGHT;
            if (dx < -d) b |= B_LEFT;
            if (dy > d * 1.4f) b |= B_DOWN;
            if (dy < -d * 1.4f) b |= B_UP;
        } else if (f->role >= 2 && !buttons[f->role - 2].toggle) {
            b |= buttons[f->role - 2].bit;
        }
    }
    if (b != held) dirty = 1;
    held = b;
    for (int k = 0; k < 10; k++) if (fingers[k].id && fingers[k].role == 1) dirty = 1;   /* the stick follows the thumb */
}
/* type: 0 down, 1 move, 2 up; x, y in 0..1 of the window; id: the finger (mouse: 1) */
void touch_event(int type, long id, float x, float y, float screen_aspect);
void touch_event(int type, long id, float x, float y, float screen_aspect) {
    if (!active) return;
    pthread_mutex_lock(&lk);
    aspect = screen_aspect > 0.1f ? screen_aspect : aspect;
    int k;
    for (k = 0; k < 10 && fingers[k].id != id + 1; k++) {}
    if (type == 0) {
        if (paused) {                                         /* the settings bar takes taps while paused */
            for (int i = 0; i < 4; i++) {
                Rect r = setting_rect(i);
                if (x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1) { setting_tap(i); dirty = 1; pthread_mutex_unlock(&lk); return; }
            }
        }
        for (k = 0; k < 10 && fingers[k].id; k++) {}
        if (k == 10) { pthread_mutex_unlock(&lk); return; }
        Finger *f = &fingers[k];
        *f = (Finger){id + 1, x, y, x, y, 0};
        int bi = hit_button(x, y);
        if (bi >= 0) {
            f->role = bi + 2;
            if (buttons[bi].toggle) run_on = !run_on;
            if (buttons[bi].bit == B_START) paused = !paused;
            else if (buttons[bi].bit & (B_A | B_B)) paused = 0;   /* a choice in the pause menu ends it */
        } else if (set.on && x < 0.5f) {
            f->role = 1;
        }
    } else if (k < 10) {
        fingers[k].x = x;
        fingers[k].y = y;
        if (type == 2) { if (fingers[k].role == 1) dirty = 1; fingers[k].id = 0; }
    }
    recompute();
    pthread_mutex_unlock(&lk);
}
uint16_t touch_buttons(void);
uint16_t touch_buttons(void) { return active ? held : 0; }

/* USF_TOUCH="f1-f2:x,y;..." (tests): a finger down at (x, y) (0..1 of the screen) from frame f1, lifted at f2 */
void touch_script(uint32_t frame);
void touch_script(uint32_t frame) {
    static uint8_t state[64];                                  /* 0 waiting, 1 down, 2 done */
    const char *sc = getenv("USF_TOUCH");
    if (!sc || !active) return;
    int n = 0;
    for (const char *p = sc; *p && n < 64; n++) {
        unsigned f1, f2;
        float x, y;
        if (sscanf(p, "%u-%u:%f,%f", &f1, &f2, &x, &y) == 4) {
            if (state[n] == 0 && frame >= f1) { state[n] = 1; touch_event(0, 1000 + n, x, y, 0); }
            if (state[n] == 1 && frame >= f2) { state[n] = 2; touch_event(2, 1000 + n, x, y, 0); }
        }
        p = strchr(p, ';');
        if (!p) break;
        p++;
    }
}

/* ---- drawing (RGBA, premultiplied alpha) ---- */
static const uint8_t font5x7[][7] = {      /* ' ' 0-9 A-Z % : . / - ( ) */
    {0, 0, 0, 0, 0, 0, 0},
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 2, 4, 8, 31}, {31, 2, 4, 2, 1, 17, 14},
    {2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14}, {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
    {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12},
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14}, {28, 18, 17, 17, 17, 18, 28},
    {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16}, {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17},
    {14, 4, 4, 4, 4, 4, 14}, {7, 2, 2, 2, 2, 18, 12}, {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
    {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14}, {30, 17, 17, 30, 16, 16, 16},
    {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17}, {15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4},
    {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4}, {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
    {17, 17, 10, 4, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31},
    {24, 25, 2, 4, 8, 19, 3}, {0, 12, 12, 0, 12, 12, 0}, {0, 0, 0, 0, 0, 12, 12}, {1, 1, 2, 4, 8, 16, 16},
    {0, 0, 0, 31, 0, 0, 0}, {2, 4, 8, 8, 8, 4, 2}, {8, 4, 2, 2, 2, 4, 8},
};
static int glyph(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    if (ch >= '0' && ch <= '9') return 1 + ch - '0';
    if (ch >= 'A' && ch <= 'Z') return 11 + ch - 'A';
    const char *s = "%:./-()";
    const char *p = strchr(s, ch);
    return p && ch ? 37 + (int)(p - s) : 0;
}
static uint8_t *pix;
static int pw, ph;
static void blend(int x, int y, uint32_t rgb, float a) {
    if (x < 0 || y < 0 || x >= pw || y >= ph || a <= 0) return;
    uint8_t *p = pix + 4 * ((size_t)y * pw + x);
    float r = (float)((rgb >> 16) & 255) * a, g = (float)((rgb >> 8) & 255) * a, b = (float)(rgb & 255) * a;
    p[0] = (uint8_t)(r + p[0] * (1 - a)); p[1] = (uint8_t)(g + p[1] * (1 - a)); p[2] = (uint8_t)(b + p[2] * (1 - a));
    p[3] = (uint8_t)(255 * a + p[3] * (1 - a));
}
static void circle(float cx, float cy, float r, uint32_t rgb, float a, int ring) {
    int x0 = (int)(cx - r - 2), x1 = (int)(cx + r + 2), y0 = (int)(cy - r - 2), y1 = (int)(cy + r + 2);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
            float cov = r + 0.5f - d;
            if (ring) cov = fminf(cov, d - (r - (float)ring) + 0.5f);
            if (cov > 0) blend(x, y, rgb, a * fminf(cov, 1.0f));
        }
}
static void rect(float x0, float y0, float x1, float y1, uint32_t rgb, float a) {
    for (int y = (int)y0; y < (int)y1; y++) for (int x = (int)x0; x < (int)x1; x++) blend(x, y, rgb, a);
}
static void text(const char *s, float cx, float cy, float px, uint32_t rgb, float a) {
    int n = (int)strlen(s);
    float x = cx - (float)n * 6 * px / 2, y = cy - 7 * px / 2;
    for (int i = 0; i < n; i++, x += 6 * px) {
        const uint8_t *g = font5x7[glyph(s[i])];
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] & (16 >> c)) rect(x + c * px, y + r * px, x + (c + 1) * px, y + (r + 1) * px, rgb, a);
    }
}

/* The layer's picture for a w x h screen; NULL if there is nothing to draw. *changed: it differs from last time. */
const uint8_t *touch_picture(int w, int h, int *changed);
const uint8_t *touch_picture(int w, int h, int *changed) {
    *changed = 0;
    if (!active) return NULL;
    pthread_mutex_lock(&lk);
    if (w != pw || h != ph) { free(pix); pix = calloc((size_t)w * h, 4); pw = w; ph = h; dirty = 1; }
    if (!dirty) { pthread_mutex_unlock(&lk); return pix; }
    memset(pix, 0, (size_t)w * h * 4);
    float alpha = set.opacity == 0 ? 0.25f : set.opacity == 2 ? 0.75f : 0.5f, s = scale();
    for (int i = 0; i < NB; i++) {
        Button *b = &buttons[i];
        if (!set.on && b->bit != B_START) continue;
        float cx = b->x * (float)w, cy = b->y * (float)h, r = b->r * s * (float)h;
        int down = (held & b->bit) || (b->toggle && run_on);
        circle(cx, cy, r, down ? 0xFFD040u : 0x202020u, alpha * (down ? 0.9f : 0.6f), 0);
        circle(cx, cy, r, 0xFFFFFFu, alpha, (int)(r * 0.08f) + 2);
        text(b->label, cx, cy, fmaxf(2.0f, r / 9.0f), 0xFFFFFFu, fminf(1.0f, alpha + 0.3f));
    }
    for (int k = 0; k < 10 && set.on; k++)                     /* the stick under the thumb */
        if (fingers[k].id && fingers[k].role == 1) {
            float r = 0.13f * s * (float)h;
            circle(fingers[k].x0 * (float)w, fingers[k].y0 * (float)h, r, 0xFFFFFFu, alpha * 0.8f, (int)(r * 0.06f) + 2);
            circle(fingers[k].x * (float)w, fingers[k].y * (float)h, r * 0.45f, 0xFFFFFFu, alpha * 0.8f, 0);
        }
    if (paused) {
        for (int i = 0; i < 4; i++) {
            Rect r = setting_rect(i);
            char t[48];
            setting_text(i, t, sizeof t);
            rect(r.x0 * w, r.y0 * h, r.x1 * w, r.y1 * h, 0x7A0E0Eu, 0.85f);   /* the game's dark red */
            text(t, (r.x0 + r.x1) / 2 * w, (r.y0 + r.y1) / 2 * h, fmaxf(2.0f, (float)h / 300.0f), 0xFFF2C0u, 1.0f);
        }
    }
    dirty = 0;
    *changed = 1;
    pthread_mutex_unlock(&lk);
    return pix;
}
