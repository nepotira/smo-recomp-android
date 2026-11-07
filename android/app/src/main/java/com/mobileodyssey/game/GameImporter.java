/* GameImporter - the player's own NSP or XCI, with their own prod.keys, into the files the runtime reads.
 *
 * A port of tools/extract (switchfs.py, extract.py, romfs.py --manual) for the launcher, so a phone needs no PC:
 *   <out>/dump/base/exefs/<name>   the ExeFS files as stored (main, sdk, subsdk0, rtld, main.npdm)
 *   <out>/dump/base/elf/<name>.elf every NSO converted to ELF (image at file offset 0x1000), as switchfs.nso_to_elf
 *   <out>/romfs/...                the Program NCA's RomFS
 *   <out>/manual/...               the Manual NCAs' RomFS (the game's Help and legal pages)
 * Only version 1.0.0 is accepted (the code in the app is recompiled from it): the ExeFS `main` must have the 1.0.0
 * hash. Formats: NSP (PFS0) with its ticket, XCI (the secure HFS0 partition). The keys are read, used and dropped;
 * nothing is kept or sent anywhere. No Android classes, so it can be run and checked on a PC (main()).
 * Android port (2026-11-02). */
package com.mobileodyssey.game;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import javax.crypto.Cipher;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

public class GameImporter {
    static final long TITLE_ID = 0x0100000000010000L;                               /* Super Mario Odyssey, base game */
    static final String MAIN_1_0_0_SHA256 = "e21692d90f8fd2def2d2d22d983d62ac81df3b8b3c762d1f2dca9d9ab7b3053a";
    static final int TYPE_PROGRAM = 0, TYPE_MANUAL = 3;

    public interface Progress { void say(String s); }

    /* Positional reads of the package file (a FileChannel: on Android one opened from the picked document). */
    final FileChannel ch;
    final Map<String, byte[]> keys;
    final Progress progress;

    public GameImporter(FileChannel ch, Map<String, byte[]> keys, Progress progress) {
        this.ch = ch; this.keys = keys; this.progress = progress;
    }

    interface Reader { byte[] read(long off, int n) throws IOException; }

    byte[] readFile(long off, int n) throws IOException {
        ByteBuffer b = ByteBuffer.allocate(n);
        while (b.hasRemaining()) {
            int r = ch.read(b, off + b.position());
            if (r < 0) throw new IOException("the file ends early (a broken or partial dump?)");
        }
        return b.array();
    }

    /* ------------------------------------------------------------ keys */
    public static Map<String, byte[]> parseKeys(String text) {
        Map<String, byte[]> out = new HashMap<>();
        for (String line : text.split("\\r?\\n")) {
            int eq = line.indexOf('=');
            if (eq < 0) continue;
            String k = line.substring(0, eq).trim().toLowerCase(), v = line.substring(eq + 1).trim();
            if (v.length() % 2 != 0 || !v.matches("[0-9a-fA-F]+")) continue;
            byte[] b = new byte[v.length() / 2];
            for (int i = 0; i < b.length; i++) b[i] = (byte) Integer.parseInt(v.substring(2 * i, 2 * i + 2), 16);
            out.put(k, b);
        }
        return out;
    }

    byte[] named(String name) throws IOException {
        byte[] k = keys.get(name);
        if (k == null) throw new IOException("\"" + name + "\" is missing from your prod.keys (dump them again from your console)");
        return k;
    }

    /* ------------------------------------------------------------ crypto */
    static byte[] ecb(byte[] key, byte[] data, boolean decrypt) throws IOException {
        try {
            Cipher c = Cipher.getInstance("AES/ECB/NoPadding");
            c.init(decrypt ? Cipher.DECRYPT_MODE : Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"));
            return c.doFinal(data);
        } catch (Exception e) { throw new IOException("AES: " + e); }
    }

    /* AES-128-XTS with the Nintendo (big-endian) sector tweak, as switchfs.xts_decrypt */
    static byte[] xtsDecrypt(byte[] key, byte[] data, long sector) throws IOException {
        byte[] k1 = new byte[16], k2 = new byte[16];
        System.arraycopy(key, 0, k1, 0, 16);
        System.arraycopy(key, 16, k2, 0, 16);
        byte[] out = new byte[data.length];
        try {
            Cipher d = Cipher.getInstance("AES/ECB/NoPadding"), e = Cipher.getInstance("AES/ECB/NoPadding");
            d.init(Cipher.DECRYPT_MODE, new SecretKeySpec(k1, "AES"));
            e.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(k2, "AES"));
            for (int s = 0; s < data.length / 0x200; s++) {
                byte[] sec = new byte[16];
                long n = sector + s;
                for (int i = 0; i < 8; i++) sec[15 - i] = (byte) (n >>> (8 * i));
                byte[] t = e.doFinal(sec);                                          /* the tweak, little-endian as a number */
                for (int b = 0; b < 0x200; b += 16) {
                    int at = s * 0x200 + b;
                    byte[] blk = new byte[16];
                    for (int i = 0; i < 16; i++) blk[i] = (byte) (data[at + i] ^ t[i]);
                    blk = d.doFinal(blk);
                    for (int i = 0; i < 16; i++) out[at + i] = (byte) (blk[i] ^ t[i]);
                    int carry = 0;                                                  /* tweak *= x in GF(2^128) */
                    for (int i = 0; i < 16; i++) {
                        int v = ((t[i] & 0xff) << 1) | carry;
                        carry = (t[i] & 0x80) != 0 ? 1 : 0;
                        t[i] = (byte) v;
                    }
                    if (carry != 0) t[0] ^= (byte) 0x87;
                }
            }
        } catch (Exception ex) { throw new IOException("AES-XTS: " + ex); }
        return out;
    }

    /* ------------------------------------------------------------ PFS0 / HFS0 */
    static class Pfs {
        final Reader r;
        final Map<String, long[]> files = new LinkedHashMap<>();                    /* name -> {offset, size} */
        Pfs(Reader r, long at) throws IOException {
            this.r = r;
            ByteBuffer h = le(r.read(at, 16));
            int magic = h.getInt(0), count = h.getInt(4), strSize = h.getInt(8);
            boolean hfs = magic == 0x30534648;                                      /* 'HFS0': 0x40-byte entries */
            if (!hfs && magic != 0x30534650) throw new IOException("not a PFS0/HFS0 (an NSP or XCI?)");
            int es = hfs ? 0x40 : 0x18;
            byte[] table = r.read(at + 16, es * count + strSize);
            ByteBuffer t = le(table);
            long base = at + 16 + (long) es * count + strSize;
            for (int i = 0; i < count; i++) {
                long off = t.getLong(i * es), size = t.getLong(i * es + 8);
                int nameOff = t.getInt(i * es + 16), s = es * count + nameOff, e = s;
                while (table[e] != 0) e++;
                files.put(new String(table, s, e - s, StandardCharsets.UTF_8), new long[]{base + off, size});
            }
        }
        byte[] read(String name) throws IOException {
            long[] f = files.get(name);
            if (f == null) throw new IOException(name + " is missing");
            return r.read(f[0], (int) f[1]);
        }
    }

    /* The container's files: an NSP's PFS0, or an XCI's secure partition. */
    Pfs open() throws IOException {
        byte[] head = readFile(0, 0x200);
        if (head[0] == 'P' && head[1] == 'F' && head[2] == 'S' && head[3] == '0') return new Pfs(this::readFile, 0);
        for (long base : new long[]{0, 0x1000}) {                                   /* 0x1000: a dump with its key area in front */
            byte[] h = readFile(base + 0x100, 0x40);
            if (h[0] == 'H' && h[1] == 'E' && h[2] == 'A' && h[3] == 'D') {
                long rootAt = base + le(h).getLong(0x30);
                Pfs root = new Pfs(this::readFile, rootAt);
                long[] sec = root.files.get("secure");
                if (sec == null) throw new IOException("this XCI has no secure partition");
                return new Pfs(this::readFile, sec[0]);
            }
        }
        throw new IOException("not an NSP or XCI");
    }

    /* ------------------------------------------------------------ NCA */
    class Nca {
        final String name;
        final long at;
        final byte[] header;
        final int type, keyGen;
        final long titleId;
        final byte[] rights = new byte[16];
        final List<Section> sections = new ArrayList<>();
        final Pfs pkg;
        byte[] key;

        class Section {
            int index, fsType, crypto;
            long offset, size;
            byte[] fs, ctr = new byte[8];
        }

        Nca(Pfs pkg, String name) throws IOException {
            this.pkg = pkg; this.name = name;
            at = pkg.files.get(name)[0];
            header = xtsDecrypt(named("header_key"), readFile(at, 0xC00), 0);
            if (header[0x200] != 'N' || header[0x201] != 'C' || header[0x202] != 'A' || header[0x203] != '3')
                throw new IOException("the NCA header did not decrypt: header_key in your prod.keys is wrong");
            ByteBuffer h = le(header);
            type = header[0x205];
            keyGen = Math.max(Math.max(header[0x206] & 0xff, header[0x220] & 0xff) - 1, 0);
            titleId = h.getLong(0x210);
            System.arraycopy(header, 0x230, rights, 0, 16);
            for (int i = 0; i < 4; i++) {
                long start = h.getInt(0x240 + i * 16) & 0xffffffffL, end = h.getInt(0x244 + i * 16) & 0xffffffffL;
                if (end == 0) continue;
                Section s = new Section();
                s.index = i;
                s.offset = start * 0x200;
                s.size = (end - start) * 0x200;
                s.fs = new byte[0x200];
                System.arraycopy(header, 0x400 + i * 0x200, s.fs, 0, 0x200);
                s.fsType = s.fs[2];
                s.crypto = s.fs[4];
                for (int b = 0; b < 8; b++) s.ctr[b] = s.fs[0x147 - b];               /* stored reversed */
                sections.add(s);
            }
        }

        byte[] key() throws IOException {
            if (key != null) return key;
            boolean titleKey = false;
            for (byte b : rights) titleKey |= b != 0;
            if (titleKey) {
                byte[] tkey = null;
                for (String n : pkg.files.keySet()) {
                    if (!n.endsWith(".tik")) continue;
                    byte[] tik = pkg.read(n);
                    boolean same = true;
                    for (int i = 0; i < 16; i++) same &= tik[0x2A0 + i] == rights[i];
                    if (same) { tkey = new byte[16]; System.arraycopy(tik, 0x180, tkey, 0, 16); }
                }
                if (tkey == null) throw new IOException("no ticket for this game in the file (dump it again with its ticket)");
                key = ecb(named(String.format("titlekek_%02x", keyGen)), tkey, true);
            } else {
                int k = header[0x207];
                String kaek = k == 1 ? "ocean" : k == 2 ? "system" : "application";
                byte[] area = new byte[0x40];
                System.arraycopy(header, 0x300, area, 0, 0x40);
                area = ecb(named(String.format("key_area_key_%s_%02x", kaek, keyGen)), area, true);
                key = new byte[16];
                System.arraycopy(area, 0x20, key, 0, 16);
            }
            return key;
        }

        /* Decrypted bytes at a section-relative offset (AES-CTR sections), as switchfs.Nca.read */
        byte[] read(Section s, long off, int n) throws IOException {
            if (n <= 0) return new byte[0];
            long abs = s.offset + off, aligned = abs & ~0xFL;
            int skip = (int) (abs - aligned), len = (skip + n + 15) & ~15;
            byte[] data = readFile(at + aligned, len);
            if (s.crypto != 1) {
                byte[] iv = new byte[16];
                System.arraycopy(s.ctr, 0, iv, 0, 8);
                long blk = aligned >>> 4;
                for (int i = 0; i < 8; i++) iv[15 - i] = (byte) (blk >>> (8 * i));
                try {
                    Cipher c = Cipher.getInstance("AES/CTR/NoPadding");
                    c.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key(), "AES"), new IvParameterSpec(iv));
                    data = c.doFinal(data);
                } catch (IOException e) { throw e; } catch (Exception e) { throw new IOException("AES-CTR: " + e); }
            }
            if (skip == 0 && len == n) return data;
            byte[] out = new byte[n];
            System.arraycopy(data, skip, out, 0, n);
            return out;
        }

        Section first(int fsType) throws IOException {
            for (Section s : sections) if (s.fsType == fsType) return s;
            throw new IOException(name + ": no " + (fsType == 1 ? "ExeFS" : "RomFS") + " section");
        }

        Pfs exefs() throws IOException {
            Section s = first(1);
            long pfsOff = le(s.fs).getLong(0x40);
            return new Pfs((o, n) -> read(s, pfsOff + o, n), 0);
        }

        /* the RomFS's offset in its section: the last level of the section's hash tree (IVFC) */
        long romfsBase(Section s) throws IOException {
            if (s.fs[8] != 'I' || s.fs[9] != 'V' || s.fs[10] != 'F' || s.fs[11] != 'C') throw new IOException(name + ": no IVFC");
            if (s.crypto == 4) throw new IOException("this is an update package: choose the base game (1.0.0)");
            ByteBuffer f = le(s.fs);
            int count = f.getInt(0x8 + 0xC);
            return f.getLong(0x8 + 0x10 + (count - 2) * 0x18);
        }
    }

    /* ------------------------------------------------------------ RomFS */
    long[] totals;                                                                  /* {files, bytes} of the RomFS being written */

    /* every file of a RomFS: {path, offset, size}, as switchfs.romfs_walk */
    List<Object[]> romfsFiles(Nca nca, Nca.Section s, long base) throws IOException {
        ByteBuffer h = le(nca.read(s, base, 0x50));
        if (h.getLong(0) != 0x50) throw new IOException(nca.name + ": no RomFS header");
        long dirOff = h.getLong(24), dirSize = h.getLong(32), fileOff = h.getLong(56), fileSize = h.getLong(64), dataOff = h.getLong(72);
        byte[] dirs = nca.read(s, base + dirOff, (int) dirSize), files = nca.read(s, base + fileOff, (int) fileSize);
        ByteBuffer D = le(dirs), F = le(files);
        List<Object[]> out = new ArrayList<>();
        ArrayDeque<Object[]> stack = new ArrayDeque<>();
        stack.push(new Object[]{0, ""});
        while (!stack.isEmpty()) {
            Object[] top = stack.pop();
            int d = (Integer) top[0];
            String path = (String) top[1];
            int sibling = D.getInt(d + 4), child = D.getInt(d + 8), file = D.getInt(d + 12), nameLen = D.getInt(d + 20);
            String name = new String(dirs, d + 24, nameLen, StandardCharsets.UTF_8);
            String here = name.isEmpty() ? path : path + "/" + name;
            if (sibling != -1) stack.push(new Object[]{sibling, path});
            if (child != -1) stack.push(new Object[]{child, here});
            while (file != -1) {
                int next = F.getInt(file + 4), fnLen = F.getInt(file + 28);
                long off = F.getLong(file + 8), size = F.getLong(file + 16);
                out.add(new Object[]{here + "/" + new String(files, file + 32, fnLen, StandardCharsets.UTF_8), base + dataOff + off, size});
                file = next;
            }
        }
        return out;
    }

    void extractRomfs(Nca nca, File dest, String what) throws IOException {
        Nca.Section s = nca.first(0);
        List<Object[]> files = romfsFiles(nca, s, nca.romfsBase(s));
        long total = 0, done = 0;
        for (Object[] f : files) total += (Long) f[2];
        String canon = dest.getCanonicalPath() + File.separator;
        int i = 0;
        long lastSay = 0;
        for (Object[] f : files) {
            File out = new File(dest, ((String) f[0]).substring(1));
            if (!out.getCanonicalPath().startsWith(canon)) throw new IOException("a file outside the folder: " + f[0]);
            out.getParentFile().mkdirs();
            long off = (Long) f[1], size = (Long) f[2];
            try (OutputStream os = new FileOutputStream(out)) {
                for (long p = 0; p < size; ) {
                    int n = (int) Math.min(8 << 20, size - p);
                    os.write(nca.read(s, off + p, n));
                    p += n;
                    done += n;
                }
            }
            i++;
            long now = System.currentTimeMillis();
            if (now - lastSay > 300 || i == files.size()) {
                lastSay = now;
                progress.say(String.format("%s: %d of %d files, %.1f of %.1f GB", what, i, files.size(), done / 1e9, total / 1e9));
            }
        }
    }

    /* ------------------------------------------------------------ the whole import */
    public void run(File out) throws IOException {
        progress.say("Reading the package…");
        Pfs pkg = open();
        Nca program = null;
        List<Nca> manuals = new ArrayList<>();
        for (String n : pkg.files.keySet()) {
            if (!n.endsWith(".nca")) continue;
            Nca nca = new Nca(pkg, n);
            if (nca.type == TYPE_PROGRAM && nca.titleId == TITLE_ID) program = nca;
            else if (nca.type == TYPE_MANUAL && nca.titleId == TITLE_ID) manuals.add(nca);
        }
        if (program == null) throw new IOException("Super Mario Odyssey (base game, 0100000000010000) is not in this file");

        progress.say("Checking the version…");
        Pfs exefs = program.exefs();
        byte[] main = exefs.read("main");
        if (!MAIN_1_0_0_SHA256.equals(sha256(main)))
            throw new IOException("this is not version 1.0.0 of the game (the app is made from 1.0.0): use the base game without updates");

        File exeDir = new File(out, "dump/base/exefs"), elfDir = new File(out, "dump/base/elf");
        exeDir.mkdirs();
        elfDir.mkdirs();
        for (String n : exefs.files.keySet()) {
            byte[] d = n.equals("main") ? main : exefs.read(n);
            write(new File(exeDir, n), d);
            if (d.length >= 4 && d[0] == 'N' && d[1] == 'S' && d[2] == 'O' && d[3] == '0') {
                progress.say("Converting " + n + "…");
                write(new File(elfDir, n + ".elf"), nsoToElf(d));
            }
        }
        File romfs = new File(out, "romfs");
        deleteTree(romfs);
        extractRomfs(program, romfs, "Game files");
        File manual = new File(out, "manual");
        deleteTree(manual);
        for (Nca m : manuals) extractRomfs(m, manual, "Help pages");
        progress.say("Done");
    }

    /* ------------------------------------------------------------ NSO -> ELF, as switchfs.nso_to_elf */
    static byte[] lz4(byte[] src, int at, int len, int outLen) throws IOException {
        byte[] dst = new byte[outLen];
        int s = at, end = at + len, d = 0;
        try {
            while (s < end) {
                int token = src[s++] & 0xff, lit = token >>> 4;
                if (lit == 15) for (int b = 255; b == 255; ) { b = src[s++] & 0xff; lit += b; }
                System.arraycopy(src, s, dst, d, lit);
                s += lit; d += lit;
                if (s >= end) break;
                int off = (src[s] & 0xff) | ((src[s + 1] & 0xff) << 8), ml = token & 15;
                s += 2;
                if (ml == 15) for (int b = 255; b == 255; ) { b = src[s++] & 0xff; ml += b; }
                ml += 4;
                for (int i = 0, from = d - off; i < ml; i++) dst[d++] = dst[from + i];
            }
        } catch (ArrayIndexOutOfBoundsException e) { throw new IOException("a broken LZ4 block in an executable"); }
        if (d != outLen) throw new IOException("an executable's segment has the wrong size");
        return dst;
    }

    static byte[] nsoToElf(byte[] nso) throws IOException {
        ByteBuffer h = le(nso);
        int flags = h.getInt(12), bss = h.getInt(0x3c);
        int[] vaddr = new int[3];
        byte[][] seg = new byte[3][];
        for (int i = 0; i < 3; i++) {
            int foff = h.getInt(0x10 + i * 0x10), moff = h.getInt(0x14 + i * 0x10), size = h.getInt(0x18 + i * 0x10), csz = h.getInt(0x60 + i * 4);
            vaddr[i] = moff;
            if ((flags & (1 << i)) != 0) seg[i] = lz4(nso, foff, csz, size);
            else { seg[i] = new byte[size]; System.arraycopy(nso, foff, seg[i], 0, size); }
        }
        int tv = vaddr[0], rv = vaddr[1], dv = vaddr[2];
        byte[] text = seg[0], ro = seg[1], data = seg[2];
        byte[] mem = new byte[dv + data.length];
        for (int i = 0; i < 3; i++) System.arraycopy(seg[i], 0, mem, vaddr[i], seg[i].length);
        ByteBuffer M = le(mem);
        int mod = le(text).getInt(4);
        if (mem[mod] != 'M' || mem[mod + 1] != 'O' || mem[mod + 2] != 'D' || mem[mod + 3] != '0') throw new IOException("no MOD0 in an executable");
        int dyn = mod + M.getInt(mod + 4), bssS = M.getInt(mod + 8), bssE = M.getInt(mod + 12);
        Map<Long, Long> tags = new HashMap<>();
        int n = 0;
        while (true) {
            long t = M.getLong(dyn + n * 16), v = M.getLong(dyn + n * 16 + 8);
            n++;
            if (t == 0) break;
            tags.putIfAbsent(t, v);
        }
        int dynsz = n * 16;
        int symtab = (int) (long) tags.get(6L), strtab = (int) (long) tags.get(5L), strsz = (int) (long) tags.get(10L);
        int nsym = tags.containsKey(4L) ? M.getInt((int) (long) tags.get(4L) + 4) : (strtab - symtab) / 24;
        final int OFF = 0x1000;
        java.io.ByteArrayOutputStream names = new java.io.ByteArrayOutputStream();
        names.write(0);
        List<byte[]> secs = new ArrayList<>();
        secs.add(new byte[64]);
        Object[][] list = {
            {".text", 1, 6, tv, OFF + tv, text.length, 0, 0, 16, 0},
            {".rodata", 1, 2, rv, OFF + rv, symtab - rv, 0, 0, 16, 0},
            {".dynsym", 11, 2, symtab, OFF + symtab, nsym * 24, 4, 1, 8, 24},
            {".dynstr", 3, 2, strtab, OFF + strtab, strsz, 0, 0, 1, 0},
            {".rodata.1", 1, 2, strtab + strsz, OFF + strtab + strsz, rv + ro.length - (strtab + strsz), 0, 0, 1, 0},
            {".data", 1, 3, dv, OFF + dv, dyn - dv, 0, 0, 16, 0},
            {".dynamic", 6, 3, dyn, OFF + dyn, dynsz, 4, 0, 8, 16},
            {".data.1", 1, 3, dyn + dynsz, OFF + dyn + dynsz, dv + data.length - (dyn + dynsz), 0, 0, 1, 0},
            {".bss", 8, 3, bssS, OFF + dv + data.length, bssE - bssS, 0, 0, 16, 0},
        };
        for (Object[] s : list) secs.add(section(names, s));
        int shstrI = secs.size(), shstrOff = OFF + mem.length;
        byte[] last = section(names, new Object[]{".shstrtab", 3, 0, 0, shstrOff, 0, 0, 0, 1, 0});
        byte[] blob = names.toByteArray();
        le(last).putLong(32, blob.length);
        secs.add(last);
        int shoff = (shstrOff + blob.length + 7) & ~7;
        ByteBuffer out = le(new byte[shoff + secs.size() * 64]);
        out.put(new byte[]{0x7f, 'E', 'L', 'F', 2, 1, 1}).position(16);
        out.putShort((short) 3).putShort((short) 183).putInt(1).putLong(0).putLong(0x40).putLong(shoff).putInt(0)
           .putShort((short) 0x40).putShort((short) 0x38).putShort((short) 4).putShort((short) 0x40)
           .putShort((short) secs.size()).putShort((short) shstrI);
        long dataMem = Math.max(Math.max(data.length, (long) bssE - dv), (long) data.length + bss);
        phdr(out, 1, 5, tv, text.length, text.length, 0x1000, OFF);
        phdr(out, 1, 4, rv, ro.length, ro.length, 0x1000, OFF);
        phdr(out, 1, 6, dv, data.length, dataMem, 0x1000, OFF);
        phdr(out, 2, 6, dyn, dynsz, dynsz, 8, OFF);
        out.position(OFF);
        out.put(mem).put(blob);
        out.position(shoff);
        for (byte[] s : secs) out.put(s);
        return out.array();
    }

    static byte[] section(java.io.ByteArrayOutputStream names, Object[] s) {
        int ni = names.size();
        byte[] nm = ((String) s[0]).getBytes(StandardCharsets.US_ASCII);
        names.write(nm, 0, nm.length);
        names.write(0);
        ByteBuffer b = le(new byte[64]);
        b.putInt(ni).putInt((Integer) s[1]).putLong((Integer) s[2]).putLong((Integer) s[3]).putLong((Integer) s[4])
         .putLong((Integer) s[5]).putInt((Integer) s[6]).putInt((Integer) s[7]).putLong((Integer) s[8]).putLong((Integer) s[9]);
        return b.array();
    }

    static void phdr(ByteBuffer b, int type, int fl, long v, long fsz, long msz, long align, int off) {
        b.putInt(type).putInt(fl).putLong(off + v).putLong(v).putLong(v).putLong(fsz).putLong(msz).putLong(align);
    }

    /* ------------------------------------------------------------ helpers */
    static ByteBuffer le(byte[] b) { return ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN); }

    static String sha256(byte[] d) {
        try {
            StringBuilder s = new StringBuilder();
            for (byte b : MessageDigest.getInstance("SHA-256").digest(d)) s.append(String.format("%02x", b));
            return s.toString();
        } catch (Exception e) { return ""; }
    }

    static void write(File f, byte[] d) throws IOException {
        f.getParentFile().mkdirs();
        try (OutputStream o = new FileOutputStream(f)) { o.write(d); }
    }

    static void deleteTree(File f) {
        File[] k = f.listFiles();
        if (k != null) for (File c : k) deleteTree(c);
        f.delete();
    }

    /* On a PC, to check it against tools/extract: java GameImporter <nsp or xci> <prod.keys> <out folder> */
    public static void main(String[] a) throws Exception {
        String keys = new String(java.nio.file.Files.readAllBytes(new File(a[1]).toPath()), StandardCharsets.UTF_8);
        try (RandomAccessFile f = new RandomAccessFile(a[0], "r")) {
            long t0 = System.currentTimeMillis();
            new GameImporter(f.getChannel(), parseKeys(keys), s -> System.out.print("\r" + s + "        ")).run(new File(a[2]));
            System.out.printf("%n%.0f s%n", (System.currentTimeMillis() - t0) / 1000.0);
        }
    }
}
