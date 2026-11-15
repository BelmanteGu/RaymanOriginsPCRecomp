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

#endif
