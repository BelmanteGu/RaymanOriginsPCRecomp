package io.github.belmantegu.raymanpc;

import android.content.Context;
import android.content.Intent;
import android.content.res.AssetFileDescriptor;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.RectF;
import android.media.MediaPlayer;
import android.net.Uri;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The launcher in the game's own look (MenuArt draws the game's pieces): the title screen with the animated logo asks
 * for the player's Steam folder; the menu has the game's animated bars (Play, Options, Game files, Quit Game); the
 * settings are the game's Options screen with the port's settings, written to port_settings.txt in the data folder
 * (the runtime reads touch, size, opacity: touch.c; lang, lines: hle_advapi.c). The menu theme plays in a loop.
 * Touch, a pad (d-pad, A / Start, B / Back) or a keyboard. "Projeto Belmante" on the game's red plank, bottom left.
 */
final class GameMenu extends View {
    interface Host {
        boolean hasGame(); void pickFolder(); void play(); void quit(); File dataDir();
        /* over the game (GameActivity): close this screen, move the on-screen buttons, back to the launcher */
        default void closeMenu() {}
        default void editLayout() {}
        default void toLauncher() {}
    }

    private static final int ORANGE = 0xFFF89018, WHITE = 0xFFF8F8F8, SHADOW = 0xFF200000, FIELD = 0xFFF8ECE0, FOCUS = 0xFFFFD040;
    private final MenuArt art;
    private final Host host;
    private final boolean inGame;                         /* the Options screen over the game: no title, no menu */
    private final long t0 = SystemClock.uptimeMillis();
    private MediaPlayer music;
    private int screen;                                   /* 0 title, 1 menu, 2 settings */
    private int sel, f;
    private String status = "";
    private String busy;                                  /* a long job (the light copy): shown, and input waits */

    private static final String[] HOME = {"Play", "Options", "Game files", "Quit Game"};
    private static final String[] SIZES = {"Off", "S", "M", "L"};
    static final String[] LANGS = {"English", "Français", "Deutsch", "Español", "Italiano", "Português", "Polski", "Русский",
        "Nederlands", "Čeština", "Magyar", "Japanese"};                 /* the game's 12 (hle_advapi.c maps them) */
    private static final String[][] ROWS = {{"Defaults", "RESET"}, {"Language"}, {"Opacity", "50 %", "75 %", "100 %"},
        {"Stick", "Floating", "Fixed"}, {"Run", "Toggle", "Hold"}, {"Vibration", "On", "Off"}, {"Pad hides", "On", "Off"},
        {"Show FPS", "Off", "On"}, {"Game files", "Change"}};
    private final List<String> res = new ArrayList<>();
    private final List<String> focus = new ArrayList<>();
    /* the settings: size 0 off / 1..3 S M L, rows[i] the chosen value of ROWS[i], res, volumes 0..10 */
    private int size = 2, resIdx, music10 = 10, sfx10 = 10;
    private final int[] rows = new int[9];
    private int[] savedRows; private int savedSize, savedRes, savedMusic, savedSfx;

    GameMenu(Context c, MenuArt art, Host host) { this(c, art, host, false); }
    GameMenu(Context c, MenuArt art, Host host, boolean inGame) {
        super(c);
        this.art = art;
        this.host = host;
        this.inGame = inGame;
        if (inGame) screen = 2;
        setFocusable(true);
        setBackgroundColor(inGame ? 0x99000000 : Color.BLACK);
        DisplayMetrics m = c.getResources().getDisplayMetrics();
        int sw = Math.max(m.widthPixels, m.heightPixels), sh = Math.min(m.widthPixels, m.heightPixels);
        for (int h : new int[] {sh, 900, 720, 540}) {
            String r = ((sw * h / sh) & ~7) + " x " + h;
            if (h <= sh && !res.contains(r)) res.add(r);
        }
        focus.add("tabs");
        for (int i = 0; i < ROWS.length; i++) focus.add("row" + i);
        for (String s : new String[] {"res", "music", "sfx", "accept", "cancel"}) focus.add(s);
        if (inGame) focus.add("launcher");
        loadSettings();
    }

    /* ---- language: the port's own texts follow the language chosen here (fr de es it nl pt pl ru; else English) ---- */
    private static final int[] TCOL = {-1, 0, 1, 2, 3, 5, 6, 7, 4, -1, -1, -1};
    String text(String s) { return t(s); }
    private String t(String s) {
        String[] v = MenuText.I18N.get(s);
        int l = TCOL[rows[1] < TCOL.length ? rows[1] : 0];
        return v != null && l >= 0 ? v[l] : s;
    }

    /* ---- music: the game's menu theme ---- */
    void resume() {
        if (music == null) {
            try (AssetFileDescriptor fd = getContext().getAssets().openFd("launcher/menu_music.ogg")) {
                music = new MediaPlayer();
                music.setDataSource(fd.getFileDescriptor(), fd.getStartOffset(), fd.getLength());
                music.setLooping(true);
                music.prepare();
            } catch (Exception e) { music = null; }
        }
        if (music != null) { music.setVolume(music10 / 10f, music10 / 10f); music.start(); }
    }
    void pause() { if (music != null && music.isPlaying()) music.pause(); }
    void release() { if (music != null) { music.release(); music = null; } }
    /** over the game: open again with the saved values */
    void reopen() { loadSettings(); screen = 2; f = 0; invalidate(); }
    /** a long job's text ("Making the light copy 45%"), or null when it is over */
    void showBusy(String text) { busy = text; invalidate(); }
    void gameChosen(boolean ok) { status = ""; screen = ok ? 1 : 0; invalidate(); }

    /* ---- settings file (port_settings.txt; lines the launcher does not own are kept) ---- */
    private File file() { return new File(host.dataDir(), "port_settings.txt"); }
    private Map<String, String> read() {
        Map<String, String> kv = new LinkedHashMap<>();
        try {
            for (String l : new String(Files.readAllBytes(file().toPath()), StandardCharsets.UTF_8).split("\n")) {
                int i = l.indexOf('=');
                if (i > 0) kv.put(l.substring(0, i).trim(), l.substring(i + 1).trim());
            }
        } catch (Exception ignored) {}
        return kv;
    }
    private static int get(Map<String, String> kv, String k, int d) { try { return Integer.parseInt(kv.get(k)); } catch (Exception e) { return d; } }
    private void loadSettings() {
        Map<String, String> kv = read();
        size = get(kv, "touch", 1) != 0 ? get(kv, "size", 1) + 1 : 0;
        rows[1] = Math.max(0, Math.min(LANGS.length - 1, get(kv, "lang", 0)));
        rows[2] = get(kv, "opacity", 1); rows[3] = get(kv, "stick", 0); rows[4] = get(kv, "runhold", 0);
        rows[5] = get(kv, "vibration", 1) != 0 ? 0 : 1; rows[6] = get(kv, "padhides", 1) != 0 ? 0 : 1; rows[7] = get(kv, "showfps", 0);
        String lines = " " + get(kv, "lines", 720);
        resIdx = 0;
        for (int i = 0; i < res.size(); i++) if (res.get(i).endsWith(lines)) resIdx = i;
        music10 = get(kv, "music", 10); sfx10 = get(kv, "sfx", 10);
        remember();
    }
    private void remember() { savedRows = rows.clone(); savedSize = size; savedRes = resIdx; savedMusic = music10; savedSfx = sfx10; }
    private void saveSettings() {
        Map<String, String> kv = read();
        kv.put("touch", size > 0 ? "1" : "0"); kv.put("size", String.valueOf(Math.max(0, size - 1)));
        kv.put("opacity", String.valueOf(rows[2])); kv.put("lang", String.valueOf(rows[1])); kv.put("stick", String.valueOf(rows[3]));
        kv.put("runhold", String.valueOf(rows[4])); kv.put("vibration", rows[5] == 0 ? "1" : "0"); kv.put("padhides", rows[6] == 0 ? "1" : "0");
        kv.put("showfps", String.valueOf(rows[7])); kv.put("lines", res.get(resIdx).split(" x ")[1]);
        kv.put("music", String.valueOf(music10)); kv.put("sfx", String.valueOf(sfx10));
        StringBuilder b = new StringBuilder();
        for (Map.Entry<String, String> e : kv.entrySet()) b.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        try (FileOutputStream o = new FileOutputStream(file())) { o.write(b.toString().getBytes(StandardCharsets.UTF_8)); } catch (Exception ignored) {}
        remember();
    }

    /* ---- drawing: the stage (2340x1080) scaled to the screen; hit areas collected while drawing ---- */
    private final List<RectF> hitBox = new ArrayList<>();
    private final List<Runnable> hitDo = new ArrayList<>();
    private float scale = 1, ox, oy;
    private void hit(float x0, float y0, float x1, float y1, Runnable r) { hitBox.add(new RectF(x0, y0, x1, y1)); hitDo.add(r); }

    @Override
    protected void onDraw(Canvas c) {
        scale = Math.min(getWidth() / MenuArt.W, getHeight() / MenuArt.H);
        ox = (getWidth() - MenuArt.W * scale) / 2; oy = (getHeight() - MenuArt.H * scale) / 2;
        c.save();
        c.translate(ox, oy);
        c.scale(scale, scale);
        hitBox.clear(); hitDo.clear();
        long ms = SystemClock.uptimeMillis() - t0;
        if (screen == 0) drawTitle(c, ms); else if (screen == 1) drawHome(c, ms); else drawOpts(c);
        art.image(c, "settingsbackbut", art.box("brand"));                         /* "Projeto Belmante" */
        art.image(c, art.canal, 78, 968, 68, 68);
        art.text(c, "Projeto Belmante", 0.44f, WHITE, 335, 1003, SHADOW);
        hit(40, 960, 560, 1044, () -> getContext().startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse("https://www.youtube.com/@belmante"))));
        if (busy != null) {                               /* the job's band over everything */
            android.graphics.Paint p = new android.graphics.Paint();
            p.setColor(0xC0000000);
            c.drawRect(0, 860, MenuArt.W, 960, p);
            art.text(c, busy, 0.5f, WHITE, 1170, 912, SHADOW);
        }
        c.restore();
        postInvalidateOnAnimation();
    }

    private void drawTitle(Canvas c, long ms) {
        art.image(c, art.bgTitle, 0, 0, MenuArt.W, MenuArt.H);
        for (MenuArt.Draw d : art.frame(art.logo, ms)) art.patches(c, d, 0);
        int a = (int) (255 * (0.72 + 0.28 * Math.cos(ms / 1600.0 * 2 * Math.PI)));
        art.text(c, t("Select your Rayman Origins folder or .zip (Steam)"), 0.5f, (a << 24) | (WHITE & 0xFFFFFF), 1170, 858, (a << 24) | (SHADOW & 0xFFFFFF));
        art.text(c, t("Your own copy is read in place, nothing is downloaded"), 0.3f, 0xFFE8D8C0, 1170, 948, SHADOW);
        if (!status.isEmpty()) art.text(c, status, 0.36f, ORANGE, 1170, 1010, SHADOW);
        hit(0, 0, MenuArt.W, 940, this::titleGo);
    }
    private void titleGo() { if (!host.hasGame()) host.pickFolder(); else { screen = 1; status = ""; } }

    /* the carousel: pos follows sel with an ease-out (about 0.2 s); an item at slot s (fractional while moving) sits
     * 0.3 clip units per slot lower (the game's +1 / +2 bars: -0.3, -0.6), its big bar (with the hand) fading into
     * the small one as it leaves the centre */
    private float pos;
    private long lastMs;
    private static float slotClip(float s) { return -0.3f * s; }
    private void drawHome(Canvas c, long ms) {
        art.image(c, art.bgMenu, 0, 0, MenuArt.W, MenuArt.H);
        float dt = lastMs == 0 ? 0 : Math.min(0.1f, (ms - lastMs) / 1000f);
        lastMs = ms;
        pos += (sel - pos) * Math.min(1, dt * 14);
        if (Math.abs(sel - pos) < 0.002f) pos = sel;
        List<MenuArt.Draw> fr = art.frame(art.bars, ms);
        for (int i = 0; i < HOME.length; i++) {
            float s = i - pos, a = Math.abs(s);
            if (a > 2.6f) continue;
            float big = Math.max(0, 1 - a), small = Math.min(1, a), clip = slotClip(s), fade = a > 2 ? Math.max(0, 1 - (a - 2) / 0.6f) : 1;
            for (int k = 0; k < 3; k++) art.patches(c, fr.get(k), clip - fr.get(k).m[7], big);
            MenuArt.Draw d = fr.get(a < 1.5f ? 3 : 4);
            art.patches(c, d, clip - d.m[7], small * fade);
        }
        for (int i = 0; i < HOME.length; i++) {
            float s = i - pos, a = Math.abs(s);
            if (a > 2.6f) continue;
            float y = (1 - slotClip(s)) / 2 * MenuArt.H + 4, k = Math.min(1, a), fade = a > 2 ? Math.max(0, 1 - (a - 2) / 0.6f) : 1;
            int alpha = (int) (255 * fade) << 24;
            art.text(c, t(HOME[i]), 0.95f + (0.62f - 0.95f) * k, (k < 0.5f ? WHITE : ORANGE) & 0xFFFFFF | alpha, 1170, y, SHADOW & 0xFFFFFF | alpha);
            final int item = i;
            float h = a < 0.5f ? 80 : 48;
            if (fade > 0.5f) hit(700, y - h, 1700, y + h, () -> { if (item == sel) choose(); else sel = item; });
        }
        art.text(c, "Port 0.1 - 1.0.32504", 0.41f, WHITE, 2010, 974, SHADOW);
        if (!status.isEmpty()) art.text(c, t(status), 0.42f, ORANGE, 1170, 1020, SHADOW);
    }
    private void choose() {
        switch (HOME[sel]) {
            case "Play": host.play(); break;
            case "Options": screen = 2; f = 0; break;
            case "Game files": host.pickFolder(); break;
            default: host.quit(); break;
        }
    }

    private String rowLabel(int i) { return inGame && i == 8 ? "Button layout" : ROWS[i][0]; }
    private String rowValue(int i) {
        if (inGame && i == 8) return "Edit";
        return i == 1 ? LANGS[rows[1]] : ROWS[i][1 + Math.min(rows[i], ROWS[i].length - 2)];
    }
    private void drawOpts(Canvas c) {
        if (!inGame) art.image(c, art.bgMenu, 0, 0, MenuArt.W, MenuArt.H);   /* over the game: the game, dimmed */
        String on = focus.get(f);
        for (String[] p : new String[][] {{"settingscontrolsbg1", "ctrlhead"}, {"settingscontrolsbg2", "ctrllabels"}, {"settingscontrolsbg3", "ctrlvalues"},
            {"settingsdisplaybg", "display"}, {"settingssoundbg", "sound"}, {"settingsbackbut", "accept"}, {"settingsbackbut", "cancel"}})
            art.image(c, p[0], art.box(p[1]));
        art.text(c, t("Touch Controls"), 0.5f, WHITE, 771, 222, SHADOW);
        art.text(c, t("Size"), 0.42f, on.equals("tabs") ? FOCUS : WHITE, 548, 274, SHADOW);
        for (int i = 0; i < 4; i++) {
            boolean s = i == size;
            art.image(c, s ? "settingsplayerbutsel" : "settingsplayerbut", art.box(s ? "tabsel" : "tabs", i));
            art.text(c, t(SIZES[i]), 0.4f, s ? WHITE : 0xFFF8E8D8, 676 + i * 114, 274, SHADOW);
            final int k = i;
            hit(624 + i * 114, 250, 728 + i * 114, 296, () -> { f = 0; size = k; });
        }
        for (int i = 0; i < ROWS.length; i++) {
            float y = 362 + i * 50.75f;
            boolean fo = on.equals("row" + i);
            art.text(c, t(rowLabel(i)), 0.4f, fo ? WHITE : ORANGE, 551, y, SHADOW);
            art.image(c, fo ? "settingscontrolbutsel" : "settingscontrolbut", art.box(fo ? "fieldsel" : "field", i));
            art.text(c, t(rowValue(i)), 0.33f, fo ? WHITE : FIELD, 881, y, SHADOW);
            final int k = i;
            hit(444, y - 25, 1092, y + 25, () -> { if (f == k + 1) change(1); else f = k + 1; });
        }
        art.text(c, t("Display Options"), 0.5f, WHITE, 1513, 220, SHADOW);
        boolean fr = on.equals("res");
        art.text(c, t("Resolution"), 0.42f, fr ? FOCUS : WHITE, 1278, 297, SHADOW);
        art.image(c, "settingscontrolbutsel", art.box("resfield"));
        art.image(c, fr ? "settingsarrowleftsel" : "settingsarrowleft", art.box("arrowl"));
        art.image(c, fr ? "settingsarrowrightsel" : "settingsarrowright", art.box("arrowr"));
        art.text(c, res.get(resIdx), 0.47f, WHITE, 1624, 297, 0xFF503808);
        hit(1396, 256, 1520, 334, () -> { f = focus.indexOf("res"); change(-1); });
        hit(1700, 256, 1852, 334, () -> { f = focus.indexOf("res"); change(1); });
        art.text(c, t("Sound Options"), 0.5f, WHITE, 1509, 485, SHADOW);
        for (int i = 0; i < 2; i++) {
            String k = i == 0 ? "music" : "sfx";
            boolean fo = on.equals(k);
            float y = 555 + i * 72;
            art.text(c, t(i == 0 ? "Music Volume" : "SFX Volume"), 0.42f, fo ? WHITE : ORANGE, 1305, y, SHADOW);
            art.image(c, fo ? "settingssliderbgsel" : "settingssliderbg", art.box(fo ? "slidersel" : "slider", i));
            float v = (i == 0 ? music10 : sfx10) / 10f;
            android.graphics.Bitmap knob = art.tex.get(fo ? "settingsslidersel" : "settingsslider");
            org.json.JSONArray kb = art.box("knob", i);
            if (knob != null && kb != null) art.image(c, knob, (float) (1474 + v * 346 - kb.optDouble(2) / 2 + 8), (float) kb.optDouble(1), (float) kb.optDouble(2), (float) kb.optDouble(3));
            final String key = k;
            hit(1460, y - 30, 1860, y + 35, () -> f = focus.indexOf(key));
        }
        art.text(c, t("Accept"), 0.52f, on.equals("accept") ? WHITE : ORANGE, 1324, 758, SHADOW);
        art.text(c, t("Cancel"), 0.52f, on.equals("cancel") ? WHITE : ORANGE, 1703, 758, SHADOW);
        hit(1158, 720, 1490, 796, this::accept);
        hit(1544, 722, 1862, 794, this::cancel);
        if (inGame) {                                     /* back to the launcher: a plank like the brand's, on the right */
            org.json.JSONArray b = art.box("brand"), r = new org.json.JSONArray();   /* the brand's plank, moved right */
            try { for (int k = 0; k < 4; k++) r.put(k == 0 ? b.getDouble(0) + 1740 : b.getDouble(k)); } catch (Exception ignored) {}
            art.image(c, "settingsbackbut", r);
            art.text(c, t("Back to launcher"), 0.44f, on.equals("launcher") ? WHITE : ORANGE, 2040, 1003, SHADOW);
            hit(1780, 960, 2300, 1044, host::toLauncher);
        }
    }
    private void change(int d) {
        String on = focus.get(f);
        if (on.equals("tabs")) size = (size + d + 4) % 4;
        else if (on.startsWith("row")) {
            int i = Integer.parseInt(on.substring(3));
            if (i == 0) { size = 2; java.util.Arrays.fill(rows, 0); rows[2] = 1; resIdx = Math.min(2, res.size() - 1); music10 = sfx10 = 10; }
            else if (i == 8) { if (inGame) host.editLayout(); else host.pickFolder(); }
            else { int n = i == 1 ? LANGS.length : ROWS[i].length - 1; rows[i] = (rows[i] + d + n) % n; }
        } else if (on.equals("res")) resIdx = (resIdx + d + res.size()) % res.size();
        else if (on.equals("music")) { music10 = Math.max(0, Math.min(10, music10 + d)); if (music != null) music.setVolume(music10 / 10f, music10 / 10f); }
        else if (on.equals("sfx")) sfx10 = Math.max(0, Math.min(10, sfx10 + d));
        else if (on.equals("accept")) accept();
        else if (on.equals("cancel")) cancel();
        else if (on.equals("launcher")) host.toLauncher();
    }
    private void accept() { saveSettings(); status = "Settings saved"; if (inGame) host.closeMenu(); else screen = 1; }
    private void cancel() {
        System.arraycopy(savedRows, 0, rows, 0, rows.length); size = savedSize; resIdx = savedRes; music10 = savedMusic; sfx10 = savedSfx;
        if (music != null) music.setVolume(music10 / 10f, music10 / 10f);
        status = "";
        if (inGame) host.closeMenu(); else screen = 1;
    }

    /* ---- input ---- */
    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (e.getActionMasked() != MotionEvent.ACTION_UP || busy != null) return true;
        float x = (e.getX() - ox) / scale, y = (e.getY() - oy) / scale;
        for (int i = hitBox.size() - 1; i >= 0; i--) if (hitBox.get(i).contains(x, y)) { hitDo.get(i).run(); break; }
        invalidate();
        return true;
    }
    /** a pad / keyboard key: d-pad or arrows, A / Start / Enter choose, B / Back / Escape back */
    boolean key(KeyEvent e) {
        String k;
        switch (e.getKeyCode()) {
            case KeyEvent.KEYCODE_DPAD_UP: case KeyEvent.KEYCODE_W: k = "up"; break;
            case KeyEvent.KEYCODE_DPAD_DOWN: case KeyEvent.KEYCODE_S: k = "down"; break;
            case KeyEvent.KEYCODE_DPAD_LEFT: case KeyEvent.KEYCODE_A: k = "left"; break;
            case KeyEvent.KEYCODE_DPAD_RIGHT: case KeyEvent.KEYCODE_D: k = "right"; break;
            case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_BUTTON_START: case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_DPAD_CENTER: case KeyEvent.KEYCODE_SPACE: k = "ok"; break;
            case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_BACK: case KeyEvent.KEYCODE_ESCAPE: k = "back"; break;
            default: return false;
        }
        if (e.getAction() != KeyEvent.ACTION_DOWN || busy != null) return true;
        if (screen == 0) { if (k.equals("ok")) titleGo(); else if (k.equals("back")) host.quit(); }
        else if (screen == 1) {
            if (k.equals("down") && sel < HOME.length - 1) sel++;
            else if (k.equals("up") && sel > 0) sel--;
            else if (k.equals("ok")) choose();
            else if (k.equals("back")) screen = 0;
        } else {
            if (k.equals("down")) f = Math.min(focus.size() - 1, f + 1);
            else if (k.equals("up")) f = Math.max(0, f - 1);
            else if (k.equals("right") || k.equals("ok")) change(1);
            else if (k.equals("left")) change(-1);
            else cancel();
        }
        invalidate();
        return true;
    }
}
