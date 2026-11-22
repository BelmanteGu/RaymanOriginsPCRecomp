package io.github.belmantegu.raymanpc;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.util.SparseArray;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.GZIPInputStream;

/**
 * The game's own menu art for the launcher (GameMenu), from assets/launcher/ (tools/launcher_mock.py makes them from
 * the player's copy; a personal build packs them): the title and menu scenes, the Options screen's textures, the font
 * (font02) and the title logo / menu bars animations.
 *
 * The animations are the game's: pieces of an atlas, each a cubic Bezier patch (8 control points: position xy, UV zw;
 * 4 corner colours; the draw's matrix), recorded per frame by tools/geo_anim.py and drawn here as the game's
 * default_PCT_Patch_VS evaluates them (a 3x3 grid per patch, as the game's vertex buffer has).
 * Everything is drawn on a 2340x1080 stage (the captures' size); GameMenu scales it to the screen.
 */
final class MenuArt {
    static final float W = 2340, H = 1080;
    Bitmap bgTitle, bgMenu, font, canal;
    final Map<String, Bitmap> tex = new HashMap<>();
    final SparseArray<int[]> chars = new SparseArray<>();      /* x, y, w, h, xoffset, yoffset, xadvance */
    JSONObject geo;
    Anim logo, bars;

    static final class Draw { String atlas; float[] m = new float[16]; float[] vc = new float[4]; int n; float[] pts; float[] col; }
    static final class Anim { int fps; final List<List<Draw>> frames = new ArrayList<>(); }

    static boolean available(Context c) {
        try (InputStream in = c.getAssets().open("launcher/geo.json")) { return true; } catch (Exception e) { return false; }
    }

    static MenuArt load(Context c) throws Exception {
        MenuArt a = new MenuArt();
        a.bgTitle = bitmap(c, "launcher/bg_title.jpg");
        a.bgMenu = bitmap(c, "launcher/bg_menu.jpg");
        a.font = bitmap(c, "launcher/font.png");
        a.canal = BitmapFactory.decodeResource(c.getResources(), R.drawable.canal_logo);
        for (String n : c.getAssets().list("launcher/tex"))
            a.tex.put(n.replace(".png", ""), bitmap(c, "launcher/tex/" + n));
        a.geo = new JSONObject(text(c, "launcher/geo.json", false));
        JSONObject fc = new JSONObject(text(c, "launcher/font.json", false)).getJSONObject("chars");
        for (java.util.Iterator<String> it = fc.keys(); it.hasNext(); ) {
            String k = it.next();
            JSONArray v = fc.getJSONArray(k);
            int[] g = new int[7];
            for (int i = 0; i < 7; i++) g[i] = v.getInt(i);
            a.chars.put(Integer.parseInt(k), g);
        }
        a.logo = anim(c, "launcher/logo.json.gz");
        a.bars = anim(c, "launcher/bars.json.gz");
        return a;
    }

    private static Bitmap bitmap(Context c, String path) throws Exception {
        try (InputStream in = c.getAssets().open(path)) { return BitmapFactory.decodeStream(in); }
    }
    private static String text(Context c, String path, boolean gz) throws Exception {
        try (InputStream raw = c.getAssets().open(path); InputStream in = gz ? new GZIPInputStream(raw) : raw) {
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            byte[] b = new byte[65536];
            for (int n; (n = in.read(b)) > 0; ) o.write(b, 0, n);
            return new String(o.toByteArray(), StandardCharsets.UTF_8);
        }
    }
    /* a frame's draw: [atlas, matrix c0..c3 (1e-5), colour c13 (%), patches, control points (1e-3), corner colours (% or 1)] */
    private static Anim anim(Context c, String path) throws Exception {
        JSONObject j = new JSONObject(text(c, path, true));
        Anim a = new Anim();
        a.fps = j.getInt("fps");
        JSONArray frames = j.getJSONArray("frames");
        for (int f = 0; f < frames.length(); f++) {
            JSONArray fr = frames.getJSONArray(f);
            List<Draw> list = new ArrayList<>();
            for (int i = 0; i < fr.length(); i++) {
                JSONArray d = fr.getJSONArray(i);
                Draw dr = new Draw();
                dr.atlas = d.getString(0);
                JSONArray m = d.getJSONArray(1), vc = d.getJSONArray(2), pts = d.getJSONArray(4);
                for (int k = 0; k < 16; k++) dr.m[k] = m.getInt(k) / 1e5f;
                for (int k = 0; k < 4; k++) dr.vc[k] = vc.getInt(k) / 100f;
                dr.n = d.getInt(3);
                dr.pts = new float[pts.length()];
                for (int k = 0; k < pts.length(); k++) dr.pts[k] = pts.getInt(k) / 1000f;
                Object col = d.get(5);
                if (col instanceof JSONArray) {
                    JSONArray cl = (JSONArray) col;
                    dr.col = new float[cl.length()];
                    for (int k = 0; k < cl.length(); k++) dr.col[k] = cl.getInt(k) / 100f;
                }
                list.add(dr);
            }
            a.frames.add(list);
        }
        return a;
    }

    List<Draw> frame(Anim a, long ms) { return a.frames.get((int) (ms * a.fps / 1000 % a.frames.size())); }

    /* ---- drawing on the stage ---- */
    private final Paint glyph = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG), img = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final Rect src = new Rect();
    private final RectF dst = new RectF();

    /** the game's font, centred on (cx, cy), with its dark drop shadow (0 = none) */
    void text(Canvas c, String s, float scale, int color, float cx, float cy, int shadow) {
        float w = 0;
        for (int i = 0; i < s.length(); ) { int cp = s.codePointAt(i); w += g(cp)[6]; i += Character.charCount(cp); }
        w *= scale;
        float sh = Math.max(2, scale * 7);
        if (shadow != 0) run(c, s, scale, shadow, cx - w / 2 + sh * .55f, cy + sh);
        run(c, s, scale, color, cx - w / 2, cy);
    }
    private int[] g(int cp) { int[] v = chars.get(cp); return v != null ? v : chars.get('?'); }
    private void run(Canvas c, String s, float scale, int color, float x, float cy) {
        glyph.setColorFilter(new PorterDuffColorFilter(color, PorterDuff.Mode.SRC_IN));
        float top = cy - 57 * scale;
        for (int i = 0; i < s.length(); ) {
            int cp = s.codePointAt(i);
            int[] v = g(cp);
            src.set(v[0], v[1], v[0] + v[2], v[1] + v[3]);
            dst.set(x + v[4] * scale, top + v[5] * scale, x + (v[4] + v[2]) * scale, top + (v[5] + v[3]) * scale);
            c.drawBitmap(font, src, dst, glyph);
            x += v[6] * scale;
            i += Character.charCount(cp);
        }
    }
    float textWidth(String s, float scale) { float w = 0; for (int i = 0; i < s.length(); i++) w += g(s.charAt(i))[6]; return w * scale; }

    /** a texture of the Options screen at a box [x, y, w, h] of geo.json */
    void image(Canvas c, String name, JSONArray box) {
        Bitmap b = tex.get(name);
        if (b == null || box == null) return;
        dst.set((float) box.optDouble(0), (float) box.optDouble(1), (float) (box.optDouble(0) + box.optDouble(2)), (float) (box.optDouble(1) + box.optDouble(3)));
        c.drawBitmap(b, null, dst, img);
    }
    void image(Canvas c, Bitmap b, float x, float y, float w, float h) { dst.set(x, y, x + w, y + h); c.drawBitmap(b, null, dst, img); }
    JSONArray box(String k) { return geo.optJSONArray(k); }
    JSONArray box(String k, int i) { JSONArray a = geo.optJSONArray(k); return a != null ? a.optJSONArray(i) : null; }

    /* ---- the game's patches ---- */
    private final Map<String, Paint> shaders = new HashMap<>();
    private static final float[] G3 = {0, .5f, 1};
    private float[] verts = new float[0], texs = new float[0];
    private int[] cols = new int[0];

    /** one draw of an animation frame; dy: an extra vertical offset in clip units (the menu's other slots) */
    void patches(Canvas c, Draw d, float dy) {
        Bitmap atlas = tex.get(d.atlas.equals("bubble") ? "bubblemenu_a" : d.atlas);
        if (atlas == null) return;
        Paint p = shaders.get(d.atlas);
        if (p == null) {
            p = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
            p.setShader(new BitmapShader(atlas, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP));
            shaders.put(d.atlas, p);
        }
        int nv = d.n * 24;
        if (verts.length < nv * 2) { verts = new float[nv * 2]; texs = new float[nv * 2]; cols = new int[nv]; }
        float[] gx = new float[9], gy = new float[9], gu = new float[9], gv = new float[9];
        int[] gc = new int[9];
        float[] m = d.m;
        int o = 0;
        for (int pa = 0; pa < d.n; pa++) {
            int k = 0;
            for (float t : G3) for (float s : G3) {
                float u = 1 - s, b0 = u * u * u, b1 = 3 * s * u * u, b2 = 3 * s * s * u, b3 = s * s * s;
                float[] r = new float[4];
                for (int ch = 0; ch < 4; ch++) {
                    float f1 = cp(d, pa, 0, ch) * (1 - t) + cp(d, pa, 4, ch) * t, f1x = cp(d, pa, 1, ch) * (1 - t) + cp(d, pa, 5, ch) * t;
                    float fx2 = cp(d, pa, 2, ch) * (1 - t) + cp(d, pa, 6, ch) * t, f2 = cp(d, pa, 3, ch) * (1 - t) + cp(d, pa, 7, ch) * t;
                    r[ch] = f1 * b0 + f1x * b1 + fx2 * b2 + f2 * b3;
                }
                float x = m[0] * r[0] + m[1] * r[1] + m[3], y = m[4] * r[0] + m[5] * r[1] + m[7] + dy, w = m[12] * r[0] + m[13] * r[1] + m[15];
                gx[k] = (x / w + 1) / 2 * W;
                gy[k] = (1 - y / w) / 2 * H;
                gu[k] = r[2] * atlas.getWidth();
                gv[k] = r[3] * atlas.getHeight();
                float[] cc = new float[4];
                for (int ch = 0; ch < 4; ch++) {
                    float c0 = corner(d, pa, 0, ch), c1 = corner(d, pa, 1, ch), c2 = corner(d, pa, 2, ch), c3 = corner(d, pa, 3, ch);
                    float l1 = c0 + (c1 - c0) * s, l2 = c2 + (c3 - c2) * s;
                    cc[ch] = Math.min(1, (l1 + (l2 - l1) * t) * d.vc[ch]);
                }
                gc[k] = ((int) (cc[3] * 255) << 24) | ((int) (cc[0] * 255) << 16) | ((int) (cc[1] * 255) << 8) | (int) (cc[2] * 255);
                k++;
            }
            for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
                int a = j * 3 + i, b = a + 1, cc2 = a + 3, e = a + 4;
                for (int q : new int[] {a, b, e, a, e, cc2}) {
                    verts[o * 2] = gx[q]; verts[o * 2 + 1] = gy[q]; texs[o * 2] = gu[q]; texs[o * 2 + 1] = gv[q]; cols[o] = gc[q]; o++;
                }
            }
        }
        c.drawVertices(Canvas.VertexMode.TRIANGLES, o * 2, verts, 0, texs, 0, cols, 0, null, 0, 0, p);
    }
    private static float cp(Draw d, int pa, int k, int ch) { return d.pts[(8 * pa + k) * 4 + ch]; }
    private static float corner(Draw d, int pa, int k, int ch) { return d.col == null ? 1 : d.col[16 * pa + 4 * k + ch]; }
}
