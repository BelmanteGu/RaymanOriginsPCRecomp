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

/* The window when the player sets nothing: 21:9 that fits a 1920-wide desktop. RAYMAN_RES=WxH changes it. */
#define GAME_DEFAULT_W 1920
#define GAME_DEFAULT_H 822

#endif
