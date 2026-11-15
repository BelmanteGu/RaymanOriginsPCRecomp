/* The import table built from imports_list.h, and the lookup the loader and GetProcAddress use.
 *
 * What it does: declares every implementation as a weak symbol (k32_<name>, u32_<name>, x_<name>), so a function
 * nobody wrote yet is NULL and the dispatcher reports it; builds one array of HleImport; hle_find matches a DLL name
 * (case-insensitive, with or without ".dll") and a function name or "#ordinal".
 * What it does not do: implement anything.
 */
#include <string.h>
#include <strings.h>

#include "hle.h"

#define K(name, n) uint32_t k32_##name(CPU *c, uint32_t a) __attribute__((weak));
#define U(name, n) uint32_t u32_##name(CPU *c, uint32_t a) __attribute__((weak));
#define S(dll, name, n) uint32_t x_##name(CPU *c, uint32_t a) __attribute__((weak));
#define C(dll, name) uint32_t x_##name(CPU *c, uint32_t a) __attribute__((weak));
#include "imports_list.h"
#undef K
#undef U
#undef S
#undef C

uint32_t x_DirectSoundCreate8(CPU *c, uint32_t a) __attribute__((weak));
uint32_t x_XInputGetBatteryInformation(CPU *c, uint32_t a) __attribute__((weak));

static const HleImport table[] = {
#define K(name, n) {"kernel32.dll", #name, n, CONV_STDCALL, k32_##name},
#define U(name, n) {"user32.dll", #name, n, CONV_STDCALL, u32_##name},
#define S(dll, name, n) {dll, #name, n, CONV_STDCALL, x_##name},
#define C(dll, name) {dll, #name, 0, CONV_CDECL, x_##name},
#include "imports_list.h"
#undef K
#undef U
#undef S
#undef C
    /* imported by ordinal */
    {"dsound.dll", "#11", 3, CONV_STDCALL, x_DirectSoundCreate8},
    {"dsound.dll", "#12", 3, CONV_STDCALL, x_DirectSoundCreate8},
    {"xinput1_3.dll", "#7", 3, CONV_STDCALL, x_XInputGetBatteryInformation},
    {NULL, NULL, 0, 0, NULL},
};

static int same_dll(const char *a, const char *b) {
    size_t la = strcspn(a, "."), lb = strcspn(b, ".");
    return la == lb && !strncasecmp(a, b, la);
}

/* tables with names that are not C identifiers (C++ symbols, `_BinkOpen@8`) or data: hle_crt*.c, hle_msvcp.c, hle_bink.c */
extern const HleImport hle_crt_table[] __attribute__((weak)), hle_msvcp_table[] __attribute__((weak)),
    hle_bink_table[] __attribute__((weak));

const HleImport *hle_find(const char *dll, const char *name) {
    for (const HleImport *t = table; t->dll; t++)
        if (same_dll(t->dll, dll) && !strcmp(t->name, name)) return t;
    const HleImport *extra[] = {hle_crt_table, hle_msvcp_table, hle_bink_table};
    for (int k = 0; k < 3; k++)
        for (const HleImport *t = extra[k]; t && t->dll; t++)
            if (same_dll(t->dll, dll) && !strcmp(t->name, name)) return t;
    /* GetProcAddress on a DLL we know may ask for a function the game did not import: match the name only */
    if (dll[0] == '*')
        for (const HleImport *t = table; t->dll; t++)
            if (!strcmp(t->name, name)) return t;
    return NULL;
}
