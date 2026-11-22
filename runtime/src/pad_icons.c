/* Button icons in the game's texts instead of "[1-A Button]".
 *
 * Why: the game's texts name pad buttons with icon tags ("Press [icon:X360_BUTTON_A] to fire"), drawn as the pad's
 * button pictures (world/common/ui/ui_menu/consoles_buttons/x360). The PC build first rewrites every tag (0x7A72A0):
 * it looks for "[icon:" (its own copy of the pattern, an engine String at 0xAFB114, made on first use) and puts
 * "[" + the pad manager's name for the button + "]" instead (method 17 at 0x4B66E0: "1-A Button"). The text drawing
 * (0x7BAB30) looks for "[icon:" with another copy (0xAFB168) and draws the picture.
 * What it does: once a frame (game_pace), with a pad in use (an SDL gamepad, or the touch layer on Android) the
 * rewrite's copy of the pattern gets a first character no text has, so the tags reach the drawing untouched and the
 * pictures show; with only a keyboard the pattern is put back and the key names show, as before.
 * Hints the game composes itself ("Enter (1-A Button)", the Options' bindings) take the name from method 16
 * (0x4B7080, an action's binding: "1-[4631]", a localisation id): with a pad in use a pad button's name becomes its
 * icon tag ("[icon:X360_BUTTON_A]"; ids 4623..4654: the sticks, A B X Y, bumpers, triggers, d-pad, Start, Back).
 * A text made in the same call that first makes the pattern (a menu's hints) is caught by method 17 itself, answered
 * here: with a pad it breaks the pattern at once and returns "icon:<name>", so that tag comes back whole and the
 * rewrite's next search finds nothing (answering "icon:<name>" with the pattern intact would loop forever).
 */
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

#define REWRITE_PATTERN 0x00AFB114u        /* engine String: +4 wide characters, +0xC length */
#define REWRITE_PATTERN_MADE 0x00AFB128u   /* bit 0: made */

int touch_active(void);
static int pad_in_use(void) {
    if (touch_active() || getenv("USF_PAD_ICONS")) return 1;           /* (USF_PAD_ICONS=1: tests without a pad) */
    static uint32_t last_ms, last;
    uint32_t now = SDL_GetTicks();
    if (now - last_ms < 500 && last_ms) return (int)last;              /* (the pads are looked at twice a second) */
    last_ms = now;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    SDL_free(ids);
    return (int)(last = n > 0);
}

#define PAD_VTABLE_ICON_NAME 0x009F1F1Cu   /* the pad manager's vtable 0x9F1ED8, method 17 */
#define PAD_ICON_NAME_GAME 0x004B66E0u
#define STRING_FROM_CHARS 0x008B6370u      /* engine String constructor from a char string (thiscall) */

static void set_pattern(int broken) {
    uint32_t chars = R32(REWRITE_PATTERN + 4);
    if (!(R32(REWRITE_PATTERN_MADE) & 1u) || !chars || R32(REWRITE_PATTERN + 0xC) != 6) return;
    uint16_t want = broken ? 0x0001 : '[';
    if (R16(chars) != want && (R16(chars) == '[' || R16(chars) == 0x0001)) W16(chars, want);
}

static uint32_t icon_name(CPU *c, uint32_t a) {          /* method 17, thiscall (this = ecx, String *name) */
    uint32_t self = c->ecx, name = ARG(0);
    if (getenv("USF_PAD_ICONS_LOG")) {
        char t[48] = "";
        for (uint32_t i = 0; name && R32(name + 4) && i < R32(name + 0xC) && i < 40; i++) t[i] = (char)R16(R32(name + 4) + 2 * i);
        fprintf(stderr, "usf: pad icon name '%s' (pad %d)\n", t, pad_in_use());
    }
    if (!pad_in_use() || !name) { set_pattern(0); return guest_thiscall(c, self, PAD_ICON_NAME_GAME, 1, name); }
    set_pattern(1);
    char n[48] = "icon:";
    uint32_t chars = R32(name + 4), len = R32(name + 0xC);
    for (uint32_t i = 0; chars && i < len && i < 40; i++) n[5 + i] = (char)R16(chars + 2 * i);
    static struct { char name[48]; uint32_t str; } cache[16];
    for (int i = 0; i < 16; i++) {
        if (!cache[i].str) {
            uint32_t text = gmalloc(48), str = gcalloc(0x14);
            memcpy(HOST(text), n, sizeof n);
            guest_thiscall(c, str, STRING_FROM_CHARS, 1, text);
            snprintf(cache[i].name, sizeof cache[i].name, "%s", n);
            cache[i].str = str;
            return str;
        }
        if (!strcmp(cache[i].name, n)) return cache[i].str;
    }
    return guest_thiscall(c, self, PAD_ICON_NAME_GAME, 1, name);
}

#define PAD_VTABLE_ACTION_NAME 0x009F1F18u /* method 16 */
#define PAD_ACTION_NAME_GAME 0x004B7080u

static const char *button_icon(int id) {
    if (id >= 4623 && id <= 4630) return "X360_STICK";
    if ((id >= 4639 && id <= 4642) || (id >= 4651 && id <= 4654)) return "X360_DPAD";
    static const char *b[] = {"X360_BUTTON_A", "X360_BUTTON_B", "X360_BUTTON_X", "X360_BUTTON_Y", "X360_BUTTON_LB",
                              "X360_BUTTON_RB", "X360_BUTTON_LT", "X360_BUTTON_RT"};
    if (id >= 4631 && id <= 4638) return b[id - 4631];
    return id == 4643 ? "X360_BUTTON_START" : id == 4644 ? "X360_BUTTON_BACK" : NULL;
}
static uint32_t engine_string(CPU *c, const char *text) {       /* one engine String per text, made once */
    static struct { char text[48]; uint32_t str; } cache[32];
    for (int i = 0; i < 32; i++) {
        if (cache[i].str && !strcmp(cache[i].text, text)) return cache[i].str;
        if (!cache[i].str) {
            uint32_t t = gmalloc(48), str = gcalloc(0x14);
            snprintf((char *)HOST(t), 48, "%s", text);
            guest_thiscall(c, str, STRING_FROM_CHARS, 1, t);
            snprintf(cache[i].text, sizeof cache[i].text, "%s", text);
            cache[i].str = str;
            return str;
        }
    }
    return 0;
}
/* the raw name of player 0..3's binding for an action (as 0x4B7080 and 0x4B71A0 find it): "#-[4631]", a
 * localisation id; into t */
static void binding_raw(uint32_t self, uint32_t player, uint32_t action, char *t, size_t n) {
    uint32_t e = action + player * 12, type = R32(self + e * 16 + 0x12B0);
    uint32_t idx = R32(self + e * 16 + 0x12B8), sub = R32(self + e * 16 + 0x12BC), p = 0;
    switch (type) {
    case 0: p = R32(self + idx * 4 + 0x20D8); break;
    case 1: p = R32(self + idx * 4 + 0x24D4); break;
    case 2: p = R32(self + (sub + idx * 2) * 4 + 0x2564); break;
    case 3: p = R32(self + idx * 4 + 0x2594); break;
    case 4: p = R32(self + (sub + idx * 2) * 4 + 0x2624); break;
    }
    t[0] = 0;
    for (uint32_t i = 0; p && i < n - 1 && R8(p + i); i++) { t[i] = (char)R8(p + i); t[i + 1] = 0; }
    if (getenv("USF_PAD_ICONS_LOG")) fprintf(stderr, "usf: pad binding %u %u type %u '%s'\n", player, action, type, t);
}
/* "#-[4631]" -> fmt with "X360_BUTTON_A" in an engine String, or 0 when it is not a pad button */
static uint32_t icon_string(CPU *c, const char *raw, const char *fmt) {
    int id = 0;
    char tag[48];
    if (sscanf(raw, "#-[%d]", &id) != 1 || !button_icon(id)) return 0;
    snprintf(tag, sizeof tag, fmt, button_icon(id));
    return engine_string(c, tag);
}
static uint32_t action_name(CPU *c, uint32_t a) {         /* method 16, thiscall (this = ecx, player, action) */
    uint32_t self = c->ecx, player = ARG(0), action = ARG(1);
    uint32_t r = guest_thiscall(c, self, PAD_ACTION_NAME_GAME, 2, player, action);
    if (!r || !pad_in_use()) return r;
    char t[32];
    binding_raw(self, player, action, t, sizeof t);
    uint32_t s = icon_string(c, t, "[icon:%s]");
    return s ? s : r;
}
/* method 18 (0x4B71A0, thiscall: this, which): the button of a menu action for the menus' hints ("Enter [1-A Button]",
 * "[1-B Button] Back"): which 0..3 is player 1's action 9, 4, 2 or 10 (the keyboard's key name instead when the
 * game's last input was the keyboard); the caller puts the brackets around it */
#define PAD_VTABLE_MENU_NAME 0x009F1F20u
#define PAD_MENU_NAME_GAME 0x004B71A0u
static uint32_t menu_name(CPU *c, uint32_t a) {
    uint32_t self = c->ecx, which = ARG(0);
    uint32_t r = guest_thiscall(c, self, PAD_MENU_NAME_GAME, 1, which);
    static const uint8_t action[4] = {9, 4, 2, 10};
    if (!r || which > 3 || !pad_in_use()) return r;
    char t[32];
    binding_raw(self, 0, action[which], t, sizeof t);
    uint32_t s = icon_string(c, t, "icon:%s");
    return s ? s : r;
}
/* method 19 (0x4B7380, thiscall: this, index): the name of the pad binding at an index (the caller adds brackets) */
#define PAD_VTABLE_BUTTON_NAME 0x009F1F24u
#define PAD_BUTTON_NAME_GAME 0x004B7380u
static uint32_t button_name(CPU *c, uint32_t a) {
    uint32_t self = c->ecx, idx = ARG(0);
    uint32_t r = guest_thiscall(c, self, PAD_BUTTON_NAME_GAME, 1, idx);
    if (!r || !pad_in_use() || idx > 255) return r;
    char t[32] = "";
    uint32_t p = R32(self + idx * 4 + 0x20D8);
    for (uint32_t i = 0; p && i < 31 && R8(p + i); i++) { t[i] = (char)R8(p + i); t[i + 1] = 0; }
    if (getenv("USF_PAD_ICONS_LOG")) fprintf(stderr, "usf: pad button name %u '%s'\n", idx, t);
    uint32_t s = icon_string(c, t, "icon:%s");
    return s ? s : r;
}

void pad_icons_install(void);
void pad_icons_install(void) {
    if (R32(PAD_VTABLE_ACTION_NAME) == PAD_ACTION_NAME_GAME)
        W32(PAD_VTABLE_ACTION_NAME, usf_thunk_fn("pad action name", action_name, 2, CONV_STDCALL));
    if (R32(PAD_VTABLE_MENU_NAME) == PAD_MENU_NAME_GAME)
        W32(PAD_VTABLE_MENU_NAME, usf_thunk_fn("pad menu name", menu_name, 1, CONV_STDCALL));
    if (R32(PAD_VTABLE_BUTTON_NAME) == PAD_BUTTON_NAME_GAME)
        W32(PAD_VTABLE_BUTTON_NAME, usf_thunk_fn("pad button name", button_name, 1, CONV_STDCALL));
    if (R32(PAD_VTABLE_ICON_NAME) != PAD_ICON_NAME_GAME) {
        fprintf(stderr, "usf: pad icons: method 17 is not where expected; button names stay as text\n");
        return;
    }
    W32(PAD_VTABLE_ICON_NAME, usf_thunk_fn("pad icon name", icon_name, 1, CONV_STDCALL));
}

void pad_icons_frame(void);
void pad_icons_frame(void) { set_pattern(pad_in_use()); }

/* The pictures: the PC build's atlas for these tags (consoles_buttons/x360/x360_pad_buttons.tga, 512x256, 6 x 3
 * cells) holds keyboard keys ("DELETE", "F1", arrows). When the game makes it (D3DXCreateTextureFromFileInMemoryEx,
 * d3dx9.c) the cells of pad buttons get Xbox-style balls (pad_icons_art.c, from tools/pad_icons_art.py), in every
 * mip level; the gameplay pictures (REDUCTION, STEP) and the empty cells stay the game's. */
extern const uint8_t pad_icons_art[];                       /* RGBA, 512x256 and its mips down to 1x1 */
/* one level (0 = 512x256) of the game's atlas, w x h pixels of 4 bytes: our cells get our pictures */
static void put_level(uint8_t *px, uint32_t level) {
    static const uint8_t ours[18] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1};
    const uint8_t *src = pad_icons_art;
    uint32_t w = 512, h = 256;
    for (uint32_t l = 0; l < level; l++) { src += 4 * w * h; w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            if (ours[(y * 3 / h) * 6 + x * 6 / w]) memcpy(px + 4 * (y * w + x), src + 4 * (y * w + x), 4);
}
/* A texture level the game wrote through LockRect, before it goes to the GPU (dxvk_locks.c). The atlas is made with
 * D3DXCreateTexture (512x256, 10 levels, A8B8G8R8: bytes R G B A) and filled level by level from 0; its level 0 is
 * known by its first 4 KB (the game's own pictures), its other levels by being the same texture. */
void pad_icons_level(const void *tex, uint32_t level, uint8_t *px, uint32_t size);
void pad_icons_level(const void *tex, uint32_t level, uint8_t *px, uint32_t size) {
    static const void *atlas;
    if (level == 0) {
        if (size != 512 * 256 * 4) return;
        uint32_t h = 0x811C9DC5u;
        for (int i = 0; i < 4096; i++) h = (h ^ px[i]) * 0x01000193u;
        if (h != 0x04583DC5u) return;
        atlas = tex;
        if (getenv("USF_PAD_ICONS_LOG")) fprintf(stderr, "usf: pad icons: the button atlas gets the pad's pictures\n");
    } else if (tex != atlas || level > 9) {
        return;
    }
    uint32_t w = 512 >> level, h = 256 >> level;
    if (size == (w ? w : 1) * (h ? h : 1) * 4) put_level(px, level);
}
