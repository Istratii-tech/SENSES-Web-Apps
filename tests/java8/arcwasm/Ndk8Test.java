package arcwasm;

import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.os.ServiceManager;

import arcwasm.server.SystemServer;

import dalvik.system.PathClassLoader;

import java.io.File;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/**
 * What NDK libraries built for Android 7.0-8.1 (API 24-27) import from the
 * system (topic fw8-system): the test APK (tests/ndk8/apk,
 * org.arcwasm.ndk8test, minSdk 26) carries libndk8test.so
 * (tests/ndk8/ndk8test.c, armeabi-v7a), loaded the way an app loads its
 * code. It links libsync, libnativewindow and libmediandk as Firefox's libxul
 * does, and checks bionic's additions (stdin/stdout/stderr variables, *64,
 * API 24 FORTIFY, clock_nanosleep, getifaddrs, barriers, spin locks,
 * sendmmsg/recvmmsg) and ASharedMemory. One line per check; exits with the
 * number of failures.
 */
public class Ndk8Test {
    private static final String PKG = "org.arcwasm.ndk8test";

    private static int passed;
    private static int failed;
    private static Class<?> lib;
    private static ApplicationInfo info;

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
            if (!(t instanceof AssertionError)) t.printStackTrace(System.out);
        }
    }

    private static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    private static void nativeCheck(String name, Object... args) throws Exception {
        for (Method m : lib.getDeclaredMethods()) {
            if (m.getName().equals(name)) {
                String r;
                try {
                    r = (String) m.invoke(null, args);
                } catch (InvocationTargetException e) {
                    throw (Exception) e.getCause();
                }
                expect(r != null && r.startsWith("ok"), String.valueOf(r));
                if (r.length() > 2) System.out.println("       " + r.substring(3));
                return;
            }
        }
        throw new NoSuchMethodException(name);
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        SystemServer.start();
        check("package service: the minSdk 26 APK is installed, its armeabi-v7a library copied",
                new Check() {
            public void run() throws Exception {
                IPackageManager pm = IPackageManager.Stub.asInterface(
                        ServiceManager.getService("package"));
                info = pm.getApplicationInfo(PKG, 0, 0);
                expect(info != null, "no package " + PKG);
                expect(info.minSdkVersion == 26, "minSdk " + info.minSdkVersion);
                File f = new File(info.nativeLibraryDir, "libndk8test.so");
                expect(f.isFile() && f.length() > 1000, "missing " + f);
            }
        });
        check("System.loadLibrary: libndk8test.so (DT_NEEDED libsync, libnativewindow, "
                + "libmediandk)", new Check() {
            public void run() throws Exception {
                PathClassLoader loader = new PathClassLoader(info.sourceDir, info.nativeLibraryDir,
                        Ndk8Test.class.getClassLoader());
                lib = Class.forName("org.arcwasm.ndk8test.Native8", true, loader);
            }
        });
        if (lib != null) {
            final File dir = new File(info.dataDir);
            dir.mkdirs();
            check("libc: stdin/stdout/stderr variables, in6addr_*, clock_nanosleep, "
                    + "posix_madvise, open64/stat64, dprintf, __fread_chk, stpcpy, nl_langinfo, "
                    + "long double math", new Check() {
                public void run() throws Exception {
                    nativeCheck("libc", dir.getPath());
                }
            });
            check("threads: pthread_barrier_*, pthread_spin_*", new Check() {
                public void run() throws Exception {
                    nativeCheck("threads");
                }
            });
            check("net: getifaddrs, if_nameindex, sendmmsg/recvmmsg", new Check() {
                public void run() throws Exception {
                    nativeCheck("net");
                }
            });
            check("ASharedMemory_create/getSize/setProt: one region, two MAP_SHARED mappings",
                    new Check() {
                public void run() throws Exception {
                    nativeCheck("sharedMemory");
                }
            });
            check("libsync sync_merge, AHardwareBuffer (gralloc or none), libmediandk, "
                    + "android_dlopen_ext", new Check() {
                public void run() throws Exception {
                    nativeCheck("api26Libraries");
                }
            });
        }
        System.out.println("Ndk8Test: " + passed + " passed, " + failed + " failed ("
                + (System.nanoTime() - start) / 1000000 + " ms)");
        System.exit(failed);
    }
}
