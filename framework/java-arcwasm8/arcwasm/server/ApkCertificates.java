// arc-wasm: the signing certificate of an APK, for PackageInfo.signatures
// (GET_SIGNATURES) and checkSignatures -- read, not verified. The real
// service verifies every APK at install (PackageParser.collectCertificates:
// the v2 digest of the whole file, or every JAR entry for v1), which for a
// 150 MB game in wasm would cost seconds at each boot; the APKs here come
// from the user's own library. Apps ask for their own signature: the
// Facebook SDK hashes it at init (com.facebook.unity.FB.getKeyHash, a
// NullPointerException on null signatures, after which Facebook's init
// callback never came and Subway Surfers' bootstrap waited for ever).
//
// The signer of a v1-signed APK is the first certificate of the first
// signer of META-INF/*.RSA|DSA|EC (PKCS #7, as StrictJarVerifier reads
// it); an APK with only a v2/v3 signature (minSdk 24+) has it in the APK
// Signing Block before the central directory (the first certificate of
// the first signer's signed data).
package arcwasm.server;

import android.content.pm.Signature;
import android.util.Slog;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.cert.X509Certificate;
import java.util.Enumeration;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

import libcore.io.Streams;
import sun.security.pkcs.PKCS7;
import sun.security.pkcs.SignerInfo;

final class ApkCertificates {
    private static final String TAG = "PackageManager";

    private ApkCertificates() {}

    /** The APK's signer as one Signature, or null (unsigned, unreadable). */
    static Signature[] read(File apk) {
        long start = System.nanoTime();
        byte[] cert = null;
        try {
            cert = fromJarSignature(apk);
            if (cert == null) {
                cert = fromSigningBlock(apk);
            }
        } catch (Exception e) {
            Slog.w(TAG, "signature of " + apk + ": " + e);
        }
        Slog.i(TAG, "signature of " + apk + ": " + (cert == null ? "none" : cert.length + " bytes")
                + " (" + (System.nanoTime() - start) / 1000000 + " ms)");
        return cert == null ? null : new Signature[] {new Signature(cert)};
    }

    private static byte[] fromJarSignature(File apk) throws Exception {
        try (ZipFile zip = new ZipFile(apk)) {
            for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements();) {
                String name = en.nextElement().getName();
                if (!name.startsWith("META-INF/") || name.indexOf('/', 9) >= 0) {
                    continue;
                }
                String upper = name.toUpperCase(Locale.ROOT);
                if (!upper.endsWith(".RSA") && !upper.endsWith(".DSA") && !upper.endsWith(".EC")) {
                    continue;
                }
                byte[] bytes;
                try (InputStream in = zip.getInputStream(zip.getEntry(name))) {
                    bytes = Streams.readFully(in);
                }
                PKCS7 block = new PKCS7(bytes);
                SignerInfo[] signers = block.getSignerInfos();
                if (signers != null && signers.length > 0) {
                    java.util.List<X509Certificate> chain = signers[0].getCertificateChain(block);
                    if (chain != null && !chain.isEmpty()) {
                        return chain.get(0).getEncoded();
                    }
                }
                X509Certificate[] certs = block.getCertificates();
                if (certs != null && certs.length > 0) {
                    return certs[0].getEncoded();
                }
            }
        }
        return null;
    }

    private static final long APK_SIG_BLOCK_MAGIC_LO = 0x20676953204b5041L;  // "APK Sig "
    private static final long APK_SIG_BLOCK_MAGIC_HI = 0x3234206b636f6c42L;  // "Block 42"
    private static final int V2_ID = 0x7109871a;
    private static final int V3_ID = 0xf05368c0;

    private static byte[] fromSigningBlock(File apk) throws IOException {
        try (RandomAccessFile f = new RandomAccessFile(apk, "r")) {
            // End of central directory: its offset field gives where the
            // signing block ends.
            long size = f.length();
            int tail = (int) Math.min(size, 0xffff + 22);
            ByteBuffer end = read(f, size - tail, tail);
            int eocd = -1;
            for (int i = tail - 22; i >= 0; i--) {
                if (end.getInt(i) == 0x06054b50) {
                    eocd = i;
                    break;
                }
            }
            if (eocd < 0) {
                return null;
            }
            long cd = end.getInt(eocd + 16) & 0xffffffffL;
            if (cd < 32) {
                return null;
            }
            ByteBuffer footer = read(f, cd - 24, 24);
            if (footer.getLong(8) != APK_SIG_BLOCK_MAGIC_LO
                    || footer.getLong(16) != APK_SIG_BLOCK_MAGIC_HI) {
                return null;
            }
            long blockSize = footer.getLong(0);
            if (blockSize < 24 || blockSize > cd - 8) {
                return null;
            }
            ByteBuffer pairs = read(f, cd - blockSize - 8 + 8, (int) (blockSize - 24));
            byte[] v3 = null;
            while (pairs.remaining() >= 12) {
                long len = pairs.getLong();
                int id = pairs.getInt();
                int next = pairs.position() + (int) len - 4;
                if (id == V2_ID || id == V3_ID) {
                    ByteBuffer value = slice(pairs, (int) len - 4);
                    byte[] cert = firstCertificate(value);
                    if (id == V2_ID && cert != null) {
                        return cert;
                    }
                    v3 = v3 != null ? v3 : cert;
                }
                pairs.position(next);
            }
            return v3;
        }
    }

    // signers: a length-prefixed sequence of length-prefixed signers; a
    // signer begins with its length-prefixed signed data, which begins with
    // length-prefixed digests, then length-prefixed certificates.
    private static byte[] firstCertificate(ByteBuffer value) {
        ByteBuffer signers = prefixed(value);
        ByteBuffer signer = prefixed(signers);
        ByteBuffer signed = prefixed(signer);
        prefixed(signed);  // digests
        ByteBuffer certs = prefixed(signed);
        ByteBuffer cert = prefixed(certs);
        byte[] out = new byte[cert.remaining()];
        cert.get(out);
        return out.length > 0 ? out : null;
    }

    private static ByteBuffer prefixed(ByteBuffer b) {
        return slice(b, b.getInt());
    }

    private static ByteBuffer slice(ByteBuffer b, int len) {
        if (len < 0 || len > b.remaining()) {
            throw new IllegalArgumentException("bad length " + len);
        }
        ByteBuffer s = b.slice();
        s.limit(len);
        s.order(ByteOrder.LITTLE_ENDIAN);
        b.position(b.position() + len);
        return s;
    }

    private static ByteBuffer read(RandomAccessFile f, long at, int len) throws IOException {
        byte[] bytes = new byte[len];
        f.seek(at);
        f.readFully(bytes);
        return ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
    }
}
