/* user32, shell32, comdlg32 and the debug-help DLLs: the functions Rayman Origins imports beyond USFIV-PC's set.
 *
 * What it does: window title (to the SDL window), cursor visibility and clipping, display modes (one: the desktop),
 * key state, the "is another copy of the game running?" window search (there is none), and the message/dialog
 * calls of the editor and crash paths, answered as "nothing happened". The stack walk and minidump calls of the
 * crash handler fail cleanly.
 * What it does not do: dialogs, drag and drop, the clipboard, or shell execution.
 */
#include <SDL3/SDL.h>
#include <string.h>

#include "hle.h"

extern SDL_Window *g_window;
uint32_t u32_GetAsyncKeyState(CPU *c, uint32_t a);
uint32_t u32_AdjustWindowRect(CPU *c, uint32_t a);

uint32_t u32_SetWindowTextW(CPU *c, uint32_t a) {
    (void)c;
    char t[256], full[300];
    w2utf8(ARG(1), t, sizeof t);
    snprintf(full, sizeof full, "%s (native)", t);
    if (g_window) SDL_SetWindowTitle(g_window, full);
    return 1;
}
uint32_t u32_AdjustWindowRectEx(CPU *c, uint32_t a) { return u32_AdjustWindowRect(c, a); }
uint32_t u32_GetMenu(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t u32_ClientToScreen(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }   /* the window is at 0,0 */
uint32_t u32_RegisterClipboardFormatW(CPU *c, uint32_t a) { (void)c; (void)a; return 0xC200u; }
uint32_t u32_GetWindowThreadProcessId(CPU *c, uint32_t a) {
    (void)c;
    if (ARG(1)) W32(ARG(1), 0x1000);
    return 0x104;
}
uint32_t u32_UpdateWindow(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t u32_RedrawWindow(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t u32_ShowCursor(CPU *c, uint32_t a) {
    (void)c;
    static int count;
    count += ARG(0) ? 1 : -1;
    if (g_window && !g_headless) { if (count >= 0) SDL_ShowCursor(); else SDL_HideCursor(); }
    return (uint32_t)count;
}
uint32_t u32_LoadCursorW(CPU *c, uint32_t a) { (void)c; (void)a; return 0x00020005u; }
uint32_t u32_ClipCursor(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t u32_EnumWindows(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }   /* no other top-level windows */
uint32_t u32_GetActiveWindow(CPU *c, uint32_t a) { (void)c; (void)a; return g_window ? 0x00010010u : 0; }
uint32_t u32_EndDialog(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t u32_SendMessageW(CPU *c, uint32_t a) { (void)c; usf_log("SendMessageW(%x, %x) ignored\n", ARG(0), ARG(1)); return 0; }
uint32_t u32_SendMessageTimeoutW(CPU *c, uint32_t a) { (void)c; if (ARG(6)) W32(ARG(6), 0); return 1; }
uint32_t u32_GetProcessWindowStation(CPU *c, uint32_t a) { (void)c; (void)a; return 0x7C000001u; }
uint32_t u32_GetUserObjectInformationW(CPU *c, uint32_t a) {    /* (h, index, info, size, &needed): visible station */
    (void)c;
    if (ARG(1) == 1 && ARG(3) >= 12) { W32(ARG(2), 0); W32(ARG(2) + 4, 0); W32(ARG(2) + 8, 1); }   /* WSF_VISIBLE */
    if (ARG(4)) W32(ARG(4), 12);
    return 1;
}
uint32_t u32_GetKeyState(CPU *c, uint32_t a) { return u32_GetAsyncKeyState(c, a); }

/* EnumDisplaySettingsW(device, mode, DEVMODEW*): mode 0 and ENUM_CURRENT_SETTINGS are the desktop; no others */
uint32_t u32_EnumDisplaySettingsW(CPU *c, uint32_t a) {
    (void)c;
    uint32_t mode = ARG(1), dm = ARG(2);
    if (mode != 0xFFFFFFFFu && mode != 0xFFFFFFFEu && mode != 0) return 0;
    int w = 1920, h = 1080, hz = 60;
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (m && m->w > 0) { w = m->w; h = m->h; hz = m->refresh_rate > 0 ? (int)(m->refresh_rate + 0.5f) : 60; }
    }
    uint16_t size = R16(dm + 68) ? R16(dm + 68) : 220;
    memset(HOST(dm + 64), 0, size - 64);
    W16(dm + 68, size);
    W32(dm + 72, 0x5C0000u);                                   /* DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY */
    W32(dm + 168, 32);
    W32(dm + 172, (uint32_t)w);
    W32(dm + 176, (uint32_t)h);
    W32(dm + 184, (uint32_t)hz);
    return 1;
}

/* ---- shell32 ---- */
uint32_t x_DragAcceptFiles(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_DragQueryFileW(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_DragFinish(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_ShellExecuteW(CPU *c, uint32_t a) { (void)c; (void)a; return 2; }   /* "file not found": nothing opens */
uint32_t x_SHGetSpecialFolderPathA(CPU *c, uint32_t a) {          /* (hwnd, path, csidl, create) */
    (void)c;
    uint32_t csidl = ARG(2) & 0xFF;
    const char *p = csidl == 0x05 ? "C:\\Users\\player\\Documents" : csidl == 0x1C ? "C:\\Users\\player\\AppData\\Local" :
                    csidl == 0x23 ? "C:\\ProgramData" : "C:\\Users\\player\\AppData\\Roaming";
    snprintf((char *)HOST(ARG(1)), 260, "%s", p);
    usf_log("SHGetSpecialFolderPathA(%x) -> %s\n", csidl, p);
    return 1;
}
uint32_t x_SHBrowseForFolderW(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_SHGetPathFromIDListW(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_SHParseDisplayName(CPU *c, uint32_t a) { (void)c; (void)a; return 0x80004005u; }
uint32_t x_GetOpenFileNameW(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_GetSaveFileNameW(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }

/* ---- imagehlp / dbghelp (crash reports): unavailable ---- */
uint32_t x_SymGetOptions(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_SymCleanup(CPU *c, uint32_t a) { (void)c; (void)a; return 1; }
uint32_t x_StackWalk(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_SymGetModuleBase(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_SymFunctionTableAccess(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_SymGetLineFromAddr(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_SymGetSymFromAddr(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_SymGetModuleInfo(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_UnDecorateSymbolName(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_MiniDumpWriteDump(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }

/* ---- wininet / iphlpapi: offline ---- */
uint32_t x_InternetGetCookieExA(CPU *c, uint32_t a) { set_last_error(c, 12030); (void)a; return 0; }
uint32_t x_InternetSetCookieExA(CPU *c, uint32_t a) { (void)c; (void)a; return 0; }
uint32_t x_GetAdaptersAddresses(CPU *c, uint32_t a) { (void)c; (void)a; return 232; }   /* ERROR_NO_DATA */
uint32_t x_GetIfEntry(CPU *c, uint32_t a) { (void)c; (void)a; return 1168; }            /* ERROR_NOT_FOUND */
uint32_t x_GetBestInterface(CPU *c, uint32_t a) { (void)c; (void)a; return 1168; }
