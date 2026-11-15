package io.github.belmantegu.raymanpc;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** A .zip of the game folder, extracted into the app's folder (with its own top folder or without). */
final class ZipImport {
    interface Progress { void at(long done, long total, String name); }

    /** null on success, else the error to show. */
    static String extract(Context ctx, Uri zip, File dest, Progress p) {
        long total = size(ctx, zip), done = 0;
        byte[] buf = new byte[1 << 20];
        String destPath;
        try { destPath = dest.getCanonicalPath(); } catch (Exception e) { return e.getMessage(); }
        try (InputStream raw = ctx.getContentResolver().openInputStream(zip);
             ZipInputStream in = new ZipInputStream(new BufferedInputStream(raw, 1 << 20))) {
            ZipEntry e;
            long last = 0;
            while ((e = in.getNextEntry()) != null) {
                File out = new File(dest, e.getName());
                if (!out.getCanonicalPath().startsWith(destPath)) return "Bad path in the .zip: " + e.getName();
                if (e.isDirectory()) { out.mkdirs(); continue; }
                File parent = out.getParentFile();
                if (parent != null) parent.mkdirs();
                try (OutputStream o = new FileOutputStream(out)) {
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        o.write(buf, 0, n);
                        done += n;
                        if (done - last > (32 << 20)) { last = done; p.at(done, total, e.getName()); }
                    }
                }
            }
        } catch (Exception ex) {
            return "Could not extract the .zip: " + ex.getMessage();
        }
        return null;
    }

    /** The folder inside dest that holds Rayman Origins.exe (dest itself, or one level down). */
    static String gameRoot(File dest) {
        if (LauncherActivity.isGame(dest.getPath())) return dest.getPath();
        File[] subs = dest.listFiles();
        if (subs != null) for (File s : subs) if (s.isDirectory() && LauncherActivity.isGame(s.getPath())) return s.getPath();
        return null;
    }

    private static long size(Context ctx, Uri u) {
        try (Cursor c = ctx.getContentResolver().query(u, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getLong(0);
        } catch (Exception ignored) { }
        return 0;
    }
}
