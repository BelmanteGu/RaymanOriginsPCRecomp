package io.github.belmantegu.raymanpc;

import android.content.Context;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The Vulkan driver the game uses on an Adreno: the system's (Qualcomm's), or Turnip (Mesa), downloaded once from its
 * GitHub releases. Layout read by the runtime (runtime/src/gpu_driver.c, through libadrenotools):
 * files/gpu_drivers/<folder>/{meta.json, the library}, files/gpu_drivers/selected (the folder, then the library's file
 * name), files/gpu_drivers/failed (a driver that never drew: the runtime went back to the system's).
 */
final class GpuDrivers {
    /** Turnip (Mesa 26.3.0) for Adreno 6xx/7xx, in the adrenotools pack format (meta.json + libvulkan_freedreno.so).
     *  Pinned: Turnip before Mesa 26.3 offers only Vulkan 1.0 on an Adreno 610 (no hardware multiview; Mesa MR !43117
     *  added it in software), and DXVK needs 1.1; with 26.3 the 610 is Vulkan 1.3 with BC textures. */
    static final String TURNIP_URL = "https://github.com/StevenMXZ/Adreno-Tools-Drivers/releases/download/v37/Turnip_v26.3.0-R6.zip";

    interface Progress { void at(String text); }

    static File root(Context c) { File r = new File(c.getFilesDir(), "gpu_drivers"); r.mkdirs(); return r; }

    /** the selected driver's folder, or null for the system's */
    static String selected(Context c) {
        try {
            String s = new String(java.nio.file.Files.readAllBytes(new File(root(c), "selected").toPath()), "UTF-8");
            String folder = s.split("\n")[0].trim();
            return folder.isEmpty() ? null : folder;
        } catch (Exception e) { return null; }
    }
    /** a driver the runtime gave up on (it never drew), or null */
    static String failed(Context c) {
        try {
            String s = new String(java.nio.file.Files.readAllBytes(new File(root(c), "failed").toPath()), "UTF-8");
            return s.split("\n")[0].trim();
        } catch (Exception e) { return null; }
    }
    static void useSystem(Context c) { new File(root(c), "selected").delete(); new File(root(c), "failed").delete(); }

    /** Downloads the Turnip driver's zip from GitHub and selects it. null, or what went wrong. */
    static String downloadTurnip(Context c, Progress p) {
        try {
            String url = TURNIP_URL, asset = "Turnip 26.3.0";
            File zip = new File(c.getCacheDir(), "turnip.zip");
            download(url, zip, p, asset);
            String folder = install(c, zip, "turnip");
            zip.delete();
            return folder == null ? "The driver zip has no library." : null;
        } catch (Exception e) {
            return "Could not download the driver: " + e.getMessage();
        }
    }

    /** a driver zip (meta.json + .so) into files/gpu_drivers/<folder>/, selected; the folder, or null */
    static String install(Context c, File zip, String folder) throws Exception {
        File dir = new File(root(c), folder);
        Packer.deleteAll(dir);
        dir.mkdirs();
        String lib = null, metaLib = null;
        try (InputStream in = new java.io.FileInputStream(zip); ZipInputStream z = new ZipInputStream(in)) {
            ZipEntry e;
            byte[] buf = new byte[1 << 16];
            while ((e = z.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                String base = new File(e.getName()).getName();      /* (nothing outside the folder) */
                if (!base.equalsIgnoreCase("meta.json") && !base.endsWith(".so")) continue;
                if (base.equals("vulkan.adreno.so")) base = "vulkan.turnip.so";   /* (the system's name is taken) */
                try (OutputStream o = new FileOutputStream(new File(dir, base))) {
                    int n;
                    while ((n = z.read(buf)) > 0) o.write(buf, 0, n);
                }
                if (base.equalsIgnoreCase("meta.json")) {
                    try { metaLib = new JSONObject(new String(java.nio.file.Files.readAllBytes(new File(dir, base).toPath()), "UTF-8")).optString("libraryName", null); }
                    catch (Exception ignored) {}
                } else if (lib == null || base.contains("vulkan")) lib = base;
            }
        }
        if (metaLib != null && new File(dir, metaLib).exists()) lib = metaLib;
        if (lib == null) { Packer.deleteAll(dir); return null; }
        try (FileWriter w = new FileWriter(new File(root(c), "selected"))) { w.write(folder + "\n" + lib + "\n"); }
        new File(root(c), "failed").delete();
        new File(root(c), "trial").delete();
        return folder;
    }

    @SuppressWarnings("unused")
    private static String get(String url) throws Exception {
        HttpURLConnection h = (HttpURLConnection) new URL(url).openConnection();
        h.setRequestProperty("Accept", "application/vnd.github+json");
        h.setRequestProperty("User-Agent", "rayman-origins-pc-port");
        h.setConnectTimeout(15000); h.setReadTimeout(30000);
        try (InputStream in = h.getInputStream()) {
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            byte[] b = new byte[1 << 14];
            int n;
            while ((n = in.read(b)) > 0) o.write(b, 0, n);
            return o.toString("UTF-8");
        } finally { h.disconnect(); }
    }
    private static void download(String url, File to, Progress p, String label) throws Exception {
        HttpURLConnection h = (HttpURLConnection) new URL(url).openConnection();
        h.setRequestProperty("User-Agent", "rayman-origins-pc-port");
        h.setInstanceFollowRedirects(true);
        h.setConnectTimeout(15000); h.setReadTimeout(60000);
        long total = h.getContentLengthLong(), done = 0;
        try (InputStream in = h.getInputStream(); OutputStream o = new FileOutputStream(to)) {
            byte[] b = new byte[1 << 16];
            int n;
            long last = 0;
            while ((n = in.read(b)) > 0) {
                o.write(b, 0, n);
                done += n;
                if (done - last > (256 << 10)) { last = done; p.at(label + (total > 0 ? "  " + done * 100 / total + "%" : "")); }
            }
        } finally { h.disconnect(); }
    }
    private GpuDrivers() {}
}
