package arcwasm;

import java.io.File;
import java.io.FileInputStream;
import java.math.BigInteger;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.security.Signature;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.util.Arrays;
import javax.crypto.AEADBadTagException;
import javax.crypto.Cipher;
import javax.crypto.KeyAgreement;
import javax.crypto.Mac;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;
import javax.net.ssl.SSLContext;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

/**
 * conscrypt's natives (libjavacrypto over OpenSSL 1.0.1 on Dalvik 4.4,
 * BoringSSL on ART 6.0.1; the "AndroidOpenSSL" provider) and libcore's
 * NativeBN (BigInteger on OpenSSL's BIGNUM). On ART also what conscrypt 6.0
 * adds: AES-GCM (BoringSSL's AEAD), EC keys, X.509 from the system CA store,
 * TLS 1.2 by default. Prints one line per check and exits with the number
 * of failures.
 */
public class CryptoTest {
    private static final boolean ART = System.getProperty("java.vm.version", "").startsWith("2.");

    private static int passed;
    private static int failed;

    interface Check {
        void run() throws Exception;
    }

    private static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
        } catch (Throwable t) {
            failed++;
            System.out.println("[FAIL] " + name + ": " + t);
            t.printStackTrace(System.out);
        }
    }

    private static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    private static void expectEq(Object expected, Object actual) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError("expected <" + expected + "> but was <" + actual + ">");
        }
    }

    private static String hex(byte[] b) {
        StringBuilder sb = new StringBuilder();
        for (byte x : b) sb.append(String.format("%02x", x & 0xff));
        return sb.toString();
    }

    /** Android 6.0.1 (conscrypt 6.0 on BoringSSL). */
    private static void checks6(final byte[] abc) {
        check("Cipher: AES/GCM/NoPadding with AAD (BoringSSL AEAD), tag checked", new Check() {
            public void run() throws Exception {
                SecretKeySpec key = new SecretKeySpec(new byte[16], "AES");
                GCMParameterSpec spec = new GCMParameterSpec(128, new byte[12]);
                Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
                expectEq("AndroidOpenSSL", c.getProvider().getName());
                c.init(Cipher.ENCRYPT_MODE, key, spec);
                c.updateAAD(abc);
                byte[] sealed = c.doFinal(new byte[16]);
                // NIST GCM test case 2 (zero key, IV and block), with AAD "abc"
                expectEq("0388dace60b6a392f328c2b971b2fe78",
                        hex(Arrays.copyOf(sealed, 16)));
                c.init(Cipher.DECRYPT_MODE, key, spec);
                c.updateAAD(abc);
                expect(Arrays.equals(new byte[16], c.doFinal(sealed)), "decrypted differs");
                sealed[3] ^= 1;
                c.init(Cipher.DECRYPT_MODE, key, spec);
                c.updateAAD(abc);
                try {
                    c.doFinal(sealed);
                    throw new AssertionError("tampered data decrypted");
                } catch (AEADBadTagException expected) {
                }
            }
        });

        check("EC P-256: SHA256withECDSA sign and verify, ECDH agreement", new Check() {
            public void run() throws Exception {
                KeyPairGenerator g = KeyPairGenerator.getInstance("EC");
                g.initialize(256);
                KeyPair a = g.generateKeyPair();
                KeyPair b = g.generateKeyPair();
                Signature s = Signature.getInstance("SHA256withECDSA");
                s.initSign(a.getPrivate());
                s.update(abc);
                byte[] sig = s.sign();
                s.initVerify(a.getPublic());
                s.update(abc);
                expect(s.verify(sig), "signature does not verify");
                KeyAgreement ka = KeyAgreement.getInstance("ECDH");
                ka.init(a.getPrivate());
                ka.doPhase(b.getPublic(), true);
                byte[] ab = ka.generateSecret();
                ka.init(b.getPrivate());
                ka.doPhase(a.getPublic(), true);
                expect(Arrays.equals(ab, ka.generateSecret()), "ECDH secrets differ");
                expectEq(Integer.valueOf(32), Integer.valueOf(ab.length));
            }
        });

        check("X.509: the system CA store (/system/etc/security/cacerts), self-signed roots verify", new Check() {
            public void run() throws Exception {
                File[] files = new File("/system/etc/security/cacerts").listFiles();
                expect(files != null && files.length > 50, "cacerts: " + (files == null ? 0 : files.length));
                CertificateFactory cf = CertificateFactory.getInstance("X.509");
                int roots = 0;
                for (int i = 0; i < 10; i++) {
                    FileInputStream in = new FileInputStream(files[i]);
                    X509Certificate cert = (X509Certificate) cf.generateCertificate(in);
                    in.close();
                    if (cert.getSubjectX500Principal().equals(cert.getIssuerX500Principal())) {
                        cert.verify(cert.getPublicKey());
                        roots++;
                    }
                }
                expect(roots > 0, "no self-signed root among the first 10");
                TrustManagerFactory tmf = TrustManagerFactory.getInstance(
                        TrustManagerFactory.getDefaultAlgorithm());
                tmf.init((KeyStore) null);
                X509TrustManager tm = (X509TrustManager) tmf.getTrustManagers()[0];
                expect(tm.getAcceptedIssuers().length == files.length,
                        tm.getAcceptedIssuers().length + " accepted issuers, " + files.length + " files");
            }
        });

        check("SSLContext: TLSv1.2 enabled by default", new Check() {
            public void run() throws Exception {
                String[] p = SSLContext.getDefault().getDefaultSSLParameters().getProtocols();
                expect(Arrays.asList(p).contains("TLSv1.2"), Arrays.toString(p));
            }
        });
    }

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();
        final byte[] abc = "abc".getBytes("UTF-8");

        check("MessageDigest: MD5, SHA-1, SHA-256 from AndroidOpenSSL", new Check() {
            public void run() throws Exception {
                MessageDigest md5 = MessageDigest.getInstance("MD5");
                expectEq("AndroidOpenSSL", md5.getProvider().getName());
                expectEq("900150983cd24fb0d6963f7d28e17f72", hex(md5.digest(abc)));
                expectEq("a9993e364706816aba3e25717850c26c9cd0d89d",
                        hex(MessageDigest.getInstance("SHA-1").digest(abc)));
                expectEq("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                        hex(MessageDigest.getInstance("SHA-256").digest(abc)));
            }
        });

        check("Mac: HMAC-SHA256 (RFC 4231 case 2)", new Check() {
            public void run() throws Exception {
                Mac mac = Mac.getInstance("HmacSHA256");
                mac.init(new SecretKeySpec("Jefe".getBytes("UTF-8"), "HmacSHA256"));
                expectEq("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
                        hex(mac.doFinal("what do ya want for nothing?".getBytes("UTF-8"))));
            }
        });

        check("Cipher: AES/CBC/PKCS5Padding round trip", new Check() {
            public void run() throws Exception {
                byte[] key = new byte[16];
                byte[] iv = new byte[16];
                for (int i = 0; i < 16; i++) { key[i] = (byte) i; iv[i] = (byte) (100 + i); }
                byte[] plain = "Angry Birds in a browser, encrypted".getBytes("UTF-8");
                Cipher c = Cipher.getInstance("AES/CBC/PKCS5Padding");
                c.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(key, "AES"), new IvParameterSpec(iv));
                byte[] sealed = c.doFinal(plain);
                expectEq(Integer.valueOf(48), Integer.valueOf(sealed.length));
                c.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"), new IvParameterSpec(iv));
                expect(Arrays.equals(plain, c.doFinal(sealed)), "decrypted differs");
            }
        });

        check("SecureRandom: bytes vary", new Check() {
            public void run() {
                SecureRandom r = new SecureRandom();
                byte[] a = new byte[32];
                byte[] b = new byte[32];
                r.nextBytes(a);
                r.nextBytes(b);
                expect(!Arrays.equals(a, b), "two draws equal");
            }
        });

        check("BigInteger (NativeBN): arithmetic, modPow, primes", new Check() {
            public void run() {
                BigInteger two = BigInteger.valueOf(2);
                BigInteger p = two.pow(127).subtract(BigInteger.ONE);  // a Mersenne prime
                expectEq("170141183460469231731687303715884105727", p.toString());
                expect(p.isProbablePrime(50), "2^127-1 not prime");
                expect(!p.add(two).isProbablePrime(50), "2^127+1 prime");
                BigInteger x = new BigInteger("123456789012345678901234567890");
                expectEq("15241578753238836750495351562536198787501905199875019052100",
                        x.multiply(x).toString());
                // Fermat: a^(p-1) = 1 (mod p)
                expectEq(BigInteger.ONE, BigInteger.valueOf(3).modPow(p.subtract(BigInteger.ONE), p));
                expectEq(BigInteger.valueOf(4), BigInteger.valueOf(3).modInverse(BigInteger.valueOf(11)));
            }
        });

        check("Signature: RSA-1024 key pair, SHA256withRSA sign and verify", new Check() {
            public void run() throws Exception {
                KeyPairGenerator g = KeyPairGenerator.getInstance("RSA");
                g.initialize(1024);
                KeyPair kp = g.generateKeyPair();
                Signature s = Signature.getInstance("SHA256withRSA");
                s.initSign(kp.getPrivate());
                s.update(abc);
                byte[] sig = s.sign();
                s.initVerify(kp.getPublic());
                s.update(abc);
                expect(s.verify(sig), "signature does not verify");
                s.initVerify(kp.getPublic());
                s.update("abd".getBytes("UTF-8"));
                expect(!s.verify(sig), "signature verifies other data");
            }
        });

        if (ART) checks6(abc);

        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("CryptoTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
