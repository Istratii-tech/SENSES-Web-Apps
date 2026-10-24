// arc-wasm: the "AndroidKeyStore" JCA provider without a keystore daemon
// (topic apps-sweep, docs/topics/apps-sweep.md). On a phone the provider of
// android.security.keystore talks to keystore/keymaster, which hold the keys
// (in hardware where there is some); here there is neither, and the
// provider was not installed at all: KeyStore.getInstance("AndroidKeyStore")
// threw KeyStoreException, and apps that keep their secrets the usual way
// (androidx.security's EncryptedSharedPreferences, Tink's Android keysets,
// their own AES keys) died in Application.onCreate (Fossify-like Gallery,
// Tasks.org).
//
// This provider keeps the keys in software, as ordinary JCA keys of the
// other providers (Conscrypt): KeyGenerator AES and Hmac*, KeyPairGenerator
// RSA and EC (KeyGenParameterSpec, or the old KeyPairGeneratorSpec) generate
// them under the spec's alias, a key pair with a self-signed certificate;
// KeyStore "AndroidKeyStore" lists, returns, imports (KeyProtection) and
// deletes them. Cipher, Signature and Mac then work with the keys as they
// are. The keys of the app live in a file of its data directory
// (/data/data/PACKAGE/.arcwasm-keystore): they go with the app's data, as a
// phone's go with the app. What is not kept: a key's restrictions (purposes,
// user authentication, validity) and attestation; KeyInfo is not offered.
//
// A second provider, "AndroidKeyStoreBCWorkaround", is there as on a phone,
// where AndroidKeyStoreProvider.install adds it beside Bouncy Castle: apps
// ask it by name for the ciphers, MACs and signatures of their keystore keys
// (flutter_secure_storage: Cipher.getInstance("RSA/ECB/PKCS1Padding",
// "AndroidKeyStoreBCWorkaround") to wrap its AES key; without the provider
// NoSuchProviderException, and Saber started with no secure storage). Our
// keys are ordinary software keys, so each of its services hands the work to
// the first other provider (Conscrypt, Bouncy Castle) that takes the key.
//
// The 8.1 and 10 servers have the same file (framework/java-arcwasm{8,10}/
// arcwasm/keystore): change both. SystemServer.start installs the provider.
package arcwasm.keystore;

import android.util.Log;

import java.io.ByteArrayInputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.math.BigInteger;
import java.security.AlgorithmParameters;
import java.security.GeneralSecurityException;
import java.security.InvalidAlgorithmParameterException;
import java.security.InvalidKeyException;
import java.security.InvalidParameterException;
import java.security.Key;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.KeyStoreException;
import java.security.KeyStoreSpi;
import java.security.NoSuchAlgorithmException;
import java.security.PrivateKey;
import java.security.Provider;
import java.security.PublicKey;
import java.security.SecureRandom;
import java.security.Security;
import java.security.Signature;
import java.security.SignatureException;
import java.security.SignatureSpi;
import java.security.cert.Certificate;
import java.security.cert.CertificateFactory;
import java.security.spec.AlgorithmParameterSpec;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.PKCS8EncodedKeySpec;
import java.security.spec.RSAKeyGenParameterSpec;
import java.util.Arrays;
import java.util.Collections;
import java.util.Date;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.Map;

import javax.crypto.BadPaddingException;
import javax.crypto.Cipher;
import javax.crypto.CipherSpi;
import javax.crypto.IllegalBlockSizeException;
import javax.crypto.KeyGenerator;
import javax.crypto.KeyGeneratorSpi;
import javax.crypto.Mac;
import javax.crypto.MacSpi;
import javax.crypto.SecretKey;
import javax.crypto.ShortBufferException;
import javax.crypto.spec.SecretKeySpec;
import javax.security.auth.x500.X500Principal;

public final class ArcKeyStore {
    private static final String TAG = "ArcKeyStore";
    public static final String NAME = "AndroidKeyStore";
    public static final String BC_WORKAROUND = "AndroidKeyStoreBCWorkaround";

    private ArcKeyStore() {}

    /** Adds the providers (once per VM), unless ones of those names are there. */
    public static synchronized void install() {
        if (Security.getProvider(NAME) == null) {
            Security.addProvider(new ArcProvider());
        }
        if (Security.getProvider(BC_WORKAROUND) == null) {
            Security.addProvider(new WorkaroundProvider());
        }
    }

    static final class ArcProvider extends Provider {
        ArcProvider() {
            super(NAME, 1.0, "Android KeyStore in software (arc-wasm)");
            String p = ArcKeyStore.class.getName();
            put("KeyStore." + NAME, p + "$Spi");
            for (String alg : new String[] {"AES", "HmacSHA1", "HmacSHA224", "HmacSHA256",
                    "HmacSHA384", "HmacSHA512"}) {
                put("KeyGenerator." + alg, p + "$KeyGen" + alg);
            }
            put("KeyPairGenerator.RSA", p + "$PairGenRSA");
            put("KeyPairGenerator.EC", p + "$PairGenEC");
        }
    }

    // --- "AndroidKeyStoreBCWorkaround": the other providers' work under its name -------------

    // The names of AOSP's AndroidKeyStoreBCWorkaroundProvider (with the "/None/" aliases).
    private static final String[] CIPHERS = {"AES/ECB/NoPadding", "AES/ECB/PKCS7Padding",
            "AES/CBC/NoPadding", "AES/CBC/PKCS7Padding", "AES/CTR/NoPadding", "AES/GCM/NoPadding",
            "RSA/ECB/NoPadding", "RSA/ECB/PKCS1Padding", "RSA/ECB/OAEPPadding",
            "RSA/ECB/OAEPWithSHA-1AndMGF1Padding", "RSA/ECB/OAEPWithSHA-224AndMGF1Padding",
            "RSA/ECB/OAEPWithSHA-256AndMGF1Padding", "RSA/ECB/OAEPWithSHA-384AndMGF1Padding",
            "RSA/ECB/OAEPWithSHA-512AndMGF1Padding"};
    private static final String[] MACS = {"HmacSHA1", "HmacSHA224", "HmacSHA256", "HmacSHA384",
            "HmacSHA512"};
    private static final String[] SIGNATURES = {"NONEwithRSA", "MD5withRSA", "SHA1withRSA",
            "SHA224withRSA", "SHA256withRSA", "SHA384withRSA", "SHA512withRSA", "SHA1withRSA/PSS",
            "SHA224withRSA/PSS", "SHA256withRSA/PSS", "SHA384withRSA/PSS", "SHA512withRSA/PSS",
            "NONEwithECDSA", "SHA1withECDSA", "SHA224withECDSA", "SHA256withECDSA",
            "SHA384withECDSA", "SHA512withECDSA"};

    static final class WorkaroundProvider extends Provider {
        WorkaroundProvider() {
            super(BC_WORKAROUND, 1.0, "AndroidKeyStore ciphers, MACs, signatures (arc-wasm)");
            for (String t : CIPHERS) {
                putService(new Delegate(this, "Cipher", t, t.startsWith("RSA/ECB/")
                        ? Arrays.asList("RSA/None/" + t.substring(8)) : null));
            }
            for (String m : MACS) {
                putService(new Delegate(this, "Mac", m, null));
            }
            for (String g : SIGNATURES) {
                putService(new Delegate(this, "Signature", g, null));
            }
        }
    }

    static boolean ours(Provider p) {
        return NAME.equals(p.getName()) || BC_WORKAROUND.equals(p.getName());
    }

    /** A service whose engine, once it has a key, passes the work to another provider. */
    static final class Delegate extends Provider.Service {
        Delegate(Provider p, String type, String algorithm, java.util.List<String> aliases) {
            super(p, type, algorithm, Delegate.class.getName(), aliases, null);
        }

        @Override
        public Object newInstance(Object param) throws NoSuchAlgorithmException {
            switch (getType()) {
                case "Cipher": return new CipherWrap(getAlgorithm());
                case "Mac": return new MacWrap(getAlgorithm());
                default: return new SignatureWrap(getAlgorithm());
            }
        }
    }

    /** How an engine is initialized: tried on each other provider's in turn. */
    interface Init<T> {
        void init(T engine) throws GeneralSecurityException;
    }

    /** Another provider's engine (Cipher, Mac or Signature) of the algorithm. */
    interface Factory<T> {
        T get(String algorithm, Provider p) throws GeneralSecurityException;
    }

    /** The first other provider's engine that takes the key; else the first refusal. */
    static <T> T choose(String algorithm, Factory<T> factory, Init<T> how)
            throws InvalidKeyException, InvalidAlgorithmParameterException {
        GeneralSecurityException refused = null;
        for (Provider p : Security.getProviders()) {
            if (ours(p)) {
                continue;
            }
            T engine;
            try {
                engine = factory.get(algorithm, p);
            } catch (GeneralSecurityException e) {
                continue;  // not this provider's algorithm
            }
            try {
                how.init(engine);
                return engine;
            } catch (GeneralSecurityException e) {
                if (refused == null) refused = e;
            }
        }
        if (refused instanceof InvalidAlgorithmParameterException) {
            throw (InvalidAlgorithmParameterException) refused;
        }
        if (refused instanceof InvalidKeyException) {
            throw (InvalidKeyException) refused;
        }
        throw new InvalidKeyException("no provider takes the key for " + algorithm, refused);
    }

    static final class CipherWrap extends CipherSpi {
        private static final Factory<Cipher> FACTORY = new Factory<Cipher>() {
            public Cipher get(String t, Provider p) throws GeneralSecurityException {
                return Cipher.getInstance(t, p);
            }
        };
        private final String mTransformation;
        private Cipher mCipher;

        CipherWrap(String transformation) {
            mTransformation = transformation;
        }

        private Cipher cipher() {
            if (mCipher == null) {
                throw new IllegalStateException("Cipher not initialized");
            }
            return mCipher;
        }

        @Override
        protected void engineSetMode(String mode) {}  // the service's name has them

        @Override
        protected void engineSetPadding(String padding) {}

        @Override
        protected int engineGetBlockSize() {
            return mCipher != null ? mCipher.getBlockSize()
                    : mTransformation.startsWith("AES") ? 16 : 0;
        }

        @Override
        protected int engineGetOutputSize(int inputLen) {
            return cipher().getOutputSize(inputLen);
        }

        @Override
        protected byte[] engineGetIV() {
            return mCipher != null ? mCipher.getIV() : null;
        }

        @Override
        protected AlgorithmParameters engineGetParameters() {
            return mCipher != null ? mCipher.getParameters() : null;
        }

        @Override
        protected void engineInit(final int opmode, final Key key, final SecureRandom random)
                throws InvalidKeyException {
            try {
                mCipher = choose(mTransformation, FACTORY, new Init<Cipher>() {
                    public void init(Cipher c) throws GeneralSecurityException {
                        c.init(opmode, key, random);
                    }
                });
            } catch (InvalidAlgorithmParameterException e) {
                throw new InvalidKeyException(e);
            }
        }

        @Override
        protected void engineInit(final int opmode, final Key key,
                final AlgorithmParameterSpec params, final SecureRandom random)
                throws InvalidKeyException, InvalidAlgorithmParameterException {
            mCipher = choose(mTransformation, FACTORY, new Init<Cipher>() {
                public void init(Cipher c) throws GeneralSecurityException {
                    c.init(opmode, key, params, random);
                }
            });
        }

        @Override
        protected void engineInit(final int opmode, final Key key,
                final AlgorithmParameters params, final SecureRandom random)
                throws InvalidKeyException, InvalidAlgorithmParameterException {
            mCipher = choose(mTransformation, FACTORY, new Init<Cipher>() {
                public void init(Cipher c) throws GeneralSecurityException {
                    c.init(opmode, key, params, random);
                }
            });
        }

        @Override
        protected byte[] engineUpdate(byte[] input, int offset, int len) {
            return cipher().update(input, offset, len);
        }

        @Override
        protected int engineUpdate(byte[] input, int offset, int len, byte[] output,
                int outputOffset) throws ShortBufferException {
            return cipher().update(input, offset, len, output, outputOffset);
        }

        @Override
        protected void engineUpdateAAD(byte[] src, int offset, int len) {
            cipher().updateAAD(src, offset, len);
        }

        @Override
        protected byte[] engineDoFinal(byte[] input, int offset, int len)
                throws IllegalBlockSizeException, BadPaddingException {
            return cipher().doFinal(input, offset, len);
        }

        @Override
        protected int engineDoFinal(byte[] input, int offset, int len, byte[] output,
                int outputOffset)
                throws ShortBufferException, IllegalBlockSizeException, BadPaddingException {
            return cipher().doFinal(input, offset, len, output, outputOffset);
        }

        @Override
        protected byte[] engineWrap(Key key) throws IllegalBlockSizeException, InvalidKeyException {
            return cipher().wrap(key);
        }

        @Override
        protected Key engineUnwrap(byte[] wrapped, String algorithm, int type)
                throws InvalidKeyException, NoSuchAlgorithmException {
            return cipher().unwrap(wrapped, algorithm, type);
        }
    }

    static final class MacWrap extends MacSpi {
        private static final Factory<Mac> FACTORY = new Factory<Mac>() {
            public Mac get(String a, Provider p) throws GeneralSecurityException {
                return Mac.getInstance(a, p);
            }
        };
        private final String mAlgorithm;
        private Mac mMac;

        MacWrap(String algorithm) {
            mAlgorithm = algorithm;
        }

        private Mac mac() {
            if (mMac == null) {
                throw new IllegalStateException("MAC not initialized");
            }
            return mMac;
        }

        @Override
        protected int engineGetMacLength() {
            return mMac != null ? mMac.getMacLength() : 0;
        }

        @Override
        protected void engineInit(final Key key, final AlgorithmParameterSpec params)
                throws InvalidKeyException, InvalidAlgorithmParameterException {
            mMac = choose(mAlgorithm, FACTORY, new Init<Mac>() {
                public void init(Mac m) throws GeneralSecurityException {
                    m.init(key, params);
                }
            });
        }

        @Override
        protected void engineUpdate(byte input) {
            mac().update(input);
        }

        @Override
        protected void engineUpdate(byte[] input, int offset, int len) {
            mac().update(input, offset, len);
        }

        @Override
        protected byte[] engineDoFinal() {
            return mac().doFinal();
        }

        @Override
        protected void engineReset() {
            if (mMac != null) mMac.reset();
        }
    }

    static final class SignatureWrap extends SignatureSpi {
        private static final Factory<Signature> FACTORY = new Factory<Signature>() {
            public Signature get(String a, Provider p) throws GeneralSecurityException {
                return Signature.getInstance(a, p);
            }
        };
        private final String mAlgorithm;
        private Signature mSignature;
        private AlgorithmParameterSpec mParams;

        SignatureWrap(String algorithm) {
            mAlgorithm = algorithm;
        }

        private Signature signature() throws SignatureException {
            if (mSignature == null) {
                throw new SignatureException("Signature not initialized");
            }
            return mSignature;
        }

        private void init(final boolean sign, final Key key, final SecureRandom random)
                throws InvalidKeyException {
            try {
                mSignature = choose(mAlgorithm, FACTORY, new Init<Signature>() {
                    public void init(Signature s) throws GeneralSecurityException {
                        if (mParams != null) s.setParameter(mParams);
                        if (!sign) {
                            s.initVerify((PublicKey) key);
                        } else if (random != null) {
                            s.initSign((PrivateKey) key, random);
                        } else {
                            s.initSign((PrivateKey) key);
                        }
                    }
                });
            } catch (InvalidAlgorithmParameterException e) {
                throw new InvalidKeyException(e);
            }
        }

        @Override
        protected void engineInitVerify(PublicKey key) throws InvalidKeyException {
            init(false, key, null);
        }

        @Override
        protected void engineInitSign(PrivateKey key) throws InvalidKeyException {
            init(true, key, null);
        }

        @Override
        protected void engineInitSign(PrivateKey key, SecureRandom random)
                throws InvalidKeyException {
            init(true, key, random);
        }

        @Override
        protected void engineSetParameter(AlgorithmParameterSpec params)
                throws InvalidAlgorithmParameterException {
            mParams = params;
            if (mSignature != null) mSignature.setParameter(params);
        }

        @Override
        protected void engineUpdate(byte b) throws SignatureException {
            signature().update(b);
        }

        @Override
        protected void engineUpdate(byte[] b, int off, int len) throws SignatureException {
            signature().update(b, off, len);
        }

        @Override
        protected byte[] engineSign() throws SignatureException {
            return signature().sign();
        }

        @Override
        protected boolean engineVerify(byte[] sig) throws SignatureException {
            return signature().verify(sig);
        }

        @Override
        @Deprecated
        protected void engineSetParameter(String param, Object value) {
            throw new InvalidParameterException("not supported");
        }

        @Override
        @Deprecated
        protected Object engineGetParameter(String param) {
            throw new InvalidParameterException("not supported");
        }
    }

    // --- the store: the app's keys, in memory and in its data directory ---------------------

    /** A key, and for a private key its certificate chain. */
    static final class Entry {
        final Key key;
        final Certificate[] chain;
        final Date created;

        Entry(Key key, Certificate[] chain, Date created) {
            this.key = key;
            this.chain = chain;
            this.created = created;
        }
    }

    private static Map<String, Entry> sEntries;

    private static File file() {
        String pkg = android.app.ActivityThread.currentPackageName();
        return new File("/data/data/" + (pkg != null ? pkg : "android"), ".arcwasm-keystore");
    }

    static synchronized Map<String, Entry> entries() {
        if (sEntries == null) {
            sEntries = new LinkedHashMap<String, Entry>();
            File f = file();
            if (f.isFile()) {
                try (DataInputStream in = new DataInputStream(new FileInputStream(f))) {
                    read(in, sEntries);
                } catch (Exception e) {
                    Log.w(TAG, "cannot read " + f + ": starting empty", e);
                    sEntries.clear();
                }
            }
        }
        return sEntries;
    }

    static synchronized void put(String alias, Entry e) {
        entries().put(alias, e);
        save();
    }

    static synchronized void remove(String alias) {
        if (entries().remove(alias) != null) {
            save();
        }
    }

    // "AKS1", count, then per entry: alias, created, kind (0 secret, 1 private),
    // algorithm, encoded key, certificates (count, DER each).
    private static void save() {
        File f = file();
        f.getParentFile().mkdirs();
        File tmp = new File(f.getPath() + ".tmp");
        try (DataOutputStream out = new DataOutputStream(new FileOutputStream(tmp))) {
            out.writeInt(0x414b5331);
            out.writeInt(sEntries.size());
            for (Map.Entry<String, Entry> me : sEntries.entrySet()) {
                Entry e = me.getValue();
                out.writeUTF(me.getKey());
                out.writeLong(e.created.getTime());
                out.writeByte(e.key instanceof PrivateKey ? 1 : 0);
                out.writeUTF(e.key.getAlgorithm());
                bytes(out, e.key.getEncoded());
                Certificate[] chain = e.chain != null ? e.chain : new Certificate[0];
                out.writeInt(chain.length);
                for (Certificate c : chain) {
                    bytes(out, c.getEncoded());
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "cannot write " + f, e);
            tmp.delete();
            return;
        }
        if (!tmp.renameTo(f)) {
            Log.w(TAG, "cannot replace " + f);
        }
    }

    private static void read(DataInputStream in, Map<String, Entry> into) throws Exception {
        if (in.readInt() != 0x414b5331) {
            throw new IOException("not a key store");
        }
        CertificateFactory cf = CertificateFactory.getInstance("X.509");
        for (int n = in.readInt(); n > 0; n--) {
            String alias = in.readUTF();
            Date created = new Date(in.readLong());
            boolean priv = in.readByte() == 1;
            String alg = in.readUTF();
            byte[] encoded = bytes(in);
            Certificate[] chain = new Certificate[in.readInt()];
            for (int i = 0; i < chain.length; i++) {
                chain[i] = cf.generateCertificate(new ByteArrayInputStream(bytes(in)));
            }
            Key key = priv ? KeyFactory.getInstance(alg).generatePrivate(
                    new PKCS8EncodedKeySpec(encoded)) : new SecretKeySpec(encoded, alg);
            into.put(alias, new Entry(key, chain.length > 0 ? chain : null, created));
        }
    }

    private static void bytes(DataOutputStream out, byte[] b) throws IOException {
        out.writeInt(b.length);
        out.write(b);
    }

    private static byte[] bytes(DataInputStream in) throws IOException {
        byte[] b = new byte[in.readInt()];
        in.readFully(b);
        return b;
    }

    /** KeyStore "AndroidKeyStore". load(null) is all it needs; no passwords. */
    public static final class Spi extends KeyStoreSpi {
        @Override
        public Key engineGetKey(String alias, char[] password) {
            Entry e = entries().get(alias);
            return e != null ? e.key : null;
        }

        @Override
        public Certificate[] engineGetCertificateChain(String alias) {
            Entry e = entries().get(alias);
            return e != null && e.chain != null ? e.chain.clone() : null;
        }

        @Override
        public Certificate engineGetCertificate(String alias) {
            Certificate[] chain = engineGetCertificateChain(alias);
            return chain != null && chain.length > 0 ? chain[0] : null;
        }

        @Override
        public Date engineGetCreationDate(String alias) {
            Entry e = entries().get(alias);
            return e != null ? new Date(e.created.getTime()) : null;
        }

        @Override
        public void engineSetKeyEntry(String alias, Key key, char[] password, Certificate[] chain)
                throws KeyStoreException {
            if (key instanceof PrivateKey && (chain == null || chain.length == 0)) {
                throw new KeyStoreException("a private key needs its certificate chain");
            }
            if (!(key instanceof PrivateKey) && !(key instanceof SecretKey)) {
                throw new KeyStoreException("unsupported key " + key);
            }
            Key stored = key;
            if (key instanceof SecretKey) {
                byte[] encoded = key.getEncoded();
                if (encoded == null) {
                    throw new KeyStoreException("the key has no encoded form");
                }
                stored = new SecretKeySpec(encoded, key.getAlgorithm());
            }
            put(alias, new Entry(stored, chain, new Date()));
        }

        @Override
        public void engineSetKeyEntry(String alias, byte[] key, Certificate[] chain)
                throws KeyStoreException {
            throw new KeyStoreException("protected key bytes are not supported");
        }

        @Override
        public void engineSetCertificateEntry(String alias, Certificate cert)
                throws KeyStoreException {
            Entry e = entries().get(alias);
            if (e != null && e.key != null) {
                throw new KeyStoreException("a key entry is there: " + alias);
            }
            throw new KeyStoreException("certificate entries are not supported");
        }

        @Override
        public void engineDeleteEntry(String alias) {
            remove(alias);
        }

        @Override
        public Enumeration<String> engineAliases() {
            synchronized (ArcKeyStore.class) {
                return Collections.enumeration(new java.util.ArrayList<String>(
                        entries().keySet()));
            }
        }

        @Override
        public boolean engineContainsAlias(String alias) {
            return entries().containsKey(alias);
        }

        @Override
        public int engineSize() {
            return entries().size();
        }

        @Override
        public boolean engineIsKeyEntry(String alias) {
            return entries().containsKey(alias);
        }

        @Override
        public boolean engineIsCertificateEntry(String alias) {
            return false;
        }

        @Override
        public String engineGetCertificateAlias(Certificate cert) {
            synchronized (ArcKeyStore.class) {
                for (Map.Entry<String, Entry> me : entries().entrySet()) {
                    Certificate[] c = me.getValue().chain;
                    if (c != null && c.length > 0 && c[0].equals(cert)) {
                        return me.getKey();
                    }
                }
            }
            return null;
        }

        // As AOSP's AndroidKeyStoreSpi: entries without a password, and
        // KeyProtection (a protection parameter JCA's default refuses).
        @Override
        public java.security.KeyStore.Entry engineGetEntry(String alias,
                java.security.KeyStore.ProtectionParameter param) {
            Entry e = entries().get(alias);
            if (e == null) {
                return null;
            }
            if (e.key instanceof PrivateKey) {
                return new java.security.KeyStore.PrivateKeyEntry((PrivateKey) e.key,
                        e.chain.clone());
            }
            return new java.security.KeyStore.SecretKeyEntry((SecretKey) e.key);
        }

        @Override
        public void engineSetEntry(String alias, java.security.KeyStore.Entry entry,
                java.security.KeyStore.ProtectionParameter param) throws KeyStoreException {
            if (entry instanceof java.security.KeyStore.PrivateKeyEntry) {
                java.security.KeyStore.PrivateKeyEntry pe =
                        (java.security.KeyStore.PrivateKeyEntry) entry;
                engineSetKeyEntry(alias, pe.getPrivateKey(), null, pe.getCertificateChain());
            } else if (entry instanceof java.security.KeyStore.SecretKeyEntry) {
                engineSetKeyEntry(alias,
                        ((java.security.KeyStore.SecretKeyEntry) entry).getSecretKey(), null, null);
            } else if (entry instanceof java.security.KeyStore.TrustedCertificateEntry) {
                engineSetCertificateEntry(alias,
                        ((java.security.KeyStore.TrustedCertificateEntry) entry).getTrustedCertificate());
            } else {
                throw new KeyStoreException("unsupported entry " + entry);
            }
        }

        @Override
        public boolean engineEntryInstanceOf(String alias,
                Class<? extends java.security.KeyStore.Entry> entryClass) {
            Entry e = entries().get(alias);
            if (e == null) {
                return false;
            }
            return e.key instanceof PrivateKey
                    ? entryClass == java.security.KeyStore.PrivateKeyEntry.class
                    : entryClass == java.security.KeyStore.SecretKeyEntry.class;
        }

        @Override
        public void engineStore(OutputStream stream, char[] password) {
            // Kept as it changes.
        }

        @Override
        public void engineLoad(InputStream stream, char[] password) {
            entries();
        }

        @Override
        public void engineLoad(java.security.KeyStore.LoadStoreParameter param) {
            entries();
        }
    }

    // --- secret keys ------------------------------------------------------------------------

    /** The alias, size of a KeyGenParameterSpec (or KeyPairGeneratorSpec); else null. */
    static String alias(AlgorithmParameterSpec spec) {
        if (spec instanceof android.security.keystore.KeyGenParameterSpec) {
            return ((android.security.keystore.KeyGenParameterSpec) spec).getKeystoreAlias();
        }
        if (spec instanceof android.security.KeyPairGeneratorSpec) {
            return ((android.security.KeyPairGeneratorSpec) spec).getKeystoreAlias();
        }
        return null;
    }

    static int keySize(AlgorithmParameterSpec spec) {
        if (spec instanceof android.security.keystore.KeyGenParameterSpec) {
            return ((android.security.keystore.KeyGenParameterSpec) spec).getKeySize();
        }
        if (spec instanceof android.security.KeyPairGeneratorSpec) {
            return ((android.security.KeyPairGeneratorSpec) spec).getKeySize();
        }
        return -1;
    }

    abstract static class KeyGen extends KeyGeneratorSpi {
        private final String mAlgorithm;
        private final int mDefaultBits;
        private String mAlias;
        private int mBits;
        private SecureRandom mRandom;

        KeyGen(String algorithm, int defaultBits) {
            mAlgorithm = algorithm;
            mDefaultBits = defaultBits;
        }

        @Override
        protected void engineInit(SecureRandom random) {
            throw new IllegalStateException(
                    "AndroidKeyStore keys need a KeyGenParameterSpec (an alias)");
        }

        @Override
        protected void engineInit(int keysize, SecureRandom random) {
            engineInit(random);
        }

        @Override
        protected void engineInit(AlgorithmParameterSpec params, SecureRandom random)
                throws InvalidAlgorithmParameterException {
            mAlias = alias(params);
            if (mAlias == null) {
                throw new InvalidAlgorithmParameterException(
                        "a KeyGenParameterSpec is needed, not " + params);
            }
            int bits = keySize(params);
            mBits = bits > 0 ? bits : mDefaultBits;
            mRandom = random != null ? random : new SecureRandom();
        }

        @Override
        protected SecretKey engineGenerateKey() {
            if (mAlias == null) {
                throw new IllegalStateException("not initialized");
            }
            byte[] bytes = new byte[(mBits + 7) / 8];
            mRandom.nextBytes(bytes);
            SecretKey key = new SecretKeySpec(bytes, mAlgorithm);
            put(mAlias, new Entry(key, null, new Date()));
            return key;
        }
    }

    public static final class KeyGenAES extends KeyGen {
        public KeyGenAES() { super("AES", 256); }
    }
    public static final class KeyGenHmacSHA1 extends KeyGen {
        public KeyGenHmacSHA1() { super("HmacSHA1", 160); }
    }
    public static final class KeyGenHmacSHA224 extends KeyGen {
        public KeyGenHmacSHA224() { super("HmacSHA224", 224); }
    }
    public static final class KeyGenHmacSHA256 extends KeyGen {
        public KeyGenHmacSHA256() { super("HmacSHA256", 256); }
    }
    public static final class KeyGenHmacSHA384 extends KeyGen {
        public KeyGenHmacSHA384() { super("HmacSHA384", 384); }
    }
    public static final class KeyGenHmacSHA512 extends KeyGen {
        public KeyGenHmacSHA512() { super("HmacSHA512", 512); }
    }

    // --- key pairs ----------------------------------------------------------------------------

    abstract static class PairGen extends java.security.KeyPairGeneratorSpi {
        private final String mAlgorithm;
        private AlgorithmParameterSpec mSpec;
        private SecureRandom mRandom;

        PairGen(String algorithm) {
            mAlgorithm = algorithm;
        }

        @Override
        public void initialize(int keysize, SecureRandom random) {
            throw new IllegalArgumentException(
                    "AndroidKeyStore key pairs need a KeyGenParameterSpec (an alias)");
        }

        @Override
        public void initialize(AlgorithmParameterSpec params, SecureRandom random)
                throws InvalidAlgorithmParameterException {
            if (alias(params) == null) {
                throw new InvalidAlgorithmParameterException(
                        "a KeyGenParameterSpec is needed, not " + params);
            }
            mSpec = params;
            mRandom = random != null ? random : new SecureRandom();
        }

        @Override
        public KeyPair generateKeyPair() {
            if (mSpec == null) {
                throw new IllegalStateException("not initialized");
            }
            try {
                KeyPairGenerator g = KeyPairGenerator.getInstance(mAlgorithm);
                AlgorithmParameterSpec inner = null;
                int bits = keySize(mSpec);
                if (mSpec instanceof android.security.keystore.KeyGenParameterSpec) {
                    inner = ((android.security.keystore.KeyGenParameterSpec) mSpec)
                            .getAlgorithmParameterSpec();
                } else if (mSpec instanceof android.security.KeyPairGeneratorSpec) {
                    inner = ((android.security.KeyPairGeneratorSpec) mSpec)
                            .getAlgorithmParameterSpec();
                }
                if (inner instanceof ECGenParameterSpec || inner instanceof RSAKeyGenParameterSpec) {
                    g.initialize(inner, mRandom);
                } else if ("EC".equals(mAlgorithm)) {
                    g.initialize(new ECGenParameterSpec(bits == 384 ? "secp384r1"
                            : bits == 521 ? "secp521r1" : bits == 224 ? "secp224r1"
                            : "secp256r1"), mRandom);
                } else {
                    g.initialize(new RSAKeyGenParameterSpec(bits > 0 ? bits : 2048,
                            RSAKeyGenParameterSpec.F4), mRandom);
                }
                KeyPair pair = g.generateKeyPair();
                Certificate cert = selfSigned(pair, mSpec);
                put(alias(mSpec), new Entry(pair.getPrivate(), new Certificate[] {cert},
                        new Date()));
                return pair;
            } catch (Exception e) {
                throw new IllegalStateException("cannot generate a " + mAlgorithm + " key pair", e);
            }
        }
    }

    public static final class PairGenRSA extends PairGen {
        public PairGenRSA() { super("RSA"); }
    }
    public static final class PairGenEC extends PairGen {
        public PairGenEC() { super("EC"); }
    }

    /** The key pair's certificate, signed with its own key, of the spec's subject and dates. */
    static Certificate selfSigned(KeyPair pair, AlgorithmParameterSpec spec) throws Exception {
        X500Principal subject = null;
        BigInteger serial = null;
        Date start = null, end = null;
        if (spec instanceof android.security.keystore.KeyGenParameterSpec) {
            android.security.keystore.KeyGenParameterSpec s =
                    (android.security.keystore.KeyGenParameterSpec) spec;
            subject = s.getCertificateSubject();
            serial = s.getCertificateSerialNumber();
            start = s.getCertificateNotBefore();
            end = s.getCertificateNotAfter();
        } else if (spec instanceof android.security.KeyPairGeneratorSpec) {
            android.security.KeyPairGeneratorSpec s = (android.security.KeyPairGeneratorSpec) spec;
            subject = s.getSubjectDN();
            serial = s.getSerialNumber();
            start = s.getStartDate();
            end = s.getEndDate();
        }
        if (subject == null) subject = new X500Principal("CN=fake");
        if (serial == null) serial = BigInteger.ONE;
        if (start == null) start = new Date(0);
        if (end == null) end = new Date(2461449600000L);  // 2048-01-01, as keystore's default

        boolean ec = "EC".equals(pair.getPublic().getAlgorithm());
        com.android.org.bouncycastle.asn1.x509.AlgorithmIdentifier sigAlg =
                new com.android.org.bouncycastle.asn1.x509.AlgorithmIdentifier(ec
                        ? com.android.org.bouncycastle.asn1.x9.X9ObjectIdentifiers.ecdsa_with_SHA256
                        : com.android.org.bouncycastle.asn1.pkcs.PKCSObjectIdentifiers
                                .sha256WithRSAEncryption);
        com.android.org.bouncycastle.asn1.x509.V3TBSCertificateGenerator tbs =
                new com.android.org.bouncycastle.asn1.x509.V3TBSCertificateGenerator();
        tbs.setSerialNumber(new com.android.org.bouncycastle.asn1.ASN1Integer(serial));
        com.android.org.bouncycastle.asn1.x500.X500Name name =
                com.android.org.bouncycastle.asn1.x500.X500Name.getInstance(subject.getEncoded());
        tbs.setSubject(name);
        tbs.setIssuer(name);
        tbs.setStartDate(new com.android.org.bouncycastle.asn1.x509.Time(start));
        tbs.setEndDate(new com.android.org.bouncycastle.asn1.x509.Time(end));
        tbs.setSignature(sigAlg);
        tbs.setSubjectPublicKeyInfo(com.android.org.bouncycastle.asn1.x509.SubjectPublicKeyInfo
                .getInstance(pair.getPublic().getEncoded()));
        com.android.org.bouncycastle.asn1.x509.TBSCertificate tbsCert = tbs.generateTBSCertificate();
        Signature signer = Signature.getInstance(ec ? "SHA256withECDSA" : "SHA256withRSA");
        signer.initSign(pair.getPrivate());
        signer.update(tbsCert.getEncoded());
        com.android.org.bouncycastle.asn1.ASN1EncodableVector v =
                new com.android.org.bouncycastle.asn1.ASN1EncodableVector();
        v.add(tbsCert);
        v.add(sigAlg);
        v.add(new com.android.org.bouncycastle.asn1.DERBitString(signer.sign()));
        byte[] der = new com.android.org.bouncycastle.asn1.DERSequence(v).getEncoded();
        return CertificateFactory.getInstance("X.509").generateCertificate(
                new ByteArrayInputStream(der));
    }
}
