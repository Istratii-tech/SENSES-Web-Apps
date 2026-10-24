package arcwasm;

import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.os.ServiceManager;

import arcwasm.server.SystemServer;

import dalvik.system.PathClassLoader;

import java.io.File;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Arrays;

/**
 * ARM NDK code in an app (topic ndk-bridge): the test APK
 * (tests/ndk/apk, org.arcwasm.ndktest) carries libndktest.so and
 * libndkhelper.so, built from tests/ndk for armeabi and armeabi-v7a; the
 * same APK as org.arcwasm.ndktest.v5 has only armeabi, so both ABIs run. The "package"
 * service installs it (copying the libraries to its nativeLibraryDir), and
 * its class org.arcwasm.ndktest.NativeLib is loaded the way an app's code
 * is: by a PathClassLoader with the APK and that directory, so its
 * System.loadLibrary calls go through Dalvik's dlopen to the NDK bridge
 * (shim/ndk) and the ARM CPU. Then Java calls the natives and they call
 * back: JNI both ways, libc, pthreads, C++, soft-float math. Prints one line
 * per check and exits with the number of failures.
 */
public class NdkTest {
    // The same code twice: with libraries for both ABIs (the package
    // service picks armeabi-v7a) and with armeabi only.
    private static final String[][] PACKAGES = {
        {"org.arcwasm.ndktest", "armeabi-v7a"},
        {"org.arcwasm.ndktest.v5", "armeabi"},
    };

    static int passed;
    static int failed;
    private static Class<?> lib;
    static ApplicationInfo info;

    interface Check {
        void run() throws Exception;
    }

    static String prefix = "";

    static void check(String name, Check c) {
        name = prefix + name;
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

    private static Object call(String name, Object... args) throws Exception {
        for (Method m : lib.getDeclaredMethods()) {
            if (m.getName().equals(name)) {
                m.setAccessible(true);
                return m.invoke(null, args);
            }
        }
        throw new NoSuchMethodException(name);
    }

    private static Object callOn(Object target, String name, Object... args) throws Exception {
        for (Method m : lib.getDeclaredMethods()) {
            if (m.getName().equals(name)) {
                m.setAccessible(true);
                return m.invoke(target, args);
            }
        }
        throw new NoSuchMethodException(name);
    }

    /** The expression of NativeLib.math in libndktest.so, in Java. */
    private static double math(double x) {
        double r = Math.sin(x) + Math.cos(x) * 2 + Math.sqrt(Math.abs(x)) + Math.pow(x, 3)
                + Math.floor(x * 10) / 10 + (x % 0.7) + Math.atan2(x, 2) + Math.exp(x / 10)
                + Math.log(Math.abs(x) + 1) + x * 8 + Math.tanh(x);
        float f = (float) x;
        r += (float) Math.sin(f) + (float) Math.sqrt(Math.abs(f)) + (float) Math.floor(f * 3f)
                + (float) Math.atan2(f, 3.0f) + (f % 0.25f);
        int e = Math.getExponent(x * 100) + 1;
        r += x * 100 / Math.pow(2, e) + e;
        double ip = x < 0 ? Math.ceil(x) : Math.floor(x);
        r += (x - ip) + ip;
        return r;
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        SystemServer.start();
        for (String[] p : PACKAGES) {
            prefix = "[" + p[1] + "] ";
            lib = null;
            runChecks(p[0], p[1]);
        }
        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("NdkTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }

    static void runChecks(final String PKG, final String abi) throws Exception {
        check("package service: the APK is installed, its " + abi + " libraries copied", new Check() {
            public void run() throws Exception {
                IPackageManager pm = IPackageManager.Stub.asInterface(ServiceManager.getService("package"));
                info = pm.getApplicationInfo(PKG, 0, 0);
                expect(info != null, "no package " + PKG);
                expectEq("/data/app-lib/" + PKG, info.nativeLibraryDir);
                for (String so : new String[] {"libndktest.so", "libndkhelper.so"}) {
                    File f = new File(info.nativeLibraryDir, so);
                    expect(f.isFile() && f.length() > 1000, "missing " + f);
                }
            }
        });

        check("System.loadLibrary from the app's class loader runs JNI_OnLoad", new Check() {
            public void run() throws Exception {
                PathClassLoader loader = new PathClassLoader(info.sourceDir, info.nativeLibraryDir,
                        NdkTest.class.getClassLoader());
                lib = Class.forName("org.arcwasm.ndktest.NativeLib", true, loader);
                expectEq(info.nativeLibraryDir + "/libndktest.so", loader.findLibrary("ndktest"));
                // JNI_OnLoad stored GetVersion() and registered onLoadVersion.
                expectEq(Integer.valueOf(0x10006), call("onLoadVersion"));
                expectEq(abi, call("abi"));
            }
        });
        if (lib == null) {
            return;
        }

        check("Java_ natives: strings and every primitive type", new Check() {
            public void run() throws Exception {
                expectEq("Hello from ARM", call("stringFromJNI"));
                expectEq(Integer.valueOf(-5), call("add", 7, -12));
                expectEq(Long.valueOf(0x123456789L * -1000L), call("mulLong", 0x123456789L, -1000L));
                expectEq(Double.valueOf(3 + 2.5 * 2 + 0.75f + (double) (1L << 40) / 4 + 9000),
                         call("mix", 3, 2.5, 0.75f, 1L << 40, 9));
                expectEq(Float.valueOf(-1.25f), call("half", -2.5f));
                expectEq(Boolean.FALSE, call("not", true));
                expectEq(Character.valueOf('b'), call("nextChar", 'a'));
                expectEq(Short.valueOf((short) -300), call("negShort", (short) 300));
                expectEq(Byte.valueOf((byte) 128), call("negByte", (byte) -128));
                expectEq("a+bcd(3)", call("concat", "a", "bcd"));
            }
        });

        check("arrays: Get/Release<Int>ArrayElements, NewIntArray, SetIntArrayRegion", new Check() {
            public void run() throws Exception {
                int[] out = (int[]) call("squares", new int[] {1, -2, 3, 40000});
                expectEq("[1, 4, 9, 1600000000]", Arrays.toString(out));
            }
        });

        check("RegisterNatives from JNI_OnLoad", new Check() {
            public void run() throws Exception {
                expectEq(Integer.valueOf(6 * 7 + 0x10006), call("registered", 6));
            }
        });

        check("arm64-v8a".equals(abi) ? "libm (doubles in V registers)"
                : "soft-float libm (doubles in core registers)", new Check() {
            public void run() throws Exception {
                for (double x : new double[] {0.3, -1.7, 2.25, 10.5}) {
                    double got = (Double) call("math", x);
                    double want = math(x);
                    expect(Math.abs(got - want) < 1e-6 * Math.max(1, Math.abs(want)),
                           "math(" + x + ") = " + got + ", want " + want);
                }
            }
        });

        check("native calls back: GetMethodID, CallIntMethod, GetIntField", new Check() {
            public void run() throws Exception {
                Object o = lib.newInstance();
                expectEq(Integer.valueOf(12 + 1000), callOn(o, "callback", 4));
                expectEq(Integer.valueOf(21 + 2000), callOn(o, "callback", 7));
            }
        });

        check("Call*Method varargs, A and V forms; NewObject, AllocObject, nonvirtual", new Check() {
            public void run() throws Exception {
                expectEq("12345678a 1.75 s! 15 1", call("callStatics"));
                expectEq("7,s,0.5,1.25,8589934592,true,q;-1,s,-2.0,0.75,-5,false,A;",
                         call("getLog"));
            }
        });

        check("libc: stdio, strings, ctype, errno, files, time, setjmp", new Check() {
            public void run() throws Exception {
                expectEq("ok", call("libc", info.dataDir));
            }
        });

        check("pthreads: create/join, mutex, cond, once, keys", new Check() {
            public void run() throws Exception {
                expectEq("ok", call("threads"));
            }
        });

        check("C++: static constructors, new/delete, virtual calls, local statics", new Check() {
            public void run() throws Exception {
                expectEq("ok", call("cxx"));
            }
        });

        check("a second guest library (DT_NEEDED): functions, data, constructor order", new Check() {
            public void run() throws Exception {
                expectEq("helper 5 1 41 1099511627781", call("helper"));
            }
        });

        check("a native thread attaches to the VM and calls Java", new Check() {
            public void run() throws Exception {
                expectEq("ok", call("fromThread"));
                expect(((String) call("getLog")).endsWith("thread7;"), "log: " + call("getLog"));
            }
        });

        check("an exception thrown by native code (ThrowNew)", new Check() {
            public void run() throws Exception {
                try {
                    call("throwIn");
                    throw new AssertionError("no exception");
                } catch (InvocationTargetException e) {
                    expect(e.getCause() instanceof IllegalStateException, "" + e.getCause());
                    expectEq("thrown from ARM", e.getCause().getMessage());
                }
            }
        });
    }
}
