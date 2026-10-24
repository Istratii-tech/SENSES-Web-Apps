package arcwasm;

import dalvik.system.PathClassLoader;
import java.io.File;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Real apps' arm64-v8a libraries on the AArch64 bridge (topic ndk-arm64,
 * docs/topics/ndk-arm64.md). For each /system/ndk64-apps/NAME (made by
 * tools/ndk64/prepare_apps.py from APKs that are not in git: lib/*.so and a
 * base.apk with the app's dex), every library is loaded as the app's class
 * loader would load it (Runtime.load0 from a class of that loader): the ELF64
 * loader maps it, binds its imports (unresolved ones are logged), runs its
 * constructors and JNI_OnLoad. A library that does not load is retried after
 * the others (dependencies first). Prints one line per library and exits with
 * the number of failures; with no apps there, nothing to check.
 *
 * Arguments: optional app names (default: every directory).
 */
public class Arm64AppsTest {
    private static final File ROOT = new File("/system/ndk64-apps");

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();
        int passed = 0, failed = 0;
        String[] apps = args.length > 0 ? args : ROOT.list();
        if (apps == null) {
            apps = new String[0];
        }
        Arrays.sort(apps);
        for (String app : apps) {
            File dir = new File(ROOT, app);
            File[] libs = new File(dir, "lib").listFiles();
            if (libs == null) {
                continue;
            }
            Arrays.sort(libs);
            PathClassLoader loader = new PathClassLoader(new File(dir, "base.apk").getPath(),
                    new File(dir, "lib").getPath(), Arm64AppsTest.class.getClassLoader());
            Method load0 = Runtime.class.getDeclaredMethod("load0", Class.class, String.class);
            load0.setAccessible(true);
            List<File> pending = new ArrayList<File>(Arrays.asList(libs));
            List<String> errors = new ArrayList<String>();
            // Runtime.load0 takes the loader from the class: one the app's
            // loader defined, so that JNI_OnLoad's FindClass looks there.
            Class<?> from = anyAppClass(loader, dir);
            if (from == null) {
                from = Arm64AppsTest.class;
            }
            for (int round = 0; !pending.isEmpty() && round < 4; round++) {
                List<File> next = new ArrayList<File>();
                errors.clear();
                for (File so : pending) {
                    long t0 = System.nanoTime();
                    try {
                        load0.invoke(Runtime.getRuntime(), from, so.getPath());
                        passed++;
                        System.out.println("[ OK ] " + app + ": " + so.getName() + " ("
                                + (System.nanoTime() - t0) / 1000000 + " ms)");
                    } catch (java.lang.reflect.InvocationTargetException e) {
                        next.add(so);
                        errors.add(so.getName() + ": " + e.getCause());
                    }
                }
                pending = next;
            }
            for (String e : errors) {
                failed++;
                System.out.println("[FAIL] " + app + ": " + e);
            }
        }
        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("Arm64AppsTest: " + passed + " passed, " + failed + " failed (" + ms
                + " ms)");
        System.exit(failed);
    }

    /** A class of the app's dex (by the loader itself), or null. */
    private static Class<?> anyAppClass(ClassLoader loader, File dir) {
        try {
            // The first class of the dex that loads (dalvik.system.DexFile lists them).
            @SuppressWarnings("deprecation")
            dalvik.system.DexFile dex = new dalvik.system.DexFile(new File(dir, "base.apk"));
            try {
                for (java.util.Enumeration<String> e = dex.entries(); e.hasMoreElements();) {
                    String name = e.nextElement();
                    try {
                        Class<?> c = Class.forName(name, false, loader);
                        if (c.getClassLoader() == loader) {
                            return c;
                        }
                    } catch (Throwable ignored) {
                        // a class whose superclass is missing here: the next one
                    }
                }
            } finally {
                dex.close();
            }
        } catch (Throwable t) {
            System.out.println("no class of " + dir + ": " + t);
        }
        return null;
    }
}
