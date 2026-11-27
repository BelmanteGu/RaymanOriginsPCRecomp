/* The few facts about the game the runtime needs: executable name, the Windows folder it pretends to live in, the
 * default install folder and the name of our own data folder.
 * What it does not do: hold any game logic (the lifted code is the game).
 */
#ifndef GAME_H
#define GAME_H

#define GAME_EXE "Rayman Origins.exe"
#define GAME_WIN_DIR "C:\\Rayman"                 /* the game folder as the game sees it */
#define GAME_WIN_DIR_LEN 9
#define GAME_TITLE "Rayman Origins"
#define GAME_DATA_NAME "rayman-origins-pc"       /* ~/.local/share/<this>: saves, settings, copies */
#define GAME_ENV "RAYMAN_GAME"                     /* environment variable with the install folder */
#define GAME_DEFAULT_DIR "/.local/share/Steam/steamapps/common/ValveTestApp207490"   /* under $HOME */

/* UbiArt fits its camera and screen rectangles to 16:9 with this pair of floats in .rdata: 16/9, then 9/16 (found
 * from the Xbox 360 port, where the same pair is read by the engine's fit function). Wider screens: both changed. */
#define GAME_ASPECT_PAIR 0x00A2C1B4u

/* The screen rectangle's fit (the function at 0x83EFF0: the picture letterboxed or pillarboxed to 16:9, then copied
 * centred with two StretchRects and black bars cleared): two doubles in .rdata, 9/16 at 0xA2C230 and 16/9 (in float
 * precision) at 0xA2C238. Kept in step with the pair, but they alone do not remove the bars: the fit is integer code
 * (x * 9 >> 4 at 95 places), replaced in the lifter (tools/lift/lift.py FIT_SITES, docs/ANDROID-PERFORMANCE.md). */
#define GAME_FIT_INV 0x00A2C230u
#define GAME_FIT_ASPECT 0x00A2C238u

/* UbiArt's frame rates (.data): the app loop's fps and 1/fps, then the game logic's fps and 1/fps (the logic step,
 * read in 65 places), set by the engine's SetFrameRate (0x796E60) as 60 and 1/60. 120 fps = all four at 120. */
#define GAME_RATE_VARS 0x00A8A9D8u

/* The window when the player sets nothing: 21:9 that fits a 1920-wide desktop. RAYMAN_RES=WxH changes it. */
#define GAME_DEFAULT_W 1920
#define GAME_DEFAULT_H 822

#endif
