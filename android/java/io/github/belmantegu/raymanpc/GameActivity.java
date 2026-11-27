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
    /** the touch controls' own layer (dxvk_overlay.c): its Surface, or null when it goes away */
    static native void nativeTouchSurface(android.view.Surface s);

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

    /* The slow state some phones keep the GPU in after start (Redmi 10C: 10 fps in the menu until the status bar was
     * pulled down) is ended in the native side (gpu_boost.c); here only what the system is told. */
    @Override
    protected void onCreate(android.os.Bundle b) {
        super.onCreate(b);
        /* Android 13+: tell the system's game manager a game is being played (vendor game modes, e.g. MIUI's, read it) */
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            android.app.GameManager gm = getSystemService(android.app.GameManager.class);
            if (gm != null) gm.setGameState(new android.app.GameState(false, android.app.GameState.MODE_GAMEPLAY_UNINTERRUPTIBLE));
        }
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        addTouchLayer();
    }

    /* The touch controls on a layer of their own, above the game's surface and composed with it by the display
     * hardware: the game's picture can stay at its own resolution (the compositor enlarges it, not the GPU) while the
     * controls stay sharp at the screen's, and they are drawn only when they change (dxvk_overlay.c). Not clickable:
     * the touches go on to SDL's surface under it. */
    private void addTouchLayer() {
        if (mLayout == null) return;
        android.view.SurfaceView layer = new android.view.SurfaceView(this);
        layer.setZOrderMediaOverlay(true);
        layer.getHolder().setFormat(android.graphics.PixelFormat.TRANSLUCENT);
        layer.setFocusable(false);
        layer.setClickable(false);
        layer.getHolder().addCallback(new android.view.SurfaceHolder.Callback() {
            @Override public void surfaceCreated(android.view.SurfaceHolder h) { nativeTouchSurface(h.getSurface()); }
            @Override public void surfaceChanged(android.view.SurfaceHolder h, int f, int w, int hh) { nativeTouchSurface(h.getSurface()); }
            @Override public void surfaceDestroyed(android.view.SurfaceHolder h) { nativeTouchSurface(null); }
        });
        mLayout.addView(layer, new android.widget.RelativeLayout.LayoutParams(
            android.view.ViewGroup.LayoutParams.MATCH_PARENT, android.view.ViewGroup.LayoutParams.MATCH_PARENT));
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
