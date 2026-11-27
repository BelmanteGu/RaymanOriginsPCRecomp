package io.github.belmantegu.raymanpc;

import android.content.ContentResolver;
import android.net.Uri;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** The game's save, to and from the PC. The game keeps its progress in Documents\My Games\Rayman Origins\Savegame\
 * Savegame_0 on Windows; the runtime maps that drive path under <data>/drive_c (hle_k32_file.c), so the file here is
 * the very same file the PC game reads and writes (same executable, same format, nothing tied to the machine or the
 * user): exporting copies it out as "Savegame_0", importing copies one in (a raw Savegame_0, or a .zip holding one).
 * The file: 512 bytes of UTF-16 text the game shows on its save slot ("246/246 [icon:GPE_ELECTOON]  10/10
 * [icon:GPE_TOOTH]"), then UbiArt's serialized progress, which starts with the bytes 46 CB 43 0E at 0x208 (in saves of
 * the PC game and of this port alike): what tells a save from any other file.
 * The game has three files (slots), Savegame_0 to _2: a file goes to the slot its name says (the PC's Savegame_1 to the
 * second), Savegame_0 when the name says none. The save it replaces is kept as Savegame_N.bak; "restore" swaps them. */
final class SaveTransfer {
    private SaveTransfer() {}

    /** the game's three save files ("Select a file." shows them top to bottom): Savegame_0, _1, _2 */
    static final int SLOTS = 3;
    private static final int MAGIC_AT = 0x208, MAX = 4 << 20;
    private static final byte[] MAGIC = {0x46, (byte) 0xCB, 0x43, 0x0E};

    static String name(int slot) { return "Savegame_" + slot; }
    static File dir(File data) { return new File(data, "drive_c/Users/player/Documents/My Games/Rayman Origins/Savegame"); }
    static File file(File data, int slot) { return new File(dir(data), name(slot)); }
    static File backup(File data, int slot) { return new File(dir(data), name(slot) + ".bak"); }

    /** the slot a file's name says ("Savegame_1", "Savegame_1 (2)", ".../Savegame/Savegame_2"), or 0 */
    static int slotOf(String fileName) {
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("(?i)savegame_([0-9])").matcher(fileName == null ? "" : fileName);
        int k = m.find() ? m.group(1).charAt(0) - '0' : 0;
        return k < SLOTS ? k : 0;
    }

    static boolean isSave(byte[] d) {
        if (d == null || d.length < MAGIC_AT + 16 || d.length > MAX) return false;
        for (int i = 0; i < MAGIC.length; i++) if (d[MAGIC_AT + i] != MAGIC[i]) return false;
        return true;
    }

    /** the slot's text with the icons as words: "246/246 Electoons 10/10 <teeth>" */
    static String summary(byte[] d, String teeth) {
        if (d == null || d.length < 0x200) return "";
        String s = new String(d, 0, 0x200, StandardCharsets.UTF_16LE);
        int z = s.indexOf('\0');
        if (z >= 0) s = s.substring(0, z);
        return s.replace("[icon:GPE_ELECTOON]", "Electoons").replace("[icon:GPE_TOOTH]", teeth).replaceAll("\\s+", " ").trim();
    }

    static byte[] read(File f) {
        if (!f.isFile() || f.length() > MAX) return null;
        try (InputStream in = new FileInputStream(f)) { byte[] d = all(in); return isSave(d) ? d : null; } catch (IOException e) { return null; }
    }

    /** a save picked by the player, and the slot its name says */
    static final class Found { final byte[] data; final int slot; Found(byte[] d, int s) { data = d; slot = s; } }

    /** the file itself, or the first save inside it when it is a .zip; null if there is none */
    static Found read(ContentResolver cr, Uri u) {
        String shown = null;
        try (android.database.Cursor c = cr.query(u, new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) shown = c.getString(0);
        } catch (RuntimeException e) { /* no name: slot 0 */ }
        byte[] d;
        try (InputStream in = cr.openInputStream(u)) { d = in == null ? null : all(in); } catch (IOException e) { return null; }
        if (d == null) return null;
        if (isSave(d)) return new Found(d, slotOf(shown));
        if (d.length > 4 && d[0] == 'P' && d[1] == 'K' && d[2] == 3 && d[3] == 4) {
            try (ZipInputStream z = new ZipInputStream(new java.io.ByteArrayInputStream(d))) {
                for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                    if (e.isDirectory() || !new File(e.getName()).getName().toLowerCase().startsWith("savegame")) continue;
                    byte[] s = all(z);
                    if (isSave(s)) return new Found(s, slotOf(e.getName()));
                }
            } catch (IOException e) { return null; }
        }
        return null;
    }

    static boolean write(ContentResolver cr, Uri u, byte[] d) {
        try (OutputStream out = cr.openOutputStream(u, "wt")) {
            if (out == null) return false;
            out.write(d);
            return true;
        } catch (IOException e) { return false; }
    }

    /** puts d in the slot; the save it replaces becomes Savegame_N.bak */
    static boolean install(File data, int slot, byte[] d) {
        File dir = dir(data), f = file(data, slot), bak = backup(data, slot), tmp = new File(dir, name(slot) + ".new");
        dir.mkdirs();
        try (FileOutputStream out = new FileOutputStream(tmp)) { out.write(d); out.getFD().sync(); } catch (IOException e) { tmp.delete(); return false; }
        if (f.isFile()) { bak.delete(); if (!f.renameTo(bak)) { tmp.delete(); return false; } }
        else bak.delete();                                   /* an empty slot: restoring empties it again */
        return tmp.renameTo(f);
    }

    /** the slot and its .bak change places (an import undone, or redone) */
    static boolean restore(File data, int slot) {
        File f = file(data, slot), bak = backup(data, slot), tmp = new File(dir(data), name(slot) + ".swap");
        if (!bak.isFile()) return false;
        tmp.delete();
        if (f.isFile() && !f.renameTo(tmp)) return false;
        if (!bak.renameTo(f)) { tmp.renameTo(f); return false; }
        if (tmp.isFile()) tmp.renameTo(bak);
        return true;
    }

    private static byte[] all(InputStream in) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[16384];
        for (int n; (n = in.read(buf)) > 0; ) {
            b.write(buf, 0, n);
            if (b.size() > MAX) throw new IOException("too big");
        }
        return b.toByteArray();
    }
}
