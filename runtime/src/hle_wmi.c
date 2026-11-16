/* WMI, only as far as the game's "is this DirectInput pad an XInput pad?" check needs it.
 *
 * Why: Rayman Origins enumerates game controllers with DirectInput and, before that, asks WMI for the PnP devices
 * whose DeviceID contains "IG_" (Microsoft's IsXInputDevice sample, 0x4B60C0). A DirectInput device whose guidProduct
 * matches one of those VID/PIDs becomes an XInput pad (read with XInputGetState, default buttons assigned at 0x4B4F91);
 * any other pad is read with DirectInput but gets NO default mapping, so the game ignores it. Every SDL gamepad is
 * offered to the game as XInput (hle_xinput.c reads any SDL pad), so the game must see it as one.
 *
 * What it does: CoCreateInstance(CLSID_WbemLocator) -> IWbemLocator; ConnectServer -> IWbemServices;
 * CreateInstanceEnum -> one Win32_PNPEntity whose DeviceID is an Xbox 360 pad's (VID_045E PID_028E IG_00).
 * The DirectInput side (hle_dinput.c) reports the matching guidProduct (Data1 = 0x028E045E).
 * What it does not do: any other WMI class or property (Get of anything but DeviceID fails). */
#include <string.h>

#include "hle.h"

#define WMI_PAD_DEVICE_ID "USB\\VID_045E&PID_028E&IG_00\\6&1F2A3B4C&0&00"

static uint32_t one(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
static uint32_t fail(CPU *c, uint32_t a) { (void)c; (void)a; return 0x80004001u; }     /* E_NOTIMPL */
static uint32_t qi(CPU *c, uint32_t a) { (void)c; W32(ARG(2), ARG(0)); return 0; }

/* IWbemClassObject::Get(this, name, flags, VARIANT *val, CIMTYPE *type, long *flavor) */
static uint32_t obj_get(CPU *c, uint32_t a) {
    (void)c;
    char name[32] = {0};
    for (int i = 0; i < 31 && ARG(1) && R16(ARG(1) + 2u * (uint32_t)i); i++) name[i] = (char)R16(ARG(1) + 2u * (uint32_t)i);
    if (strcmp(name, "DeviceID") || !ARG(3)) return 0x80041002u;                      /* WBEM_E_NOT_FOUND */
    uint32_t n = (uint32_t)strlen(WMI_PAD_DEVICE_ID);
    uint32_t b = gmalloc(4 + 2 * n + 2);
    W32(b, 2 * n);
    utf8_to_w(WMI_PAD_DEVICE_ID, b + 4, n + 1);
    memset(HOST(ARG(3)), 0, 16);
    W16(ARG(3), 8);                                                                   /* VT_BSTR */
    W32(ARG(3) + 8, b + 4);
    if (ARG(4)) W32(ARG(4), 8);                                                       /* CIM_STRING */
    return 0;
}
static HleFn vt_obj[5] = {qi, one, one, fail, obj_get};
static const uint8_t na_obj[5] = {3, 1, 1, 2, 6};

/* IEnumWbemClassObject::Next(this, timeout, count, objects[], returned): the one pad, then the end (S_FALSE) */
static uint32_t enum_reset(CPU *c, uint32_t a) { (void)c; W32(ARG(0) + 4, 0); return 0; }
static uint32_t enum_next(CPU *c, uint32_t a) {
    (void)c;
    uint32_t self = ARG(0), want = ARG(2), out = ARG(3), got = 0;
    if (want && !R32(self + 4)) {
        static uint32_t pad;
        if (!pad) pad = obj_new_n("IWbemClassObject", 5, vt_obj, na_obj, 16);
        W32(out, pad);
        W32(self + 4, 1);
        got = 1;
    }
    if (ARG(4)) W32(ARG(4), got);
    return got == want ? 0 : 1;
}
static HleFn vt_enum[8] = {qi, one, one, enum_reset, enum_next, fail, fail, fail};
static const uint8_t na_enum[8] = {3, 1, 1, 1, 5, 3, 2, 3};

/* IWbemServices: only CreateInstanceEnum(this, class, flags, ctx, enum**) (slot 18) */
static uint32_t svc_enum(CPU *c, uint32_t a) {
    (void)c;
    W32(ARG(4), obj_new_n("IEnumWbemClassObject", 8, vt_enum, na_enum, 16));
    return 0;
}
static HleFn vt_svc[23];
static const uint8_t na_svc[23] = {3, 1, 1, 6, 2, 3, 6, 5, 4, 4, 4, 4, 5, 5, 4, 4, 4, 4, 5, 5, 6, 6, 4};

/* IWbemLocator::ConnectServer(this, resource, user, password, locale, flags, authority, ctx, services**) */
static uint32_t loc_connect(CPU *c, uint32_t a) {
    (void)c;
    static uint32_t svc;
    if (!svc) {
        for (int i = 0; i < 23; i++) vt_svc[i] = fail;
        vt_svc[0] = qi; vt_svc[1] = one; vt_svc[2] = one; vt_svc[18] = svc_enum;
        svc = obj_new_n("IWbemServices", 23, vt_svc, na_svc, 16);
    }
    W32(ARG(8), svc);
    return 0;
}
static HleFn vt_loc[4] = {qi, one, one, loc_connect};
static const uint8_t na_loc[4] = {3, 1, 1, 9};

/* CLSID_WbemLocator {4590F811-1D3A-11D0-891F-00AA004B2E24} */
uint32_t wmi_create(uint32_t clsid, uint32_t out) {
    static const uint8_t WBEM_LOCATOR[16] = {0x11, 0xF8, 0x90, 0x45, 0x3A, 0x1D, 0xD0, 0x11,
                                             0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24};
    if (memcmp(HOST(clsid), WBEM_LOCATOR, 16)) return 1;
    W32(out, obj_new_n("IWbemLocator", 4, vt_loc, na_loc, 16));
    return 0;
}
