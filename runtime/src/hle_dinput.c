/* DirectInput 8: the keyboard (the game reads it through IDirectInputDevice8::GetDeviceState).
 *
 * What it does: DirectInput8Create gives an IDirectInput8 that enumerates the system keyboard and creates its device;
 * GetDeviceState fills the 256-byte DIK table from SDL's keyboard state (SDL scancodes are USB HID codes, mapped to
 * the PC set-1 codes DirectInput uses). USF_INPUT (see hle_xinput.c) also presses keys here for unattended tests.
 * Game controllers are not enumerated through DirectInput; pads go through XInput (hle_xinput.c).
 * What it does not do: buffered data (GetDeviceData returns no events), mice, force feedback.
 */
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

uint32_t guest_call(CPU *c, uint32_t addr, int conv, int n, ...);
uint16_t usf_scripted_buttons(void);

static uint32_t di_obj, kb_obj;

static uint32_t ok(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
static uint32_t one(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
static uint32_t qi(CPU *c, uint32_t a) { (void)c; W32(ARG(2), ARG(0)); return 0; }

/* SDL scancode (USB HID usage) -> DIK (set 1) */
uint8_t dik_of_sdl(int s);
uint8_t dik_of_sdl(int s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        static const uint8_t az[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                       0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
        return az[s - SDL_SCANCODE_A];
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_0) return (uint8_t)(0x02 + (s - SDL_SCANCODE_1));
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return (uint8_t)(0x3B + (s - SDL_SCANCODE_F1));
    switch (s) {
    case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_ESCAPE: return 0x01; case SDL_SCANCODE_BACKSPACE: return 0x0E;
    case SDL_SCANCODE_TAB: return 0x0F; case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_MINUS: return 0x0C;
    case SDL_SCANCODE_EQUALS: return 0x0D; case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_BACKSLASH: return 0x2B; case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_LCTRL: return 0x1D; case SDL_SCANCODE_RCTRL: return 0x9D; case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_RALT: return 0xB8; case SDL_SCANCODE_UP: return 0xC8; case SDL_SCANCODE_DOWN: return 0xD0;
    case SDL_SCANCODE_LEFT: return 0xCB; case SDL_SCANCODE_RIGHT: return 0xCD; case SDL_SCANCODE_HOME: return 0xC7;
    case SDL_SCANCODE_END: return 0xCF; case SDL_SCANCODE_PAGEUP: return 0xC9; case SDL_SCANCODE_PAGEDOWN: return 0xD1;
    case SDL_SCANCODE_INSERT: return 0xD2; case SDL_SCANCODE_DELETE: return 0xD3; case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_KP_0: return 0x52; case SDL_SCANCODE_KP_1: return 0x4F; case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_KP_3: return 0x51; case SDL_SCANCODE_KP_4: return 0x4B; case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_KP_6: return 0x4D; case SDL_SCANCODE_KP_7: return 0x47; case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_KP_9: return 0x49; case SDL_SCANCODE_KP_ENTER: return 0x9C;
    default: return 0;
    }
}

static uint32_t kb_state(CPU *c, uint32_t a) {
    (void)c;
    uint32_t n = ARG(1), p = ARG(2);
    memset(HOST(p), 0, n);
    int nk = 0;
    const bool *k = SDL_GetKeyboardState(&nk);
    for (int s = 0; s < nk; s++) {
        uint8_t d = k[s] ? dik_of_sdl(s) : 0;
        if (d && d < n) W8(p + d, 0x80);
    }
    uint16_t b = usf_scripted_buttons();                      /* USF_INPUT: the same letters, as keys */
    static const struct { uint16_t bit; uint8_t dik; } sm[] = {{0x0001, 0xC8}, {0x0002, 0xD0}, {0x0004, 0xCB},
        {0x0008, 0xCD}, {0x0010, 0x1C}, {0x0020, 0x01}, {0x1000, 0x2C}, {0x2000, 0x2D}, {0x4000, 0x1E}, {0x8000, 0x1F}};
    for (size_t i = 0; i < sizeof sm / sizeof sm[0]; i++) if ((b & sm[i].bit) && sm[i].dik < n) W8(p + sm[i].dik, 0x80);
    return 0;
}
static uint32_t kb_data(CPU *c, uint32_t a) { (void)c; if (ARG(3)) W32(ARG(3), 0); return 0; }   /* no buffered events */
static uint32_t kb_caps(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(1);
    W32(p + 4, 0x7); W32(p + 8, 0x0413);                       /* attached | polled; DI8DEVTYPE_KEYBOARD | PCENH */
    W32(p + 16, 128);                                          /* buttons */
    return 0;
}
static uint32_t kb_info(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(1);
    memset(HOST(p + 4), 0, R32(p) > 4 ? R32(p) - 4 : 0);
    W32(p + 36, 0x0413);
    return 0;
}
static HleFn vt_kb[32] = {qi, one, one, kb_caps, ok, ok, ok, ok, ok, kb_state, kb_data, ok, ok, ok, ok, kb_info, ok, ok,
                          ok, ok, ok, ok, ok, ok, ok, ok, ok, ok, ok, ok, ok, ok};
static const uint8_t na_kb[32] = {3, 1, 1, 2, 4, 3, 3, 1, 1, 3, 5, 2, 2, 3, 4, 2, 3, 4, 5, 4, 3, 2, 2, 4, 2, 1, 5, 5, 5,
                                  4, 4, 2};

/* ---- a game controller (an Xbox 360-style pad as DirectInput shows it): SDL pad 0 + the keyboard ---- */
static uint32_t pad_obj;
static int32_t ax_min = 0, ax_max = 65535;
static uint32_t pad_data_size = 272;                        /* DIJOYSTATE2 unless SetDataFormat says DIJOYSTATE */
static SDL_Gamepad *sdlpad;

static int32_t scale(int v) {                               /* -32768..32767 -> the range the game asked for */
    return (int32_t)(ax_min + (int64_t)(v + 32768) * (ax_max - ax_min) / 65535);
}

static uint32_t pad_state(CPU *c, uint32_t a) {
    (void)c;
    uint32_t n = ARG(1), p = ARG(2);
    memset(HOST(p), 0, n);
    if (!sdlpad) { int k = 0; SDL_JoystickID *ids = SDL_GetGamepads(&k); if (k > 0) sdlpad = SDL_OpenGamepad(ids[0]); SDL_free(ids); }
    int lx = 0, ly = 0, rx = 0, ry = 0, z = 0;
    uint8_t btn[16] = {0};
    int up = 0, down = 0, left = 0, right = 0;
    if (sdlpad) {
        lx = SDL_GetGamepadAxis(sdlpad, SDL_GAMEPAD_AXIS_LEFTX); ly = SDL_GetGamepadAxis(sdlpad, SDL_GAMEPAD_AXIS_LEFTY);
        rx = SDL_GetGamepadAxis(sdlpad, SDL_GAMEPAD_AXIS_RIGHTX); ry = SDL_GetGamepadAxis(sdlpad, SDL_GAMEPAD_AXIS_RIGHTY);
        z = (SDL_GetGamepadAxis(sdlpad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) - SDL_GetGamepadAxis(sdlpad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) / 2;
        static const SDL_GamepadButton order[10] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
            SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
            SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
        for (int i = 0; i < 10; i++) btn[i] = SDL_GetGamepadButton(sdlpad, order[i]);
        up = SDL_GetGamepadButton(sdlpad, SDL_GAMEPAD_BUTTON_DPAD_UP); down = SDL_GetGamepadButton(sdlpad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        left = SDL_GetGamepadButton(sdlpad, SDL_GAMEPAD_BUTTON_DPAD_LEFT); right = SDL_GetGamepadButton(sdlpad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    }
    /* Rayman reads the keyboard itself (GetKeyState): the pad is the physical gamepad only, plus USF_INPUT for tests */
    uint16_t sb = usf_scripted_buttons();
    up |= (sb & 1) != 0; down |= (sb & 2) != 0; left |= (sb & 4) != 0; right |= (sb & 8) != 0;
    btn[0] |= (sb & 0x1000) != 0; btn[1] |= (sb & 0x2000) != 0; btn[2] |= (sb & 0x4000) != 0; btn[3] |= (sb & 0x8000) != 0;
    btn[4] |= (sb & 0x0100) != 0; btn[5] |= (sb & 0x0200) != 0;   /* LB, RB: run */
    btn[6] |= (sb & 0x20) != 0; btn[7] |= (sb & 0x0410) != 0;
    if (getenv("USF_PADLOG")) {
        static char last[128]; char now[128];
        snprintf(now, sizeof now, "btn %d%d%d%d%d%d%d%d%d%d dpad %d%d%d%d lx %d ly %d", btn[0], btn[1], btn[2], btn[3], btn[4], btn[5],
                 btn[6], btn[7], btn[8], btn[9], up, down, left, right, lx / 8192, ly / 8192);
        if (strcmp(now, last)) { fprintf(stderr, "pad: DI sdl=%p n=%u %s\n", (void *)sdlpad, n, now); strcpy(last, now); }
    }
    int32_t v[6] = {scale(lx), scale(ly), scale(z), scale(rx), scale(ry), scale(0)};
    for (int i = 0; i < 6 && 4u * (uint32_t)i + 4 <= n; i++) W32(p + 4u * (uint32_t)i, (uint32_t)v[i]);
    int pov = -1;                                          /* hundredths of a degree, clockwise from up */
    if (up) pov = right ? 4500 : left ? 31500 : 0;
    else if (down) pov = right ? 13500 : left ? 22500 : 18000;
    else if (right) pov = 9000;
    else if (left) pov = 27000;
    if (n >= 36) W32(p + 32, (uint32_t)pov);
    for (int i = 1; i < 4 && n >= 36 + 4u * i; i++) W32(p + 32 + 4u * (uint32_t)i, 0xFFFFFFFFu);
    for (int i = 0; i < 16 && 48u + (uint32_t)i < n; i++) W8(p + 48 + (uint32_t)i, btn[i] ? 0x80 : 0);
    return 0;
}
static uint32_t pad_setformat(CPU *c, uint32_t a) { (void)c; pad_data_size = R32(ARG(1) + 12); return 0; }
static uint32_t pad_setprop(CPU *c, uint32_t a) {
    (void)c;
    if (ARG(1) == 4u) { ax_min = (int32_t)R32(ARG(2) + 16); ax_max = (int32_t)R32(ARG(2) + 20); }   /* DIPROP_RANGE */
    return 0;
}
static uint32_t pad_caps(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(1);
    W32(p + 4, 0x5); W32(p + 8, 0x0215);                   /* attached | polled; DI8DEVTYPE_GAMEPAD */
    W32(p + 12, 6); W32(p + 16, 10); W32(p + 20, 1);       /* axes, buttons, POVs */
    return 0;
}
static uint32_t pad_info(CPU *c, uint32_t a) {
    (void)c;
    uint32_t p = ARG(1);
    memset(HOST(p + 4), 0, R32(p) > 4 ? R32(p) - 4 : 0);
    W32(p + 36, 0x0215);
    return 0;
}
/* EnumObjects(callback, ref, flags): the axes, the POV and the buttons, as DIDEVICEOBJECTINSTANCEW (0x240 bytes) */
static uint32_t pad_enum_objects(CPU *c, uint32_t a) {
    uint32_t cb = ARG(1), ref = ARG(2), flags = ARG(3);
    static const struct { uint8_t guid; uint32_t ofs, type; const char *name; } objs[] = {
        {0xE0, 0, 0x2, "X Axis"}, {0xE1, 4, 0x2, "Y Axis"}, {0xE2, 8, 0x2, "Z Axis"}, {0xF4, 12, 0x2, "X Rotation"},
        {0xF5, 16, 0x2, "Y Rotation"}, {0xE3, 20, 0x2, "Z Rotation"}, {0xF2, 32, 0x10, "Hat Switch"}};
    uint32_t o = gcalloc(0x240);
    int axis_n = 0;
    for (int i = 0; i < 7 + 10; i++) {
        uint8_t g = i < 7 ? objs[i].guid : 0xF0;
        uint32_t ofs = i < 7 ? objs[i].ofs : 48u + (uint32_t)(i - 7), type = i < 7 ? objs[i].type : 0x4;
        if (flags && !(flags & type) && flags != 0) continue;       /* DIDFT_AXIS 3 / BUTTON 0xC / POV 0x10 */
        int inst = type == 0x2 ? axis_n++ : type == 0x10 ? 0 : i - 7;
        memset(HOST(o), 0, 0x240);
        W32(o, 0x240);
        static const uint8_t base[16] = {0, 0x02, 0x6D, 0xA3, 0xF3, 0xC9, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0};
        memcpy(HOST(o + 4), base, 16);
        W8(o + 4, g);                                               /* GUID_XAxis ... GUID_Button, GUID_POV */
        W32(o + 20, ofs);
        W32(o + 24, type | (uint32_t)inst << 8);
        char name[32];
        /* the game shows these names in its button hints: the attacks as on the touch pad and the arcade panel */
        static const char *bnames[10] = {"LK", "MK", "LP", "MP", "3P", "HP", "BACK", "START", "L3", "R3"};
        if (i < 7) snprintf(name, sizeof name, "%s", objs[i].name); else snprintf(name, sizeof name, "%s", bnames[i - 7]);
        utf8_to_w(name, o + 32, 260);
        if (guest_call(c, cb, CONV_STDCALL, 2, o, ref) == 0) break;  /* DIENUM_STOP */
    }
    gfree(o);
    return 0;
}
static HleFn vt_pad[32] = {qi, one, one, pad_caps, pad_enum_objects, ok, pad_setprop, ok, ok, pad_state, kb_data,
                           pad_setformat, ok, ok, ok, pad_info, ok, ok, ok, ok, ok, ok, ok, ok, ok, ok, ok, ok, ok, ok, ok, ok};
/* USF_PADLOG: log every pad method the game calls (debug) */
static uint32_t pad_trace(int slot, CPU *c, uint32_t a) {
    static unsigned cnt[32];
    if (getenv("USF_PADLOG") && cnt[slot]++ < 3) fprintf(stderr, "pad: DI slot %d (call %u)\n", slot, cnt[slot]);
    return vt_pad[slot](c, a);
}
#define T(n) static uint32_t tr##n(CPU *c, uint32_t a) { return pad_trace(n, c, a); }
T(0) T(1) T(2) T(3) T(4) T(5) T(6) T(7) T(8) T(9) T(10) T(11) T(12) T(13) T(14) T(15) T(16) T(17) T(18) T(19) T(20)
T(21) T(22) T(23) T(24) T(25) T(26) T(27) T(28) T(29) T(30) T(31)
#undef T
static HleFn vt_pad_tr[32] = {tr0, tr1, tr2, tr3, tr4, tr5, tr6, tr7, tr8, tr9, tr10, tr11, tr12, tr13, tr14, tr15, tr16,
                              tr17, tr18, tr19, tr20, tr21, tr22, tr23, tr24, tr25, tr26, tr27, tr28, tr29, tr30, tr31};
static const uint8_t PAD_GUID[16] = {'U', 'S', 'F', 'P', 'A', 'D', 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

static const uint8_t GUID_SysKeyboard[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45,
                                             0x53, 0x54, 0x00, 0x00};

static uint32_t create_device(CPU *c, uint32_t a) {
    (void)c;
    if (!memcmp(HOST(ARG(1)), PAD_GUID, 16)) {
        if (!pad_obj) pad_obj = obj_new_n("IDirectInputDevice8 (pad)", 32, getenv("USF_PADLOG") ? vt_pad_tr : vt_pad, na_kb, 16);
        W32(ARG(2), pad_obj);
        fprintf(stderr, "usf: DirectInput game controller created (keyboard + SDL pad)\n");
        return 0;
    }
    if (memcmp(HOST(ARG(1)), GUID_SysKeyboard, 16)) {
        usf_log("DirectInput CreateDevice: not the keyboard (%08x)\n", R32(ARG(1)));
        W32(ARG(2), 0);
        return 0x80040154u;
    }
    if (!kb_obj) kb_obj = obj_new_n("IDirectInputDevice8", 32, vt_kb, na_kb, 16);
    W32(ARG(2), kb_obj);
    return 0;
}

/* EnumDevices(type, callback, ref, flags): one game controller (DI8DEVCLASS_GAMECTRL 4), the keyboard (3) */
static uint32_t enum_devices(CPU *c, uint32_t a) {
    uint32_t type = ARG(1), cb = ARG(2), ref = ARG(3);
    if ((type == 0 || type == 4 || type == 0x15) && !getenv("USF_NO_DIPAD")) {
        uint32_t inst = gcalloc(0x44C);                           /* DIDEVICEINSTANCEW */
        W32(inst, 0x44C);
        memcpy(HOST(inst + 4), PAD_GUID, 16);
        static const uint8_t X360[16] = {0x5E, 0x04, 0x8E, 0x02, 0, 0, 0, 0, 0, 0, 'P', 'I', 'D', 'V', 'I', 'D'};
        memcpy(HOST(inst + 20), X360, 16);     /* guidProduct: the Xbox 360 pad WMI reports, so the game reads it as XInput */
        W32(inst + 36, 0x0215);
        utf8_to_w("Controller", inst + 40, 260);
        utf8_to_w("Controller", inst + 560, 260);
        W16(inst + 1096, 1); W16(inst + 1098, 5);              /* HID usage page generic desktop, usage game pad */
        guest_call(c, cb, CONV_STDCALL, 2, inst, ref);
        gfree(inst);
    }
    if (type != 0 && type != 3 && type != 0x13) return 0;
    uint32_t inst = gcalloc(0x24C);                             /* DIDEVICEINSTANCEA (wide: 0x354 is larger; both fit) */
    W32(inst, 0x244);
    memcpy(HOST(inst + 4), GUID_SysKeyboard, 16);
    memcpy(HOST(inst + 20), GUID_SysKeyboard, 16);
    W32(inst + 36, 0x0413);
    strcpy((char *)HOST(inst + 40), "Keyboard");
    strcpy((char *)HOST(inst + 300), "Keyboard");
    guest_call(c, cb, CONV_STDCALL, 2, inst, ref);
    gfree(inst);
    return 0;
}
static HleFn vt_di[11] = {qi, one, one, create_device, enum_devices, ok, ok, ok, ok, ok, ok};
static const uint8_t na_di[11] = {3, 1, 1, 4, 5, 2, 3, 3, 4, 6, 5};

uint32_t x_DirectInput8Create(CPU *c, uint32_t a) {
    (void)c;
    if (!di_obj) di_obj = obj_new_n("IDirectInput8", 11, vt_di, na_di, 16);
    W32(ARG(3), di_obj);
    return 0;
}
