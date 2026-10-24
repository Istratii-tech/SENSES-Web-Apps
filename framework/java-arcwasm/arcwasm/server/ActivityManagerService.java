// arc-wasm: the "activity" service of the in-process system server, a lean
// IActivityManager (docs/topics/activity.md). It does what
// ActivityManagerService + ActivityStackSupervisor + ActivityStack of AOSP
// 4.4.4 do to start, pause, stop, resume and destroy activities, in their
// order and with the same IApplicationThread calls, for one app "process":
//
//   startActivity  resolve (package service) -> ActivityRecord in a task ->
//                  pause the resumed activity first -> start the process if
//                  needed -> scheduleLaunchActivity
//   process start  no zygote, no fork: a new Java thread runs
//                  ActivityThread.main(), whose attach() calls
//                  attachApplication -> bindApplication + pending launches
//   activityPaused -> launch/resume the next top activity; the paused one is
//                  stopped (or destroyed, if finishing) once the next is idle
//   finishActivity, moveTaskToBack -> pause -> stop -> destroy
//
// The app is reached the way system_server reaches an app process: through
// ApplicationThreadProxy, so every call is a transaction (Parcel and
// ApplicationThreadNative.onTransact), and callers reach this service through
// ActivityManagerProxy (arcwasm.server.base.ActivityManagerBase). The window
// manager is told about activity tokens through IWindowManager (addAppToken,
// setAppVisibility, setFocusedApp, app transitions, removeAppToken), as the
// real service does.
//
// Not done: service restarts and foreground state (services run: topic
// userspace), ordered and manifest broadcasts (registered receivers
// get unordered delivery and stickies: topic userspace), starting another
// app's process for its content provider (published providers are handed
// out: topic userspace), results to the calling activity, launch modes and
// task affinities beyond "same task unless FLAG_ACTIVITY_NEW_TASK", timeouts
// and ANRs, more than one app process per VM (other packages are hosted in
// the one process: topic userspace), permissions
// (the whole VM runs as one uid, so checkPermission grants what the calling
// uid has: everything).
package arcwasm.server;

import android.app.ActivityManager;
import android.app.ApplicationThreadNative;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.content.ComponentName;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.res.CompatibilityInfo;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.hardware.display.IDisplayManager;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.util.Slog;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.IApplicationToken;
import android.view.IWindowManager;
import android.view.WindowManager;

import arcwasm.server.base.ActivityManagerBase;

import java.lang.reflect.Constructor;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public final class ActivityManagerService extends ActivityManagerBase {
    static final String TAG = "ActivityManager";
    /** ActivityManagerService.STOCK_PM_FLAGS. */
    static final int STOCK_PM_FLAGS = PackageManager.GET_SHARED_LIBRARY_FILES;

    /** ActivityStack.ActivityState. */
    public enum State {
        INITIALIZING, RESUMED, PAUSING, PAUSED, STOPPING, STOPPED, FINISHING, DESTROYING, DESTROYED
    }

    /** The binder that identifies an activity to the app and the window manager. */
    static final class Token extends IApplicationToken.Stub {
        final ActivityRecord record;

        Token(ActivityRecord record) {
            this.record = record;
        }

        // From the window manager.
        @Override public void windowsDrawn() {}
        @Override public void windowsVisible() {}
        @Override public void windowsGone() {}
        @Override public boolean keyDispatchingTimedOut(String reason) { return false; }
        @Override public long getKeyDispatchingTimeout() { return 5000; }

        @Override
        public String toString() {
            return "Token{" + Integer.toHexString(System.identityHashCode(this)) + " " + record + "}";
        }
    }

    static final class ProcessRecord {
        final String processName;
        final ApplicationInfo info;
        Thread mainThread;
        IApplicationThread thread;
        boolean dead;
        final List<ActivityRecord> activities = new ArrayList<ActivityRecord>();
        /** Packages whose providers are installed here (topic userspace). */
        final java.util.Set<String> packages = new java.util.LinkedHashSet<String>();

        ProcessRecord(String processName, ApplicationInfo info) {
            this.processName = processName;
            this.info = info;
        }

        @Override
        public String toString() {
            return "ProcessRecord{" + processName + (thread == null ? " starting" : "") + "}";
        }
    }

    static final class TaskRecord {
        final int taskId;
        final Intent intent;
        final List<ActivityRecord> activities = new ArrayList<ActivityRecord>();
        /** Moved to the back: no home screen comes up instead, it is just not shown. */
        boolean inBackground;

        TaskRecord(int taskId, Intent intent) {
            this.taskId = taskId;
            this.intent = intent;
        }

        ActivityRecord topRunning() {
            for (int i = activities.size() - 1; i >= 0; i--) {
                ActivityRecord r = activities.get(i);
                if (!r.finishing) {
                    return r;
                }
            }
            return null;
        }
    }

    public static final class ActivityRecord {
        final Token appToken = new Token(this);
        final ActivityInfo info;
        final Intent intent;
        final ComponentName realActivity;
        final String launchedFromPackage;
        TaskRecord task;
        ProcessRecord app;
        State state = State.INITIALIZING;
        boolean finishing;
        boolean visible = true;
        boolean idle;
        Bundle icicle;

        ActivityRecord(ActivityInfo info, Intent intent, String launchedFromPackage) {
            this.info = info;
            this.intent = intent;
            this.realActivity = new ComponentName(info.packageName, info.name);
            this.launchedFromPackage = launchedFromPackage;
        }

        @Override
        public String toString() {
            return "ActivityRecord{" + realActivity.flattenToShortString() + " " + state
                    + (finishing ? " finishing" : "") + "}";
        }
    }

    private final IPackageManager mPackageManager;
    private final Configuration mConfiguration = new Configuration();
    private final Map<String, ProcessRecord> mProcesses = new HashMap<String, ProcessRecord>();
    /** Front task last (ActivityStack.mTaskHistory). */
    private final List<TaskRecord> mTasks = new ArrayList<TaskRecord>();
    /** Paused activities to stop (or destroy, when finishing) once the next is idle. */
    private final List<ActivityRecord> mStopping = new ArrayList<ActivityRecord>();
    private final SettingsProvider mSettings = new SettingsProvider();
    private final UserDictionaryProvider mUserDictionary = new UserDictionaryProvider();
    private ActivityRecord mResumedActivity;
    private ActivityRecord mPausingActivity;
    private int mNextTaskId = 1;
    private boolean mAppProcessStarted;
    /**
     * The VM's one app process, and the other process names it hosts (topic
     * userspace): every package runs here, as packages sharing a process do.
     */
    private ProcessRecord mVmProcess;
    private final Map<String, ProcessRecord> mHosted = new HashMap<String, ProcessRecord>();
    /** Lifecycle requests sent to the app, "STATE activity", for tests and dumps. */
    private final List<String> mHistory = new ArrayList<String>();

    ActivityManagerService(IPackageManager pm) {
        mPackageManager = pm;
        computeConfiguration();
    }

    /**
     * The global configuration (WindowManagerService.computeNewConfiguration
     * on a device): the default display from the "display" service, the
     * default locale, a touch screen and the browser's keyboard.
     */
    private void computeConfiguration() {
        mConfiguration.setToDefaults();
        mConfiguration.locale = Locale.getDefault();
        mConfiguration.setLayoutDirection(mConfiguration.locale);
        DisplayInfo di = null;
        try {
            IDisplayManager dm = IDisplayManager.Stub.asInterface(ServiceManager.getService("display"));
            di = dm == null ? null : dm.getDisplayInfo(Display.DEFAULT_DISPLAY);
        } catch (RemoteException e) {
            // In process: not thrown.
        }
        if (di != null) {
            float density = di.logicalDensityDpi / 160f;
            mConfiguration.densityDpi = di.logicalDensityDpi;
            mConfiguration.screenWidthDp = (int) (di.appWidth / density);
            mConfiguration.screenHeightDp = (int) (di.appHeight / density);
            mConfiguration.smallestScreenWidthDp = Math.min(mConfiguration.screenWidthDp,
                    mConfiguration.screenHeightDp);
            mConfiguration.orientation = di.appWidth > di.appHeight
                    ? Configuration.ORIENTATION_LANDSCAPE : Configuration.ORIENTATION_PORTRAIT;
            int longDp = Math.max(mConfiguration.screenWidthDp, mConfiguration.screenHeightDp);
            int shortDp = mConfiguration.smallestScreenWidthDp;
            int size = longDp >= 960 && shortDp >= 720 ? Configuration.SCREENLAYOUT_SIZE_XLARGE
                    : longDp >= 640 && shortDp >= 480 ? Configuration.SCREENLAYOUT_SIZE_LARGE
                    : longDp >= 470 && shortDp >= 320 ? Configuration.SCREENLAYOUT_SIZE_NORMAL
                    : Configuration.SCREENLAYOUT_SIZE_SMALL;
            mConfiguration.screenLayout = size | Configuration.SCREENLAYOUT_LONG_NO
                    | (mConfiguration.screenLayout & Configuration.SCREENLAYOUT_LAYOUTDIR_MASK);
        }
        mConfiguration.touchscreen = Configuration.TOUCHSCREEN_FINGER;
        mConfiguration.keyboard = Configuration.KEYBOARD_QWERTY;
        mConfiguration.keyboardHidden = Configuration.KEYBOARDHIDDEN_NO;
        mConfiguration.hardKeyboardHidden = Configuration.HARDKEYBOARDHIDDEN_NO;
        mConfiguration.navigation = Configuration.NAVIGATION_NONAV;
        mConfiguration.navigationHidden = Configuration.NAVIGATIONHIDDEN_YES;
        mConfiguration.uiMode = Configuration.UI_MODE_TYPE_NORMAL | Configuration.UI_MODE_NIGHT_NO;
        mConfiguration.seq = 1;
    }

    private static IWindowManager windowManager() {
        return IWindowManager.Stub.asInterface(ServiceManager.getService("window"));
    }

    /** The app is called as from another process: through ApplicationThreadProxy. */
    private static IApplicationThread proxyFor(IApplicationThread thread) {
        try {
            Class<?> c = Class.forName("android.app.ApplicationThreadProxy");
            Constructor<?> ctor = c.getDeclaredConstructor(IBinder.class);
            ctor.setAccessible(true);
            return (IApplicationThread) ctor.newInstance(new ReplyingBinder(thread.asBinder()));
        } catch (Exception e) {
            throw new IllegalStateException("ApplicationThreadProxy", e);
        }
    }

    /**
     * The app's binder as the driver delivers to it (topic userspace): the
     * receiving side of a one-way call still gets a reply Parcel. In process,
     * Binder.transact hands onTransact the caller's null, and
     * ApplicationThreadNative writes "no exception" into it anyway
     * (scheduleInstallProvider, which ApplicationThreadProxy sends one-way).
     */
    private static final class ReplyingBinder implements IBinder {
        private final IBinder mTarget;

        ReplyingBinder(IBinder target) {
            mTarget = target;
        }

        @Override
        public boolean transact(int code, android.os.Parcel data, android.os.Parcel reply,
                int flags) throws RemoteException {
            android.os.Parcel r = reply != null ? reply : android.os.Parcel.obtain();
            try {
                return mTarget.transact(code, data, r, flags);
            } finally {
                if (reply == null) {
                    r.recycle();
                }
            }
        }

        @Override public String getInterfaceDescriptor() throws RemoteException {
            return mTarget.getInterfaceDescriptor();
        }
        @Override public boolean pingBinder() { return mTarget.pingBinder(); }
        @Override public boolean isBinderAlive() { return mTarget.isBinderAlive(); }
        @Override public android.os.IInterface queryLocalInterface(String descriptor) { return null; }
        @Override public void dump(java.io.FileDescriptor fd, String[] args) throws RemoteException {
            mTarget.dump(fd, args);
        }
        @Override public void dumpAsync(java.io.FileDescriptor fd, String[] args)
                throws RemoteException {
            mTarget.dumpAsync(fd, args);
        }
        @Override public void linkToDeath(DeathRecipient recipient, int flags)
                throws RemoteException {
            mTarget.linkToDeath(recipient, flags);
        }
        @Override public boolean unlinkToDeath(DeathRecipient recipient, int flags) {
            return mTarget.unlinkToDeath(recipient, flags);
        }
    }

    private void history(String what, ActivityRecord r) {
        mHistory.add(what + " " + r.realActivity.getShortClassName());
        Slog.i(TAG, what + " " + r);
    }

    /** What this service asked of the app so far ("launch .MainActivity", ...). */
    public synchronized List<String> getLifecycleHistory() {
        return new ArrayList<String>(mHistory);
    }

    /** The state of the activities of every task, front task last. */
    public synchronized List<String> dumpActivities() {
        List<String> out = new ArrayList<String>();
        for (TaskRecord t : mTasks) {
            for (ActivityRecord r : t.activities) {
                out.add("task " + t.taskId + (t.inBackground ? " (back)" : "") + ": "
                        + r.realActivity.getShortClassName() + " " + r.state
                        + (r.finishing ? " finishing" : ""));
            }
        }
        return out;
    }

    private ActivityRecord forToken(IBinder token) {
        if (token instanceof Token) {
            ActivityRecord r = ((Token) token).record;
            if (r.state != State.DESTROYED) {
                return r;
            }
        }
        return null;
    }

    private ActivityRecord topRunningActivity() {
        for (int i = mTasks.size() - 1; i >= 0; i--) {
            TaskRecord t = mTasks.get(i);
            ActivityRecord r = t.inBackground ? null : t.topRunning();
            if (r != null) {
                return r;
            }
        }
        return null;
    }

    // --- users (topic userspace): one, the owner ----------------------------------

    @Override
    public android.content.pm.UserInfo getCurrentUser() {
        return new android.content.pm.UserInfo(0, "Owner", null,
                android.content.pm.UserInfo.FLAG_ADMIN | android.content.pm.UserInfo.FLAG_PRIMARY
                        | android.content.pm.UserInfo.FLAG_INITIALIZED);
    }

    @Override
    public boolean isUserRunning(int userId, boolean orStopped) {
        return userId == 0;
    }

    @Override
    public int[] getRunningUserIds() {
        return new int[] {0};
    }

    // --- the home screen and recents (topic userspace) ----------------------------

    private ComponentName mHomeComponent;

    /** The home activity SystemServer started: what HOME brings back. */
    public synchronized void setHomeComponent(ComponentName component) {
        mHomeComponent = component;
        PackageManagerService pm = SystemServer.packageManager();
        if (pm != null) {
            pm.setPreferredHome(component);
        }
    }

    /**
     * PhoneWindowManager.launchHomeFromHotKey: the home task to the front (it is
     * found by its root, singleTask, and gets the HOME intent as onNewIntent).
     * Without a known home screen, a MAIN/HOME activity other than Settings'
     * CryptKeeper (a home screen only while the device is being decrypted; it
     * disables itself otherwise, which is not done here).
     */
    public void startHome() {
        if (Senses.enabled()) {
            sensesHome();
            return;
        }
        Intent intent = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_HOME)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
        ComponentName home;
        synchronized (this) {
            home = mHomeComponent;
        }
        if (home == null) {
            try {
                for (android.content.pm.ResolveInfo ri : mPackageManager.queryIntentActivities(
                        intent, null, 0, 0)) {
                    if (!"com.android.settings".equals(ri.activityInfo.packageName)) {
                        home = new ComponentName(ri.activityInfo.packageName, ri.activityInfo.name);
                        break;
                    }
                }
            } catch (RemoteException e) {
                // In process: not thrown.
            }
        }
        if (home == null) {
            Slog.w(TAG, "HOME: no home screen");
            return;
        }
        intent.setComponent(home);
        startActivityAsUser(null, "android", intent, null, null, null, 0, 0, null, null, null, 0);
    }

    /**
     * The recent tasks, most recent first (topic userspace): SystemUI's recents
     * panel. Only tasks that still have activities are remembered (no task
     * history), and there are no thumbnails.
     */
    @Override
    public List<ActivityManager.RecentTaskInfo> getRecentTasks(int maxNum, int flags, int userId) {
        List<ActivityManager.RecentTaskInfo> list = new ArrayList<ActivityManager.RecentTaskInfo>();
        synchronized (this) {
            for (int i = mTasks.size() - 1; i >= 0 && list.size() < maxNum; i--) {
                TaskRecord t = mTasks.get(i);
                if (t.activities.isEmpty()) {
                    continue;
                }
                ActivityRecord root = t.activities.get(0);
                ActivityManager.RecentTaskInfo ri = new ActivityManager.RecentTaskInfo();
                ri.id = t.taskId;
                ri.persistentId = t.taskId;
                ri.baseIntent = new Intent(t.intent);
                ri.baseIntent.setComponent(root.realActivity);
                ri.origActivity = root.realActivity;
                ri.stackId = 0;
                list.add(ri);
            }
        }
        return list;
    }

    @Override
    public android.graphics.Bitmap getTaskTopThumbnail(int taskId) {
        return null;
    }

    @Override
    public ActivityManager.TaskThumbnails getTaskThumbnails(int taskId) {
        return new ActivityManager.TaskThumbnails();
    }

    /** ACTION_CLOSE_SYSTEM_DIALOGS to the registered receivers (shade, recents, dialogs). */
    @Override
    public void closeSystemDialogs(String reason) {
        Intent intent = new Intent(Intent.ACTION_CLOSE_SYSTEM_DIALOGS);
        intent.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY);
        if (reason != null) {
            intent.putExtra("reason", reason);
        }
        broadcastIntent(null, intent, null, null, 0, null, null, null, 0, false, false, 0);
    }

    /** Recents' swipe-away: the task's activities finish. */
    @Override
    public boolean removeTask(int taskId, int flags) {
        synchronized (this) {
            for (TaskRecord t : mTasks) {
                if (t.taskId != taskId) {
                    continue;
                }
                for (ActivityRecord r : new ArrayList<ActivityRecord>(t.activities)) {
                    if (r.finishing) {
                        continue;
                    }
                    r.finishing = true;
                    if (r == mResumedActivity) {
                        startPausingLocked(r, false);  // the next one resumes when paused
                    } else if (r.app != null && r.app.thread != null) {
                        destroyActivityLocked(r);
                    } else {
                        removeActivityLocked(r);
                    }
                }
                return true;
            }
        }
        return false;
    }

    // --- SENSES Web Apps' one-app mode (topic install8, arcwasm.server.Senses) ----------

    /**
     * HOME in one-app mode: no home screen; the page goes back to its
     * library. The front task goes to the back (its activities stop and
     * save their state), then "SENSES: app exit PKG home".
     */
    private void sensesHome() {
        String pkg = Senses.app();
        synchronized (this) {
            for (int i = mTasks.size() - 1; i >= 0; i--) {
                TaskRecord t = mTasks.get(i);
                if (!t.inBackground && t.topRunning() != null) {
                    pkg = t.activities.get(0).info.packageName;
                    moveTaskToBackLocked(t);
                    break;
                }
            }
        }
        Senses.exit(pkg, "home");
    }

    private boolean mSensesExitPending;

    /**
     * One-app mode: no activity left in any task (Back on the last one, or
     * the app finished itself) -> "SENSES: app exit PKG back". After a short
     * wait, as an app may finish one activity before starting the next.
     */
    private void maybeSensesExitLocked() {
        if (!Senses.enabled() || mSensesExitPending) {
            return;
        }
        for (TaskRecord t : mTasks) {
            if (t.topRunning() != null) {
                return;  // e.g. sent to the back by HOME: not an exit
            }
        }
        if (mProcesses.isEmpty()) {
            return;  // nothing ran yet
        }
        mSensesExitPending = true;
        new Thread("SensesExit") {
            @Override
            public void run() {
                android.os.SystemClock.sleep(700);
                synchronized (ActivityManagerService.this) {
                    mSensesExitPending = false;
                    for (TaskRecord t : mTasks) {
                        if (t.topRunning() != null) {
                            return;
                        }
                    }
                }
                Senses.exit(Senses.app(), "back");
            }
        }.start();
    }

    /** APP_SWITCH: SystemUI's recent apps when its status bar runs, else home. */
    public void showRecentApps() {
        if (Senses.enabled()) {
            Slog.i(TAG, "APP_SWITCH: no recent apps (one-app mode)");
            return;
        }
        StatusBarManagerService bar = SystemServer.statusBar();
        if (bar != null && bar.hasBar()) {
            bar.toggleRecentApps();
        } else {
            startHome();
        }
    }

    // --- starting activities ---------------------------------------------------

    @Override
    public int startActivity(IApplicationThread caller, String callingPackage, Intent intent,
            String resolvedType, IBinder resultTo, String resultWho, int requestCode,
            int startFlags, String profileFile, ParcelFileDescriptor profileFd, Bundle options) {
        return startActivityAsUser(caller, callingPackage, intent, resolvedType, resultTo,
                resultWho, requestCode, startFlags, profileFile, profileFd, options, 0);
    }

    @Override
    public int startActivityAsUser(IApplicationThread caller, String callingPackage,
            Intent intent, String resolvedType, IBinder resultTo, String resultWho,
            int requestCode, int startFlags, String profileFile, ParcelFileDescriptor profileFd,
            Bundle options, int userId) {
        // ActivityStackSupervisor.resolveActivity.
        intent = new Intent(intent);
        ActivityInfo aInfo;
        try {
            if (intent.getComponent() != null) {
                aInfo = mPackageManager.getActivityInfo(intent.getComponent(), STOCK_PM_FLAGS, userId);
            } else {
                android.content.pm.ResolveInfo ri = mPackageManager.resolveIntent(intent,
                        resolvedType, PackageManager.MATCH_DEFAULT_ONLY | STOCK_PM_FLAGS, userId);
                aInfo = ri == null ? null : ri.activityInfo;
            }
        } catch (RemoteException e) {
            aInfo = null;
        }
        // Play Store links (topic gms): the store's web page in the browser; an
        // activity of the user's Google packages is left at once (no results to
        // activities here).
        if (Gms.isGoogleActivity(intent, aInfo == null ? null : aInfo.packageName)) {
            Intent page = Gms.webPage(intent);
            if (page != null && Senses.openUrl(page)) {
                return ActivityManager.START_SUCCESS;
            }
            if (aInfo != null) {  // the user's Google package: no process of its own
                Slog.i(TAG, "Google's activity " + intent + ": left at once");
                return ActivityManager.START_SUCCESS;
            }
        }
        if (aInfo == null) {
            Slog.w(TAG, "Unable to resolve " + intent);
            if (Unimplemented.lenient("ActivityManager.startActivity(" + intent.getAction() + ")")) {
                // No app here handles it: the caller goes on (results to
                // activities are not kept here), instead of dying of
                // ActivityNotFoundException with every app in this process.
                return ActivityManager.START_SUCCESS;
            }
            if (intent.getComponent() == null && Senses.openUrl(intent)) {
                return ActivityManager.START_SUCCESS;  // the page's browser shows it
            }
            return intent.getComponent() != null ? ActivityManager.START_CLASS_NOT_FOUND
                    : ActivityManager.START_INTENT_NOT_RESOLVED;
        }
        intent.setComponent(new ComponentName(aInfo.applicationInfo.packageName, aInfo.name));
        synchronized (this) {
            ActivityRecord source = forToken(resultTo);
            ComponentName component = intent.getComponent();
            boolean newTask = source == null
                    || (intent.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0;
            if (newTask) {
                // An existing task for this activity comes to the front as it is
                // (what a launcher's FLAG_ACTIVITY_NEW_TASK | RESET_TASK_IF_NEEDED
                // does when the task's root matches).
                for (TaskRecord t : mTasks) {
                    if (!t.activities.isEmpty()
                            && t.activities.get(0).realActivity.equals(component)
                            && t.topRunning() != null) {
                        Slog.i(TAG, "bringing task " + t.taskId + " to the front for " + intent);
                        ActivityRecord root = t.activities.get(0);
                        int mode = root.info.launchMode;
                        if ((mode == ActivityInfo.LAUNCH_SINGLE_TASK
                                || mode == ActivityInfo.LAUNCH_SINGLE_INSTANCE)
                                && root.app != null && root.app.thread != null) {
                            // singleTask: the root is the target, cleared to the top,
                            // and gets the intent (Launcher2: HOME shows the default page).
                            for (int i = t.activities.size() - 1; i > 0; i--) {
                                ActivityRecord above = t.activities.get(i);
                                if (!above.finishing && above.app != null
                                        && above.app.thread != null) {
                                    above.finishing = true;
                                    destroyActivityLocked(above);  // removed when destroyed
                                }
                            }
                            try {
                                List<Intent> intents = new ArrayList<Intent>();
                                intents.add(new Intent(intent));
                                root.app.thread.scheduleNewIntent(intents, root.appToken);
                            } catch (RemoteException e) {
                                Slog.w(TAG, "new intent " + root, e);
                            }
                        }
                        moveTaskToFrontLocked(t);
                        return ActivityManager.START_TASK_TO_FRONT;
                    }
                }
            }
            ActivityRecord r = new ActivityRecord(aInfo, intent, callingPackage);
            TaskRecord task;
            if (newTask) {
                task = new TaskRecord(mNextTaskId++, intent);
                mTasks.add(task);
            } else {
                task = source.task;
                mTasks.remove(task);
                mTasks.add(task);
            }
            task.inBackground = false;
            r.task = task;
            task.activities.add(r);
            Slog.i(TAG, "START " + intent + " in task " + task.taskId);
            IWindowManager wm = windowManager();
            try {
                wm.prepareAppTransition(newTask ? 8 /* TRANSIT_TASK_OPEN */
                        : 6 /* TRANSIT_ACTIVITY_OPEN */, false);
                wm.addAppToken(task.activities.indexOf(r), r.appToken, task.taskId,
                        0 /* the one stack */, aInfo.screenOrientation, true,
                        (aInfo.flags & ActivityInfo.FLAG_SHOW_ON_LOCK_SCREEN) != 0, userId,
                        aInfo.configChanges);
            } catch (RemoteException e) {
                // In process: not thrown.
            }
            resumeTopActivityLocked();
            return ActivityManager.START_SUCCESS;
        }
    }

    private void moveTaskToFrontLocked(TaskRecord t) {
        mTasks.remove(t);
        mTasks.add(t);
        t.inBackground = false;
        resumeTopActivityLocked();
    }

    /** ActivityStack.resumeTopActivityLocked. */
    private void resumeTopActivityLocked() {
        ActivityRecord next = topRunningActivity();
        if (next == null) {
            // Nothing to show (and no home screen): what waits to be
            // stopped or destroyed can go now.
            processStoppingLocked();
            maybeSensesExitLocked();
            return;
        }
        if (next == mResumedActivity || mPausingActivity != null) {
            return;
        }
        if (mResumedActivity != null) {
            startPausingLocked(mResumedActivity, false);
            return;
        }
        IWindowManager wm = windowManager();
        updateRotationLocked(next);
        try {
            wm.setAppVisibility(next.appToken, true);
            wm.setFocusedApp(next.appToken, true);
        } catch (RemoteException e) {
            // In process: not thrown.
        }
        next.visible = true;
        mStopping.remove(next);
        if (next.state != State.INITIALIZING && next.app != null && next.app.thread != null) {
            // Already launched (paused or stopped): resume it.
            history("resume", next);
            next.state = State.RESUMED;
            next.idle = false;
            mResumedActivity = next;
            try {
                next.app.thread.scheduleResumeActivity(next.appToken,
                        ActivityManager.PROCESS_STATE_TOP, true);
                wm.executeAppTransition();
            } catch (RemoteException e) {
                Slog.w(TAG, "resume " + next, e);
            }
            return;
        }
        startSpecificActivityLocked(next);
    }

    /** ActivityStackSupervisor.startSpecificActivityLocked. */
    private void startSpecificActivityLocked(ActivityRecord r) {
        String processName = r.info.processName;
        ProcessRecord app = mProcesses.get(processName);
        if (app == null) {
            app = mHosted.get(processName);
        }
        if (app == null && mVmProcess != null && !mVmProcess.dead) {
            app = mVmProcess;
            mHosted.put(processName, app);
            Slog.i(TAG, "hosting " + processName + " in process " + app.processName);
        }
        if (app != null && app.thread != null) {
            realStartActivityLocked(r, app);
            return;
        }
        if (app == null) {
            app = startProcessLocked(processName, r.info.applicationInfo);
        }
        if (app.dead) {
            // Not started (one app process per VM): drop the activity and bring
            // back what was on top before, e.g. the home screen (topic userspace).
            mProcesses.remove(processName);
            r.finishing = true;
            removeActivityLocked(r);
            resumeTopActivityLocked();
            return;
        }
        // The launch happens in attachApplication.
        r.app = app;
    }

    /**
     * The first activity or service of another package in the VM's process: install that
     * package's content providers first (ActivityThread.handleInstallProvider,
     * queued before the launch), as bindApplication does for the bound app.
     */
    private void installPackageLocked(ProcessRecord app, android.content.pm.ComponentInfo info) {
        if (!app.packages.add(info.packageName)) {
            return;
        }
        try {
            for (ProviderInfo pi : mPackageManager.queryContentProviders(info.processName,
                    info.applicationInfo.uid, PackageManager.GET_URI_PERMISSION_PATTERNS)) {
                if (info.packageName.equals(pi.packageName)) {
                    app.thread.scheduleInstallProvider(pi);
                }
            }
        } catch (RemoteException e) {
            Slog.w(TAG, "install providers of " + info.packageName, e);
        }
    }

    /** ActivityStackSupervisor.realStartActivityLocked (andResume = true). */
    private void realStartActivityLocked(ActivityRecord r, ProcessRecord app) {
        installPackageLocked(app, r.info);
        r.app = app;
        if (!app.activities.contains(r)) {
            app.activities.add(r);
        }
        history("launch", r);
        r.state = State.RESUMED;
        r.idle = false;
        mResumedActivity = r;
        try {
            app.thread.scheduleLaunchActivity(new Intent(r.intent), r.appToken,
                    System.identityHashCode(r), r.info, new Configuration(mConfiguration),
                    CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                    ActivityManager.PROCESS_STATE_TOP, r.icicle, null, null, false, true,
                    null, null, false);
            windowManager().executeAppTransition();
        } catch (RemoteException e) {
            Slog.w(TAG, "launch " + r, e);
        }
    }

    /**
     * Process.start() of ActivityManagerService.startProcessLocked. There is
     * no zygote and no fork: the "process" is a thread of this VM running
     * ActivityThread.main(), with its own main Looper. Main Looper and
     * ActivityThread are per VM, so this works once per VM.
     */
    private ProcessRecord startProcessLocked(String processName, ApplicationInfo info) {
        final ProcessRecord app = new ProcessRecord(processName, info);
        mProcesses.put(processName, app);
        if (mAppProcessStarted) {
            // The VM's process is gone: ActivityThread runs once per VM.
            Slog.e(TAG, "one app process per VM: not starting " + processName);
            app.dead = true;
            return app;
        }
        mAppProcessStarted = true;
        mVmProcess = app;
        Slog.i(TAG, "Start proc " + processName + " for activity: uid=" + info.uid);
        app.mainThread = new Thread(new Runnable() {
            public void run() {
                Throwable t;
                try {
                    android.app.ActivityThread.main(new String[0]);
                    t = new IllegalStateException("main loop exited");
                } catch (Throwable e) {
                    t = e;
                }
                // An app that crashed is closed, and the others keep running
                // on this thread's Looper (CrashRecovery).
                while (forceCloseCrashed(t)) {
                    try {
                        android.os.Looper.loop();
                        t = new IllegalStateException("main loop exited");
                    } catch (Throwable e) {
                        t = e;
                    }
                }
                Senses.crash(app.info.packageName, "process died: " + Senses.reason(t));
                try {
                    Slog.e(TAG, "process " + app.processName + " died", t);
                } finally {
                    appDied(app);
                }
            }
        // "main", as an app's UI thread on a device: some apps check the name
        // (Burstly's ads in Angry Birds 2011: "not in UI thread" otherwise).
        }, "main");
        app.mainThread.start();
        return app;
    }

    /**
     * CrashRecovery: T ended the app thread's Looper. Closes the package whose
     * code threw, as Android does when it kills that package's process: its
     * activities go, the top activity left, or the home screen, resumes.
     * False when the process is to die instead.
     */
    private boolean forceCloseCrashed(Throwable t) {
        List<String> packages;
        List<String> essential = new ArrayList<String>();
        synchronized (this) {
            packages = new ArrayList<String>(mHosted.keySet());
        }
        essential.add("com.android.systemui");
        String pkg = CrashRecovery.packageToClose(t, packages, essential);
        if (pkg == null) {
            return false;
        }
        Senses.crash(pkg, Senses.reason(t));  // one-app mode: back to the page's library
        synchronized (this) {
            Slog.w(TAG, "force-closing " + pkg + " (the other apps keep running)");
            for (TaskRecord task : new ArrayList<TaskRecord>(mTasks)) {
                for (ActivityRecord r : new ArrayList<ActivityRecord>(task.activities)) {
                    if (!pkg.equals(r.info.packageName)) {
                        continue;
                    }
                    if (mPausingActivity == r) {
                        mPausingActivity = null;
                    }
                    r.finishing = true;
                    try {
                        windowManager().setAppVisibility(r.appToken, false);
                    } catch (RemoteException e) {
                        // In process: not thrown.
                    }
                    if (r.state != State.INITIALIZING && r.app != null && r.app.thread != null) {
                        try {
                            r.app.thread.scheduleDestroyActivity(r.appToken, true, 0);
                        } catch (RemoteException e) {
                            // In process: not thrown.
                        }
                    }
                    removeActivityLocked(r);
                }
            }
            if (topRunningActivity() != null) {
                resumeTopActivityLocked();
                return true;
            }
        }
        startHome();
        return true;
    }

    private void appDied(ProcessRecord app) {
        synchronized (this) {
            app.dead = true;
            app.thread = null;
            mHosted.values().removeAll(java.util.Collections.singleton(app));
            for (ActivityRecord r : new ArrayList<ActivityRecord>(app.activities)) {
                removeActivityLocked(r);
            }
            if (mResumedActivity != null && mResumedActivity.app == app) {
                mResumedActivity = null;
            }
            if (mPausingActivity != null && mPausingActivity.app == app) {
                mPausingActivity = null;
            }
        }
    }

    /** The process this service started and the app has not attached to yet. */
    private ProcessRecord pendingProcessLocked() {
        for (ProcessRecord app : mProcesses.values()) {
            if (app.thread == null && !app.dead) {
                return app;
            }
        }
        return null;
    }

    @Override
    public void attachApplication(IApplicationThread thread) {
        synchronized (this) {
            ProcessRecord app = pendingProcessLocked();
            if (app == null) {
                Slog.w(TAG, "attachApplication from an unknown process");
                return;
            }
            app.thread = proxyFor(thread);
            app.packages.add(app.info.packageName);  // its providers come with bindApplication
            Map<String, IBinder> services = new HashMap<String, IBinder>();
            services.put("package", ServiceManager.getService("package"));
            services.put("window", ServiceManager.getService("window"));
            try {
                // The app's own content providers, installed by ActivityThread
                // before Application.onCreate and published back (topic userspace).
                List<ProviderInfo> providers = mPackageManager.queryContentProviders(
                        app.processName, app.info.uid, PackageManager.GET_URI_PERMISSION_PATTERNS);
                app.thread.bindApplication(app.processName, app.info,
                        providers.isEmpty() ? null : providers, null, null, null,
                        false, null, null, null, 0, false, false, false,
                        new Configuration(mConfiguration),
                        CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO, services, new Bundle());
            } catch (RemoteException e) {
                Slog.w(TAG, "bindApplication", e);
                return;
            }
            Slog.i(TAG, "bound " + app);
            ActivityRecord top = topRunningActivity();
            if (top != null && top.app == app && top.state == State.INITIALIZING
                    && mResumedActivity == null) {
                realStartActivityLocked(top, app);
            }
            // Services that waited for this process (topic userspace).
            for (ServiceRecord sr : new ArrayList<ServiceRecord>(mServices.values())) {
                if (sr.app == app && !sr.created) {
                    realStartServiceLocked(sr);
                }
            }
        }
    }

    // --- callbacks from the app ----------------------------------------------------

    @Override
    public void activityResumed(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r != null) {
                Slog.i(TAG, "resumed " + r);
            }
        }
    }

    @Override
    public void activityIdle(IBinder token, Configuration config, boolean stopProfiling) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r == null) {
                return;
            }
            r.idle = true;
            Slog.i(TAG, "idle " + r);
            // ActivityStackSupervisor.activityIdleInternalLocked.
            processStoppingLocked();
        }
    }

    private void processStoppingLocked() {
        List<ActivityRecord> stopping = new ArrayList<ActivityRecord>(mStopping);
        mStopping.clear();
        for (ActivityRecord s : stopping) {
            if (s.app == null || s.app.thread == null) {
                removeActivityLocked(s);
            } else if (s.finishing) {
                // Stopped first, if it is not yet, by ActivityThread.handleDestroyActivity.
                destroyActivityLocked(s);
            } else if (s.state == State.PAUSED) {
                stopActivityLocked(s);
            }
        }
    }

    private void stopActivityLocked(ActivityRecord r) {
        history("stop", r);
        r.state = State.STOPPING;
        r.visible = false;
        try {
            windowManager().setAppVisibility(r.appToken, false);
            r.app.thread.scheduleStopActivity(r.appToken, false, r.info.configChanges);
        } catch (RemoteException e) {
            Slog.w(TAG, "stop " + r, e);
        }
    }

    private void destroyActivityLocked(ActivityRecord r) {
        history("destroy", r);
        r.state = State.DESTROYING;
        r.visible = false;
        try {
            windowManager().setAppVisibility(r.appToken, false);
            r.app.thread.scheduleDestroyActivity(r.appToken, r.finishing, r.info.configChanges);
        } catch (RemoteException e) {
            Slog.w(TAG, "destroy " + r, e);
        }
    }

    private void startPausingLocked(ActivityRecord r, boolean userLeaving) {
        history("pause", r);
        mResumedActivity = null;
        mPausingActivity = r;
        r.state = State.PAUSING;
        try {
            r.app.thread.schedulePauseActivity(r.appToken, r.finishing, userLeaving,
                    r.info.configChanges);
        } catch (RemoteException e) {
            Slog.w(TAG, "pause " + r, e);
        }
    }

    @Override
    public void activityPaused(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r == null) {
                return;
            }
            // ActivityStack.completePauseLocked.
            r.state = State.PAUSED;
            if (mPausingActivity == r) {
                mPausingActivity = null;
            }
            Slog.i(TAG, "paused " + r);
            if (r.finishing || r != topRunningActivity()) {
                // Hidden behind the next activity (or going away): stopped or
                // destroyed once the next one is idle.
                mStopping.add(r);
            }
            resumeTopActivityLocked();
        }
    }

    @Override
    public void activityStopped(IBinder token, Bundle icicle, Bitmap thumbnail,
            CharSequence description) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r == null) {
                return;
            }
            r.icicle = icicle;
            Slog.i(TAG, "stopped " + r);
            if (r.state == State.STOPPING) {
                r.state = State.STOPPED;
            }
            if (r.finishing) {
                destroyActivityLocked(r);
            }
        }
    }

    @Override
    public void activityDestroyed(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r != null) {
                Slog.i(TAG, "destroyed " + r);
                removeActivityLocked(r);
            }
        }
    }

    private void removeActivityLocked(ActivityRecord r) {
        r.state = State.DESTROYED;
        mStopping.remove(r);
        if (mResumedActivity == r) {
            mResumedActivity = null;
        }
        if (r.app != null) {
            r.app.activities.remove(r);
        }
        TaskRecord task = r.task;
        task.activities.remove(r);
        if (task.activities.isEmpty()) {
            mTasks.remove(task);
        }
        try {
            windowManager().removeAppToken(r.appToken);
        } catch (RemoteException e) {
            // In process: not thrown.
        }
    }

    @Override
    public boolean finishActivity(IBinder token, int resultCode, Intent resultData) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r == null) {
                return false;
            }
            return finishActivityLocked(r);
        }
    }

    /** ActivityStack.finishActivityLocked. */
    private boolean finishActivityLocked(ActivityRecord r) {
        if (r.finishing) {
            return false;
        }
        Slog.i(TAG, "finishing " + r);
        r.finishing = true;
        try {
            windowManager().prepareAppTransition(7 /* TRANSIT_ACTIVITY_CLOSE */, false);
        } catch (RemoteException e) {
            // In process: not thrown.
        }
        if (r == mResumedActivity) {
            startPausingLocked(r, false);
        } else if (r.state == State.STOPPED) {
            destroyActivityLocked(r);
        } else if (r.state == State.PAUSED || r.state == State.STOPPING) {
            mStopping.add(r);
            resumeTopActivityLocked();
        } else if (r.state == State.INITIALIZING) {
            removeActivityLocked(r);
        }
        return true;
    }

    @Override
    public boolean willActivityBeVisible(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            return r != null && r == topRunningActivity();
        }
    }

    @Override
    public void moveTaskToBack(int taskId) {
        synchronized (this) {
            for (TaskRecord t : mTasks) {
                if (t.taskId == taskId) {
                    moveTaskToBackLocked(t);
                    return;
                }
            }
        }
    }

    @Override
    public boolean moveActivityTaskToBack(IBinder token, boolean nonRoot) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r == null || (!nonRoot && r.task.activities.indexOf(r) != 0)) {
                return false;
            }
            moveTaskToBackLocked(r.task);
            return true;
        }
    }

    private void moveTaskToBackLocked(TaskRecord t) {
        Slog.i(TAG, "moving task " + t.taskId + " to the back");
        mTasks.remove(t);
        mTasks.add(0, t);
        t.inBackground = true;
        try {
            windowManager().prepareAppTransition(11 /* TRANSIT_TASK_TO_BACK */, false);
        } catch (RemoteException e) {
            // In process: not thrown.
        }
        for (ActivityRecord r : t.activities) {
            r.visible = false;
            if (r.state == State.PAUSED && !mStopping.contains(r)) {
                mStopping.add(r);
            }
        }
        if (mResumedActivity != null && mResumedActivity.task == t) {
            startPausingLocked(mResumedActivity, true);
        } else {
            resumeTopActivityLocked();
        }
    }

    @Override
    public void moveTaskToFront(int taskId, int flags, Bundle options) {
        synchronized (this) {
            for (TaskRecord t : mTasks) {
                if (t.taskId == taskId) {
                    moveTaskToFrontLocked(t);
                    return;
                }
            }
        }
    }

    @Override
    public int getTaskForActivity(IBinder token, boolean onlyRoot) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r == null || (onlyRoot && r.task.activities.indexOf(r) != 0)) {
                return -1;
            }
            return r.task.taskId;
        }
    }

    @Override
    public String getCallingPackage(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            return r == null ? null : r.launchedFromPackage;
        }
    }

    @Override
    public ComponentName getCallingActivity(IBinder token) {
        return null;
    }

    @Override
    public void setRequestedOrientation(IBinder token, int requestedOrientation) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r != null) {
                try {
                    windowManager().setAppOrientation(r.appToken, requestedOrientation);
                } catch (RemoteException e) {
                    // In process: not thrown.
                }
                if (r == mResumedActivity) {
                    updateRotationLocked(r);
                }
            }
        }
    }

    /**
     * Before |r| shows (topic userspace): the window manager turns the
     * display if |r| wants another orientation (a landscape game on the
     * portrait screen, home again after it); then the configuration follows,
     * the app hears of it (scheduleConfigurationChanged), and the windows lay
     * out on the turned display. ActivityStack.ensureActivityConfigurationLocked
     * would relaunch activities that do not handle the change; here they get
     * the new resources only (one comes back to its own orientation anyway).
     */
    private void updateRotationLocked(ActivityRecord r) {
        IWindowManager wm = windowManager();
        if (!(wm instanceof WindowManagerService)
                || !((WindowManagerService) wm).updateRotationFor(r.appToken)) {
            return;
        }
        final int seq = mConfiguration.seq;
        computeConfiguration();
        mConfiguration.seq = seq + 1;
        Configuration config = new Configuration(mConfiguration);
        Slog.i(TAG, "configuration after the rotation: " + config);
        if (mVmProcess != null && mVmProcess.thread != null) {
            try {
                mVmProcess.thread.scheduleConfigurationChanged(config);
            } catch (RemoteException e) {
                // In process: not thrown.
            }
        }
        ((WindowManagerService) wm).relayoutAll(config);
    }

    @Override
    public int getRequestedOrientation(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            return r == null ? ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED : r.info.screenOrientation;
        }
    }

    @Override
    public void overridePendingTransition(IBinder token, String packageName, int enterAnim,
            int exitAnim) {
    }

    @Override
    public void reportActivityFullyDrawn(IBinder token) {
    }

    @Override
    public boolean isImmersive(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r == null) {
                throw new IllegalArgumentException();
            }
            return (r.info.flags & ActivityInfo.FLAG_IMMERSIVE) != 0;
        }
    }

    @Override
    public boolean isTopActivityImmersive() {
        synchronized (this) {
            ActivityRecord r = topRunningActivity();
            return r != null && (r.info.flags & ActivityInfo.FLAG_IMMERSIVE) != 0;
        }
    }

    // --- state for ActivityManager --------------------------------------------------

    @Override
    public Configuration getConfiguration() {
        synchronized (this) {
            return new Configuration(mConfiguration);
        }
    }

    @Override
    public List<ActivityManager.RunningTaskInfo> getTasks(int maxNum, int flags,
            android.app.IThumbnailReceiver receiver) {
        synchronized (this) {
            List<ActivityManager.RunningTaskInfo> list = new ArrayList<ActivityManager.RunningTaskInfo>();
            for (int i = mTasks.size() - 1; i >= 0 && list.size() < maxNum; i--) {
                TaskRecord t = mTasks.get(i);
                ActivityManager.RunningTaskInfo ti = new ActivityManager.RunningTaskInfo();
                ti.id = t.taskId;
                ti.baseActivity = t.activities.get(0).realActivity;
                ActivityRecord top = t.topRunning();
                ti.topActivity = top != null ? top.realActivity
                        : t.activities.get(t.activities.size() - 1).realActivity;
                ti.numActivities = t.activities.size();
                for (ActivityRecord r : t.activities) {
                    if (r.app != null && r.app.thread != null && r.state != State.STOPPED) {
                        ti.numRunning++;
                    }
                }
                ti.lastActiveTime = SystemClock.elapsedRealtime();
                list.add(ti);
            }
            return list;
        }
    }

    @Override
    public List<ActivityManager.RunningAppProcessInfo> getRunningAppProcesses() {
        synchronized (this) {
            List<ActivityManager.RunningAppProcessInfo> list =
                    new ArrayList<ActivityManager.RunningAppProcessInfo>();
            for (ProcessRecord app : mProcesses.values()) {
                if (app.thread == null) {
                    continue;
                }
                ActivityManager.RunningAppProcessInfo pi = new ActivityManager.RunningAppProcessInfo(
                        app.processName, Process.myPid(),
                        app.packages.toArray(new String[app.packages.size()]));
                pi.uid = app.info.uid;
                boolean top = mResumedActivity != null && mResumedActivity.app == app;
                pi.importance = top ? ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND
                        : ActivityManager.RunningAppProcessInfo.IMPORTANCE_BACKGROUND;
                list.add(pi);
            }
            return list;
        }
    }

    @Override
    public boolean isUserAMonkey() {
        return false;
    }

    @Override
    public int checkPermission(String permission, int pid, int uid) {
        // ActivityManagerService.checkComponentPermission: root and system
        // have everything; the rest is the package manager's grant.
        if (uid == 0 || uid == Process.SYSTEM_UID || uid == Process.myUid()) {
            return PackageManager.PERMISSION_GRANTED;
        }
        try {
            return mPackageManager.checkUidPermission(permission, uid);
        } catch (RemoteException e) {
            return PackageManager.PERMISSION_DENIED;
        }
    }

    // --- crashes and StrictMode reports: logged (no DropBox, no crash dialog) ----

    @Override
    public void handleApplicationStrictModeViolation(IBinder app, int violationMask,
            android.os.StrictMode.ViolationInfo info) {
        Slog.w(TAG, "StrictMode violation 0x" + Integer.toHexString(violationMask) + ": "
                + (info == null || info.crashInfo == null ? null : info.crashInfo.exceptionMessage));
    }

    @Override
    public void handleApplicationCrash(IBinder app,
            android.app.ApplicationErrorReport.CrashInfo crashInfo) {
        Slog.e(TAG, "app crashed: " + crashInfo.exceptionClassName + ": "
                + crashInfo.exceptionMessage + "\n" + crashInfo.stackTrace);
        Senses.crash(Senses.app(), crashInfo.exceptionClassName + ": "
                + crashInfo.exceptionMessage);
    }

    @Override
    public boolean handleApplicationWtf(IBinder app, String tag,
            android.app.ApplicationErrorReport.CrashInfo crashInfo) {
        Slog.e(TAG, "wtf from app, " + tag + ": " + crashInfo.exceptionMessage);
        return false;
    }

    // --- broadcasts: accepted, not delivered -----------------------------------------

    // --- broadcasts (topic userspace): registered receivers and stickies ----------
    // No ordered delivery, no manifest receivers, no permissions: every
    // registered receiver whose filter matches gets the intent, unordered.

    private static final class Registered {
        final IIntentReceiver receiver;
        final IntentFilter filter;

        Registered(IIntentReceiver receiver, IntentFilter filter) {
            this.receiver = receiver;
            this.filter = filter;
        }
    }

    private final List<Registered> mReceivers = new ArrayList<Registered>();
    private final Map<String, Intent> mStickies = new HashMap<String, Intent>();

    private static boolean matches(IntentFilter filter, Intent intent) {
        return filter.match(intent.getAction(), intent.resolveType((android.content.ContentResolver) null),
                intent.getScheme(), intent.getData(), intent.getCategories(), TAG) >= 0;
    }

    @Override
    public Intent registerReceiver(IApplicationThread caller, String callerPackage,
            IIntentReceiver receiver, IntentFilter filter, String permission, int userId) {
        Intent sticky = null;
        synchronized (mReceivers) {
            for (Intent i : mStickies.values()) {
                if (matches(filter, i)) {
                    sticky = i;
                    break;
                }
            }
            if (receiver != null) {
                mReceivers.add(new Registered(receiver, filter));
            }
        }
        if (receiver != null && sticky != null) {
            try {
                receiver.performReceive(new Intent(sticky), 0, null, null, false, true, userId);
            } catch (RemoteException e) {
                // In process: not thrown.
            }
        }
        return sticky;
    }

    @Override
    public void unregisterReceiver(IIntentReceiver receiver) {
        IBinder b = receiver.asBinder();
        synchronized (mReceivers) {
            for (int i = mReceivers.size() - 1; i >= 0; i--) {
                if (mReceivers.get(i).receiver.asBinder() == b) {
                    mReceivers.remove(i);
                }
            }
        }
    }

    @Override
    public int broadcastIntent(IApplicationThread caller, Intent intent, String resolvedType,
            IIntentReceiver resultTo, int resultCode, String resultData, Bundle map,
            String requiredPermission, int appOp, boolean serialized, boolean sticky, int userId) {
        List<IIntentReceiver> targets = new ArrayList<IIntentReceiver>();
        synchronized (mReceivers) {
            if (sticky && intent.getAction() != null) {
                mStickies.put(intent.getAction(), new Intent(intent));
            }
            for (Registered r : mReceivers) {
                if (matches(r.filter, intent)) {
                    targets.add(r.receiver);
                }
            }
        }
        for (IIntentReceiver r : targets) {
            try {
                r.performReceive(new Intent(intent), resultCode, resultData, map, false, sticky,
                        userId);
            } catch (RemoteException e) {
                Slog.w(TAG, "receiver died", e);
            }
        }
        if (resultTo != null) {
            try {
                resultTo.performReceive(new Intent(intent), resultCode, resultData, map, false,
                        false, userId);
            } catch (RemoteException e) {
                // In process: not thrown.
            }
        }
        return ActivityManager.BROADCAST_SUCCESS;
    }

    // --- PendingIntent: intent senders ---------------------------------------------------
    // What ActivityManagerService's PendingIntentRecord does, without users,
    // permissions or result receivers: one sender per (type, package,
    // requestCode, intent) key, as PendingIntent.equals() relies on.
    // send() starts the activity or delivers the broadcast; services are not
    // run yet (see startService).

    private final class IntentSender extends IIntentSender.Stub {
        final int type;
        final String packageName;
        final int requestCode;
        Intent[] intents;
        String[] resolvedTypes;
        boolean canceled;

        IntentSender(int type, String packageName, int requestCode, Intent[] intents,
                String[] resolvedTypes) {
            this.type = type;
            this.packageName = packageName;
            this.requestCode = requestCode;
            this.intents = intents;
            this.resolvedTypes = resolvedTypes;
        }

        String key() {
            return senderKey(type, packageName, requestCode, intents[intents.length - 1]);
        }

        @Override
        public int send(int code, Intent fillIn, String resolvedType,
                IIntentReceiver finishedReceiver, String requiredPermission) {
            if (canceled) {
                return ActivityManager.START_CANCELED;
            }
            Intent intent = new Intent(intents[intents.length - 1]);
            if (fillIn != null) {
                intent.fillIn(fillIn, 0);
            }
            switch (type) {
                case ActivityManager.INTENT_SENDER_ACTIVITY:
                    return startActivity(null, packageName, intent, resolvedType, null, null, 0,
                            Intent.FLAG_ACTIVITY_NEW_TASK, null, null, null);
                case ActivityManager.INTENT_SENDER_BROADCAST:
                    return broadcastIntent(null, intent, resolvedType, finishedReceiver, code,
                            null, null, requiredPermission, 0, finishedReceiver != null, false, 0);
                case ActivityManager.INTENT_SENDER_SERVICE:
                    startService(null, intent, resolvedType, 0);
                    return 0;
                default:
                    Slog.w(TAG, "intent sender of type " + type + " not supported: " + intent);
                    return 0;
            }
        }
    }

    private final Map<String, IntentSender> mIntentSenders = new HashMap<String, IntentSender>();

    private static String senderKey(int type, String packageName, int requestCode, Intent intent) {
        // Intent.filterEquals compares action, data, type, class and
        // categories; filterHashCode is consistent with it.
        StringBuilder key = new StringBuilder();
        key.append(type).append('|').append(packageName).append('|').append(requestCode)
                .append('|').append(intent.getAction()).append('|').append(intent.getData())
                .append('|').append(intent.getType()).append('|').append(intent.getComponent())
                .append('|').append(intent.getPackage()).append('|').append(intent.getCategories());
        return key.toString();
    }

    @Override
    public synchronized IIntentSender getIntentSender(int type, String packageName,
            IBinder token, String resultWho, int requestCode, Intent[] intents,
            String[] resolvedTypes, int flags, Bundle options, int userId) {
        if (intents == null || intents.length == 0) {
            throw new IllegalArgumentException("no intents");
        }
        String key = senderKey(type, packageName, requestCode, intents[intents.length - 1]);
        IntentSender existing = mIntentSenders.get(key);
        if ((flags & android.app.PendingIntent.FLAG_CANCEL_CURRENT) != 0 && existing != null) {
            existing.canceled = true;
            mIntentSenders.remove(key);
            existing = null;
        }
        if (existing != null) {
            if ((flags & android.app.PendingIntent.FLAG_UPDATE_CURRENT) != 0) {
                existing.intents = intents;
                existing.resolvedTypes = resolvedTypes;
            }
            return existing;
        }
        if ((flags & android.app.PendingIntent.FLAG_NO_CREATE) != 0) {
            return null;
        }
        IntentSender sender = new IntentSender(type, packageName, requestCode, intents,
                resolvedTypes);
        if ((flags & android.app.PendingIntent.FLAG_ONE_SHOT) == 0) {
            mIntentSenders.put(key, sender);
        }
        return sender;
    }

    @Override
    public synchronized void cancelIntentSender(IIntentSender sender) {
        if (sender instanceof IntentSender) {
            IntentSender s = (IntentSender) sender;
            s.canceled = true;
            mIntentSenders.remove(s.key());
        }
    }

    @Override
    public String getPackageForIntentSender(IIntentSender sender) {
        return sender instanceof IntentSender ? ((IntentSender) sender).packageName : null;
    }

    @Override
    public int getUidForIntentSender(IIntentSender sender) {
        // One uid per VM (see docs/topics/activity.md).
        return android.os.Process.myUid();
    }

    @Override
    public boolean isIntentSenderTargetedToPackage(IIntentSender sender) {
        if (!(sender instanceof IntentSender)) {
            return false;
        }
        for (Intent i : ((IntentSender) sender).intents) {
            if (i.getPackage() == null && i.getComponent() == null) {
                return false;
            }
        }
        return true;
    }

    @Override
    public boolean isIntentSenderAnActivity(IIntentSender sender) {
        return sender instanceof IntentSender
                && ((IntentSender) sender).type == ActivityManager.INTENT_SENDER_ACTIVITY;
    }

    @Override
    public Intent getIntentForIntentSender(IIntentSender sender) {
        if (!(sender instanceof IntentSender)) {
            return null;
        }
        Intent[] intents = ((IntentSender) sender).intents;
        return new Intent(intents[intents.length - 1]);
    }

    // --- services ----------------------------------------------------------------------
    // --- services (topic userspace, docs/topics/userspace.md) ---------------------
    // ActiveServices, lean: a service runs in the VM's one process (started for
    // it if no app runs yet); startService creates it and delivers each start
    // (onStartCommand), bindService asks the service once per intent for its
    // binder (onBind -> publishService) and hands it to every connection. No
    // restarts, no foreground state, no timeouts; unbinding forgets the
    // connection, stopping destroys the service.

    static final class Binding {
        final Intent intent;
        IBinder service;
        boolean requested;
        boolean published;
        final List<android.app.IServiceConnection> connections =
                new ArrayList<android.app.IServiceConnection>();

        Binding(Intent intent) {
            this.intent = intent;
        }
    }

    /** A running service; the record is also its token (ServiceRecord extends Binder). */
    static final class ServiceRecord extends Binder {
        final ComponentName name;
        final android.content.pm.ServiceInfo info;
        ProcessRecord app;
        boolean created;
        int lastStartId;
        final List<Intent> pendingStarts = new ArrayList<Intent>();
        final Map<Intent.FilterComparison, Binding> bindings =
                new HashMap<Intent.FilterComparison, Binding>();

        ServiceRecord(android.content.pm.ServiceInfo info) {
            this.info = info;
            this.name = new ComponentName(info.packageName, info.name);
        }

        @Override
        public String toString() {
            return "ServiceRecord{" + name.flattenToShortString() + "}";
        }
    }

    private final Map<ComponentName, ServiceRecord> mServices =
            new HashMap<ComponentName, ServiceRecord>();

    private ServiceRecord retrieveServiceLocked(Intent service, String resolvedType) {
        android.content.pm.ResolveInfo ri;
        try {
            ri = mPackageManager.resolveService(service, resolvedType, STOCK_PM_FLAGS, 0);
        } catch (RemoteException e) {
            ri = null;
        }
        if (ri == null || ri.serviceInfo == null) {
            return null;
        }
        if (Gms.isUserGoogle(ri.serviceInfo)) {
            return null;  // the user's Google package (topic gms): no process of its own here
        }
        ComponentName name = new ComponentName(ri.serviceInfo.packageName, ri.serviceInfo.name);
        ServiceRecord sr = mServices.get(name);
        if (sr == null) {
            sr = new ServiceRecord(ri.serviceInfo);
            mServices.put(name, sr);
        }
        return sr;
    }

    private ServiceRecord forServiceToken(IBinder token) {
        for (ServiceRecord sr : mServices.values()) {
            if (sr == token) {
                return sr;
            }
        }
        return null;
    }

    /** The process a component runs in: the VM's one process, started if needed. */
    private ProcessRecord processForLocked(String processName, ApplicationInfo info) {
        ProcessRecord app = mProcesses.get(processName);
        if (app == null) {
            app = mHosted.get(processName);
        }
        if (app == null && mVmProcess != null && !mVmProcess.dead) {
            app = mVmProcess;
            mHosted.put(processName, app);
            Slog.i(TAG, "hosting " + processName + " in process " + app.processName);
        }
        if (app == null) {
            app = startProcessLocked(processName, info);
        }
        return app;
    }

    private void bringUpServiceLocked(ServiceRecord sr) {
        if (sr.app == null || sr.app.dead) {
            sr.app = processForLocked(sr.info.processName, sr.info.applicationInfo);
            sr.created = false;
        }
        if (sr.app.dead) {
            Slog.w(TAG, "no process for " + sr);
            return;
        }
        if (sr.app.thread != null) {
            realStartServiceLocked(sr);
        }  // else: attachApplication starts it
    }

    private void realStartServiceLocked(ServiceRecord sr) {
        try {
            if (!sr.created) {
                installPackageLocked(sr.app, sr.info);
                Slog.i(TAG, "create " + sr);
                sr.app.thread.scheduleCreateService(sr, sr.info,
                        CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                        ActivityManager.PROCESS_STATE_SERVICE);
                sr.created = true;
            }
            for (Intent i : sr.pendingStarts) {
                sr.app.thread.scheduleServiceArgs(sr, false, ++sr.lastStartId, 0, i);
            }
            sr.pendingStarts.clear();
            for (Binding b : sr.bindings.values()) {
                requestBindLocked(sr, b);
            }
        } catch (RemoteException e) {
            Slog.w(TAG, "start " + sr, e);
        }
    }

    private void requestBindLocked(ServiceRecord sr, Binding b) throws RemoteException {
        if (!b.requested && !b.connections.isEmpty() && sr.created) {
            b.requested = true;
            sr.app.thread.scheduleBindService(sr, b.intent, false,
                    ActivityManager.PROCESS_STATE_SERVICE);
        }
    }

    @Override
    public ComponentName startService(IApplicationThread caller, Intent service,
            String resolvedType, int userId) {
        synchronized (this) {
            ServiceRecord sr = retrieveServiceLocked(service, resolvedType);
            if (sr == null) {
                // What Android answers for a service it cannot find (apps handle
                // it, e.g. GCM without Google Play services).
                Slog.w(TAG, "Unable to start service " + service + ": not found");
                return null;
            }
            sr.pendingStarts.add(new Intent(service));
            bringUpServiceLocked(sr);
            return sr.name;
        }
    }

    private void stopServiceLocked(ServiceRecord sr) {
        mServices.remove(sr.name);
        if (sr.created && sr.app != null && sr.app.thread != null) {
            try {
                Slog.i(TAG, "stop " + sr);
                sr.app.thread.scheduleStopService(sr);
            } catch (RemoteException e) {
                // In process: not thrown.
            }
        }
    }

    @Override
    public int stopService(IApplicationThread caller, Intent service, String resolvedType,
            int userId) {
        synchronized (this) {
            ServiceRecord sr = retrieveServiceLocked(service, resolvedType);
            if (sr == null || !sr.created) {
                return 0;
            }
            stopServiceLocked(sr);
            return 1;
        }
    }

    @Override
    public boolean stopServiceToken(ComponentName className, IBinder token, int startId) {
        synchronized (this) {
            ServiceRecord sr = forServiceToken(token);
            if (sr == null || (startId >= 0 && startId != sr.lastStartId)) {
                return false;
            }
            stopServiceLocked(sr);
            return true;
        }
    }

    @Override
    public void setServiceForeground(ComponentName className, IBinder token, int id,
            android.app.Notification notification, boolean removeNotification) {
        // No foreground bookkeeping: services are never killed for memory here.
    }

    @Override
    public int bindService(IApplicationThread caller, IBinder token, Intent service,
            String resolvedType, android.app.IServiceConnection connection, int flags,
            int userId) {
        synchronized (this) {
            ServiceRecord sr = retrieveServiceLocked(service, resolvedType);
            if (sr == null) {
                // 0: no such service (ContextImpl.bindService then returns false), as
                // for an app whose service provider (Google Play billing) is absent.
                Slog.w(TAG, "Unable to bind service " + service + ": not found");
                return 0;
            }
            Intent.FilterComparison key = new Intent.FilterComparison(new Intent(service));
            Binding b = sr.bindings.get(key);
            if (b == null) {
                b = new Binding(new Intent(service));
                sr.bindings.put(key, b);
            }
            b.connections.add(connection);
            if (b.published) {
                try {
                    connection.connected(sr.name, b.service);
                } catch (RemoteException e) {
                    // In process: not thrown.
                }
            } else {
                bringUpServiceLocked(sr);
            }
            return 1;
        }
    }

    @Override
    public void publishService(IBinder token, Intent intent, IBinder service) {
        List<android.app.IServiceConnection> targets;
        ComponentName name;
        synchronized (this) {
            ServiceRecord sr = forServiceToken(token);
            Binding b = sr == null ? null : sr.bindings.get(new Intent.FilterComparison(intent));
            if (b == null) {
                return;
            }
            b.service = service;
            b.published = true;
            name = sr.name;
            targets = new ArrayList<android.app.IServiceConnection>(b.connections);
        }
        for (android.app.IServiceConnection c : targets) {
            try {
                c.connected(name, service);
            } catch (RemoteException e) {
                Slog.w(TAG, "connection died", e);
            }
        }
    }

    @Override
    public boolean unbindService(android.app.IServiceConnection connection) {
        IBinder b = connection.asBinder();
        boolean found = false;
        synchronized (this) {
            for (ServiceRecord sr : mServices.values()) {
                for (Binding binding : sr.bindings.values()) {
                    for (int i = binding.connections.size() - 1; i >= 0; i--) {
                        if (binding.connections.get(i).asBinder() == b) {
                            binding.connections.remove(i);
                            found = true;
                        }
                    }
                }
            }
        }
        return found;
    }

    @Override
    public void unbindFinished(IBinder token, Intent intent, boolean doRebind) {
    }

    @Override
    public void serviceDoneExecuting(IBinder token, int type, int startId, int res) {
    }

    @Override
    public IBinder peekService(Intent service, String resolvedType) {
        synchronized (this) {
            ServiceRecord sr = retrieveServiceLocked(service, resolvedType);
            Binding b = sr == null ? null
                    : sr.bindings.get(new Intent.FilterComparison(new Intent(service)));
            return b != null && b.published ? b.service : null;
        }
    }

    @Override
    public List<ActivityManager.RunningServiceInfo> getServices(int maxNum, int flags) {
        List<ActivityManager.RunningServiceInfo> list =
                new ArrayList<ActivityManager.RunningServiceInfo>();
        synchronized (this) {
            for (ServiceRecord sr : mServices.values()) {
                if (list.size() >= maxNum) {
                    break;
                }
                ActivityManager.RunningServiceInfo ri = new ActivityManager.RunningServiceInfo();
                ri.service = sr.name;
                ri.pid = Process.myPid();
                ri.uid = sr.info.applicationInfo.uid;
                ri.process = sr.info.processName;
                ri.started = sr.lastStartId > 0;
                ri.clientCount = sr.bindings.size();
                list.add(ri);
            }
        }
        return list;
    }

    // --- content providers: the system's "settings" --------------------------------------

    /** Providers apps published (authority -> holder), topic userspace. */
    private final Map<String, IActivityManager.ContentProviderHolder> mPublished =
            new HashMap<String, IActivityManager.ContentProviderHolder>();

    @Override
    public void publishContentProviders(IApplicationThread caller,
            List<IActivityManager.ContentProviderHolder> providers) {
        if (providers == null) {
            return;
        }
        synchronized (mPublished) {
            for (IActivityManager.ContentProviderHolder h : providers) {
                if (h == null || h.info == null || h.info.authority == null) {
                    continue;
                }
                IActivityManager.ContentProviderHolder pub =
                        new IActivityManager.ContentProviderHolder(h.info);
                pub.provider = h.provider;
                pub.noReleaseNeeded = true;
                for (String a : h.info.authority.split(";")) {
                    mPublished.put(a, pub);
                    Slog.i(TAG, "published provider " + a + " of " + h.info.packageName);
                }
            }
        }
    }

    @Override
    public IActivityManager.ContentProviderHolder getContentProvider(IApplicationThread caller,
            String name, int userId, boolean stable) {
        android.content.IContentProvider system = systemProvider(name);
        if (system == null) {
            synchronized (mPublished) {
                IActivityManager.ContentProviderHolder h = mPublished.get(name);
                if (h != null) {
                    return h;
                }
            }
            // Not running: starting another app's process for its provider
            // (one app per VM) is not done yet.
            Slog.w(TAG, "Failed to find provider info for " + name);
            return null;
        }
        ProviderInfo info = new ProviderInfo();
        info.authority = name;
        info.name = system.getClass().getName();
        info.packageName = "android";
        info.applicationInfo = new ApplicationInfo();
        info.applicationInfo.packageName = "android";
        info.applicationInfo.uid = Process.SYSTEM_UID;
        info.exported = true;
        IActivityManager.ContentProviderHolder holder = new IActivityManager.ContentProviderHolder(info);
        holder.provider = system;
        holder.noReleaseNeeded = true;
        return holder;
    }

    /** The providers of the system's own (on a device: system apps'). */
    private android.content.IContentProvider systemProvider(String authority) {
        if (SettingsProvider.AUTHORITY.equals(authority)) {
            return mSettings;
        }
        if (UserDictionaryProvider.AUTHORITY.equals(authority)) {
            return mUserDictionary;
        }
        return null;
    }

    @Override
    public void removeContentProvider(IBinder connection, boolean stable) {
    }

    @Override
    public boolean refContentProvider(IBinder connection, int stable, int unstable) {
        return true;
    }

    SettingsProvider settings() {
        return mSettings;
    }
}
