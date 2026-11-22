package io.github.belmantegu.raymanpc;

import android.os.StatFs;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * The light copy of the player's game (about 0.6 GB instead of 2.4 GB), made once in the app's folder: the game's
 * program, its archive compressed (GameData/bundle_PC.lite: the sounds as Opus, libpack.so / runtime/src/lite_make.c)
 * and the intro and ending movies; the attract-mode movie (rolling_demo.bik, 162 MB) is left out (the game plays on
 * without it). The game reads the compressed archive as if it were the original (runtime/src/lite_read.c).
 */
final class Packer {
    static { System.loadLibrary("pack"); }

    static native String make(String ipk, String out, int threads);
    static native int progress();                         /* thousandths of the archive done */

    static final long NEED = 700L << 20;                  /* free space wanted for the copy */
    private static final String[] MOVIES = {"intro.bik", "cine_fin.bik", "sample.bik"};

    /** the folder's GameData has the compressed archive (and not the original) */
    static boolean isLight(String dir) {
        File gd = child(new File(dir), "GameData");
        return gd != null && child(gd, "bundle_PC.lite") != null && child(gd, "bundle_PC.ipk") == null;
    }

    /** null when it went well, or what went wrong */
    static String compress(File src, File dest) {
        File gd = child(src, "GameData"), ipk = gd != null ? child(gd, "bundle_PC.ipk") : null, exe = child(src, "Rayman Origins.exe");
        if (ipk == null || exe == null) return "The game's files were not found.";
        File out = new File(dest, "GameData");
        out.mkdirs();
        if (new StatFs(dest.getPath()).getAvailableBytes() < NEED) return "Not enough free space (0.7 GB) for the light copy.";
        try {
            copy(exe, new File(dest, "Rayman Origins.exe"));
            for (String m : MOVIES) { File f = child(gd, m); if (f != null) copy(f, new File(out, m)); }
        } catch (Exception e) { return "Could not copy the game's files: " + e.getMessage(); }
        int threads = Math.max(2, Math.min(6, Runtime.getRuntime().availableProcessors() - 2));
        return make(ipk.getPath(), new File(out, "bundle_PC.lite").getPath(), threads);
    }

    /** a file in dir whose name is `name` in any case, or null */
    static File child(File dir, String name) {
        String[] names = dir.list();
        if (names != null) for (String n : names) if (n.equalsIgnoreCase(name)) return new File(dir, n);
        return null;
    }
    static void deleteAll(File f) {
        File[] in = f.listFiles();
        if (in != null) for (File k : in) deleteAll(k);
        f.delete();
    }
    private static void copy(File a, File b) throws Exception {
        if (b.exists() && b.length() == a.length()) return;
        try (InputStream in = new FileInputStream(a); OutputStream o = new FileOutputStream(b)) {
            byte[] buf = new byte[1 << 20];
            int n;
            while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
        }
    }
}
