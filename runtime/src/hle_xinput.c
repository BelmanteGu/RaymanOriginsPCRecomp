/* XInput for the game: physical controllers through SDL3, and the keyboard as a virtual pad on player 1.
 *
 * What it does: XInputGetState fills XINPUT_STATE from the SDL gamepad of that index; player 1 also gets the
 * keyboard: arrows / WASD = d-pad, U I O = LP MP HP (X, Y, RB), J K L = LK MK HK (A, B, RT), Enter = Start,
 * Backspace / Esc = Back, Q = LB (3 punches), E = LT (3 kicks). XInputGetCapabilities reports a standard gamepad.
 * Street Fighter IV's default pad layout maps X/Y/RB to light/medium/heavy punch and A/B/RT to the kicks.
 * What it does not do: rumble (XInputSetState is accepted and ignored), more than 4 pads.
 */
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

static SDL_Gamepad *pads[4];
static uint32_t packet;

static void open_pads(void) {
    static int inited;
    if (!inited) { SDL_InitSubSystem(SDL_INIT_GAMEPAD); inited = 1; }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < 4; i++) {
        if (pads[i] && !SDL_GamepadConnected(pads[i])) { SDL_CloseGamepad(pads[i]); pads[i] = NULL; }
        if (!pads[i] && i < n) pads[i] = SDL_OpenGamepad(ids[i]);
    }
    SDL_free(ids);
}

static int16_t axis(SDL_Gamepad *g, SDL_GamepadAxis a, int invert) {
    int v = SDL_GetGamepadAxis(g, a);
    if (invert) v = -v - 1;
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* USF_INPUT="600-610:S;700-705:A;..." presses buttons on pad 0 during frame ranges (unattended tests).
 * Letters: U D L R (d-pad), S start, K back, A B X Y, 1 = LB, 2 = RB, 3 = LT, 4 = RT. */
#include "d3d9/d3d9.h"
static uint16_t scripted(uint8_t *lt, uint8_t *rt);
uint16_t touch_buttons(void);                                  /* the on-screen controls (touch.c) */
uint16_t usf_scripted_buttons(void) { uint8_t l = 0, r = 0; return scripted(&l, &r) | touch_buttons(); }
static uint16_t scripted(uint8_t *lt, uint8_t *rt) {
    const char *sc = getenv("USF_INPUT");
    if (!sc) return 0;
    uint16_t b = 0;
    for (const char *p = sc; *p;) {
        unsigned f1 = 0, f2 = 0;
        char keys[32] = {0};
        if (sscanf(p, "%u-%u:%31[^;]", &f1, &f2, keys) == 3 && g_dev.frames >= f1 && g_dev.frames <= f2)
            for (char *k = keys; *k; k++)
                switch (*k) {
                case 'U': b |= 0x0001; break; case 'D': b |= 0x0002; break; case 'L': b |= 0x0004; break;
                case 'R': b |= 0x0008; break; case 'S': b |= 0x0010; break; case 'K': b |= 0x0020; break;
                case 'A': b |= 0x1000; break; case 'B': b |= 0x2000; break; case 'X': b |= 0x4000; break;
                case 'Y': b |= 0x8000; break; case '1': b |= 0x0100; break; case '2': b |= 0x0200; break;
                case '3': *lt = 255; break; case '4': *rt = 255; break;
                }
        p = strchr(p, ';');
        if (!p) break;
        p++;
    }
    return b;
}

uint32_t x_XInputGetState(CPU *c, uint32_t a) {
    (void)c;
    uint32_t idx = ARG(0), st = ARG(1);
    if (idx > 3) return 1167;
    open_pads();
    uint16_t b = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    int connected = 0;
    SDL_Gamepad *g = pads[idx];
    if (g) {
        connected = 1;
        static const struct { SDL_GamepadButton s; uint16_t x; } map[] = {
            {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
            {SDL_GAMEPAD_BUTTON_START, 0x0010}, {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
            {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000}, {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
            {SDL_GAMEPAD_BUTTON_WEST, 0x4000}, {SDL_GAMEPAD_BUTTON_NORTH, 0x8000}};
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (SDL_GetGamepadButton(g, map[i].s)) b |= map[i].x;
        lt = (uint8_t)(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
        rt = (uint8_t)(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
        lx = axis(g, SDL_GAMEPAD_AXIS_LEFTX, 0); ly = axis(g, SDL_GAMEPAD_AXIS_LEFTY, 1);
        rx = axis(g, SDL_GAMEPAD_AXIS_RIGHTX, 0); ry = axis(g, SDL_GAMEPAD_AXIS_RIGHTY, 1);
    }
    if (idx == 0) {                                            /* the keyboard is player 1 */
        connected = 1;
        const bool *k = SDL_GetKeyboardState(NULL);
        if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]) b |= 0x0001;
        if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]) b |= 0x0002;
        if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]) b |= 0x0004;
        if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) b |= 0x0008;
        if (k[SDL_SCANCODE_RETURN]) b |= 0x0010;
        if (k[SDL_SCANCODE_BACKSPACE] || k[SDL_SCANCODE_ESCAPE]) b |= 0x0020;
        if (k[SDL_SCANCODE_U]) b |= 0x4000;                    /* LP = X */
        if (k[SDL_SCANCODE_I]) b |= 0x8000;                    /* MP = Y */
        if (k[SDL_SCANCODE_O]) b |= 0x0200;                    /* HP = RB */
        if (k[SDL_SCANCODE_J]) b |= 0x1000;                    /* LK = A */
        if (k[SDL_SCANCODE_K]) b |= 0x2000;                    /* MK = B */
        if (k[SDL_SCANCODE_L]) rt = 255;                       /* HK = RT */
        if (k[SDL_SCANCODE_Q]) b |= 0x0100;                    /* 3P = LB */
        if (k[SDL_SCANCODE_E]) lt = 255;                       /* 3K = LT */
        b |= scripted(&lt, &rt);
    }
    if (!connected) return 1167;                               /* ERROR_DEVICE_NOT_CONNECTED */
    W32(st, ++packet);
    W16(st + 4, b); W8(st + 6, lt); W8(st + 7, rt);
    W16(st + 8, (uint16_t)lx); W16(st + 10, (uint16_t)ly); W16(st + 12, (uint16_t)rx); W16(st + 14, (uint16_t)ry);
    return 0;
}

uint32_t x_XInputGetCapabilities(CPU *c, uint32_t a) {
    (void)c;
    if (ARG(0) > 3) return 1167;
    open_pads();
    if (ARG(0) != 0 && !pads[ARG(0)]) return 1167;
    uint32_t p = ARG(2);
    memset(HOST(p), 0, 20);
    W8(p, 1);                                                  /* XINPUT_DEVTYPE_GAMEPAD */
    W8(p + 1, 1);                                              /* XINPUT_DEVSUBTYPE_GAMEPAD */
    W16(p + 4, 0xF3FF);
    W8(p + 6, 255); W8(p + 7, 255);
    return 0;
}

uint32_t x_XInputSetState(CPU *c, uint32_t a) { (void)c; return ARG(0) <= 3 ? 0 : 1167; }
