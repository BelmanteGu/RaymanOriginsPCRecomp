package io.github.belmantegu.raymanpc;

import android.content.Intent;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;

import org.libsdl.app.SDLActivity;

import java.io.File;

/**
 * The game: SDL3 runs libmain.so (the recompiled game + the runtime) with the game folder and the data folder as
 * arguments. The on-screen controls are drawn by the game process itself (touch.c, dxvk_overlay.c). The gear button
 * there calls showPortMenu: the launcher's own Options screen (GameMenu, the game's look) over the game, which is held
 * still meanwhile (port_android.c); from it, "Button layout" lets the buttons be moved on the game's screen and "Back
 * to launcher" ends the game and opens the launcher (the game runs in its own process, ":game").
 */
public class GameActivity extends SDLActivity {
    private GameMenu menu;

    static native void nativeSetFrozen(boolean on);
    static native void nativeReloadSettings();
    static native void nativeEditLayout();

    @Override
    protected String[] getLibraries() { return new String[] {"SDL3", "main"}; }

    @Override
    protected String[] getArguments() {
        String game = getIntent().getStringExtra("game"), data = getIntent().getStringExtra("data");
        return new String[] {game != null ? game : "", data != null ? data : getFilesDir().getPath()};
    }

    private File dataDir() {
        String d = getIntent().getStringExtra("data");
        return new File(d != null ? d : getFilesDir().getPath());
    }

    /** from the game's thread (port_android.c): the gear button */
    public void showPortMenu() {
        runOnUiThread(() -> {
            if (menu == null) {
                if (!MenuArt.available(this)) return;
                try {
                    menu = new GameMenu(this, MenuArt.load(this), new GameMenu.Host() {
                        public boolean hasGame() { return true; }
                        public void pickFolder() {}
                        public void play() { closeMenu(); }
                        public void quit() { closeMenu(); }
                        public File dataDir() { return GameActivity.this.dataDir(); }
                        public void closeMenu() { GameActivity.this.closeMenu(); }
                        public void editLayout() { GameActivity.this.closeMenu(); nativeEditLayout(); }
                        public void toLauncher() { GameActivity.this.toLauncher(); }
                    }, true);
                } catch (Exception e) { menu = null; return; }
                addContentView(menu, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            }
            nativeSetFrozen(true);
            menu.reopen();
            menu.setVisibility(View.VISIBLE);
            menu.requestFocus();
        });
    }

    private void closeMenu() {
        if (menu == null) return;
        menu.setVisibility(View.GONE);
        nativeReloadSettings();
        nativeSetFrozen(false);
    }

    private void toLauncher() {
        startActivity(new Intent(this, LauncherActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP));
        finishAndRemoveTask();
        android.os.Process.killProcess(android.os.Process.myPid());   /* the game's own process: nothing to keep */
    }

    /** from the game's thread: a touch button pressed with "Vibration" on */
    public void haptic() {
        runOnUiThread(() -> getWindow().getDecorView().performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY));
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (menu != null && menu.getVisibility() == View.VISIBLE) { menu.key(e); return true; }
        return super.dispatchKeyEvent(e);
    }
}
