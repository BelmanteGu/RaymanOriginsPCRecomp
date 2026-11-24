/* On-screen controls and the port's own menu, drawn over the game (the touch layer).
 *
 * What it does: a floating stick on the left half (it appears where the thumb lands), jump (A), attack (X), back (B)
 * and run (RT) on the right, and two separate buttons at the top right: pause (the game's own pause menu, Start) and
 * the port's menu (the gear). Fingers are tracked individually; the result is a set of pad bits that hle_xinput.c
 * gives to the game as player 1's XInput pad, and only that way (the game reads the pad once it sees an XInput one,
 * hle_wmi.c; also feeding them as keys made one tap count twice).
 * The port's menu is a panel in the look of the game's pause board: on-screen controls on/off, their size and
 * opacity, run as toggle or hold, close; in the language chosen in the launcher (port_settings.txt "lang", the same
 * 12 as GameMenu.java; the ones this font cannot write fall back to English). While it is open the game gets no input
 * from the screen. The values are kept in <data>/port_settings.txt, shared with the launcher.
 * The layer is drawn on the CPU into an RGBA picture that dxvk_overlay.c blends over the frame: the buttons are the
 * drawings made for the Xbox 360 port (touch_art.c), the menu uses shapes and a built-in 5x7 font. On Android it is
 * on by default; on the PC with RAYMAN_TOUCH=1 (the mouse is a finger).
 * What it does not do: pause the game by itself when the port's menu opens (press pause for that); haptics.
 */
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "hle.h"

enum { B_UP = 0x0001, B_DOWN = 0x0002, B_LEFT = 0x0004, B_RIGHT = 0x0008, B_START = 0x0400 /* pause: Esc + Start */, B_A = 0x1000,
       B_B = 0x2000, B_X = 0x4000, B_RUN = 0x0200, B_MENU = 0x8000u << 1 /* not a pad bit: the port's menu */ };

/* size 0..2, opacity 0..2; stick 0 floating / 1 fixed; the launcher's keys too (GameMenu.java) */
typedef struct { int on, size, opacity, fps120, runhold, lang, stick, padhides, showfps, vibration, frameskip; } Settings;
static Settings set = {1, 1, 1, 0, 0, 0, 0, 1, 0, 1};
static int active;                                     /* the layer exists (Android, or RAYMAN_TOUCH on the PC) */
static int menu_open, editing, run_on, dirty = 1, pad_hidden;
int port_open_menu(void);                               /* port_android.c: the launcher's Options over the game */
void port_haptic(void);
static uint16_t held;                                  /* pad bits from fingers this frame */
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

typedef struct { long id; float x, y, x0, y0; int role; } Finger;   /* role: 0 none, 1 stick, 2+ button index */
static Finger fingers[10];

enum { ART_JUMP, ART_ATTACK, ART_BACK, ART_RUN, ART_PAUSE, ART_STICK_BASE, ART_STICK_KNOB, ART_MENU, ART_SIZE = 256 };
extern const uint8_t touch_art[];                       /* touch_art.c */
typedef struct { float x, y, r; uint32_t bit; int art; } Button;   /* x: of the width, y and r: of the height */
static Button buttons[] = {
    {0.89f, 0.72f, 0.075f, B_A, ART_JUMP},           /* jump */
    {0.79f, 0.84f, 0.065f, B_X, ART_ATTACK},         /* attack */
    {0.80f, 0.58f, 0.055f, B_B, ART_BACK},           /* back / attack */
    {0.94f, 0.47f, 0.050f, B_RUN, ART_RUN},          /* run (toggle or hold) */
    {0.955f, 0.09f, 0.050f, B_START, ART_PAUSE},     /* the game's pause */
    {0.955f, 0.215f, 0.050f, B_MENU, ART_MENU},      /* the port's menu, under pause (clear of the game's HUD) */
};
#define NB (int)(sizeof buttons / sizeof buttons[0])
/* where each button (and, last, the fixed stick) is: the defaults above, or where the player put it (b<i>x / b<i>y in
 * port_settings.txt, thousandths of the screen) */
static const float STICK_X = 0.14f, STICK_Y = 0.70f, STICK_R = 0.13f;
static float bx[NB + 1], by[NB + 1];
static void default_places(void) {
    for (int i = 0; i < NB; i++) { bx[i] = buttons[i].x; by[i] = buttons[i].y; }
    bx[NB] = STICK_X; by[NB] = STICK_Y;
}
static float aspect = 16.0f / 9.0f;                     /* of the screen: circles stay round */
static int always_shown(const Button *b) { return b->bit == B_START || b->bit == B_MENU; }

/* ---- settings ---- */
static void path(char *out, size_t n) { snprintf(out, n, "%s/port_settings.txt", g_data_dir); }
/* port_settings.txt is shared with the launcher (it writes lang=, lines=, music=...): lines this file does not own are
 * kept as they are */
static void save(void) {
    char p[1100], keep[2048] = "", line[160];
    path(p, sizeof p);
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f))
        if (strncmp(line, "touch=", 6) && strncmp(line, "size=", 5) && strncmp(line, "opacity=", 8) && strncmp(line, "fps120=", 7) &&
            strncmp(line, "runhold=", 8) && !(line[0] == 'b' && line[1] >= '0' && line[1] <= '9') &&
            strlen(keep) + strlen(line) < sizeof keep) strcat(keep, line);
    if (f) fclose(f);
    f = fopen(p, "w");
    if (f) {
        fprintf(f, "touch=%d\nsize=%d\nopacity=%d\nfps120=%d\nrunhold=%d\n", set.on, set.size, set.opacity, set.fps120, set.runhold);
        for (int i = 0; i <= NB; i++) {
            float dx = i < NB ? buttons[i].x : STICK_X, dy = i < NB ? buttons[i].y : STICK_Y;
            if (fabsf(bx[i] - dx) > 0.001f || fabsf(by[i] - dy) > 0.001f)
                fprintf(f, "b%dx=%d\nb%dy=%d\n", i, (int)lroundf(bx[i] * 1000), i, (int)lroundf(by[i] * 1000));
        }
        fputs(keep, f);
        fclose(f);
    }
}
/* one value of port_settings.txt, or def (the launcher's keys: lang, lines, music, sfx, stick, run, vibration...) */
int port_setting(const char *key, int def);
int port_setting(const char *key, int def) {
    char p[1100], line[160];
    size_t n = strlen(key);
    path(p, sizeof p);
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, key, n) && line[n] == '=') { def = atoi(line + n + 1); break; }
    if (f) fclose(f);
    return def;
}
static void load(void) {
    set.on = port_setting("touch", set.on);
    set.size = port_setting("size", set.size) % 3;
    set.opacity = port_setting("opacity", set.opacity) % 3;
    set.fps120 = port_setting("fps120", set.fps120);
    set.runhold = port_setting("runhold", set.runhold) ? 1 : 0;
    set.lang = port_setting("lang", 0);
    set.stick = port_setting("stick", 0) ? 1 : 0;
    set.padhides = port_setting("padhides", 1) ? 1 : 0;
    set.showfps = port_setting("showfps", 0) ? 1 : 0;
    set.vibration = port_setting("vibration", 1) ? 1 : 0;
#ifdef __ANDROID__
    set.frameskip = port_setting("frameskip", 1) ? 1 : 0;       /* on by default on a phone (see usf_frameskip) */
#else
    set.frameskip = port_setting("frameskip", 0) ? 1 : 0;
#endif
    if (getenv("USF_FRAMESKIP")) set.frameskip = atoi(getenv("USF_FRAMESKIP")) != 0;
    default_places();
    for (int i = 0; i <= NB; i++) {
        char k[8];
        snprintf(k, sizeof k, "b%dx", i);
        int x = port_setting(k, -1);
        snprintf(k, sizeof k, "b%dy", i);
        int y = port_setting(k, -1);
        if (x >= 0 && x <= 1000 && y >= 0 && y <= 1000) { bx[i] = (float)x / 1000; by[i] = (float)y / 1000; }
    }
}
/* after the launcher's Options screen saved over the game (port_android.c) */
void touch_reload(void);
void touch_reload(void) {           /* (the finger on the gear was lifted over the Options screen: forget them all) */
    pthread_mutex_lock(&lk); load(); run_on = 0; memset(fingers, 0, sizeof fingers); held = 0; dirty = 1; pthread_mutex_unlock(&lk);
}
/* moving the buttons: drag them; RESET and DONE at the top (port_android.c starts it from the Options screen) */
void touch_edit_layout(void);
void touch_edit_layout(void) { pthread_mutex_lock(&lk); editing = 1; menu_open = 0; memset(fingers, 0, sizeof fingers); dirty = 1; pthread_mutex_unlock(&lk); }
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
/* Frameskip (the launcher's Options, Display): the game advances one step per presented frame, so a phone that draws
 * 35 frames a second plays it at 58% speed; with it, a frame behind the clock is run without being drawn (the DXVK
 * bridge sends none of its GPU work), up to three in a row, and the game keeps its speed (d3d9_device.c game_pace). */
int usf_frameskip(void);
int usf_frameskip(void) { return set.frameskip; }

/* ---- the port's menu ---- */
enum { T_TITLE, T_CONTROLS, T_SIZE, T_OPACITY, T_RUN, T_CLOSE, T_ON, T_OFF, T_S, T_M, T_L, T_TOGGLE, T_HOLD, T_DRAG, T_RESET, T_DONE, T_N };
/* the launcher's language order: en fr de es it pt pl ru nl cs hu ja; the 5x7 font has no accents (written without) */
static const char *const words[][T_N] = {
    {"PORT OPTIONS", "TOUCH CONTROLS", "SIZE", "OPACITY", "RUN", "CLOSE", "ON", "OFF", "SMALL", "MEDIUM", "LARGE", "TOGGLE", "HOLD", "DRAG THE BUTTONS", "RESET", "DONE"},
    {"OPTIONS DU PORT", "COMMANDES TACTILES", "TAILLE", "OPACITE", "COURIR", "FERMER", "OUI", "NON", "PETIT", "MOYEN", "GRAND", "BASCULE", "MAINTENIR", "DEPLACEZ LES BOUTONS", "DEFAUT", "OK"},
    {"PORT-OPTIONEN", "TOUCH-STEUERUNG", "GROESSE", "DECKKRAFT", "RENNEN", "SCHLIESSEN", "AN", "AUS", "KLEIN", "MITTEL", "GROSS", "UMSCHALTEN", "HALTEN", "TASTEN VERSCHIEBEN", "STANDARD", "FERTIG"},
    {"OPCIONES DEL PORT", "CONTROLES TACTILES", "TAMANO", "OPACIDAD", "CORRER", "CERRAR", "SI", "NO", "PEQUENO", "MEDIANO", "GRANDE", "ALTERNAR", "MANTENER", "ARRASTRA LOS BOTONES", "RESTABLECER", "LISTO"},
    {"OPZIONI DEL PORT", "CONTROLLI TOUCH", "DIMENSIONE", "OPACITA", "CORSA", "CHIUDI", "SI", "NO", "PICCOLO", "MEDIO", "GRANDE", "ALTERNA", "TIENI", "TRASCINA I PULSANTI", "RIPRISTINA", "FATTO"},
    {"OPCOES DO PORT", "CONTROLES NA TELA", "TAMANHO", "OPACIDADE", "CORRER", "FECHAR", "SIM", "NAO", "PEQUENO", "MEDIO", "GRANDE", "ALTERNAR", "SEGURAR", "ARRASTE OS BOTOES", "PADRAO", "PRONTO"},
};
static const char *word(int w) {
    int l = set.lang >= 0 && set.lang < (int)(sizeof words / sizeof words[0]) ? set.lang : 0;
    return words[l][w];
}
#define NROWS 5                                          /* controls, size, opacity, run, close */
typedef struct { float x0, y0, x1, y1; } Rect;
static const Rect panel = {0.25f, 0.14f, 0.75f, 0.86f};    /* of the screen */
static Rect row_rect(int i) {
    float top = panel.y0 + 0.17f, hgt = 0.105f;
    return (Rect){panel.x0 + 0.03f, top + hgt * (float)i, panel.x1 - 0.03f, top + hgt * (float)i + hgt * 0.86f};
}
static const char *row_value(int i) {
    switch (i) {
    case 0: return word(set.on ? T_ON : T_OFF);
    case 1: return word(T_S + set.size);
    case 2: return set.opacity == 0 ? "50%" : set.opacity == 2 ? "100%" : "75%";
    case 3: return word(set.runhold ? T_HOLD : T_TOGGLE);
    default: return "";
    }
}
static void row_tap(int i, int back) {
    int d = back ? 2 : 1;                                     /* the value's '<' side steps back */
    if (i == 0) set.on = !set.on;
    else if (i == 1) set.size = (set.size + d) % 3;
    else if (i == 2) set.opacity = (set.opacity + d) % 3;
    else if (i == 3) { set.runhold = !set.runhold; run_on = 0; }
    else { menu_open = 0; return; }
    save();
}
static int in_rect(Rect r, float x, float y) { return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1; }
static const Rect edit_reset = {0.32f, 0.16f, 0.49f, 0.27f}, edit_done = {0.51f, 0.16f, 0.68f, 0.27f};

/* ---- input ---- */
static float scale(void) { return set.size == 0 ? 0.8f : set.size == 2 ? 1.25f : 1.0f; }
static int hit_button(float x, float y) {
    for (int i = 0; i < NB; i++) {
        if ((!set.on || pad_hidden) && !always_shown(&buttons[i]) && !editing) continue;
        if (menu_open && buttons[i].bit != B_MENU) continue;
        float r = buttons[i].r * (always_shown(&buttons[i]) ? 1.15f : scale() * 1.25f);   /* a forgiving target */
        float dx = (x - bx[i]) * aspect, dy = y - by[i];
        if (dx * dx + dy * dy <= r * r) return i;
    }
    return -1;
}
static void recompute(void) {
    uint16_t b = run_on && !menu_open && !editing ? B_RUN : 0;
    for (int k = 0; k < 10 && !menu_open && !editing; k++) {
        Finger *f = &fingers[k];
        if (!f->id) continue;
        if (f->role == 1) {                                   /* stick: direction from where the thumb landed */
            float dx = (f->x - f->x0) * aspect, dy = f->y - f->y0, d = 0.035f;
            if (dx > d) b |= B_RIGHT;
            if (dx < -d) b |= B_LEFT;
            if (dy > d * 1.4f) b |= B_DOWN;
            if (dy < -d * 1.4f) b |= B_UP;
        } else if (f->role >= 2) {
            uint32_t bit = buttons[f->role - 2].bit;
            if (bit == B_MENU || (bit == B_RUN && !set.runhold)) continue;
            b |= (uint16_t)bit;
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
    if (editing) {                                            /* moving the buttons */
        if (type == 0) {
            if (in_rect(edit_reset, x, y)) { default_places(); save(); }
            else if (in_rect(edit_done, x, y)) { editing = 0; save(); }
            else {
                int bi = hit_button(x, y);
                float sdx = (x - bx[NB]) * aspect, sdy = y - by[NB], sr = STICK_R * scale();
                if (bi < 0 && set.stick && sdx * sdx + sdy * sdy <= sr * sr) bi = NB;
                for (k = 0; k < 10 && fingers[k].id; k++) {}
                if (bi >= 0 && k < 10) fingers[k] = (Finger){id + 1, x, y, bx[bi], by[bi], bi + 2};
            }
        } else if (k < 10) {
            int i = fingers[k].role - 2;
            bx[i] = fminf(0.98f, fmaxf(0.02f, fingers[k].x0 + x - fingers[k].x));   /* x, y: where the finger landed */
            by[i] = fminf(0.97f, fmaxf(0.03f, fingers[k].y0 + y - fingers[k].y));
            if (type == 2) fingers[k].id = 0;
        }
        dirty = 1;
        pthread_mutex_unlock(&lk);
        return;
    }
    if (type == 0) {
        int bi = hit_button(x, y);
        if (menu_open && bi < 0) {                            /* the menu takes every other tap */
            for (int i = 0; i < NROWS; i++) {
                Rect r = row_rect(i);
                if (in_rect(r, x, y)) { row_tap(i, x < r.x1 - 0.11f && x > r.x1 - 0.30f); break; }
            }
            if (!in_rect(panel, x, y)) menu_open = 0;          /* a tap outside closes it */
            dirty = 1;
            pthread_mutex_unlock(&lk);
            return;
        }
        for (k = 0; k < 10 && fingers[k].id; k++) {}
        if (k == 10) { pthread_mutex_unlock(&lk); return; }
        Finger *f = &fingers[k];
        *f = (Finger){id + 1, x, y, x, y, 0};
        if (bi >= 0) {
            f->role = bi + 2;
            if (buttons[bi].bit == B_MENU) {
                if (menu_open) menu_open = 0;
                else if (!port_open_menu()) { menu_open = 1; set.lang = port_setting("lang", 0); }   /* the PC: its own panel */
            } else {
                if (buttons[bi].bit == B_RUN && !set.runhold) run_on = !run_on;
                if (set.vibration && buttons[bi].bit != B_START) port_haptic();
            }
            dirty = 1;
        } else if (set.on && !pad_hidden && set.stick) {             /* the fixed stick: where it was put */
            float dx = (x - bx[NB]) * aspect, dy = y - by[NB], r = STICK_R * scale() * 1.4f;
            if (dx * dx + dy * dy <= r * r) { f->role = 1; f->x0 = bx[NB]; f->y0 = by[NB]; }
        } else if (set.on && !pad_hidden && x < 0.5f) {
            f->role = 1;
        }
    } else if (k < 10) {
        fingers[k].x = x;
        fingers[k].y = y;
        if (type == 2) { if (fingers[k].role) dirty = 1; fingers[k].id = 0; }
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
static const uint8_t font5x7[][7] = {      /* ' ' 0-9 A-Z % : . / - ( ) < > */
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
    {0, 0, 0, 31, 0, 0, 0}, {2, 4, 8, 8, 8, 4, 2}, {8, 4, 2, 2, 2, 4, 8}, {2, 4, 8, 16, 8, 4, 2}, {8, 4, 2, 1, 2, 4, 8},
};
static int glyph(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    if (ch >= '0' && ch <= '9') return 1 + ch - '0';
    if (ch >= 'A' && ch <= 'Z') return 11 + ch - 'A';
    const char *s = "%:./-()<>";
    const char *p = strchr(s, ch);
    return p && ch ? 37 + (int)(p - s) : 0;
}
static uint8_t *pix;
static int pw, ph;
static char fps_text[16] = "";
/* The DXVK layer draws the drawings itself, as textured quads (dxvk_overlay.c): sprite() then only lists them, and
 * the picture keeps the shapes and text (menu, editor, fps), usually none. Rasterising the buttons into a picture of
 * the whole screen and uploading it at every finger move cost a tenth of the game thread on a weak phone. */
typedef struct { int art; float cx, cy, half, a; int bright; } TouchQuad;
int touch_gpu_sprites;
static TouchQuad quads[32];
static int nquads, pix_has;
const TouchQuad *touch_quads(int *n);
const TouchQuad *touch_quads(int *n) { *n = nquads; return quads; }
int touch_picture_has(void);
int touch_picture_has(void) { return pix_has; }
static void blend(int x, int y, uint32_t rgb, float a) {
    if (x < 0 || y < 0 || x >= pw || y >= ph || a <= 0) return;
    pix_has = 1;
    uint8_t *p = pix + 4 * ((size_t)y * pw + x);
    float r = (float)((rgb >> 16) & 255) * a, g = (float)((rgb >> 8) & 255) * a, b = (float)(rgb & 255) * a;
    p[0] = (uint8_t)(r + p[0] * (1 - a)); p[1] = (uint8_t)(g + p[1] * (1 - a)); p[2] = (uint8_t)(b + p[2] * (1 - a));
    p[3] = (uint8_t)(255 * a + p[3] * (1 - a));
}
/* one drawing centred on (cx, cy): its circle (84% of the picture: the art keeps a margin) has radius r; bilinear,
 * premultiplied; bright: pressed (lighter) */
static void sprite(int art, float cx, float cy, float r, float a, int bright) {
    const uint8_t *src = touch_art + (size_t)art * ART_SIZE * ART_SIZE * 4;
    float half = r / 0.84f, k = (float)ART_SIZE / (2 * half);
    if (touch_gpu_sprites) { if (nquads < 32) quads[nquads++] = (TouchQuad){art, cx, cy, half, a, bright}; return; }
    pix_has = 1;
    int x0 = (int)(cx - half), x1 = (int)(cx + half), y0 = (int)(cy - half), y1 = (int)(cy + half);
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < ph; y++)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < pw; x++) {
            float u = ((float)x + 0.5f - (cx - half)) * k - 0.5f, v = ((float)y + 0.5f - (cy - half)) * k - 0.5f;
            if (u < 0 || v < 0 || u > ART_SIZE - 1 || v > ART_SIZE - 1) continue;
            int iu = (int)u, iv = (int)v, ju = iu + 1 < ART_SIZE ? iu + 1 : iu, jv = iv + 1 < ART_SIZE ? iv + 1 : iv;
            float fu = u - (float)iu, fv = v - (float)iv, c[4];
            for (int ch = 0; ch < 4; ch++)
                c[ch] = (src[4 * (iv * ART_SIZE + iu) + ch] * (1 - fu) + src[4 * (iv * ART_SIZE + ju) + ch] * fu) * (1 - fv) +
                        (src[4 * (jv * ART_SIZE + iu) + ch] * (1 - fu) + src[4 * (jv * ART_SIZE + ju) + ch] * fu) * fv;
            if (c[3] < 1) continue;
            uint8_t *p = pix + 4 * ((size_t)y * pw + x);
            float sa = c[3] / 255.0f * a, lift = bright ? 1.35f : 1.0f;
            for (int ch = 0; ch < 3; ch++) {
                float o = fminf(c[ch] * a * lift, 255.0f * sa) + p[ch] * (1 - sa);
                p[ch] = (uint8_t)(o > 255 ? 255 : o);
            }
            p[3] = (uint8_t)(255 * sa + p[3] * (1 - sa));
        }
}
static void rect(float x0, float y0, float x1, float y1, uint32_t rgb, float a) {
    for (int y = (int)y0; y < (int)y1; y++) for (int x = (int)x0; x < (int)x1; x++) blend(x, y, rgb, a);
}
/* text centred on (cx, cy), with a dark drop shadow like the game's menu text */
static void text(const char *s, float cx, float cy, float px, uint32_t rgb, float a) {
    int n = (int)strlen(s);
    for (int pass = 0; pass < 2; pass++) {
        float off = pass ? 0 : px * 0.6f, x = cx - (float)n * 6 * px / 2 + off, y = cy - 7 * px / 2 + off;
        uint32_t col = pass ? rgb : 0x200000u;
        for (int i = 0; i < n; i++, x += 6 * px) {
            const uint8_t *g = font5x7[glyph(s[i])];
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++)
                    if (g[r] & (16 >> c)) rect(x + c * px, y + r * px, x + (c + 1) * px, y + (r + 1) * px, col, a);
        }
    }
}
static void text_left(const char *s, float x, float cy, float px, uint32_t rgb) {
    text(s, x + (float)strlen(s) * 6 * px / 2, cy, px, rgb, 1.0f);
}
/* the panel: the game's pause board (dark red, a darker rim) */
static void draw_menu(int w, int h) {
    const uint32_t RED = 0x780000u, RIM = 0x3A0000u, ORANGE = 0xF89018u, WHITE = 0xF8F8F8u, FIELD = 0x5A0000u;
    rect(0, 0, (float)w, (float)h, 0x000000u, 0.45f);                         /* the game behind, dimmed */
    float X0 = panel.x0 * w, Y0 = panel.y0 * h, X1 = panel.x1 * w, Y1 = panel.y1 * h, e = h / 120.0f;
    rect(X0 - e, Y0 - e, X1 + e, Y1 + e, RIM, 1.0f);
    rect(X0, Y0, X1, Y1, RED, 1.0f);
    float big = fmaxf(2.0f, (float)h / 140.0f), px = fmaxf(2.0f, (float)h / 200.0f);
    text(word(T_TITLE), (X0 + X1) / 2, Y0 + 0.085f * h, big, ORANGE, 1.0f);
    for (int i = 0; i < NROWS; i++) {
        Rect r = row_rect(i);
        float cy = (r.y0 + r.y1) / 2 * h;
        if (i == NROWS - 1) {                                                   /* close */
            text(word(T_CLOSE), (X0 + X1) / 2, cy + 0.02f * h, big, WHITE, 1.0f);
            continue;
        }
        rect(r.x0 * w, r.y0 * h, r.x1 * w, r.y1 * h, FIELD, 0.85f);
        text_left(word(T_CONTROLS + i), r.x0 * w + 0.015f * w, cy, px, ORANGE);
        char v[40];
        snprintf(v, sizeof v, "<  %s  >", row_value(i));
        text(v, (r.x1 - 0.11f) * w, cy, px, WHITE, 1.0f);
    }
}

/* The layer's picture for a w x h screen; NULL if there is nothing to draw. *changed: it differs from last time. */
const uint8_t *touch_picture(int w, int h, int *changed);
const uint8_t *touch_picture(int w, int h, int *changed) {
    *changed = 0;
    if (!active) return NULL;
    pthread_mutex_lock(&lk);
    if (w != pw || h != ph) { free(pix); pix = calloc((size_t)w * h, 4); pw = w; ph = h; dirty = 1; }
    static uint32_t frames, t_last;
    uint32_t now = SDL_GetTicks();
    frames++;
    if (now - t_last >= 500) {                                  /* twice a second: the fps text, a pad plugged or not */
        if (set.showfps) { snprintf(fps_text, sizeof fps_text, "%d FPS", (int)(frames * 1000.0f / (float)(now - t_last) + 0.5f)); dirty = 1; }
        int ph_now = set.padhides && SDL_HasGamepad();
        if (ph_now != pad_hidden) { pad_hidden = ph_now; dirty = 1; }
        frames = 0; t_last = now;
    }
    if (!dirty) { pthread_mutex_unlock(&lk); return pix; }
    if (pix_has) memset(pix, 0, (size_t)w * h * 4);
    pix_has = 0;
    nquads = 0;
    float alpha = set.opacity == 0 ? 0.5f : set.opacity == 2 ? 1.0f : 0.75f, s = scale();
    if (menu_open) draw_menu(w, h);
    if (editing) {                                                        /* moving the buttons */
        rect(0, 0, (float)w, (float)h, 0x000000u, 0.35f);
        text(word(T_DRAG), w * 0.5f, h * 0.08f, fmaxf(2.0f, (float)h / 170.0f), 0xF8F8F8u, 1.0f);
        const Rect *rs[2] = {&edit_reset, &edit_done};
        for (int i = 0; i < 2; i++) {
            const Rect *r = rs[i];
            rect(r->x0 * w - h / 160.0f, r->y0 * h - h / 160.0f, r->x1 * w + h / 160.0f, r->y1 * h + h / 160.0f, 0x3A0000u, 1.0f);
            rect(r->x0 * w, r->y0 * h, r->x1 * w, r->y1 * h, 0x780000u, 1.0f);
            text(word(i ? T_DONE : T_RESET), (r->x0 + r->x1) / 2 * w, (r->y0 + r->y1) / 2 * h, fmaxf(2.0f, (float)h / 200.0f), i ? 0xF8F8F8u : 0xF89018u, 1.0f);
        }
        if (set.stick) sprite(ART_STICK_BASE, bx[NB] * w, by[NB] * h, STICK_R * s * h, 1.0f, 0);
    }
    for (int i = 0; i < NB; i++) {
        Button *b = &buttons[i];
        if ((!set.on || pad_hidden) && !always_shown(b) && !editing) continue;
        if (menu_open && b->bit != B_MENU) continue;
        float cx = bx[i] * (float)w, cy = by[i] * (float)h, r = b->r * (always_shown(b) ? 1.0f : s) * (float)h;
        int down = (held & b->bit) || (b->bit == B_RUN && run_on) || (b->bit == B_MENU && menu_open);
        sprite(b->art, cx, cy, down ? r * 0.94f : r, always_shown(b) ? fmaxf(alpha, 0.75f) : alpha, down);
    }
    if (set.on && set.stick && !pad_hidden && !menu_open && !editing) {      /* the fixed stick, at rest */
        int held_now = 0;
        for (int k = 0; k < 10; k++) if (fingers[k].id && fingers[k].role == 1) held_now = 1;
        if (!held_now) {
            sprite(ART_STICK_BASE, bx[NB] * w, by[NB] * h, STICK_R * s * h, alpha, 0);
            sprite(ART_STICK_KNOB, bx[NB] * w, by[NB] * h, STICK_R * s * h * 0.42f, alpha, 0);
        }
    }
    if (set.showfps) text(fps_text, w * 0.035f, h * 0.955f, fmaxf(2.0f, (float)h / 280.0f), 0xF8F8F8u, 0.9f);
    for (int k = 0; k < 10 && set.on && !menu_open && !editing; k++)        /* the stick under the thumb */
        if (fingers[k].id && fingers[k].role == 1) {
            float r = 0.13f * s * (float)h;
            sprite(ART_STICK_BASE, fingers[k].x0 * (float)w, fingers[k].y0 * (float)h, r, alpha, 0);
            sprite(ART_STICK_KNOB, fingers[k].x * (float)w, fingers[k].y * (float)h, r * 0.42f, alpha, 0);
        }
    dirty = 0;
    *changed = 1;
    pthread_mutex_unlock(&lk);
    return pix;
}
