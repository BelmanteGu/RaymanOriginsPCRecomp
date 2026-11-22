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
static uint32_t action_name(CPU *c, uint32_t a) {         /* method 16, thiscall (this = ecx, player, action) */
    uint32_t self = c->ecx, player = ARG(0), action = ARG(1);
    uint32_t r = guest_thiscall(c, self, PAD_ACTION_NAME_GAME, 2, player, action);
    if (!r || !pad_in_use()) return r;
    /* the binding's own name, before translation (as 0x4B7080 finds it): "#-[4631]", a localisation id */
    uint32_t e = action + player * 12, type = R32(self + e * 16 + 0x12B0);
    uint32_t idx = R32(self + e * 16 + 0x12B8), sub = R32(self + e * 16 + 0x12BC), p = 0;
    switch (type) {
    case 0: p = R32(self + idx * 4 + 0x20D8); break;
    case 1: p = R32(self + idx * 4 + 0x24D4); break;
    case 2: p = R32(self + (sub + idx * 2) * 4 + 0x2564); break;
    case 3: p = R32(self + idx * 4 + 0x2594); break;
    case 4: p = R32(self + (sub + idx * 2) * 4 + 0x2624); break;
    }
    char t[32] = "";
    for (uint32_t i = 0; p && i < 31 && R8(p + i); i++) t[i] = (char)R8(p + i);
    if (getenv("USF_PAD_ICONS_LOG")) fprintf(stderr, "usf: pad action name %u %u type %u '%s'\n", player, action, type, t);
    int id = 0;
    char tag[48];
    if (sscanf(t, "#-[%d]", &id) != 1 || !button_icon(id)) return r;
    snprintf(tag, sizeof tag, "[icon:%s]", button_icon(id));
    uint32_t s = engine_string(c, tag);
    return s ? s : r;
}

void pad_icons_install(void);
void pad_icons_install(void) {
    if (R32(PAD_VTABLE_ACTION_NAME) == PAD_ACTION_NAME_GAME)
        W32(PAD_VTABLE_ACTION_NAME, usf_thunk_fn("pad action name", action_name, 2, CONV_STDCALL));
    if (R32(PAD_VTABLE_ICON_NAME) != PAD_ICON_NAME_GAME) {
        fprintf(stderr, "usf: pad icons: method 17 is not where expected; button names stay as text\n");
        return;
    }
    W32(PAD_VTABLE_ICON_NAME, usf_thunk_fn("pad icon name", icon_name, 1, CONV_STDCALL));
}

void pad_icons_frame(void);
void pad_icons_frame(void) { set_pattern(pad_in_use()); }
