package arcwasm;

import android.app.ActivityManager;
import android.content.ComponentCallbacks2;
import android.content.ComponentName;
import android.content.Intent;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Process;
import android.os.StatFs;
import android.os.SystemProperties;
import android.view.View;

import arcwasm.server.MemoryBudget;
import arcwasm.server.MemoryMonitor;
import arcwasm.server.SystemServer;
import arcwasm.server.WindowManagerService;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/**
 * The memory budget (topic mem-budget, docs/topics/mem-budget.md) on ART 8.1
 * with the test app of topic fw8-system (tests/apk8):
 *
 *   the budget  /proc/meminfo, ActivityManager's memory classes, MemoryInfo,
 *               the Java heap's limits and dalvik.vm.* from it; the sample of
 *               what memory holds
 *   pressure    a smaller budget (as on a smaller device) makes the app get
 *               onTrimMemory(RUNNING_MODERATE, _LOW, _CRITICAL) and
 *               onLowMemory(); MemoryInfo.lowMemory, lastTrimLevel
 *   codecs      getMaxSupportedInstances; starting one codec more than the
 *               limit reclaims the one idle the longest (ERROR_RECLAIMED); in
 *               a browser also video decoders and their buffer count
 *   storage     near the browser's quota, the app's cache directory is
 *               emptied; StatFs("/data") tells the quota
 *   the page    hidden: the activity is stopped and its state written;
 *               shown: it resumes
 *   restart     above the budget after the trims: the task is saved and the
 *               VM restarted (Node: exit 75, which tools/run_java_tests.sh
 *               answers by starting it again with the same /data; a page
 *               reloads); the second run finds the activity restored with
 *               its state (the button's click count), and is not restarted
 *               again when it is over the budget at once (no restart loop).
 *
 * The first run leaves its counts in /data for the second, which prints the
 * summary and exits with the number of failures.
 */
public class MemoryTest {
    private static final String PKG = "org.arcwasm.testapp8";
    private static final ComponentName MAIN = new ComponentName(PKG, PKG + ".MainActivity");
    private static final File MARK = new File("/data/local/tmp/memorytest/phase");
    private static final File TASK_STATE = new File("/data/system/arcwasm/task-state.bin");
    private static final long MB = 1024 * 1024;

    private static int passed;
    private static int failed;
    private static int skipped;
    private static long sStart;

    interface Check {
        void run() throws Exception;
    }

    static final class Skip extends RuntimeException {
        Skip(String why) {
            super(why);
        }
    }

    private static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
        } catch (Skip s) {
            skipped++;
            System.out.println("[SKIP] " + name + ": " + s.getMessage());
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

    private static int countEvents(String prefix) throws Exception {
        int n = 0;
        for (String e : ActivityTest8.events()) {
            if (e.startsWith(prefix)) n++;
        }
        return n;
    }

    private static void awaitCount(String prefix, int count) throws Exception {
        long deadline = System.currentTimeMillis() + 30000;
        while (countEvents(prefix) < count) {
            if (System.currentTimeMillis() > deadline) {
                throw new AssertionError("waited for " + count + " x " + prefix + ", events: "
                        + ActivityTest8.events());
            }
            Thread.sleep(20);
        }
    }

    /** In use of the budget now, after the VM's garbage is gone (as the monitor sees it). */
    private static long inUse() {
        for (int i = 0; i < 2; i++) {
            System.gc();
            System.runFinalization();
        }
        return MemoryBudget.inUse(MemoryBudget.sample(true));
    }

    /** A budget that makes the current use |fraction| of it. */
    private static void budgetFor(double fraction) {
        int mb = (int) Math.max(64, inUse() / fraction / MB);
        MemoryBudget.setBudgetForTest(mb);
        MemoryMonitor.get().poke();
    }

    private static long memTotalKb() throws Exception {
        for (String line : Files.readAllLines(new File("/proc/meminfo").toPath())) {
            if (line.startsWith("MemTotal:")) {
                return Long.parseLong(line.replaceAll("[^0-9]", ""));
            }
        }
        throw new AssertionError("no MemTotal in /proc/meminfo");
    }

    private static int clicks() throws Exception {
        android.app.Activity a = ActivityTest8.live("MainActivity");
        java.lang.reflect.Field f = a.getClass().getDeclaredField("mClicks");
        f.setAccessible(true);
        return f.getInt(a);
    }

    public static void main(String[] args) throws Exception {
        sStart = System.nanoTime();
        String mark = MARK.isFile() ? new String(Files.readAllBytes(MARK.toPath()),
                StandardCharsets.UTF_8).trim() : null;
        if (mark != null) {
            String[] f = mark.split(" ");
            if (f.length == 4 && System.currentTimeMillis() - Long.parseLong(f[3]) < 600000) {
                passed = Integer.parseInt(f[0]);
                failed = Integer.parseInt(f[1]);
                skipped = Integer.parseInt(f[2]);
                MARK.delete();
                restored();
                return;
            }
            MARK.delete();  // stale: a run that did not come back
        }
        first();
    }

    // --- the first run ----------------------------------------------------------------------

    private static void first() throws Exception {
        final MemoryBudget.Config cfg = MemoryBudget.config();
        final ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();

        check("budget: the VM has it, the system server watches it", () -> {
            expect(MemoryBudget.available(), "no libarcwasm_membudget in this VM");
            SystemServer.start();
            expect(MemoryMonitor.get() != null, "no MemoryMonitor");
            expect(cfg.budgetMb >= 128 && cfg.capMb <= 4096 && cfg.capMb > 0,
                    "budget " + cfg.budgetMb + ", cap " + cfg.capMb);
            System.out.println("MemoryTest: budget " + cfg.budgetMb + " MB, cap " + cfg.capMb
                    + " MB, heap " + cfg.heapGrowthMb + "/" + cfg.heapSizeMb + " MB");
        });

        check("budget -> /proc/meminfo, Process.getTotalMemory, MemoryInfo", () -> {
            expectEq((long) cfg.budgetMb * 1024, memTotalKb());
            expectEq((long) cfg.budgetMb * MB, Process.getTotalMemory());
            ActivityManager.getService().getMemoryInfo(mi);
            expectEq((long) cfg.budgetMb * MB, mi.totalMem);
            expect(mi.availMem > 0 && mi.availMem < mi.totalMem, "availMem " + mi.availMem);
            expect(mi.threshold > 0 && mi.threshold < mi.totalMem, "threshold " + mi.threshold);
            expect(!mi.lowMemory, "lowMemory at start");
            long free = Process.getFreeMemory();
            expect(free > 0 && free <= mi.totalMem, "Process.getFreeMemory " + free);
        });

        check("budget -> memory classes, dalvik.vm.heap*, the Java heap's limit, low RAM", () -> {
            expectEq(cfg.heapGrowthMb, ActivityManager.staticGetMemoryClass());
            expectEq(cfg.heapSizeMb, ActivityManager.staticGetLargeMemoryClass());
            expectEq(cfg.heapGrowthMb + "m", SystemProperties.get("dalvik.vm.heapgrowthlimit"));
            expectEq(cfg.heapSizeMb + "m", SystemProperties.get("dalvik.vm.heapsize"));
            expectEq((long) cfg.heapGrowthMb * MB, Runtime.getRuntime().maxMemory());
            expectEq(cfg.lowRam, ActivityManager.isLowRamDeviceStatic());
        });

        check("the sample: wasm memory, malloc, Java spaces, dex, threads; in use", () -> {
            long[] s = MemoryBudget.sample(true);
            // (The page's thread counts the threads once a second.)
            for (long until = System.currentTimeMillis() + 3000;
                    s[MemoryBudget.THREADS] < 2 && System.currentTimeMillis() < until; ) {
                Thread.sleep(100);
                s = MemoryBudget.sample(true);
            }
            expect(s[MemoryBudget.WASM] > 0 && s[MemoryBudget.BRK] <= s[MemoryBudget.WASM],
                    "wasm " + s[MemoryBudget.WASM] + ", brk " + s[MemoryBudget.BRK]);
            expectEq((long) cfg.capMb * MB, s[MemoryBudget.CAP]);
            expect(s[MemoryBudget.WASM] <= s[MemoryBudget.CAP], "wasm memory above its cap");
            expect(s[MemoryBudget.MALLOC_USED] > 0 && s[MemoryBudget.MALLOC_FREE] >= 0,
                    "malloc " + s[MemoryBudget.MALLOC_USED]);
            expect(s[MemoryBudget.JAVA_SPACES] >= (long) cfg.heapSizeMb * MB,
                    "Java spaces " + s[MemoryBudget.JAVA_SPACES]);
            // (The boot jars' dex are mapped from the /system image, topic
            // boot-dex8: counted as the image, so dex is the test's own.)
            expect(s[MemoryBudget.DEX] >= 0, "dex " + s[MemoryBudget.DEX]);
            expect(s[MemoryBudget.IMAGE] > MB, "image " + s[MemoryBudget.IMAGE]);
            expect(s[MemoryBudget.THREADS] >= 2, "threads " + s[MemoryBudget.THREADS]);
            long inUse = MemoryBudget.inUse(s);
            expect(inUse > 16 * MB && inUse < s[MemoryBudget.BRK], "in use " + inUse);
            // The wasm memory touched (the page's "low memory"): some of it
            // known untouched (the Java heap's spaces at least), the rest
            // not less than what is in use but for those spaces' high water.
            long touched = MemoryBudget.touched(s, MemoryBudget.javaInSpaces(s));
            expect(s[MemoryBudget.UNTOUCHED] >= 0 && touched > 16 * MB && touched < s[MemoryBudget.BRK],
                    "touched " + touched + " (untouched " + s[MemoryBudget.UNTOUCHED] + ", brk "
                            + s[MemoryBudget.BRK] + ", image " + s[MemoryBudget.IMAGE_FILLED] + "/"
                            + s[MemoryBudget.IMAGE] + ", Java spaces " + s[MemoryBudget.JAVA_SPACES]
                            + " holding " + MemoryBudget.javaInSpaces(s) + ")");
            System.out.println("MemoryTest: " + MemoryMonitor.get().report());
            // What a sample costs (the monitor takes one a second, a precise one near the budget).
            long t0 = System.nanoTime();
            for (int i = 0; i < 20; i++) MemoryBudget.sample(false);
            long t1 = System.nanoTime();
            for (int i = 0; i < 20; i++) MemoryBudget.sample(true);
            long t2 = System.nanoTime();
            System.out.printf("MemoryTest: a sample takes %.2f ms, a precise one %.2f ms%n",
                    (t1 - t0) / 20e6, (t2 - t1) / 20e6);
        });

        check("the app's activity (count 3 clicks: its saved state later)", () -> {
            if (!WindowManagerService.hasGraphics()) {
                throw new Skip("no graphics natives in this VM (headless)");
            }
            Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
                    .setComponent(MAIN).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            expectEq(ActivityManager.START_SUCCESS, ActivityManager.getService().startActivity(
                    null, "android", intent, null, null, null, -1, 0, null, null));
            ActivityTest8.awaitEvents("MainActivity.onCreate", "MainActivity.onResume");
            for (int i = 0; i < 3; i++) {
                ActivityTest8.onUi(() -> {
                    android.app.Activity a = ActivityTest8.live("MainActivity");
                    View v = a.findViewById(a.getResources().getIdentifier("button", "id", PKG));
                    return v.performClick();
                });
            }
            ActivityTest8.awaitEvents("MainActivity.onClick 3");
            expectEq(3, clicks());
        });
        final boolean app = ActivityTest8.app() != null && countEvents("MainActivity.onResume") > 0;

        check("pressure: onTrimMemory RUNNING_MODERATE, _LOW, _CRITICAL, onLowMemory", () -> {
            if (!app) throw new Skip("no app activity");
            budgetFor(0.75);
            ActivityTest8.awaitEvents("App8.onTrimMemory "
                    + ComponentCallbacks2.TRIM_MEMORY_RUNNING_MODERATE);
            ActivityManager.RunningAppProcessInfo state = new ActivityManager.RunningAppProcessInfo();
            ActivityManager.getMyMemoryState(state);
            expectEq(ComponentCallbacks2.TRIM_MEMORY_RUNNING_MODERATE, state.lastTrimLevel);
            budgetFor(0.85);
            ActivityTest8.awaitEvents("App8.onTrimMemory " + ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW);
            ActivityManager.getService().getMemoryInfo(mi);
            expect(mi.lowMemory, "MemoryInfo.lowMemory at 85%: avail " + mi.availMem
                    + ", threshold " + mi.threshold);
            budgetFor(0.95);
            ActivityTest8.awaitEvents("App8.onTrimMemory "
                    + ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL, "App8.onLowMemory");
            // Back to the device's budget: the level goes, a new rise trims again.
            MemoryBudget.setBudgetForTest(0);
            MemoryMonitor.get().poke();
            MemoryMonitor.get().poke();
            expectEq(0, MemoryMonitor.get().lastTrimLevel());
            ActivityManager.getService().getMemoryInfo(mi);
            expect(!mi.lowMemory, "lowMemory after the budget came back");
        });

        check("codecs: getMaxSupportedInstances; one more reclaims the idle one (ERROR_RECLAIMED)",
                () -> {
            MediaCodecInfo raw = null;
            for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
                for (String t : info.getSupportedTypes()) {
                    if (t.equals("audio/raw") && !info.isEncoder()) raw = info;
                }
            }
            expect(raw != null, "no audio/raw decoder");
            expectEq(cfg.maxAudioDecoders,
                    raw.getCapabilitiesForType("audio/raw").getMaxSupportedInstances());
            MemoryBudget.setDecoderLimitsForTest(0, 2);
            List<MediaCodec> codecs = new ArrayList<MediaCodec>();
            try {
                MediaFormat f = MediaFormat.createAudioFormat("audio/raw", 44100, 2);
                for (int i = 0; i < 2; i++) {
                    MediaCodec c = MediaCodec.createDecoderByType("audio/raw");
                    c.configure(f, null, null, 0);
                    c.start();
                    codecs.add(c);
                }
                Thread.sleep(700);  // idle long enough to be reclaimed
                expect(codecs.get(1).dequeueInputBuffer(0) >= 0, "second codec");
                MediaCodec third = MediaCodec.createDecoderByType("audio/raw");
                third.configure(f, null, null, 0);
                third.start();
                codecs.add(third);
                expect(third.dequeueInputBuffer(0) >= 0, "third codec");
                try {
                    codecs.get(0).dequeueInputBuffer(0);
                    throw new AssertionError("the idle codec was not reclaimed");
                } catch (MediaCodec.CodecException e) {
                    expectEq(MediaCodec.CodecException.ERROR_RECLAIMED, e.getErrorCode());
                    expect(!e.isRecoverable() && !e.isTransient(), "reclaim is final");
                }
                expect(codecs.get(1).dequeueInputBuffer(0) >= 0, "the codec in use kept working");
            } finally {
                for (MediaCodec c : codecs) c.release();
                MemoryBudget.setDecoderLimitsForTest(cfg.maxVideoDecoders, cfg.maxAudioDecoders);
            }
        });

        check("video decoders (a browser's WebCodecs): the limit, reclaim, buffers of the budget",
                () -> {
            MediaCodecInfo avc = null;
            for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.ALL_CODECS).getCodecInfos()) {
                for (String t : info.getSupportedTypes()) {
                    if (t.equals("video/avc") && !info.isEncoder()) avc = info;
                }
            }
            if (avc == null) throw new Skip("no video decoder (no WebCodecs)");
            expectEq(cfg.maxVideoDecoders,
                    avc.getCapabilitiesForType("video/avc").getMaxSupportedInstances());
            List<MediaCodec> codecs = new ArrayList<MediaCodec>();
            try {
                MediaFormat f = MediaFormat.createVideoFormat("video/avc", 320, 240);
                for (int i = 0; i < cfg.maxVideoDecoders; i++) {
                    MediaCodec c = MediaCodec.createDecoderByType("video/avc");
                    c.configure(f, null, null, 0);
                    c.start();
                    codecs.add(c);
                }
                expectEq(cfg.videoOutputs, codecs.get(0).getOutputBuffers().length);
                expectEq(cfg.videoInputs, codecs.get(0).getInputBuffers().length);
                Thread.sleep(700);
                for (int i = 1; i < codecs.size(); i++) {
                    codecs.get(i).dequeueInputBuffer(0);  // in use
                }
                MediaCodec more = MediaCodec.createDecoderByType("video/avc");
                more.configure(f, null, null, 0);
                more.start();
                codecs.add(more);
                try {
                    codecs.get(0).dequeueInputBuffer(0);
                    throw new AssertionError("the idle decoder was not reclaimed");
                } catch (MediaCodec.CodecException e) {
                    expectEq(MediaCodec.CodecException.ERROR_RECLAIMED, e.getErrorCode());
                }
                expect(more.dequeueInputBuffer(0) >= 0, "the new decoder");
            } finally {
                for (MediaCodec c : codecs) c.release();
            }
        });

        check("storage: near the quota the app's cache goes; StatFs(/data) tells the quota", () -> {
            if (!app) throw new Skip("no app");
            File cache = new File("/data/data/" + PKG + "/cache/memtest.bin");
            cache.getParentFile().mkdirs();
            try (FileOutputStream out = new FileOutputStream(cache)) {
                out.write(new byte[1 << 20]);
            }
            expect(cache.length() == 1 << 20, "cache file");
            long quota = 1000 * MB, usage = 900 * MB;
            long before = MemoryMonitor.get().storageFreed();
            MemoryBudget.setStorageForTest(usage, quota);
            try {
                StatFs st = new StatFs("/data");
                expectEq(quota, st.getTotalBytes());
                expectEq(quota - usage, st.getAvailableBytes());
                MemoryMonitor.get().poke();
                long deadline = System.currentTimeMillis() + 10000;
                while (cache.exists() && System.currentTimeMillis() < deadline) {
                    Thread.sleep(50);
                }
                expect(!cache.exists(), "the cache file is still there");
                expect(MemoryMonitor.get().storageFreed() >= before + (1 << 20), "freed bytes");
            } finally {
                MemoryBudget.setStorageForTest(0, 0);
            }
        });

        check("the page hidden: the activity stops and its state is written; shown: it resumes",
                () -> {
            if (!app) throw new Skip("no app activity");
            int resumes = countEvents("MainActivity.onResume");
            MemoryMonitor.get().setFollowPageVisibility(true);
            try {
                MemoryBudget.setPageHiddenForTest(true);
                MemoryMonitor.get().poke();
                ActivityTest8.awaitEvents("MainActivity.onSaveInstanceState clicks=3",
                        "MainActivity.onStop", "App8.onTrimMemory "
                        + ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN);
                long deadline = System.currentTimeMillis() + 10000;
                while (!TASK_STATE.isFile() && System.currentTimeMillis() < deadline) {
                    Thread.sleep(20);
                }
                expect(TASK_STATE.isFile(), "no " + TASK_STATE);
                MemoryBudget.setPageHiddenForTest(false);
                MemoryMonitor.get().poke();
                awaitCount("MainActivity.onResume", resumes + 1);
                expect(!TASK_STATE.exists(), "the saved state outlived the page coming back");
            } finally {
                MemoryBudget.setPageHiddenForTest(false);
                MemoryMonitor.get().setFollowPageVisibility(false);
            }
        });

        if (!app) {
            check("restart with the task restored", () -> {
                throw new Skip("no app activity");
            });
            finish();
            return;
        }
        // lowmemorykiller's step: the counts so far go to /data for the run
        // after the restart, then the budget is made smaller than the use.
        MARK.getParentFile().mkdirs();
        Files.write(MARK.toPath(), (passed + " " + failed + " " + skipped + " "
                + System.currentTimeMillis()).getBytes(StandardCharsets.UTF_8));
        System.out.println("MemoryTest: over the budget now; the VM restarts (first run: "
                + passed + " passed, " + failed + " failed)");
        budgetFor(1.3);
        Thread.sleep(60000);
        MARK.delete();
        check("restart: over the budget, the VM restarts", () -> {
            throw new AssertionError("no restart within 60 s: " + MemoryMonitor.get().report());
        });
        finish();
    }

    // --- after the restart -------------------------------------------------------------------

    private static void restored() throws Exception {
        System.out.println("MemoryTest: second run, after the restart");
        check("restart: the task comes back with its saved state (3 clicks)", () -> {
            expect(TASK_STATE.isFile(), "no saved task in /data");
            SystemServer.start();
            expectEq(MAIN, SystemServer.activityManager().restoreSavedTask(PKG));
            expect(!TASK_STATE.exists(), "the saved task is kept after restoring");
            ActivityTest8.awaitEvents("MainActivity.onCreate restored clicks=3",
                    "MainActivity.onResume");
            expectEq(3, clicks());
        });
        check("restart: the budget is the device's again", () -> {
            expectEq((long) MemoryBudget.config().budgetMb * MB,
                    MemoryBudget.sample(false)[MemoryBudget.BUDGET]);
        });
        check("restart: over the budget again at once -- no restart loop, only trims", () -> {
            expect(MemoryMonitor.get().startedByRestart(), "this VM does not know the restart");
            budgetFor(1.3);
            long until = System.currentTimeMillis() + 15000;
            while (MemoryMonitor.get().overBudgetTicks() < 5 && System.currentTimeMillis() < until) {
                Thread.sleep(100);
            }
            expect(MemoryMonitor.get().overBudgetTicks() >= 5, "not over the budget: "
                    + MemoryMonitor.get().report());
            ActivityTest8.awaitEvents("App8.onTrimMemory "
                    + ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL);
            MemoryBudget.setBudgetForTest(0);
            MemoryMonitor.get().poke();
        });
        finish();
    }

    private static void finish() {
        System.out.println("MemoryTest: " + passed + " passed, " + failed + " failed"
                + (skipped > 0 ? ", " + skipped + " skipped" : "") + " ("
                + (System.nanoTime() - sStart) / 1000000 + " ms)");
        System.exit(failed);
    }
}
