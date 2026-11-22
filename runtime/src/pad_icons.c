/* Button icons in the game's texts instead of "[1-A Button]".
 *
 * Why: the game's texts name pad buttons with icon tags ("Press [icon:X360_BUTTON_A] to fire"), drawn as the pad's
 * button pictures (world/common/ui/ui_menu/consoles_buttons/x360). The PC build first rewrites each tag
 * (0x7A72A0) as "[" + the pad manager's name for it + "]": its method 17 (0x4B66E0, vtable 0x9F1ED8 + 0x44) turns
 * X360_BUTTON_A / X / RT / BACK / STICK into the bound control's name, "1-A Button".
 * What it does: method 17 is answered here: with a pad in use (an SDL gamepad, or the touch layer on Android) it
 * returns "icon:<name>", so the rewrite gives back "[icon:<name>]" and the text draws the button's picture; with only a
 * keyboard the game's own answer (the key's name) stays. One engine String per name is made once (its constructor
 * 0x8B6370 from a char string; layout: +4 wide characters, +0xC length).
 */
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

#define PAD_VTABLE_ICON_NAME 0x009F1F1Cu   /* vtable 0x9F1ED8, method 17 */
#define PAD_ICON_NAME_GAME 0x004B66E0u
#define STRING_FROM_CHARS 0x008B6370u

int touch_active(void);
static int pad_in_use(void) {
    if (touch_active() || getenv("USF_PAD_ICONS")) return 1;           /* (USF_PAD_ICONS=1: tests without a pad) */
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    SDL_free(ids);
    return n > 0;
}

static uint32_t icon_name(CPU *c, uint32_t a) {          /* thiscall (this = ecx, String *name) */
    uint32_t self = c->ecx, name = ARG(0);
    if (!pad_in_use() || !name) return guest_thiscall(c, self, PAD_ICON_NAME_GAME, 1, name);
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

void pad_icons_install(void);
void pad_icons_install(void) {
    if (R32(PAD_VTABLE_ICON_NAME) != PAD_ICON_NAME_GAME) {
        fprintf(stderr, "usf: pad icons: method 17 is not where expected; button names stay as text\n");
        return;
    }
    W32(PAD_VTABLE_ICON_NAME, usf_thunk_fn("pad icon name", icon_name, 1, CONV_STDCALL));
}
