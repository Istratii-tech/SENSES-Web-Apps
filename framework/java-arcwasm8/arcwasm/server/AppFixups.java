// arc-wasm: what an app from SENSES Web Apps' library needs changed before it
// runs on this one-process VM (topic install8, docs/topics/install8.md).
//
// The library holds the user's APK as it came; most apps run from it in
// place (/mnt/apps/PACKAGE, read-only). An app with a fixup gets a copy in
// /data/app/PACKAGE/base.apk with its edits, made once per library APK (the
// source's size and time in .arcwasm-source next to it) and installed from
// there. The copy keeps the APK's layout: the replaced entry stays where it
// was, padded so that every later entry keeps its alignment (zipalign's 4
// bytes for stored entries, 4096 for libraries), and every other entry is
// copied byte for byte (nothing is recompressed).
//
// Firefox (org.mozilla.firefox): GeckoView's <browser> is always remote
// (content processes); in one process it must not be. The edits of
// assets/omni.ja are those of tools/firefox8_apk.py (topic firefox8), each
// checking the text it replaces: another version is left as it is. Its
// GeckoView configuration (/system/etc/arcwasm/data-local-tmp, e10s off)
// is used when Settings.Global.debug_app is the app (SystemServer --senses
// sets it for an app that has one).
package arcwasm.server;

import android.util.Slog;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

final class AppFixups {
    private static final String TAG = "AppFixups";
    /** Bumped when the edits change: copies made by older edits are made again. */
    private static final int VERSION = 1;

    private AppFixups() {}

    /** (file in omni.ja, text, replacement): tools/firefox8_apk.py's OMNI_EDITS. */
    private static final String[][] FIREFOX_OMNI_EDITS = {
        {"chrome/geckoview/content/geckoview.js",
         "  browser.setAttribute(\"maychangeremoteness\", \"true\");\n"
         + "  browser.setAttribute(\"remote\", \"true\");\n"
         + "  browser.setAttribute(\n"
         + "    \"remoteType\",\n",
         "  // arc-wasm: one process (tools/firefox8_apk.py): no process switches.\n"
         + "  const arcwasmRemote = Services.appinfo.browserTabsRemoteAutostart;\n"
         + "  arcwasmRemote && browser.setAttribute(\"maychangeremoteness\", \"true\");\n"
         + "  dump(`arc-wasm: geckoview <browser> remote=${arcwasmRemote}\\n`);\n"
         + "  // (Its presence makes the browser remote, whatever the value.)\n"
         + "  arcwasmRemote && browser.setAttribute(\"remote\", \"true\");\n"
         + "  arcwasmRemote && browser.setAttribute(\n"
         + "    \"remoteType\",\n"},
        {"chrome/geckoview/content/geckoview.js",
         "    window.document.documentElement.appendChild(browser);\n",
         "    window.document.documentElement.appendChild(browser);\n"
         + "    dump(`arc-wasm: <browser> isRemoteBrowser=${browser.isRemoteBrowser} "
         + "remoteType=${browser.remoteType}\\n`);\n"},
    };

    /** Whether |pkg| has a fixup. */
    static boolean has(String pkg) {
        return "org.mozilla.firefox".equals(pkg);
    }

    /**
     * The directory to install |pkg| from: its library directory, or for an
     * app with a fixup /data/app/PACKAGE with the edited copy (made now if
     * the library's APK changed). The library's on failure (logged).
     */
    static File codeDir(String pkg, File libraryDir) {
        if (!has(pkg)) {
            return libraryDir;
        }
        File src = new File(libraryDir, "base.apk");
        File dir = new File("/data/app", pkg);
        File dest = new File(dir, "base.apk");
        File stamp = new File(dir, ".arcwasm-source");
        String want = VERSION + " " + src.length() + " " + src.lastModified();
        if (dest.isFile() && want.equals(AppLibrary.readSmall(stamp))) {
            return dir;
        }
        long start = System.nanoTime();
        try {
            if (!dir.isDirectory() && !dir.mkdirs()) {
                throw new IOException("cannot create " + dir);
            }
            stamp.delete();
            File tmp = new File(dir, "base.apk.tmp");
            if (!fixFirefox(src, tmp) || !tmp.renameTo(dest)) {
                tmp.delete();
                Slog.w(TAG, pkg + ": no fixup for this version; running it as it is");
                return libraryDir;
            }
            try (OutputStream out = new FileOutputStream(stamp)) {
                out.write(want.getBytes(StandardCharsets.UTF_8));
            }
            Slog.i(TAG, pkg + ": fixed copy " + dest + " (" + dest.length() / 1024 + " KB) in "
                    + (System.nanoTime() - start) / 1000000 + " ms");
            return dir;
        } catch (IOException | RuntimeException e) {
            Slog.w(TAG, pkg + ": fixup failed; running it as it is", e);
            return libraryDir;
        }
    }

    /** Firefox's APK with assets/omni.ja edited; false when the edits do not apply. */
    private static boolean fixFirefox(File src, File dest) throws IOException {
        try (RandomAccessFile in = new RandomAccessFile(src, "r")) {
            List<Entry> entries = readCentralDirectory(in);
            Entry omni = null;
            for (Entry e : entries) {
                if (e.name.equals("assets/omni.ja")) {
                    omni = e;
                }
            }
            if (omni == null) {
                return false;
            }
            byte[] omniData = readEntry(in, omni);
            byte[] fixed = editOmni(omniData);
            if (fixed == null) {
                return false;
            }
            Map<String, byte[]> replace = new HashMap<String, byte[]>();
            replace.put(omni.name, fixed);
            try (OutputStream out = new java.io.BufferedOutputStream(
                    new FileOutputStream(dest), 1 << 16)) {
                rewrite(in, entries, replace, out);
            }
            return true;
        }
    }

    /** omni.ja with FIREFOX_OMNI_EDITS made, or null when a text is not there once. */
    private static byte[] editOmni(byte[] omni) throws IOException {
        List<Entry> entries = readCentralDirectory(omni);
        Map<String, String> texts = new HashMap<String, String>();
        for (String[] edit : FIREFOX_OMNI_EDITS) {
            String text = texts.get(edit[0]);
            if (text == null) {
                Entry e = find(entries, edit[0]);
                if (e == null) {
                    Slog.w(TAG, "omni.ja: no " + edit[0]);
                    return null;
                }
                text = new String(readEntry(omni, e), StandardCharsets.UTF_8);
            }
            int at = text.indexOf(edit[1]);
            if (at < 0 || text.indexOf(edit[1], at + 1) >= 0) {
                Slog.w(TAG, "omni.ja: " + edit[0] + ": the text to replace is not there once");
                return null;
            }
            texts.put(edit[0], text.replace(edit[1], edit[2]));
        }
        Map<String, byte[]> replace = new HashMap<String, byte[]>();
        for (Map.Entry<String, String> t : texts.entrySet()) {
            replace.put(t.getKey(), t.getValue().getBytes(StandardCharsets.UTF_8));
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream(omni.length + 4096);
        rewrite(new ByteSource(omni), entries, replace, out);
        return out.toByteArray();
    }

    // --- zip, as much of it as APKs need (no zip64) --------------------------------------

    /** A central directory entry. */
    static final class Entry {
        String name;
        int method;
        int flags;
        long crc;
        long compressedSize;
        long size;
        long localOffset;
        /** The whole central directory record (rewritten with new sizes/offset). */
        byte[] record;
    }

    /** Random access to a zip's bytes: a file or an array. */
    interface Source {
        long length() throws IOException;
        void read(long at, byte[] buf, int off, int len) throws IOException;
    }

    static final class ByteSource implements Source {
        final byte[] data;

        ByteSource(byte[] data) {
            this.data = data;
        }

        public long length() {
            return data.length;
        }

        public void read(long at, byte[] buf, int off, int len) throws IOException {
            if (at < 0 || at + len > data.length) throw new IOException("zip: out of bounds");
            System.arraycopy(data, (int) at, buf, off, len);
        }
    }

    static Source source(final RandomAccessFile f) {
        return new Source() {
            public long length() throws IOException {
                return f.length();
            }

            public void read(long at, byte[] buf, int off, int len) throws IOException {
                f.seek(at);
                f.readFully(buf, off, len);
            }
        };
    }

    static int u16(byte[] b, int at) {
        return (b[at] & 0xff) | (b[at + 1] & 0xff) << 8;
    }

    static long u32(byte[] b, int at) {
        return (u16(b, at) | (long) u16(b, at + 2) << 16) & 0xffffffffL;
    }

    static void put16(byte[] b, int at, int v) {
        b[at] = (byte) v;
        b[at + 1] = (byte) (v >> 8);
    }

    static void put32(byte[] b, int at, long v) {
        put16(b, at, (int) (v & 0xffff));
        put16(b, at + 2, (int) ((v >> 16) & 0xffff));
    }

    static List<Entry> readCentralDirectory(RandomAccessFile f) throws IOException {
        return readCentralDirectory(source(f));
    }

    static List<Entry> readCentralDirectory(byte[] data) throws IOException {
        return readCentralDirectory(new ByteSource(data));
    }

    static List<Entry> readCentralDirectory(Source in) throws IOException {
        long len = in.length();
        int tail = (int) Math.min(len, 65535 + 22);
        byte[] end = new byte[tail];
        in.read(len - tail, end, 0, tail);
        int eocd = -1;
        for (int i = tail - 22; i >= 0; i--) {
            if (u32(end, i) == 0x06054b50L) {
                eocd = i;
                break;
            }
        }
        if (eocd < 0) throw new IOException("zip: no end of central directory");
        int count = u16(end, eocd + 10);
        long cdSize = u32(end, eocd + 12);
        long cdOffset = u32(end, eocd + 16);
        if (count == 0xffff || cdOffset == 0xffffffffL) throw new IOException("zip64");
        byte[] cd = new byte[(int) cdSize];
        in.read(cdOffset, cd, 0, cd.length);
        List<Entry> list = new ArrayList<Entry>();
        int p = 0;
        for (int i = 0; i < count; i++) {
            if (u32(cd, p) != 0x02014b50L) throw new IOException("zip: bad central directory");
            Entry e = new Entry();
            e.flags = u16(cd, p + 8);
            e.method = u16(cd, p + 10);
            e.crc = u32(cd, p + 16);
            e.compressedSize = u32(cd, p + 20);
            e.size = u32(cd, p + 24);
            int nameLen = u16(cd, p + 28);
            int extraLen = u16(cd, p + 30);
            int commentLen = u16(cd, p + 32);
            e.localOffset = u32(cd, p + 42);
            e.name = new String(cd, p + 46, nameLen, StandardCharsets.UTF_8);
            int recLen = 46 + nameLen + extraLen + commentLen;
            e.record = new byte[recLen];
            System.arraycopy(cd, p, e.record, 0, recLen);
            list.add(e);
            p += recLen;
        }
        return list;
    }

    static Entry find(List<Entry> entries, String name) {
        for (Entry e : entries) {
            if (e.name.equals(name)) return e;
        }
        return null;
    }

    /** Where the entry's data starts (after its local header). */
    static long dataOffset(Source in, Entry e) throws IOException {
        byte[] h = new byte[30];
        in.read(e.localOffset, h, 0, 30);
        if (u32(h, 0) != 0x04034b50L) throw new IOException("zip: bad local header " + e.name);
        return e.localOffset + 30 + u16(h, 26) + u16(h, 28);
    }

    static byte[] readEntry(RandomAccessFile f, Entry e) throws IOException {
        return readEntry(source(f), e);
    }

    static byte[] readEntry(byte[] data, Entry e) throws IOException {
        return readEntry(new ByteSource(data), e);
    }

    static byte[] readEntry(Source in, Entry e) throws IOException {
        byte[] raw = new byte[(int) e.compressedSize];
        in.read(dataOffset(in, e), raw, 0, raw.length);
        if (e.method == 0) return raw;
        if (e.method != 8) throw new IOException("zip: method " + e.method + " of " + e.name);
        Inflater inf = new Inflater(true);
        try {
            inf.setInput(raw);
            byte[] out = new byte[(int) e.size];
            int n = 0;
            while (n < out.length) {
                int r = inf.inflate(out, n, out.length - n);
                if (r == 0 && (inf.finished() || inf.needsInput())) break;
                n += r;
            }
            if (n != out.length) throw new IOException("zip: short entry " + e.name);
            return out;
        } catch (DataFormatException x) {
            throw new IOException("zip: " + e.name + ": " + x);
        } finally {
            inf.end();
        }
    }

    /**
     * Writes the zip of |in| to |out| with |replace|'s entries' contents
     * (stored) in their places; everything else byte for byte. A replaced
     * entry's local header is padded (extra field) so that the next entry
     * moves by a multiple of 4096: alignments stay.
     */
    static void rewrite(RandomAccessFile f, List<Entry> entries, Map<String, byte[]> replace,
            OutputStream out) throws IOException {
        rewrite(source(f), entries, replace, out);
    }

    static void rewrite(Source in, List<Entry> entries, Map<String, byte[]> replace,
            OutputStream out) throws IOException {
        List<Entry> byOffset = new ArrayList<Entry>(entries);
        Collections.sort(byOffset, (a, b) -> Long.compare(a.localOffset, b.localOffset));
        // Where the central directory starts: the end of the last entry's region.
        long cdStart = cdOffset(in);
        long written = 0;
        Map<Entry, Long> newOffset = new HashMap<Entry, Long>();
        byte[] buf = new byte[1 << 16];
        for (int i = 0; i < byOffset.size(); i++) {
            Entry e = byOffset.get(i);
            long regionEnd = i + 1 < byOffset.size() ? byOffset.get(i + 1).localOffset : cdStart;
            newOffset.put(e, written);
            byte[] data = replace.get(e.name);
            if (data == null) {
                // Header, data (and data descriptor) as they are.
                for (long at = e.localOffset; at < regionEnd;) {
                    int n = (int) Math.min(buf.length, regionEnd - at);
                    in.read(at, buf, 0, n);
                    out.write(buf, 0, n);
                    at += n;
                }
                written += regionEnd - e.localOffset;
                continue;
            }
            CRC32 crc = new CRC32();
            crc.update(data);
            byte[] name = e.name.getBytes(StandardCharsets.UTF_8);
            long oldLen = regionEnd - e.localOffset;
            long base = 30 + name.length + data.length;
            int pad = (int) Math.floorMod(oldLen - base, 4096L);
            if (pad > 0 && pad < 4) pad += 4096;  // an extra field takes 4 bytes at least
            byte[] h = new byte[30 + name.length + pad];
            put32(h, 0, 0x04034b50L);
            put16(h, 4, 10);   // version needed
            put16(h, 6, 0);    // flags: sizes here, no data descriptor
            put16(h, 8, 0);    // stored
            put16(h, 10, 0);
            put16(h, 12, 0x21);  // 1980-01-01
            put32(h, 14, crc.getValue());
            put32(h, 18, data.length);
            put32(h, 22, data.length);
            put16(h, 26, name.length);
            put16(h, 28, pad);
            System.arraycopy(name, 0, h, 30, name.length);
            if (pad > 0) {
                put16(h, 30 + name.length, 0xd935);  // zipalign's padding id
                put16(h, 32 + name.length, pad - 4);
            }
            out.write(h);
            out.write(data);
            written += h.length + data.length;
            e.method = 0;
            e.flags = 0;
            e.crc = crc.getValue();
            e.compressedSize = data.length;
            e.size = data.length;
        }
        long newCd = written;
        ByteArrayOutputStream cd = new ByteArrayOutputStream();
        for (Entry e : entries) {
            byte[] r = e.record.clone();
            if (replace.containsKey(e.name)) {
                put16(r, 8, e.flags);
                put16(r, 10, e.method);
                put32(r, 16, e.crc);
                put32(r, 20, e.compressedSize);
                put32(r, 24, e.size);
            }
            put32(r, 42, newOffset.get(e));
            cd.write(r);
        }
        byte[] cdBytes = cd.toByteArray();
        out.write(cdBytes);
        byte[] eocd = new byte[22];
        put32(eocd, 0, 0x06054b50L);
        put16(eocd, 8, entries.size());
        put16(eocd, 10, entries.size());
        put32(eocd, 12, cdBytes.length);
        put32(eocd, 16, newCd);
        out.write(eocd);
    }

    static long cdOffset(Source in) throws IOException {
        long len = in.length();
        int tail = (int) Math.min(len, 65535 + 22);
        byte[] end = new byte[tail];
        in.read(len - tail, end, 0, tail);
        for (int i = tail - 22; i >= 0; i--) {
            if (u32(end, i) == 0x06054b50L) {
                return u32(end, i + 16);
            }
        }
        throw new IOException("zip: no end of central directory");
    }
}
