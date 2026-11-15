package io.github.belmantegu.raymanpc;

import org.libsdl.app.SDLActivity;

/**
 * The game: SDL3 runs libmain.so (the recompiled game + the runtime) with the game folder and the data folder as
 * arguments. The on-screen controls and the port's settings are drawn by the game process itself (touch.c,
 * vk_overlay.c): in play only the controls show; the settings appear under the game's own pause menu.
 */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() { return new String[] {"SDL3", "main"}; }

    @Override
    protected String[] getArguments() {
        String game = getIntent().getStringExtra("game"), data = getIntent().getStringExtra("data");
        return new String[] {game != null ? game : "", data != null ? data : getFilesDir().getPath()};
    }
}
