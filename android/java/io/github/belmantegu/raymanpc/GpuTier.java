package io.github.belmantegu.raymanpc;

import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Locale;

/**
 * The render resolution for a phone that has none chosen yet: from its GPU's name (GL_RENDERER, read once through a
 * throwaway 1x1 EGL context). The game draws at that height and the picture is stretched to the screen, so a weak
 * GPU gets fewer pixels to fill (measured on a Redmi 10C, Adreno 610: the frame's cost scales with the pixels).
 * Only the first time (no lines= in port_settings.txt); Options, Resolution changes it at any time.
 */
final class GpuTier {
    /** the GPU's name, or "" */
    static String renderer() {
        EGLDisplay d = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        if (d == EGL14.EGL_NO_DISPLAY) return "";
        int[] v = new int[2];
        if (!EGL14.eglInitialize(d, v, 0, v, 1)) return "";
        String r = "";
        try {
            EGLConfig[] cfg = new EGLConfig[1];
            int[] n = new int[1];
            int[] attrs = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE};
            if (!EGL14.eglChooseConfig(d, attrs, 0, cfg, 0, 1, n, 0) || n[0] == 0) return "";
            EGLContext ctx = EGL14.eglCreateContext(d, cfg[0], EGL14.EGL_NO_CONTEXT, new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            EGLSurface s = EGL14.eglCreatePbufferSurface(d, cfg[0], new int[] {EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
            if (EGL14.eglMakeCurrent(d, s, s, ctx)) {
                String g = GLES20.glGetString(GLES20.GL_RENDERER);
                if (g != null) r = g;
            }
            EGL14.eglMakeCurrent(d, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            EGL14.eglDestroySurface(d, s);
            EGL14.eglDestroyContext(d, ctx);
        } catch (Exception ignored) {
        } finally {
            EGL14.eglTerminate(d);
        }
        return r;
    }

    private static int number(String s, String after) {
        int i = s.indexOf(after);
        if (i < 0) return -1;
        i += after.length();
        while (i < s.length() && !Character.isDigit(s.charAt(i))) i++;
        int n = 0, k = 0;
        while (i < s.length() && Character.isDigit(s.charAt(i)) && k < 4) { n = n * 10 + (s.charAt(i++) - '0'); k++; }
        return k > 0 ? n : -1;
    }

    /** lines for this GPU (full: the screen's own height, at most 1080) */
    static int lines(String gpu, int screenH) {
        String g = gpu.toLowerCase(Locale.ROOT);
        int full = Math.min(screenH, 1080);
        if (g.contains("adreno")) {
            int n = number(g, "adreno");
            if (n >= 640) return full;                       /* 640+, 7xx, 8xx */
            if (n >= 616) return Math.min(full, 720);        /* 616 .. 630 */
            if (n > 0) return Math.min(full, 540);           /* 610, 612, 613, 5xx and older */
        }
        if (g.contains("xclipse") || g.contains("immortalis")) return full;
        if (g.contains("mali")) {
            int n = number(g, "mali-g");
            if (n >= 610) return full;                         /* G610 .. G720, G925 */
            if (n == 68 || (n >= 71 && n <= 78)) return Math.min(full, 720);
            return Math.min(full, 540);                       /* G31 G51 G52 G57, T-series */
        }
        if (g.contains("powervr")) return Math.min(full, 540);
        return Math.min(full, 720);
    }

    /** port_settings.txt without lines=: the GPU's choice is written (with gpu= for reference) */
    static void applyDefault(File dataDir, int screenH) {
        File f = new File(dataDir, "port_settings.txt");
        String text = "";
        try { text = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); } catch (Exception ignored) {}
        for (String l : text.split("\n")) if (l.startsWith("lines=")) return;
        String gpu = renderer();
        int lines = lines(gpu, screenH);
        StringBuilder b = new StringBuilder(text);
        if (b.length() > 0 && b.charAt(b.length() - 1) != '\n') b.append('\n');
        b.append("lines=").append(lines).append('\n').append("gpu=").append(gpu.replace('\n', ' ')).append('\n');
        try { Files.write(f.toPath(), b.toString().getBytes(StandardCharsets.UTF_8)); } catch (Exception ignored) {}
        android.util.Log.i("RaymanLauncher", "gpu '" + gpu + "': " + lines + " lines");
    }
    private GpuTier() {}
}
