/* user32 (and the few gdi32/imm32 calls): the game's window as an SDL3 window, and its message loop.
 *
 * What it does: RegisterClassExW remembers the window procedure; CreateWindowExW opens an SDL window (Vulkan-capable)
 * and sends the game WM_CREATE; PeekMessageW/GetMessageW turn SDL events (quit, focus, size, keys) and posted
 * messages into Windows messages; DispatchMessageW calls the game's window procedure (lifted code). Window geometry,
 * styles, system metrics, message boxes (printed) and the rest answer as a 1920x1080 desktop would.
 * What it does not do: more than one window, child windows, or raw input (RegisterRawInputDevices is accepted, no
 * WM_INPUT is sent; keyboard arrives as WM_KEYDOWN/WM_KEYUP).
 */
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

uint32_t guest_call(CPU *c, uint32_t addr, int conv, int n, ...);

#define HWND_MAIN 0x00010010u
#define HWND_DESKTOP 0x00010004u
SDL_Window *g_window;
static uint32_t g_wndproc, g_style, g_exstyle, g_userdata;
static int g_w = 1280, g_h = 720;
static char g_class[64][128];
static uint32_t g_classproc[64], nclasses;

typedef struct { uint32_t hwnd, msg, wp, lp; } Msg;
static Msg queue[256];
static int qhead, qtail;
static int quit_posted;
volatile int g_quitting;                                       /* the player closed the window (process.c) */
static uint8_t keys[256];

static void post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    int n = (qtail + 1) % 256;
    if (n == qhead) return;
    queue[qtail] = (Msg){hwnd, msg, wp, lp};
    qtail = n;
}

uint32_t u32_RegisterClassExW(CPU *c, uint32_t a) {
    uint32_t wc = ARG(0);
    if (nclasses >= 64) return 0;
    uint32_t name = R32(wc + 40);
    if (name < 0x10000u) snprintf(g_class[nclasses], 128, "#%u", name);
    else w2utf8(name, g_class[nclasses], 128);
    g_classproc[nclasses] = R32(wc + 8);
    usf_log("RegisterClassExW(%s) wndproc %08x\n", g_class[nclasses], g_classproc[nclasses]);
    (void)c;
    return 0xC100u + nclasses++;
}

static int sdl_ready;
static void sdl_init(void) {
    if (sdl_ready) return;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) hle_fatal("SDL_Init: %s", SDL_GetError());
    sdl_ready = 1;
}

/* USF_AUTO_RES (always on Android): the game's resolution follows the screen's shape at 720 lines (a 19.5:9 phone:
 * 1560x720), so the picture fills the screen and the ultrawide code gives the extra width. 0 if not asked. */
int usf_auto_resolution(uint32_t *w, uint32_t *h);
int usf_auto_resolution(uint32_t *w, uint32_t *h) {
#ifndef __ANDROID__
    if (!getenv("USF_AUTO_RES")) return 0;
#endif
    sdl_init();
    const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (!m || m->w <= 0 || m->h <= 0) return 0;
    uint32_t dw = (uint32_t)(m->w > m->h ? m->w : m->h), dh = (uint32_t)(m->w > m->h ? m->h : m->w);   /* landscape */
    *h = 720;
    *w = (uint32_t)((uint64_t)720 * dw / dh + 7) & ~7u;
    if (*w < 1280) *w = 1280;
    return 1;
}

uint32_t u32_CreateWindowExW(CPU *c, uint32_t a) {
    char cls[128], title[256];
    if (ARG(1) < 0x10000u) snprintf(cls, sizeof cls, "#%u", ARG(1)); else w2utf8(ARG(1), cls, sizeof cls);
    if (ARG(2)) w2utf8(ARG(2), title, sizeof title); else title[0] = 0;
    for (uint32_t i = 0; i < nclasses; i++) if (!strcmp(g_class[i], cls)) g_wndproc = g_classproc[i];
    g_exstyle = ARG(0);
    g_style = ARG(3);
    int w = (int32_t)ARG(6), h = (int32_t)ARG(7);
    if (w > 0 && w < 8000 && h > 0 && h < 8000) { g_w = w; g_h = h; }
    sdl_init();
    char full[300];
    snprintf(full, sizeof full, "%s (native)", title[0] ? title : "Rayman Origins");
#ifdef __ANDROID__
    g_window = SDL_CreateWindow(full, g_w, g_h, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIDDEN);   /* immersive */
#else
    g_window = SDL_CreateWindow(full, g_w, g_h, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
#endif
    if (!g_window) hle_fatal("SDL_CreateWindow: %s", SDL_GetError());
    fprintf(stderr, "usf: CreateWindowExW class '%s' title '%s' %dx%d style %08x wndproc %08x\n", cls, title, g_w, g_h,
            g_style, g_wndproc);
    if (g_wndproc) {
        uint32_t cs = gcalloc(48);                             /* CREATESTRUCTW */
        W32(cs, ARG(11)); W32(cs + 4, ARG(9)); W32(cs + 20, (uint32_t)g_h); W32(cs + 16, (uint32_t)g_w);
        W32(cs + 32, g_style); W32(cs + 36, ARG(2)); W32(cs + 40, ARG(1)); W32(cs + 44, g_exstyle);
        guest_call(c, g_wndproc, CONV_STDCALL, 4, HWND_MAIN, 0x0081u, 0u, cs);   /* WM_NCCREATE */
        guest_call(c, g_wndproc, CONV_STDCALL, 4, HWND_MAIN, 0x0001u, 0u, cs);   /* WM_CREATE */
        guest_call(c, g_wndproc, CONV_STDCALL, 4, HWND_MAIN, 0x0005u, 0u, (uint32_t)(g_w | g_h << 16));   /* WM_SIZE */
    }
    return HWND_MAIN;
}

uint32_t u32_ShowWindow(CPU *c, uint32_t a) {
    (void)c;
    if (g_window && ARG(1)) {
        if (!g_headless) SDL_ShowWindow(g_window);
        post(HWND_MAIN, 0x001Cu, 1u, 0u);                     /* WM_ACTIVATEAPP */
        post(HWND_MAIN, 0x0006u, 1u, 0u);                     /* WM_ACTIVATE */
        post(HWND_MAIN, 0x0007u, 0u, 0u);                     /* WM_SETFOCUS: the game reads input only with focus */
    }
    return 0;
}
uint32_t u32_DestroyWindow(CPU *c, uint32_t a) {
    (void)c; (void)a;
    if (g_window) SDL_HideWindow(g_window);
    post(HWND_MAIN, 0x0002u, 0, 0);                            /* WM_DESTROY */
    return 1;
}

static uint32_t vk_of(SDL_Keycode k) {
    if (k >= 'a' && k <= 'z') return (uint32_t)(k - 'a' + 'A');
    if (k >= '0' && k <= '9') return (uint32_t)k;
    switch (k) {
    case SDLK_RETURN: return 0x0D; case SDLK_ESCAPE: return 0x1B; case SDLK_SPACE: return 0x20;
    case SDLK_BACKSPACE: return 0x08; case SDLK_TAB: return 0x09;
    case SDLK_LEFT: return 0x25; case SDLK_UP: return 0x26; case SDLK_RIGHT: return 0x27; case SDLK_DOWN: return 0x28;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10; case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
    case SDLK_LALT: case SDLK_RALT: return 0x12;
    default:
        if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70u + (uint32_t)(k - SDLK_F1);
        return 0;
    }
}

/* ---- Raw Input (the game reads the keyboard from WM_INPUT) ---- */
typedef struct { uint16_t make, flags, vkey; uint32_t msg; } RawKey;
static RawKey raws[256];
static uint32_t raw_next;                                      /* HRAWINPUT = 0x00F00000 + slot */
uint8_t dik_of_sdl(int scancode);

static void raw_key(int scancode, uint32_t vk, int down) {
    uint8_t d = dik_of_sdl(scancode);
    if (!d) return;
    uint32_t slot = raw_next++ & 255;
    raws[slot].make = d & 0x7F;
    raws[slot].flags = (uint16_t)((down ? 0 : 1) | ((d & 0x80) ? 2 : 0));   /* RI_KEY_BREAK, RI_KEY_E0 */
    raws[slot].vkey = (uint16_t)vk;
    raws[slot].msg = down ? 0x0100u : 0x0101u;
    post(HWND_MAIN, 0x00FFu, 0, 0x00F00000u + slot);           /* WM_INPUT, RIM_INPUT */
}

/* USF_INPUT presses as key transitions (unattended tests) */
uint16_t usf_scripted_buttons(void);
static void scripted_keys(void) {
    static uint16_t prev;
    uint16_t b = usf_scripted_buttons();
    static const struct { uint16_t bit; int sc; uint32_t vk; } sm[] = {
        {0x0001, SDL_SCANCODE_UP, 0x26}, {0x0002, SDL_SCANCODE_DOWN, 0x28}, {0x0004, SDL_SCANCODE_LEFT, 0x25},
        {0x0008, SDL_SCANCODE_RIGHT, 0x27}, {0x0010, SDL_SCANCODE_RETURN, 0x0D}, {0x0020, SDL_SCANCODE_ESCAPE, 0x1B},
        {0x1000, SDL_SCANCODE_Z, 'Z'}, {0x2000, SDL_SCANCODE_X, 'X'}, {0x4000, SDL_SCANCODE_A, 'A'}, {0x8000, SDL_SCANCODE_S, 'S'}};
    for (size_t i = 0; i < sizeof sm / sizeof sm[0]; i++)
        if ((b ^ prev) & sm[i].bit) {
            int down = (b & sm[i].bit) != 0;
            raw_key(sm[i].sc, sm[i].vk, down);
            keys[sm[i].vk] = (uint8_t)down;
            post(HWND_MAIN, down ? 0x0100u : 0x0101u, sm[i].vk, 1u | (uint32_t)dik_of_sdl(sm[i].sc) << 16 | (down ? 0u : 0xC0000000u));
        }
    prev = b;
}

uint32_t u32_GetRawInputData(CPU *c, uint32_t a) {
    (void)c;
    uint32_t h = ARG(0), cmd = ARG(1), p = ARG(2), psize = ARG(3);
    if (h < 0x00F00000u || h > 0x00F000FFu) return 0xFFFFFFFFu;
    if (!p) { W32(psize, 32); return 0; }
    if (R32(psize) < 32) return 0xFFFFFFFFu;
    RawKey *k = &raws[h & 255];
    if (cmd == 0x10000005u) { W32(p, 1); W32(p + 4, 32); W32(p + 8, 0x00010001u); W32(p + 12, 0); return 16; }   /* RID_HEADER */
    W32(p, 1); W32(p + 4, 32); W32(p + 8, 0x00010001u); W32(p + 12, 0);    /* RIM_TYPEKEYBOARD, size, hDevice */
    W16(p + 16, k->make); W16(p + 18, k->flags); W16(p + 20, 0); W16(p + 22, k->vkey);
    W32(p + 24, k->msg); W32(p + 28, 0);
    return 32;
}

static void pump(void) {
    if (!sdl_ready) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: g_quitting = 1; post(HWND_MAIN, 0x0010u, 0, 0); break;   /* WM_CLOSE */
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            post(HWND_MAIN, 0x001Cu, 1, 0); post(HWND_MAIN, 0x0006u, 1, 0); post(HWND_MAIN, 0x0007u, 0, 0);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (getenv("USF_ALWAYS_FOCUS")) break;            /* unattended tests: keep reading input */
            post(HWND_MAIN, 0x001Cu, 0, 0); post(HWND_MAIN, 0x0006u, 0, 0); post(HWND_MAIN, 0x0008u, 0, 0);
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            g_w = e.window.data1; g_h = e.window.data2;
            post(HWND_MAIN, 0x0005u, 0, (uint32_t)(g_w | g_h << 16));
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            uint32_t vk = vk_of(e.key.key);
            if (!vk) break;
            int down = e.type == SDL_EVENT_KEY_DOWN;
            keys[vk] = (uint8_t)down;
            post(HWND_MAIN, down ? 0x0100u : 0x0101u, vk, 1u | (uint32_t)e.key.scancode << 16 | (down ? 0u : 0xC0000000u));
            if (!e.key.repeat) raw_key(e.key.scancode, vk, down);
            break;
        }
        }
    }
    scripted_keys();
}

static int take(uint32_t msgp, uint32_t remove) {
    if (qhead == qtail) {
        if (quit_posted == 1) {
            if (remove) quit_posted = 2;
            memset(HOST(msgp), 0, 28);
            W32(msgp + 4, 0x0012u);                            /* WM_QUIT */
            return 1;
        }
        return 0;
    }
    Msg m = queue[qhead];
    if (remove) qhead = (qhead + 1) % 256;
    W32(msgp, m.hwnd); W32(msgp + 4, m.msg); W32(msgp + 8, m.wp); W32(msgp + 12, m.lp);
    W32(msgp + 16, (uint32_t)SDL_GetTicks());
    return 1;
}

uint32_t u32_PeekMessageW(CPU *c, uint32_t a) { (void)c; pump(); return (uint32_t)take(ARG(0), ARG(4) & 1u); }
uint32_t u32_GetMessageW(CPU *c, uint32_t a) {
    (void)c;
    for (;;) {
        pump();
        if (take(ARG(0), 1)) return R32(ARG(0) + 4) != 0x0012u;
        SDL_Delay(1);
    }
}
uint32_t u32_TranslateMessage(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t u32_DispatchMessageW(CPU *c, uint32_t a) {
    uint32_t m = ARG(0);
    if (!g_wndproc || R32(m + 4) == 0x0012u) return 0;
    usf_log("DispatchMessage %04x wp %08x lp %08x\n", R32(m + 4), R32(m + 8), R32(m + 12));
    return guest_call(c, g_wndproc, CONV_STDCALL, 4, R32(m), R32(m + 4), R32(m + 8), R32(m + 12));
}
uint32_t u32_DefWindowProcW(CPU *c, uint32_t a) {
    (void)c;
    switch (ARG(1)) {
    case 0x0010u: u32_DestroyWindow(c, a); return 0;           /* WM_CLOSE */
    case 0x0081u: return 1;                                    /* WM_NCCREATE */
    default: return 0;
    }
}
uint32_t u32_PostQuitMessage(CPU *c, uint32_t a) { (void)c; (void)a; if (!quit_posted) quit_posted = 1; return 0; }
uint32_t u32_PostMessageW(CPU *c, uint32_t a) { (void)c; post(ARG(0), ARG(1), ARG(2), ARG(3)); return 1; }
uint32_t u32_PostThreadMessageW(CPU *c, uint32_t a) { (void)c; post(0, ARG(1), ARG(2), ARG(3)); return 1; }
uint32_t u32_GetQueueStatus(CPU *c, uint32_t a) { (void)c; (void)a; pump(); return qhead != qtail ? 0x00FF00FFu : 0; }

/* ---- geometry and state ---- */
static void rect(uint32_t p, int l, int t, int r, int b) { W32(p, (uint32_t)l); W32(p + 4, (uint32_t)t); W32(p + 8, (uint32_t)r); W32(p + 12, (uint32_t)b); }
uint32_t u32_GetClientRect(CPU *c, uint32_t a) { (void)c; rect(ARG(1), 0, 0, g_w, g_h); return 1; }
uint32_t u32_GetWindowRect(CPU *c, uint32_t a) {
    (void)c;
    if (ARG(0) == HWND_DESKTOP) rect(ARG(1), 0, 0, 1920, 1080); else rect(ARG(1), 100, 100, 100 + g_w, 100 + g_h);
    return 1;
}
uint32_t u32_AdjustWindowRect(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t u32_SetRect(CPU *c, uint32_t a) { (void)c; rect(ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4)); return 1; }
uint32_t u32_OffsetRect(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(0);
    rect(p, (int)(R32(p) + ARG(1)), (int)(R32(p + 4) + ARG(2)), (int)(R32(p + 8) + ARG(1)), (int)(R32(p + 12) + ARG(2)));
    return 1;
}
uint32_t u32_EqualRect(CPU *c, uint32_t a) { (void)c; return !memcmp(HOST(ARG(0)), HOST(ARG(1)), 16); }
uint32_t u32_SetWindowPos(CPU *c, uint32_t a) {
    (void)c;
    int w = (int)ARG(4), h = (int)ARG(5);
    if (g_window && !(ARG(6) & 1u) && w > 0 && h > 0) { g_w = w; g_h = h; SDL_SetWindowSize(g_window, w, h); }
    return 1;
}
uint32_t u32_GetWindowPlacement(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(1);
    memset(HOST(p + 4), 0, 40);
    W32(p + 8, 1);                                             /* SW_SHOWNORMAL */
    rect(p + 28, 100, 100, 100 + g_w, 100 + g_h);
    return 1;
}
uint32_t u32_SetWindowPlacement(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t u32_GetWindowLongW(CPU *c, uint32_t a) {
    (void)c;
    switch ((int32_t)ARG(1)) {
    case -4: return g_wndproc;
    case -16: return g_style;
    case -20: return g_exstyle;
    case -21: return g_userdata;
    default: return 0;
    }
}
uint32_t u32_SetWindowLongW(CPU *c, uint32_t a) {
    (void)c;
    uint32_t old = u32_GetWindowLongW(c, a);
    switch ((int32_t)ARG(1)) {
    case -4: g_wndproc = ARG(2); break;
    case -16: g_style = ARG(2); break;
    case -20: g_exstyle = ARG(2); break;
    case -21: g_userdata = ARG(2); break;
    }
    return old;
}
uint32_t u32_GetSystemMetrics(CPU *c, uint32_t a) {
    (void)c;
    switch (ARG(0)) {
    case 0: case 16: case 78: return 1920;                     /* SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN */
    case 1: case 17: case 79: return 1080;
    case 80: return 1;                                         /* SM_CMONITORS */
    default: return 0;
    }
}
uint32_t u32_SystemParametersInfoW(CPU *c, uint32_t a) {
    (void)c;
    if (ARG(0) == 0x30u && ARG(2)) rect(ARG(2), 0, 0, 1920, 1040);   /* SPI_GETWORKAREA */
    return 1;
}
uint32_t u32_GetForegroundWindow(CPU *c, uint32_t a) { (void)c; (void)a; return HWND_MAIN; }
uint32_t u32_GetDesktopWindow(CPU *c, uint32_t a) { (void)c; (void)a; return HWND_DESKTOP; }
uint32_t u32_FindWindowW(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t u32_IsWindowVisible(CPU *c, uint32_t a) { (void)c; (void)a; return g_window != NULL; }
uint32_t u32_IsIconic(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t u32_SetFocus(CPU *c, uint32_t a) { (void)c; (void)a; return HWND_MAIN; }
uint32_t u32_SetForegroundWindow(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t u32_SetCapture(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t u32_ReleaseCapture(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t u32_LoadIconW(CPU *c, uint32_t a) { (void)c; (void)a; return 0x00020001u; }
uint32_t u32_LoadImageW(CPU *c, uint32_t a) { (void)c; (void)a; return 0x00020003u; }
uint32_t u32_SetCursor(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t u32_GetCursorPos(CPU *c, uint32_t a) { (void)c; W32(ARG(0), 0); W32(ARG(0) + 4, 0); return 1; }
uint32_t u32_SetTimer(CPU *c, uint32_t a) { (void)c; return ARG(1) ? ARG(1) : 1; }
uint32_t u32_KillTimer(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t u32_RegisterWindowMessageW(CPU *c, uint32_t a) { static uint32_t n = 0xC000; (void)c; (void)a; return n++; }
uint32_t u32_GetAsyncKeyState(CPU *c, uint32_t a) { (void)c; return ARG(0) < 256 && keys[ARG(0)] ? 0x8000u : 0u; }
uint32_t u32_SetKeyboardState(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t u32_GetKeyboardLayout(CPU *c, uint32_t a) { (void)c; (void)a; return 0x04090409u; }
uint32_t u32_RegisterRawInputDevices(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t u32_GetRawInputDeviceList(CPU *c, uint32_t a) { (void)c; if (ARG(1)) W32(ARG(1), 0); return 0; }

static uint32_t message_box(const char *text, const char *title) {
    fprintf(stderr, "usf: MessageBox [%s] %s\n", title, text);
    if (sdl_ready) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, text, g_window);
    return 1;                                                  /* IDOK */
}
uint32_t u32_MessageBoxW(CPU *c, uint32_t a) {
    (void)c;
    char text[2048], title[256];
    w2utf8(ARG(1), text, sizeof text);
    if (ARG(2)) w2utf8(ARG(2), title, sizeof title); else snprintf(title, sizeof title, "Rayman Origins");
    return message_box(text, title);
}
uint32_t u32_MessageBoxA(CPU *c, uint32_t a) { (void)c; return message_box(STR(ARG(1)), ARG(2) ? STR(ARG(2)) : "Rayman Origins"); }

uint32_t x_GetStockObject(CPU *c, uint32_t a) { (void)c; return 0x00030000u | ARG(0); }
uint32_t x_ImmGetContext(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_ImmReleaseContext(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t x_ImmGetCompositionStringW(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }

/* wsprintfW (cdecl): %s is a wide string here; %d %u %x %X %c %%, with width / zero padding */
uint32_t x_wsprintfW(CPU *c, uint32_t a) {
    (void)c;
    uint32_t out = ARG(0), fmt = ARG(1), argp = a + 8u;
    char buf[2048];
    size_t o = 0;
    for (uint32_t i = 0;; i++) {
        uint32_t ch = R16(fmt + 2u * i);
        if (!ch || o > sizeof buf - 64) break;
        if (ch != '%') { buf[o++] = (char)ch; continue; }
        char spec[16] = "%";
        int k = 1;
        uint32_t t;
        while ((t = R16(fmt + 2u * ++i)) && strchr("-0123456789l", (int)t) && k < 12) if (t != 'l') spec[k++] = (char)t;
        if (t == 's' || t == 'S') {
            char s[1024];
            w2utf8(R32(argp), s, sizeof s);
            argp += 4;
            spec[k++] = 's'; spec[k] = 0;
            o += (size_t)snprintf(buf + o, sizeof buf - o, spec, s);
        } else if (t == '%') {
            buf[o++] = '%';
        } else if (t) {
            spec[k++] = (char)t; spec[k] = 0;
            o += (size_t)snprintf(buf + o, sizeof buf - o, spec, R32(argp));
            argp += 4;
        } else break;
    }
    buf[o] = 0;
    utf8_to_w(buf, out, 1024);
    return (uint32_t)wlen(out);
}
