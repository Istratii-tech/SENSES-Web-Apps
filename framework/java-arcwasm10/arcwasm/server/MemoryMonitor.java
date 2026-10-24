// arc-wasm: what Android's ActivityManagerService (updateOomAdj's trim
// levels), lowmemorykiller and the storage manager do about memory, for the
// one app of this VM against its budget (topic mem-budget,
// docs/topics/mem-budget.md; MemoryBudget for the numbers):
//
//   every second  sample the memory (MemoryBudget.sample), in use against the
//                 budget; near it, a precise sample (malloc's free chunks)
//   in use >= 70% / 80% / 90% of the budget
//                 first the VM's own garbage (a GC), then, as the level rises,
//                 ComponentCallbacks2.onTrimMemory(TRIM_MEMORY_RUNNING_MODERATE
//                 / _LOW / _CRITICAL) to the app (IApplicationThread.
//                 scheduleTrimMemory: ActivityThread also trims hwui's caches),
//                 onLowMemory() at critical, and this layer's caches
//                 (MemoryBudget.trim: idle decoders, free graphic buffers,
//                 the ARM translator)
//   in use above the budget (or the wasm memory at its ceiling) for 3 s
//                 after that: lowmemorykiller's step. The front task is
//                 stopped (onSaveInstanceState), its state written to /data,
//                 and the VM restarted (the page reloads; Node exits with
//                 75); the activity manager brings the task back with its
//                 saved state (ActivityManagerService.restoreSavedTask). Not
//                 again in the first 2 minutes of a VM that such a restart
//                 started: an app that does not fit the budget at all is not
//                 restarted in a loop (it keeps getting trims).
//   the page hidden
//                 as the Home key: the front task goes to the back (stopped,
//                 state saved and written, so a tab the browser discards comes
//                 back where it was), TRIM_MEMORY_UI_HIDDEN; shown again: the
//                 task comes to the front.
//   storage       above 80% of the browser's quota for /data (OPFS): the apps'
//                 cache directories are emptied (PackageManager.freeStorage).
//
// A report line ("MemBudget: ...") goes to the log every 30 s (--mem-report=S)
// and whenever the level changes.
package arcwasm.server;

import android.app.ActivityManager;
import android.content.ComponentCallbacks2;
import android.util.Slog;

public final class MemoryMonitor {
    static final String TAG = "MemBudget";
    static final int LEVEL_NONE = 0;
    static final long MB = 1024 * 1024;
    /** Exit status of a VM that asks to be started again (Node's restart). */
    public static final int RESTART_EXIT = 75;
    /** When the last restart for memory was asked for (ms since the epoch). */
    static final java.io.File LAST_RESTART = new java.io.File("/data/system/arcwasm/memory-restart");
    /** A VM started this soon after a restart for memory was started by it. */
    static final long RESTART_START_MS = 180000;
    /** ... and is not restarted again for memory this soon. */
    static final long RESTART_LOOP_MS = 120000;

    private static MemoryMonitor sInstance;

    private final ActivityManagerService mAm;
    private final MemoryBudget.Config mConfig;
    private final long mReportMs;
    private final Object mWake = new Object();
    private boolean mPoked;

    // Guarded by this.
    private long[] mSample;
    private long mInUse;
    // The wasm memory touched, the highest estimate so far (MemoryBudget.touched;
    // for the page's "low memory"), and the most the Java heap's spaces held.
    private long mTouched;
    private long mJavaHigh;
    private int mLevel;
    private int mSentLevel;
    private boolean mLowMemorySent;
    private int mOverTicks;
    private boolean mRestarting;
    private boolean mPageHidden;
    private long mLastPrecise;
    private long mLastReport;
    private long mLastStorageClean;
    private long mStorageFreed;
    private int mTrims;
    private boolean mFollowPage;
    private final long mStartedAt = android.os.SystemClock.uptimeMillis();
    private final boolean mStartedByRestart;

    private MemoryMonitor(ActivityManagerService am, long reportMs) {
        mAm = am;
        mConfig = MemoryBudget.config();
        mReportMs = reportMs;
        long last = 0;
        try {
            last = Long.parseLong(new String(java.nio.file.Files.readAllBytes(LAST_RESTART.toPath()),
                    java.nio.charset.StandardCharsets.UTF_8).trim());
        } catch (Exception e) {
            // None.
        }
        LAST_RESTART.delete();
        mStartedByRestart = last > 0 && System.currentTimeMillis() - last < RESTART_START_MS;
    }

    /** Starts the monitor (SystemServer.start), once. */
    static synchronized MemoryMonitor start(ActivityManagerService am, long reportMs) {
        if (sInstance == null) {
            sInstance = new MemoryMonitor(am, reportMs);
            Thread t = new Thread(sInstance::run, "MemoryMonitor");
            t.setDaemon(true);
            t.start();
        }
        return sInstance;
    }

    public static synchronized MemoryMonitor get() {
        return sInstance;
    }

    // --- what the activity manager and tests ask -----------------------------------------

    /** ActivityManager.MemoryInfo of the budget (ActivityManagerService.getMemoryInfo). */
    public void fillMemoryInfo(ActivityManager.MemoryInfo out) {
        long budget, inUse;
        synchronized (this) {
            if (mSample == null) {
                sampleLocked(false);
            }
            budget = mSample[MemoryBudget.BUDGET];
            inUse = mInUse;
        }
        out.totalMem = budget;
        out.availMem = Math.max(0, budget - inUse);
        // ActivityManager's thresholds (ProcessList.getMemLevel): low memory
        // where the RUNNING_LOW level starts.
        out.threshold = budget - budget * mConfig.lowPermille / 1000;
        out.lowMemory = out.availMem < out.threshold;
        out.hiddenAppThreshold = out.threshold;
        out.secondaryServerThreshold = out.threshold * 3 / 4;
        out.visibleAppThreshold = out.threshold / 2;
        out.foregroundAppThreshold = out.threshold / 4;
    }

    /**
     * Whether the page's visibility moves the front task (the app page:
     * SystemServer.main; not for tests that start the server themselves).
     */
    public synchronized void setFollowPageVisibility(boolean follow) {
        mFollowPage = follow;
    }

    /** The trim level last given to the app (RunningAppProcessInfo.lastTrimLevel). */
    public synchronized int lastTrimLevel() {
        return mSentLevel;
    }

    public synchronized long inUse() {
        return mInUse;
    }

    public synchronized long[] lastSample() {
        return mSample == null ? null : mSample.clone();
    }

    /** Whether a restart for memory started this VM (just before). */
    public boolean startedByRestart() {
        return mStartedByRestart;
    }

    /** Samples in a row above the budget (lowmemorykiller acts at 3). */
    public synchronized int overBudgetTicks() {
        return mOverTicks;
    }

    /** Bytes the storage check has freed so far. */
    public synchronized long storageFreed() {
        return mStorageFreed;
    }

    /** Samples now instead of at the next tick, and waits for it (tests). */
    public void poke() {
        synchronized (mWake) {
            mPoked = true;
            mWake.notifyAll();
        }
        // The tick runs on the monitor's thread; give it the time to.
        long until = System.currentTimeMillis() + 2000;
        synchronized (mWake) {
            while (mPoked && System.currentTimeMillis() < until) {
                try {
                    mWake.wait(50);
                } catch (InterruptedException e) {
                    return;
                }
            }
        }
    }

    // --- the loop --------------------------------------------------------------------------

    private void run() {
        Slog.i(TAG, "budget " + mConfig.budgetMb + " MB (wasm memory up to " + mConfig.capMb
                + " MB), Java heap " + mConfig.heapGrowthMb + "/" + mConfig.heapSizeMb + " MB"
                + (mConfig.lowRam ? ", low-RAM device" : "") + ", video decoders "
                + mConfig.maxVideoDecoders);
        while (true) {
            try {
                tick();
            } catch (Throwable t) {
                Slog.e(TAG, "monitor", t);
            }
            synchronized (mWake) {
                if (mPoked) {
                    mPoked = false;
                    mWake.notifyAll();
                }
                try {
                    mWake.wait(1000);
                } catch (InterruptedException e) {
                    return;
                }
            }
        }
    }

    private int permille(long inUse, long budget) {
        return budget <= 0 ? 0 : (int) Math.min(10000, inUse * 1000 / budget);
    }

    private void sampleLocked(boolean precise) {
        mSample = MemoryBudget.sample(precise);
        mInUse = MemoryBudget.inUse(mSample);
        mJavaHigh = Math.max(mJavaHigh, MemoryBudget.javaInSpaces(mSample));
        mTouched = Math.max(mTouched, MemoryBudget.touched(mSample, mJavaHigh));
        if (precise) {
            mLastPrecise = System.currentTimeMillis();
        }
    }

    /** The trim level of |pm| in use (per mille of the budget), with hysteresis going down. */
    private int levelFor(int pm, int current) {
        int level = pm >= mConfig.criticalPermille ? ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL
                : pm >= mConfig.lowPermille ? ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW
                : pm >= mConfig.moderatePermille ? ComponentCallbacks2.TRIM_MEMORY_RUNNING_MODERATE
                : LEVEL_NONE;
        if (level < current && levelFor(pm + 50, LEVEL_NONE) >= current) {
            return current;  // not yet 5% below the level's threshold
        }
        return level;
    }

    private void tick() {
        final long now = System.currentTimeMillis();
        boolean hiddenChanged;
        boolean hidden;
        int raiseTo = LEVEL_NONE;
        boolean lowMemory = false;
        boolean kill = false;
        boolean storage = false;
        boolean relax = false;
        boolean user = false;
        synchronized (this) {
            if (mRestarting) {
                return;
            }
            // The shell's "Перезагрузить" (low memory): the same restart, the task kept.
            user = MemoryBudget.userRestartRequested();
            sampleLocked(false);
            long budget = mSample[MemoryBudget.BUDGET];
            int pm = permille(mInUse, budget);
            if (pm >= mConfig.moderatePermille && now - mLastPrecise >= 2000) {
                // Near the budget: how much of malloc's arena is free, too.
                sampleLocked(true);
                pm = permille(mInUse, budget);
            }
            int level = levelFor(pm, mLevel);
            if (level > mSentLevel) {
                // As before lowmemorykiller: the VM's own garbage first.
                Runtime.getRuntime().gc();
                sampleLocked(true);
                pm = permille(mInUse, budget);
                level = levelFor(pm, mLevel);
            }
            boolean changed = level != mLevel;
            mLevel = level;
            if (level == LEVEL_NONE) {
                relax = mSentLevel != LEVEL_NONE;
                mSentLevel = LEVEL_NONE;
                mLowMemorySent = false;
            } else if (level > mSentLevel) {
                raiseTo = level;
                mSentLevel = level;
                mTrims++;
                if (level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL && !mLowMemorySent) {
                    lowMemory = true;
                    mLowMemorySent = true;
                }
            }
            // lowmemorykiller: still above the budget after the trims, or the
            // wasm memory at its ceiling (malloc will fail).
            long cap = mSample[MemoryBudget.CAP];
            boolean atCeiling = cap < (4096L * MB) && mSample[MemoryBudget.BRK] > cap - cap / 20;
            if (pm >= 1000 || atCeiling) {
                mOverTicks++;
                kill = mOverTicks >= 3;
                if (kill && mStartedByRestart
                        && android.os.SystemClock.uptimeMillis() - mStartedAt < RESTART_LOOP_MS) {
                    if (mOverTicks == 3) {
                        Slog.w(TAG, "over the budget again right after a restart for memory: not"
                                + " restarting in a loop (the budget is too small for the app)");
                        System.out.println("MemBudget: over the budget again after a restart:"
                                + " not restarting again");
                    }
                    kill = false;
                }
            } else {
                mOverTicks = 0;
            }
            MemoryBudget.publish(mInUse, mTouched);
            hidden = mSample[MemoryBudget.PAGE_HIDDEN] != 0;
            hiddenChanged = mFollowPage && hidden != mPageHidden;
            if (mFollowPage) {
                mPageHidden = hidden;
            }
            long quota = mSample[MemoryBudget.STORAGE_QUOTA];
            long usage = mSample[MemoryBudget.STORAGE_USAGE];
            if (quota > 0 && usage > quota * 8 / 10 && now - mLastStorageClean >= 60000) {
                storage = true;
                mLastStorageClean = now;
            }
            if (changed || raiseTo != LEVEL_NONE || (mReportMs > 0 && now - mLastReport >= mReportMs)) {
                mLastReport = now;
                Slog.i(TAG, reportLocked());
                System.out.println("MemBudget: " + reportLocked());
            }
            if (user) {
                kill = true;
            }
            if (kill) {
                mRestarting = true;
            }
        }
        // Outside the lock: these call into the app and wait for it.
        if (hiddenChanged) {
            mAm.onPageVisibility(hidden);
            if (hidden) {
                mAm.trimApps(ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN);
            }
        }
        if (raiseTo != LEVEL_NONE) {
            long freed = MemoryBudget.trim(raiseTo);
            mAm.trimApps(raiseTo);
            if (lowMemory) {
                mAm.lowMemoryApps();
            }
            Slog.i(TAG, "trim level " + raiseTo + (lowMemory ? " + onLowMemory" : "")
                    + "; this layer freed " + freed / MB + " MB");
        }
        if (relax) {
            MemoryBudget.trim(0);  // the pressure is over (e.g. translation resumes)
        }
        if (storage) {
            freeStorage();
        }
        if (kill) {
            lowMemoryKill(user);
        }
    }

    /** PackageManager.freeStorage when OPFS nears its quota: the apps' caches. */
    private void freeStorage() {
        PackageManagerService pm = SystemServer.packageManager();
        long freed = pm != null ? pm.freeCaches(Long.MAX_VALUE) : 0;
        synchronized (this) {
            mStorageFreed += freed;
        }
        Slog.i(TAG, "storage above 80% of the quota: emptied the apps' caches, "
                + freed / 1024 + " KB");
        System.out.println("MemBudget: storage: freed " + freed / 1024 + " KB of caches");
    }

    /** lowmemorykiller's step, with the state kept: save the task, restart the VM. */
    private void lowMemoryKill(boolean user) {
        String report;
        synchronized (this) {
            report = reportLocked();
        }
        Slog.w(TAG, (user ? "the user asked for a restart" : "over the budget after trimming")
                + ": saving the task and restarting (" + report + ")");
        System.out.println("MemBudget: restart" + (user ? " (user)" : "") + ": " + report);
        boolean saved = mAm.saveTaskForRestart(5000);
        System.out.println("MemBudget: restart: task state " + (saved ? "saved" : "not saved"));
        if (Senses.enabled()) {
            Senses.signal("app restart " + Senses.app() + " memory");
        }
        // (not for the user's: the next VM may still restart itself for memory)
        if (!user) try {
            LAST_RESTART.getParentFile().mkdirs();
            java.nio.file.Files.write(LAST_RESTART.toPath(), Long.toString(
                    System.currentTimeMillis()).getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (java.io.IOException e) {
            Slog.w(TAG, "cannot write " + LAST_RESTART, e);
        }
        if (!MemoryBudget.requestRestart()) {
            // No page to reload (Node): whoever started the VM starts it again.
            System.exit(RESTART_EXIT);
        }
    }

    // --- the report ----------------------------------------------------------------------

    private static String mb(long bytes) {
        return Long.toString((bytes + MB / 2) / MB);
    }

    static String levelName(int level) {
        switch (level) {
            case LEVEL_NONE: return "normal";
            case ComponentCallbacks2.TRIM_MEMORY_RUNNING_MODERATE: return "moderate";
            case ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW: return "low";
            case ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL: return "critical";
            default: return Integer.toString(level);
        }
    }

    /** One line: in use of the budget and what holds it. */
    synchronized String reportLocked() {
        long[] s = mSample;
        StringBuilder b = new StringBuilder();
        long budget = s[MemoryBudget.BUDGET];
        b.append("in use ").append(mb(mInUse)).append('/').append(mb(budget)).append(" MB (")
                .append(permille(mInUse, budget) / 10).append("%, ").append(levelName(mLevel))
                .append(") | wasm ").append(mb(s[MemoryBudget.WASM])).append('/')
                .append(mb(s[MemoryBudget.CAP])).append(" MB (touched ").append(mb(mTouched))
                .append("), java ")
                .append(mb(s[MemoryBudget.JAVA_USED])).append('/')
                .append(mb(s[MemoryBudget.JAVA_TOTAL])).append(" (max ")
                .append(mConfig.heapGrowthMb).append("), malloc ");
        if (s[MemoryBudget.MALLOC_USED] >= 0) {
            b.append(mb(s[MemoryBudget.MALLOC_USED])).append(" (+").append(mb(s[MemoryBudget.MALLOC_FREE]))
                    .append(" free)");
        } else {
            b.append(mb(s[MemoryBudget.MALLOC_ARENA])).append(" arena");
        }
        long otherMaps = s[MemoryBudget.MAPPED] - s[MemoryBudget.DEX] - s[MemoryBudget.JAVA_SPACES]
                - s[MemoryBudget.JAVA_LOS] - s[MemoryBudget.ART_OTHER] - s[MemoryBudget.IMAGE];
        b.append(", dex ").append(mb(s[MemoryBudget.DEX]))
                .append(", art ").append(mb(s[MemoryBudget.ART_OTHER]))
                .append(", other maps ").append(mb(Math.max(0, otherMaps)))
                .append(", image ").append(mb(s[MemoryBudget.IMAGE_FILLED])).append('/')
                .append(mb(s[MemoryBudget.IMAGE]))
                .append(", arm ").append(mb(s[MemoryBudget.ARM_LIBS]))
                .append(" (+").append(mb(s[MemoryBudget.ARM_BLOCKS] + s[MemoryBudget.ARM_JIT]))
                .append(" translator+jit), threads ").append(s[MemoryBudget.THREADS])
                .append(", gralloc ").append(mb(s[MemoryBudget.GRALLOC])).append(" (")
                .append(s[MemoryBudget.GRALLOC_COUNT]).append("), codecs ")
                .append(mb(s[MemoryBudget.CODEC])).append(" (").append(s[MemoryBudget.CODEC_COUNT])
                .append("), frames ").append(s[MemoryBudget.JS_FRAMES]).append(" (")
                .append(mb(s[MemoryBudget.JS_FRAME_BYTES])).append(" MB)");
        if (s[MemoryBudget.STORAGE_QUOTA] > 0) {
            b.append(", opfs ").append(mb(s[MemoryBudget.STORAGE_USAGE])).append('/')
                    .append(mb(s[MemoryBudget.STORAGE_QUOTA])).append(" MB");
        }
        if (mTrims > 0) {
            b.append(", trims ").append(mTrims);
        }
        return b.toString();
    }

    /** Every value of the last sample, MB (SystemServer --dump). */
    public synchronized String rawReport() {
        if (mSample == null) {
            sampleLocked(false);
        }
        String[] names = {"wasm", "cap", "mallocArena", "mallocUsed", "mallocFree", "mapped",
                "largeFree", "dex", "javaSpaces", "javaLos", "artOther", "armLibs", "armBlocks",
                "armJit", "threads", "stacks", "gralloc", "grallocCount", "codec", "codecCount",
                "jsFrames", "jsFrameBytes", "storageUsage", "storageQuota", "budget", "pageHidden",
                "brk", "javaUsed", "javaTotal"};
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < names.length && i < mSample.length; i++) {
            boolean count = names[i].equals("threads") || names[i].endsWith("Count")
                    || names[i].equals("jsFrames") || names[i].equals("pageHidden");
            b.append(i == 0 ? "" : " ").append(names[i]).append('=')
                    .append(count || mSample[i] < 0 ? Long.toString(mSample[i]) : mb(mSample[i]));
        }
        return b.toString();
    }

    /** The report now (SystemServer --dump). */
    public synchronized String report() {
        if (mSample == null) {
            sampleLocked(false);
        }
        return reportLocked();
    }
}
