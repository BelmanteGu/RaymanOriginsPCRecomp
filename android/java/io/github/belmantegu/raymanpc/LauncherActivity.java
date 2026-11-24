package io.github.belmantegu.raymanpc;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * Launcher (the flow of the Celeste Android launcher, the look of the game's main menu):
 * without a game: SELECT GAME FOLDER (the Steam folder copied to the phone, read in place) or IMPORT .ZIP (extracted
 * into the app's folder); with a game: PLAY and CHANGE GAME FILES. Touch or a pad (up/down, A/Enter, B/Back).
 */
public class LauncherActivity extends Activity {
    private static final int REQ_FOLDER = 1, REQ_ZIP = 2;

    private MenuStyle st;
    private LinearLayout menu;
    private TextView heading, description, status;
    private ProgressBar progress;
    private final List<TextView> items = new ArrayList<>();
    private final List<Runnable> actions = new ArrayList<>();
    private final List<String> descriptions = new ArrayList<>();
    private int selected;
    private boolean busy;
    private GameMenu game;                   /* the launcher in the game's look, when the build has its art (GameMenu) */

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        st = new MenuStyle(this);
        GameMenu g = null;
        if (MenuArt.available(this)) {
            try {
                g = new GameMenu(this, MenuArt.load(this), new GameMenu.Host() {
                    public boolean hasGame() { return isGame(gameDir()); }
                    public void pickFolder() { LauncherActivity.this.pickFiles(); }
                    public void play() { LauncherActivity.this.play(); }
                    public void quit() { finishAndRemoveTask(); }
                    public File dataDir() { return LauncherActivity.this.dataDir(); }
                });
            } catch (Exception e) { g = null; }
        }
        game = g;
        if (game != null) setContentView(game); else setContentView(build());
        hideBars();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideBars();
        /* development over adb: am start -n .../.LauncherActivity --es game /sdcard/RaymanOrigins [--ez play true] */
        Intent in = getIntent();
        String g = in != null ? in.getStringExtra("game") : null;
        boolean go = false;
        if (g != null) android.util.Log.i("RaymanLauncher", "adb game " + g + " -> " + isGame(g) + " (" + java.util.Arrays.toString(new File(g).list()) + ")");
        if (g != null && isGame(g)) {
            prefs().edit().putString("game", g).apply();
            go = in.getBooleanExtra("play", false);
            setIntent(new Intent());
        }
        if (game != null) game.resume();
        else if (!busy) showMenu();
        if (go) play();
    }

    @Override
    protected void onPause() { super.onPause(); if (game != null) game.pause(); }
    @Override
    protected void onDestroy() { if (game != null) game.release(); super.onDestroy(); }

    @Override
    protected void onNewIntent(Intent i) { super.onNewIntent(i); setIntent(i); }

    private SharedPreferences prefs() { return getSharedPreferences("launcher", MODE_PRIVATE); }
    String gameDir() { return prefs().getString("game", ""); }
    File dataDir() { File d = new File(getExternalFilesDir(null), "data"); d.mkdirs(); return d; }

    /** A folder holding Rayman Origins.exe (any case). */
    static boolean isGame(String dir) {
        if (dir == null || dir.isEmpty()) return false;
        String[] names = new File(dir).list();
        if (names == null) return false;
        for (String n : names) if (n.equalsIgnoreCase("Rayman Origins.exe")) return true;
        return false;
    }

    private View build() {
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        int bg = getResources().getIdentifier("launcher_bg", "drawable", getPackageName());
        if (bg != 0) {
            ImageView art = new ImageView(this);
            art.setImageResource(bg);
            art.setScaleType(ImageView.ScaleType.CENTER_CROP);
            root.addView(art, match());
        }
        View veil = new View(this);                       /* dark on the left, where the menu is */
        veil.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] {Color.argb(235, 0, 0, 0), Color.argb(170, 0, 0, 0), Color.argb(40, 0, 0, 0)}));
        root.addView(veil, match());

        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        left.setPadding(st.dp(36), st.dp(18), 0, st.dp(14));
        int logo = getResources().getIdentifier("launcher_logo", "drawable", getPackageName());
        if (logo != 0) {
            ImageView l = new ImageView(this);
            l.setImageResource(logo);
            l.setAdjustViewBounds(true);
            l.setScaleType(ImageView.ScaleType.FIT_START);
            left.addView(l, new LinearLayout.LayoutParams(st.dp(190), st.dp(92)));
        }
        heading = st.heading("");
        left.addView(heading, wrap(6));
        menu = new LinearLayout(this);
        menu.setOrientation(LinearLayout.VERTICAL);
        left.addView(menu, wrap(8));
        description = st.text("", 13, Color.WHITE, st.plain);
        description.setLineSpacing(st.dp(2), 1f);
        description.setPadding(st.dp(18), 0, 0, 0);
        left.addView(description, wrap(12));
        root.addView(left, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.MATCH_PARENT));

        LinearLayout bottom = new LinearLayout(this);
        bottom.setOrientation(LinearLayout.VERTICAL);
        bottom.setPadding(st.dp(54), 0, st.dp(24), st.dp(10));
        status = st.text("", 13, MenuStyle.ORANGE, st.plain);
        bottom.addView(status, wrap(4));
        progress = st.progress();
        progress.setVisibility(View.GONE);
        bottom.addView(progress, new LinearLayout.LayoutParams(st.dp(360), st.dp(10)));
        LinearLayout hints = st.hintBar();
        hints.addView(st.hint("Confirm"));
        hints.addView(st.hint("Back"));
        LinearLayout.LayoutParams hp = wrap(10);
        hp.gravity = Gravity.CENTER_HORIZONTAL;
        bottom.addView(hints, hp);
        root.addView(bottom, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM));
        return root;
    }

    private void showMenu() {
        items.clear(); actions.clear(); descriptions.clear(); menu.removeAllViews();
        String game = gameDir();
        if (isGame(game)) {
            heading.setVisibility(View.GONE);                 /* the logo already names the game */
            add("Play", this::play, "Game: " + game + "\nYour own Steam copy, recompiled to run natively.");
            add("Change game files", this::pickFolder, "Select another copy of the game folder.");
            add("Import .zip", this::pickZip, "Extract a .zip of the game folder into the app (needs twice its size free).");
        } else {
            heading.setVisibility(View.VISIBLE);
            heading.setText("SELECT GAME FILES");
            add("Select game folder", this::pickFolder,
                    "Copy your Rayman Origins folder (with GameData) to the phone,\nthen select it here (it is read in place).");
            add("Import .zip", this::pickZip, "Or a .zip of that folder: it is extracted into the app.");
        }
        select(Math.min(selected, items.size() - 1));
    }

    private void add(String label, Runnable r, String desc) {
        TextView t = st.item(label);
        final int i = items.size();
        t.setOnClickListener(v -> { select(i); if (!busy) r.run(); });
        items.add(t); actions.add(r); descriptions.add(desc);
        menu.addView(t, wrap(2));
    }

    private void select(int i) {
        if (items.isEmpty()) return;
        selected = Math.max(0, Math.min(i, items.size() - 1));
        for (int k = 0; k < items.size(); k++) st.select(items.get(k), k == selected);
        description.setText(descriptions.get(selected));
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (game != null) return game.key(e) || super.dispatchKeyEvent(e);
        if (e.getAction() != KeyEvent.ACTION_DOWN || busy) return super.dispatchKeyEvent(e);
        switch (e.getKeyCode()) {
            case KeyEvent.KEYCODE_DPAD_UP: select(selected - 1); return true;
            case KeyEvent.KEYCODE_DPAD_DOWN: select(selected + 1); return true;
            case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_ENTER: case KeyEvent.KEYCODE_DPAD_CENTER:
                actions.get(selected).run(); return true;
            default: return super.dispatchKeyEvent(e);
        }
    }

    /* ---- files ---- */
    private boolean storageAllowed() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager()) return true;
        if (status != null) status.setText("Allow \"access to all files\" so the game folder can be read in place, then come back.");
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
        } catch (Exception ex) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
        return false;
    }

    void pickFolder() {
        if (!storageAllowed()) return;
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_FOLDER);
    }

    /** the game's look: a folder (read in place, then made light) or a .zip (of the game or of the light copy) */
    private void pickFiles() {
        if (game == null) { pickFolder(); return; }
        String[] ways = {game.text("Game folder"), game.text(".zip file"), game.text("GPU driver")};
        new android.app.AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(game.text("Game files"))
                .setItems(ways, (d, w) -> { if (w == 0) pickFolder(); else if (w == 1) pickZip(); else pickDriver(); })
                .show();
    }

    /** The Vulkan driver (GpuDrivers): the system's, or Turnip downloaded from GitHub (Adreno only). */
    private void pickDriver() {
        String sel = GpuDrivers.selected(this), failed = GpuDrivers.failed(this);
        String[] ways = {(sel == null ? "\u2713 " : "") + game.text("System driver"),
                         (sel != null ? "\u2713 " : "") + game.text("Turnip (download from GitHub)")};
        android.app.AlertDialog.Builder b = new android.app.AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(game.text("GPU driver"))
                .setItems(ways, (d, w) -> {
                    if (w == 0) { GpuDrivers.useSystem(this); android.widget.Toast.makeText(this, game.text("System driver"), android.widget.Toast.LENGTH_SHORT).show(); return; }
                    String label = game.text("Downloading the Turnip driver");
                    game.showBusy(label);
                    new Thread(() -> {
                        String err = GpuDrivers.downloadTurnip(this, t -> runOnUiThread(() -> game.showBusy(label + "  " + t)));
                        runOnUiThread(() -> {
                            game.showBusy(null);
                            android.widget.Toast.makeText(this, err != null ? err : game.text("Turnip installed"), android.widget.Toast.LENGTH_LONG).show();
                        });
                    }).start();
                });
        if (failed != null) b.setMessage(game.text("The last driver did not work; the system driver was used."));
        b.show();
    }

    private void pickZip() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/zip");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"});
        startActivityForResult(i, REQ_ZIP);
    }

    /** content://...tree/primary:Games/RaymanOrigins -> /storage/emulated/0/Games/RaymanOrigins (or /storage/XXXX-XXXX/... on a card) */
    static String treeToPath(Uri tree) {
        String id = DocumentsContract.getTreeDocumentId(tree);
        int c = id.indexOf(':');
        String vol = c < 0 ? id : id.substring(0, c), rel = c < 0 ? "" : id.substring(c + 1);
        String base = vol.equalsIgnoreCase("primary") ? Environment.getExternalStorageDirectory().getPath() : "/storage/" + vol;
        return rel.isEmpty() ? base : base + "/" + rel;
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        if (req == REQ_FOLDER && game != null) {
            String path = treeToPath(data.getData());
            boolean ok = isGame(path);
            if (ok) { prefs().edit().putString("game", path).apply(); makeLight(() -> game.gameChosen(true)); }
            else game.gameChosen(false);
        } else if (req == REQ_FOLDER) {
            String path = treeToPath(data.getData());
            if (isGame(path)) { prefs().edit().putString("game", path).apply(); status.setText(""); selected = 0; showMenu(); }
            else status.setText("Rayman Origins.exe is not in " + path + ". Select the game's own folder.");
        } else if (req == REQ_ZIP && game != null) {
            importZipGame(data.getData());
        } else if (req == REQ_ZIP) {
            importZip(data.getData());
        }
    }

    private void importZip(Uri zip) {
        busy = true;
        progress.setVisibility(View.VISIBLE);
        File dest = new File(getExternalFilesDir(null), "game");
        new Thread(() -> {
            String err = ZipImport.extract(this, zip, dest, (done, total, name) -> runOnUiThread(() -> {
                progress.setProgress(total > 0 ? (int) (done * 1000 / total) : 0);
                status.setText("Extracting " + name);
            }));
            runOnUiThread(() -> {
                busy = false;
                progress.setVisibility(View.GONE);
                String root = err == null ? ZipImport.gameRoot(dest) : null;
                if (root != null) { prefs().edit().putString("game", root).apply(); status.setText("Imported."); }
                else status.setText(err != null ? err : "Rayman Origins.exe was not found in the .zip.");
                selected = 0;
                showMenu();
            });
        }).start();
    }

    /** A .zip in the game's look: extracted into the app's folder; the light copy's own .zip becomes the game as it is
     *  (game_lite), a .zip of the whole game is made light afterwards (and its extracted files removed). */
    private void importZipGame(Uri zip) {
        if (making) return;
        making = true;
        File tmp = new File(getExternalFilesDir(null), "game_import"), lite = new File(getExternalFilesDir(null), "game_lite");
        String label = game.text("Importing the .zip");
        game.showBusy(label);
        new Thread(() -> {
            Packer.deleteAll(tmp);
            String err = ZipImport.extract(this, zip, tmp, (done, total, name) ->
                    runOnUiThread(() -> game.showBusy(label + (total > 0 ? "  " + done * 100 / total + "%" : ""))));
            String root = err == null ? ZipImport.gameRoot(tmp) : null;
            if (err == null && root == null) err = "Rayman Origins.exe was not found in the .zip.";
            boolean light = root != null && Packer.isLight(root);
            if (light) {                                  /* the light copy: it replaces the one there was */
                Packer.deleteAll(lite);
                if (!new File(root).renameTo(lite)) err = "Could not move the imported files.";
                Packer.deleteAll(tmp);
            } else if (err != null) Packer.deleteAll(tmp);
            final String e = err;
            runOnUiThread(() -> {
                making = false;
                game.showBusy(null);
                if (e != null) { android.widget.Toast.makeText(this, e, android.widget.Toast.LENGTH_LONG).show(); game.gameChosen(isGame(gameDir())); return; }
                prefs().edit().putString("game", light ? lite.getPath() : root).apply();
                if (light) game.gameChosen(true);
                else makeLight(() -> { if (Packer.isLight(gameDir())) Packer.deleteAll(tmp); game.gameChosen(isGame(gameDir())); });
            });
        }).start();
    }

    /** The light copy (Packer), once: made from the chosen folder into the app's folder, which then becomes the game;
     *  a copy that was itself in the app's folder (imported) is removed. `then` runs afterwards in any case. */
    private boolean making;
    void makeLight(Runnable then) {
        String src = gameDir();
        File dest = new File(getExternalFilesDir(null), "game_lite");
        boolean inApp = src.startsWith(getExternalFilesDir(null).getPath());
        if (making || game == null || Packer.isLight(src) || (!inApp && !storageAllowed())) { if (!making) then.run(); return; }
        making = true;
        Handler ui = new Handler(Looper.getMainLooper());
        String label = game.text("Making the light copy of the game (once)");
        Runnable tick = new Runnable() { public void run() { if (!making) return; game.showBusy(label + "  " + Packer.progress() / 10 + "%"); ui.postDelayed(this, 250); } };
        ui.post(tick);
        new Thread(() -> {
            String err = Packer.compress(new File(src), dest);
            ui.post(() -> {
                making = false;
                game.showBusy(null);
                if (err == null) {
                    prefs().edit().putString("game", dest.getPath()).apply();
                    if (inApp && !src.equals(dest.getPath())) Packer.deleteAll(new File(src));
                } else {
                    android.widget.Toast.makeText(this, err, android.widget.Toast.LENGTH_LONG).show();
                }
                then.run();
            });
        }).start();
    }

    void play() {
        if (game != null && !Packer.isLight(gameDir())) { makeLight(this::playNow); return; }
        playNow();
    }
    private void playNow() {
        boolean inApp = gameDir().startsWith(getExternalFilesDir(null).getPath());   /* imported from a .zip */
        if (!inApp && !storageAllowed()) return;
        Intent i = new Intent(this, GameActivity.class);
        i.putExtra("game", gameDir());
        i.putExtra("data", dataDir().getPath());
        startActivity(i);
    }

    /* ---- small things ---- */
    private void hideBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) getWindow().setDecorFitsSystemWindows(false);   /* edge to edge */
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && getWindow().getInsetsController() != null) {
            getWindow().getInsetsController().hide(WindowInsets.Type.systemBars());
            getWindow().getInsetsController().setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }
    private static FrameLayout.LayoutParams match() {
        return new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
    }
    private LinearLayout.LayoutParams wrap(int topDp) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        p.topMargin = st.dp(topDp);
        return p;
    }
}
