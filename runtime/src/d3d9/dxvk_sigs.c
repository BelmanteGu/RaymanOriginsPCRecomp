/* The Direct3D 9 interfaces as the DXVK bridge (dxvk_bridge.c) sees them: one string per method, one letter per
 * argument after `this`, in the order of the Windows SDK's d3d9.h.
 *
 * What it is for: the bridge calls DXVK's native method with each guest argument converted by its letter:
 *   u  32-bit value                   f  float by value (needs a typed call: the method is special-cased)
 *   p  pointer to guest data (or 0)   o  interface given (guest wrapper -> DXVK object)
 *   h  window handle                  n  pSharedHandle (always NULL)
 *   out interfaces (IFoo **, guest wrapper written back), by the type of what comes out:
 *   D device  9 IDirect3D9  T base texture (kind read with GetType)  S surface  V volume  B vertex buffer
 *   I index buffer  v vertex shader  x pixel shader  d vertex declaration  q query  k state block  w swap chain
 *   U any (QueryInterface, GetContainer: looked up, or typed by GetType)
 *   *  the method is special-cased in dxvk_bridge.c (the letters still give its argument count)
 * What it does not do: convert anything itself.
 */
#include "dxvk_bridge.h"

#define RES "*pp", "*", "*", "D", "ppuu", "ppp", "p", "u", "", "", ""   /* IUnknown + IDirect3DResource9 (0..10) */
#define BASETEX "u", "", "", "u", "", ""                            /* IDirect3DBaseTexture9 (11..16) */

const char *const dxb_sig_d3d9[17] = {
    "*pp", "*", "*", "p", "", "uup", "uu", "uuup", "up", "uuuuu", "uuuuuu", "uuuuup", "uuuuu", "uuuu", "uup",
    "*u", "*uuhupD"};

const char *const dxb_sig_device[119] = {
    "*pp", "*", "*", "", "", "", "9", "p", "up", "*p", "uuo", "uuu", "u", "*pw", "uw", "", "*p", "*ppup", "uuuS",
    "up", "u", "uup", "up",
    "uuuuuuTn", "uuuuuuuTn", "uuuuuTn", "uuuuBn", "uuuuIn", "uuuuuuSn", "uuuuuuSn",          /* 23..29 create */
    "opop", "oo", "oo", "uo", "opopu", "opu", "uuuuSn", "uo", "uS", "o", "S", "", "",       /* 30..42 */
    "*upuufu", "up", "up", "up", "p", "p", "p", "p", "up", "up", "uu", "up", "up", "up",     /* 43..56 */
    "uu", "up", "uk", "", "k", "p", "p", "uT", "uo", "uup", "uuu", "uup", "uuu", "p",       /* 57..70 */
    "up", "up", "u", "p", "p", "p", "u", "", "*f", "*", "uuu", "uiuuuu", "uupu", "uuuupupu",  /* 71..84 */
    "uuuoou", "pd", "o", "d", "u", "p", "pv", "o", "v",                                    /* 85..93 */
    "upu", "upu", "upu", "upu", "upu", "upu", "uouu", "uBpp", "uu", "up", "o", "I",       /* 94..105 */
    "px", "o", "x", "upu", "upu", "upu", "upu", "upu", "upu", "upp", "upp", "u", "uq"};    /* 106..118 */

/* IDirect3DTexture9, IDirect3DCubeTexture9, IDirect3DVolumeTexture9 (17..21 differ) */
const char *const dxb_sig_texture[22] = {RES, BASETEX, "up", "uS", "*uppu", "*u", "p"};
const char *const dxb_sig_cube[22] = {RES, BASETEX, "up", "uuS", "*uuppu", "*uu", "up"};
const char *const dxb_sig_voltex[22] = {RES, BASETEX, "up", "uV", "*uppu", "*u", "p"};
const char *const dxb_sig_surface[17] = {RES, "*pp", "p", "*ppu", "*", "*p", "u"};
const char *const dxb_sig_volume[11] = {"*pp", "*", "*", "D", "ppuu", "ppp", "p", "*pp", "p", "*ppu", "*"};
const char *const dxb_sig_buffer[14] = {RES, "*uupu", "*", "p"};                          /* VB and IB */
const char *const dxb_sig_shader[5] = {"*pp", "*", "*", "D", "pp"};                       /* VS, PS, declaration */
const char *const dxb_sig_query[8] = {"*pp", "*", "*", "D", "", "", "u", "puu"};
const char *const dxb_sig_stateblock[6] = {"*pp", "*", "*", "D", "", ""};
const char *const dxb_sig_swapchain[10] = {"*pp", "*", "*", "*pphpu", "o", "uuS", "p", "p", "D", "*p"};
