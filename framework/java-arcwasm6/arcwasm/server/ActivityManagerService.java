// arc-wasm: the "activity" service of the in-process system server on
// Android 6.0.1, a lean IActivityManager (docs/topics/fw6-system.md). The
// 4.4 build's (framework/java-arcwasm, docs/topics/activity.md) on 6.0's
// interfaces: it does what ActivityManagerService + ActivityStackSupervisor
// + ActivityStack do to start, pause, stop, resume and destroy activities,
// in their order and with the same IApplicationThread calls, for one app
// "process":
//
//   startActivity  resolve (package service) -> ActivityRecord in a task ->
//                  pause the resumed activity first -> start the process if
//                  needed -> scheduleLaunchActivity
//   process start  no zygote, no fork: a new Java thread runs
//                  ActivityThread.main(), whose attach() calls
//                  attachApplication -> bindApplication (with the app's
//                  content providers) + pending launches
//   activityPaused -> launch/resume the next top activity; the paused one is
//                  stopped (or destroyed, if finishing) once the next is idle
//   finishActivity, moveActivityTaskToBack -> pause -> stop -> destroy
//
// Added for 6.0 (beyond the 4.4 build): results to the calling activity
// (startActivityForResult -> scheduleSendResult), runtime permission
// requests (Activity.requestPermissions: answered at once, everything
// granted, as the package manager granted it at install), the app's own
// services (start/bind/unbind/stop in the app process), its content
// providers (published from bindApplication), dynamically registered
// broadcast receivers (and the sticky battery broadcast), and what 6.0's
// ActivityThread and ContextImpl ask besides (users, memory, app tasks).
//
// The app is reached the way system_server reaches an app process: through
// ApplicationThreadProxy, so every call is a transaction (Parcel and
// ApplicationThreadNative.onTransact), and callers reach this service through
// ActivityManagerProxy (arcwasm.server.base.ActivityManagerBase). The window
// manager is told about activity tokens through IWindowManager, as the real
// service does.
//
// Topic userspace (docs/topics/userspace.md): a home screen (Launcher3,
// HOME brings its singleTask root back with onNewIntent), recent tasks
// (removeTask, closeSystemDialogs), and every package in the VM's one app
// process: an activity or service of another package is hosted there, with
// its content providers installed first (scheduleInstallProvider).
//
// Not done: manifest receivers (only registered ones get broadcasts),
// starting a package for its content provider (published ones are handed
// out), launch modes and task affinities beyond "same task unless
// FLAG_ACTIVITY_NEW_TASK" and singleTask roots, timeouts and ANRs,
// configuration changes, users.
package arcwasm.server;

import android.app.ActivityManager;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.app.IServiceConnection;
import android.app.ProfilerInfo;
import android.app.ResultInfo;
import android.content.ComponentName;
import android.content.IIntentReceiver;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.ConfigurationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.content.pm.UserInfo;
import android.content.res.CompatibilityInfo;
import android.content.res.Configuration;
import android.graphics.Point;
import android.hardware.display.IDisplayManager;
import android.os.BatteryManager;
import android.os.Binder;
import android.os.Bundle;
import android.os.Debug;
import android.os.IBinder;
import android.os.PersistableBundle;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.os.UserHandle;
import android.util.Slog;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.IApplicationToken;
import android.view.IWindowManager;

import arcwasm.server.base.ActivityManagerBase;

import java.lang.reflect.Constructor;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public final class ActivityManagerService extends ActivityManagerBase {
    static final String TAG = "ActivityManager";
    /** ActivityManagerService.STOCK_PM_FLAGS. */
    static final int STOCK_PM_FLAGS = PackageManager.GET_SHARED_LIBRARY_FILES;
    /** Activity.REQUEST_PERMISSIONS_WHO_PREFIX. */
    static final String REQUEST_PERMISSIONS_WHO_PREFIX = "@android:requestPermissions:";

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
        /** Its content providers, by authority, once the app published them. */
        final Map<String, IActivityManager.ContentProviderHolder> providers =
                new HashMap<String, IActivityManager.ContentProviderHolder>();
        /** Services to create once the app is bound. */
        final List<ServiceRecord> pendingServices = new ArrayList<ServiceRecord>();
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
        /** startActivityForResult: who gets the result. */
        final ActivityRecord resultTo;
        final String resultWho;
        final int requestCode;
        int resultCode = android.app.Activity.RESULT_CANCELED;
        Intent resultData;
        /** Results waiting for this activity to come back (ActivityRecord.results). */
        final ArrayList<ResultInfo> results = new ArrayList<ResultInfo>();
        TaskRecord task;
        ProcessRecord app;
        State state = State.INITIALIZING;
        boolean finishing;
        boolean visible = true;
        boolean idle;
        Bundle icicle;
        PersistableBundle persistentState;

        ActivityRecord(ActivityInfo info, Intent intent, String launchedFromPackage,
                ActivityRecord resultTo, String resultWho, int requestCode) {
            this.info = info;
            this.intent = intent;
            this.realActivity = new ComponentName(info.packageName, info.name);
            this.launchedFromPackage = launchedFromPackage;
            this.resultTo = resultTo;
            this.resultWho = resultWho;
            this.requestCode = requestCode;
        }

        @Override
        public String toString() {
            return "ActivityRecord{" + realActivity.flattenToShortString() + " " + state
                    + (finishing ? " finishing" : "") + "}";
        }
    }

    /** A service of the app (ServiceRecord, reduced): the binder is its token. */
    static final class ServiceRecord extends Binder {
        final ServiceInfo info;
        final ComponentName name;
        final ProcessRecord app;
        boolean created;
        boolean startRequested;
        int lastStartId;
        final List<Intent> pendingStarts = new ArrayList<Intent>();
        /** Intent.filterEquals key -> binding. */
        final Map<String, IntentBinding> bindings = new HashMap<String, IntentBinding>();

        ServiceRecord(ServiceInfo info, ProcessRecord app) {
            this.info = info;
            this.name = new ComponentName(info.packageName, info.name);
            this.app = app;
        }

        @Override
        public String toString() {
            return "ServiceRecord{" + name.flattenToShortString() + "}";
        }
    }

    /** One Intent a service is bound with (IntentBindRecord) and its clients. */
    static final class IntentBinding {
        final Intent intent;
        IBinder binder;
        boolean requested;
        boolean received;
        final List<IServiceConnection> connections = new ArrayList<IServiceConnection>();

        IntentBinding(Intent intent) {
            this.intent = intent;
        }
    }

    /** A dynamically registered receiver (ReceiverList + BroadcastFilter). */
    static final class Receiver {
        final IIntentReceiver receiver;
        final IntentFilter filter;
        final IApplicationThread caller;

        Receiver(IIntentReceiver receiver, IntentFilter filter, IApplicationThread caller) {
            this.receiver = receiver;
            this.filter = filter;
            this.caller = caller;
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
    private final Map<ComponentName, ServiceRecord> mServices =
            new HashMap<ComponentName, ServiceRecord>();
    private final Map<IBinder, List<ServiceRecord>> mServiceConnections =
            new HashMap<IBinder, List<ServiceRecord>>();
    private final List<Receiver> mReceivers = new ArrayList<Receiver>();
    private final Map<String, Intent> mStickyBroadcasts = new HashMap<String, Intent>();
    private ActivityRecord mResumedActivity;
    private ActivityRecord mPausingActivity;
    private int mNextTaskId = 1;
    private boolean mAppProcessStarted;
    /** Lifecycle requests sent to the app, "STATE activity", for tests and dumps. */
    private final List<String> mHistory = new ArrayList<String>();
    /**
     * The VM's one app process, and the other process names it hosts (topic
     * userspace): every package runs here, as packages sharing a process do.
     */
    private ProcessRecord mVmProcess;
    private final Map<String, ProcessRecord> mHosted = new HashMap<String, ProcessRecord>();

    ActivityManagerService(IPackageManager pm) {
        mPackageManager = pm;
        computeConfiguration();
        // BatteryService's sticky broadcast: on mains, full.
        Intent battery = new Intent(Intent.ACTION_BATTERY_CHANGED);
        battery.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY
                | Intent.FLAG_RECEIVER_REPLACE_PENDING);
        battery.putExtra(BatteryManager.EXTRA_STATUS, BatteryManager.BATTERY_STATUS_FULL);
        battery.putExtra(BatteryManager.EXTRA_HEALTH, BatteryManager.BATTERY_HEALTH_GOOD);
        battery.putExtra(BatteryManager.EXTRA_PRESENT, true);
        battery.putExtra(BatteryManager.EXTRA_LEVEL, 100);
        battery.putExtra(BatteryManager.EXTRA_SCALE, 100);
        battery.putExtra(BatteryManager.EXTRA_PLUGGED, BatteryManager.BATTERY_PLUGGED_AC);
        battery.putExtra(BatteryManager.EXTRA_VOLTAGE, 5000);
        battery.putExtra(BatteryManager.EXTRA_TEMPERATURE, 250);
        battery.putExtra(BatteryManager.EXTRA_TECHNOLOGY, "Li-ion");
        mStickyBroadcasts.put(Intent.ACTION_BATTERY_CHANGED, battery);
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
     * (FallbackHome/CryptKeeper: home screens only while the device boots or
     * is being decrypted).
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
                for (ResolveInfo ri : mPackageManager.queryIntentActivities(intent, null, 0, 0)) {
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
        startActivityAsUser(null, "android", intent, null, null, null, 0, 0, null, null, 0);
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
        StatusBarServices.StatusBarManagerService bar = SystemServer.statusBar();
        if (bar != null && bar.hasBar()) {
            bar.toggleRecentApps();
        } else {
            startHome();
        }
    }

    /** ACTION_CLOSE_SYSTEM_DIALOGS to the registered receivers (shade, recents, dialogs). */
    @Override
    public void closeSystemDialogs(String reason) {
        Intent intent = new Intent(Intent.ACTION_CLOSE_SYSTEM_DIALOGS);
        intent.addFlags(Intent.FLAG_RECEIVER_REGISTERED_ONLY);
        if (reason != null) {
            intent.putExtra("reason", reason);
        }
        broadcastIntent(null, intent, null, null, 0, null, null, null, 0, null, false, false, 0);
    }

    /** Recents' swipe-away: the task's activities finish. */
    @Override
    public boolean removeTask(int taskId) {
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

    // --- what SystemUI registers or asks (topic userspace): one user, one stack,
    // no lock task mode, no keyguard occlusion to track ---------------------------

    @Override public void registerUserSwitchObserver(android.app.IUserSwitchObserver o) {}
    @Override public void unregisterUserSwitchObserver(android.app.IUserSwitchObserver o) {}
    @Override public void registerProcessObserver(android.app.IProcessObserver o) {}
    @Override public void unregisterProcessObserver(android.app.IProcessObserver o) {}
    @Override public void registerTaskStackListener(android.app.ITaskStackListener l) {}
    @Override public int getFocusedStackId() { return 0; }

    /** The one stack (id 0), the whole display, its tasks bottom to top. */
    @Override
    public ActivityManager.StackInfo getStackInfo(int stackId) {
        if (stackId != 0) {
            return null;
        }
        ActivityManager.StackInfo info = new ActivityManager.StackInfo();
        info.stackId = 0;
        info.displayId = Display.DEFAULT_DISPLAY;
        try {
            DisplayInfo di = IDisplayManager.Stub.asInterface(ServiceManager.getService("display"))
                    .getDisplayInfo(Display.DEFAULT_DISPLAY);
            if (di != null) {
                info.bounds.set(0, 0, di.logicalWidth, di.logicalHeight);
            }
        } catch (RemoteException e) {
            // In process: not thrown.
        }
        synchronized (this) {
            info.taskIds = new int[mTasks.size()];
            info.taskNames = new String[mTasks.size()];
            for (int i = 0; i < mTasks.size(); i++) {
                TaskRecord t = mTasks.get(i);
                info.taskIds[i] = t.taskId;
                info.taskNames[i] = t.activities.isEmpty() ? ""
                        : t.activities.get(0).realActivity.flattenToString();
            }
        }
        return info;
    }

    @Override
    public List<ActivityManager.StackInfo> getAllStackInfos() {
        List<ActivityManager.StackInfo> list = new ArrayList<ActivityManager.StackInfo>();
        list.add(getStackInfo(0));
        return list;
    }
    @Override public boolean isInLockTaskMode() { return false; }
    @Override public int getLockTaskModeState() { return ActivityManager.LOCK_TASK_MODE_NONE; }
    @Override public void updateLockTaskPackages(int userId, String[] packages) {}
    @Override public void setLockScreenShown(boolean shown) {}
    @Override public void keyguardWaitingForActivityDrawn() {}
    @Override public void keyguardGoingAway(boolean disableWindowAnimations,
            boolean keyguardGoingToNotificationShade) {}

    /** Activity.cancelInputsAndStartExitTransition asks before starting another one. */
    @Override
    public boolean isTopOfTask(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            return r != null && r.task != null && r.task.topRunning() == r;
        }
    }

    @Override
    public ActivityManager.TaskThumbnail getTaskThumbnail(int taskId) {
        return new ActivityManager.TaskThumbnail();
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

    /** The app's process, once it is bound (one per VM). */
    private ProcessRecord runningProcessLocked(String processName) {
        ProcessRecord app = mProcesses.get(processName);
        return app != null && app.thread != null && !app.dead ? app : null;
    }

    // --- starting activities ---------------------------------------------------

    @Override
    public int startActivity(IApplicationThread caller, String callingPackage, Intent intent,
            String resolvedType, IBinder resultTo, String resultWho, int requestCode,
            int startFlags, ProfilerInfo profilerInfo, Bundle options) {
        return startActivityAsUser(caller, callingPackage, intent, resolvedType, resultTo,
                resultWho, requestCode, startFlags, profilerInfo, options, 0);
    }

    @Override
    public int startActivityAsUser(IApplicationThread caller, String callingPackage,
            Intent intent, String resolvedType, IBinder resultTo, String resultWho,
            int requestCode, int startFlags, ProfilerInfo profilerInfo, Bundle options,
            int userId) {
        if (PackageManager.ACTION_REQUEST_PERMISSIONS.equals(intent.getAction())) {
            return requestPermissions(intent, resultTo, resultWho, requestCode);
        }
        // ActivityStackSupervisor.resolveActivity.
        intent = new Intent(intent);
        // A share sheet (ACTION_CHOOSER): the framework's ChooserActivity
        // would need a process of the system's; the page shares instead
        // (topic app-files, PageFiles).
        if (Intent.ACTION_CHOOSER.equals(intent.getAction())
                && PageFiles.start(intent, resolvedType, resultTo(resultTo, resultWho, requestCode))) {
            return ActivityManager.START_SUCCESS;
        }
        ActivityInfo aInfo;
        try {
            if (intent.getComponent() != null) {
                aInfo = mPackageManager.getActivityInfo(intent.getComponent(), STOCK_PM_FLAGS, userId);
            } else {
                ResolveInfo ri = mPackageManager.resolveIntent(intent,
                        resolvedType, PackageManager.MATCH_DEFAULT_ONLY | STOCK_PM_FLAGS, userId);
                aInfo = ri == null ? null : ri.activityInfo;
            }
        } catch (RemoteException e) {
            aInfo = null;
        }
        // Play Store links and Google's activities (topic gms): a store link
        // opens the store's web page; the user's Google packages' activities
        // are left at once.
        if (Gms.isGoogleActivity(intent, aInfo == null ? null : aInfo.packageName)) {
            int res = startGoogleActivity(intent, aInfo, resultTo, resultWho, requestCode);
            if (res != ActivityManager.START_INTENT_NOT_RESOLVED) {
                return res;
            }
        }
        if (aInfo == null) {
            Slog.w(TAG, "Unable to resolve " + intent);
            // Files to open, save or share: the page is the phone's side
            // (topic app-files, PageFiles).
            if (PageFiles.start(intent, resolvedType, resultTo(resultTo, resultWho, requestCode))) {
                return ActivityManager.START_SUCCESS;
            }
            if (Unimplemented.lenient("ActivityManager.startActivity(" + intent.getAction() + ")")) {
                // No app here handles it (a ringtone picker, say): the caller
                // goes on as if the user backed out of it, instead of dying
                // of ActivityNotFoundException with every app in this process.
                if (requestCode >= 0) {
                    synchronized (this) {
                        ActivityRecord source = forToken(resultTo);
                        if (source != null) {
                            sendResultLocked(source, resultWho, requestCode,
                                    android.app.Activity.RESULT_CANCELED, null);
                        }
                    }
                }
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
            ActivityRecord resultRecord = requestCode >= 0 ? source : null;
            ComponentName component = intent.getComponent();
            // A result goes back within the task (with NEW_TASK and a result,
            // Android cancels the result instead; here the task is kept).
            boolean newTask = source == null
                    || ((intent.getFlags() & Intent.FLAG_ACTIVITY_NEW_TASK) != 0
                        && resultRecord == null);
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
                            // singleTask (topic userspace): the root is the target,
                            // cleared to the top, and gets the intent (Launcher3: HOME
                            // shows the default page).
                            for (int i = t.activities.size() - 1; i > 0; i--) {
                                ActivityRecord above = t.activities.get(i);
                                if (!above.finishing && above.app != null
                                        && above.app.thread != null) {
                                    above.finishing = true;
                                    destroyActivityLocked(above);  // removed when destroyed
                                }
                            }
                            try {
                                List<com.android.internal.content.ReferrerIntent> intents =
                                        new ArrayList<com.android.internal.content.ReferrerIntent>();
                                intents.add(new com.android.internal.content.ReferrerIntent(
                                        new Intent(intent), callingPackage));
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
            ActivityRecord r = new ActivityRecord(aInfo, intent, callingPackage, resultRecord,
                    resultWho, requestCode);
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
            Slog.i(TAG, "START " + intent + " in task " + task.taskId
                    + (resultRecord != null ? " for a result (" + requestCode + ")" : ""));
            IWindowManager wm = windowManager();
            try {
                wm.prepareAppTransition(newTask ? 8 /* TRANSIT_TASK_OPEN */
                        : 6 /* TRANSIT_ACTIVITY_OPEN */, false);
                wm.addAppToken(task.activities.indexOf(r), r.appToken, task.taskId,
                        0 /* the one stack */, aInfo.screenOrientation, true,
                        (aInfo.flags & ActivityInfo.FLAG_SHOW_FOR_ALL_USERS) != 0, userId,
                        aInfo.configChanges, false, false);
            } catch (RemoteException e) {
                // In process: not thrown.
            }
            resumeTopActivityLocked();
            return ActivityManager.START_SUCCESS;
        }
    }

    /**
     * Activity.requestPermissions starts the permission controller's
     * GrantPermissionsActivity for a result. There is none: the result comes
     * back at once, each permission granted if the package manager granted
     * it (it grants every requested one at install), as if the user had
     * tapped "Allow" for each.
     */
    /**
     * An activity of Google's packages, or a market:// link (topic gms,
     * arcwasm.server.Gms): a Play Store page opens in the browser
     * (Senses.openUrl); an activity of the user's Google packages (they have
     * no process of their own here) is left at once -- a result asked for comes
     * back cancelled (sign-in: "cancelled by the user").
     * START_INTENT_NOT_RESOLVED: neither (no such activity: no Google
     * packages, as on a phone without Google Play).
     */
    private int startGoogleActivity(Intent intent, ActivityInfo aInfo, IBinder resultTo,
            String resultWho, int requestCode) {
        Intent page = Gms.webPage(intent);
        if (page != null && Senses.openUrl(page)) {
            Slog.i(TAG, "Play Store link " + intent + ": the store's page in the browser");
            if (requestCode >= 0) {
                synchronized (this) {
                    ActivityRecord source = forToken(resultTo);
                    if (source != null) {
                        sendResultLocked(source, resultWho, requestCode,
                                android.app.Activity.RESULT_CANCELED, null);
                    }
                }
            }
            return ActivityManager.START_SUCCESS;
        }
        if (aInfo == null) {
            return ActivityManager.START_INTENT_NOT_RESOLVED;
        }
        Slog.i(TAG, "Google's activity " + intent + ": left at once"
                + (requestCode >= 0 ? " (result cancelled)" : ""));
        if (requestCode >= 0) {
            synchronized (this) {
                ActivityRecord source = forToken(resultTo);
                if (source != null) {
                    sendResultLocked(source, resultWho, requestCode,
                            android.app.Activity.RESULT_CANCELED, null);
                }
            }
        }
        return ActivityManager.START_SUCCESS;
    }

    private int requestPermissions(Intent intent, IBinder resultTo, String resultWho,
            int requestCode) {
        String[] names = intent.getStringArrayExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES);
        if (names == null) {
            names = new String[0];
        }
        synchronized (this) {
            ActivityRecord r = forToken(resultTo);
            if (r == null) {
                return ActivityManager.START_INTENT_NOT_RESOLVED;
            }
            int[] results = new int[names.length];
            for (int i = 0; i < names.length; i++) {
                try {
                    results[i] = mPackageManager.checkPermission(names[i], r.info.packageName, 0);
                } catch (RemoteException e) {
                    results[i] = PackageManager.PERMISSION_DENIED;
                }
            }
            Intent data = new Intent(PackageManager.ACTION_REQUEST_PERMISSIONS);
            data.putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_NAMES, names);
            data.putExtra(PackageManager.EXTRA_REQUEST_PERMISSIONS_RESULTS, results);
            Slog.i(TAG, "permissions requested by " + r + ": " + java.util.Arrays.toString(names)
                    + " -> " + java.util.Arrays.toString(results));
            sendResultLocked(r, resultWho, requestCode, android.app.Activity.RESULT_OK, data);
            return ActivityManager.START_SUCCESS;
        }
    }

    /** ActivityStack.sendActivityResultLocked: now if it is running, else when it resumes. */
    private void sendResultLocked(ActivityRecord r, String resultWho, int requestCode,
            int resultCode, Intent data) {
        ResultInfo result = new ResultInfo(resultWho, requestCode, resultCode, data);
        if (r == mResumedActivity && r.app != null && r.app.thread != null) {
            List<ResultInfo> list = new ArrayList<ResultInfo>();
            list.add(result);
            try {
                r.app.thread.scheduleSendResult(r.appToken, list);
                return;
            } catch (RemoteException e) {
                Slog.w(TAG, "send result to " + r, e);
            }
        }
        r.results.add(result);
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
            // Already launched (paused or stopped): its results, then resume it.
            history("resume", next);
            next.state = State.RESUMED;
            next.idle = false;
            mResumedActivity = next;
            try {
                if (!next.results.isEmpty()) {
                    next.app.thread.scheduleSendResult(next.appToken,
                            new ArrayList<ResultInfo>(next.results));
                    next.results.clear();
                }
                next.app.thread.scheduleResumeActivity(next.appToken,
                        ActivityManager.PROCESS_STATE_TOP, true, null);
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
        ProcessRecord app = hostLocked(processName);
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
     * The process for processName: its own, or the VM's one app process, which
     * hosts every other package (topic userspace). Null if none runs yet.
     */
    private ProcessRecord hostLocked(String processName) {
        ProcessRecord app = mProcesses.get(processName);
        if (app == null) {
            app = mHosted.get(processName);
        }
        if (app == null && mVmProcess != null && !mVmProcess.dead) {
            app = mVmProcess;
            mHosted.put(processName, app);
            Slog.i(TAG, "hosting " + processName + " in process " + app.processName);
        }
        return app;
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
            android.content.pm.ParceledListSlice<ProviderInfo> slice = mPackageManager
                    .queryContentProviders(info.processName, info.applicationInfo.uid,
                            STOCK_PM_FLAGS);
            if (slice == null) {
                return;
            }
            for (ProviderInfo pi : slice.getList()) {
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
        List<ResultInfo> results = r.results.isEmpty() ? null : new ArrayList<ResultInfo>(r.results);
        r.results.clear();
        try {
            app.thread.scheduleLaunchActivity(new Intent(r.intent), r.appToken,
                    System.identityHashCode(r), r.info, new Configuration(mConfiguration),
                    null /* overrideConfig */, CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                    r.launchedFromPackage, null /* voiceInteractor */,
                    ActivityManager.PROCESS_STATE_TOP, r.icicle, r.persistentState, results,
                    null, false /* notResumed */, true /* isForward */, null);
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
            Slog.e(TAG, "one app process per VM: not starting " + processName);
            app.dead = true;
            return app;
        }
        mAppProcessStarted = true;
        mVmProcess = app;
        if (SystemServer.packageManager() != null) {
            SystemServer.packageManager().setAppProcessPackage(info.packageName);
        }
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
                    // Line by line: one log entry holds ~1000 characters, and
                    // an app's "Caused by" is usually deep in the trace.
                    Slog.e(TAG, "process " + app.processName + " died");
                    for (String line : android.util.Log.getStackTraceString(t).split("\n")) {
                        Slog.e(TAG, line);
                    }
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
     * activities go (callers get RESULT_CANCELED), the top activity left, or
     * the home screen, resumes. False when the process is to die instead.
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
                    if (r.resultTo != null && r.resultTo.state != State.DESTROYED
                            && !pkg.equals(r.resultTo.info.packageName)) {
                        sendResultLocked(r.resultTo, r.resultWho, r.requestCode,
                                android.app.Activity.RESULT_CANCELED, null);
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
            for (Iterator<ServiceRecord> it = mServices.values().iterator(); it.hasNext();) {
                if (it.next().app == app) {
                    it.remove();
                }
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
            // ActivityManagerService.attachApplicationLocked: the services the
            // app caches (getCommonServicesLocked) and its content providers.
            Map<String, IBinder> services = new HashMap<String, IBinder>();
            for (String name : new String[] {"package", "window", "alarm"}) {
                IBinder b = ServiceManager.getService(name);
                if (b != null) {
                    services.put(name, b);
                }
            }
            List<ProviderInfo> providers = generateApplicationProvidersLocked(app);
            try {
                app.thread.bindApplication(app.processName, app.info,
                        providers.isEmpty() ? null : providers, null, null, null, null, null,
                        0, false, false, false, new Configuration(mConfiguration),
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
            for (ServiceRecord s : new ArrayList<ServiceRecord>(app.pendingServices)) {
                bringUpServiceLocked(s);
            }
            app.pendingServices.clear();
        }
    }

    private List<ProviderInfo> generateApplicationProvidersLocked(ProcessRecord app) {
        List<ProviderInfo> out = new ArrayList<ProviderInfo>();
        try {
            android.content.pm.ParceledListSlice<ProviderInfo> slice = mPackageManager
                    .queryContentProviders(app.processName, app.info.uid, STOCK_PM_FLAGS);
            if (slice != null) {
                out.addAll(slice.getList());
            }
        } catch (RemoteException e) {
            // In process: not thrown.
        }
        return out;
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
                    r.info.configChanges, false);
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
    public void activityStopped(IBinder token, Bundle icicle, PersistableBundle persistentState,
            CharSequence description) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r == null) {
                return;
            }
            r.icicle = icicle;
            r.persistentState = persistentState;
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
    public void activitySlept(IBinder token) {}

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
    public boolean finishActivity(IBinder token, int resultCode, Intent resultData,
            boolean finishTask) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r == null) {
                return false;
            }
            if (finishTask && r.task.activities.indexOf(r) == 0) {
                boolean any = false;
                for (ActivityRecord a : new ArrayList<ActivityRecord>(r.task.activities)) {
                    any |= finishActivityLocked(a, a == r ? resultCode : 0,
                            a == r ? resultData : null);
                }
                return any;
            }
            return finishActivityLocked(r, resultCode, resultData);
        }
    }

    /** ActivityStack.finishActivityLocked (with finishActivityResultsLocked). */
    private boolean finishActivityLocked(ActivityRecord r, int resultCode, Intent resultData) {
        if (r.finishing) {
            return false;
        }
        Slog.i(TAG, "finishing " + r);
        r.finishing = true;
        if (r.resultTo != null && r.resultTo.state != State.DESTROYED) {
            sendResultLocked(r.resultTo, r.resultWho, r.requestCode, resultCode, resultData);
        }
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

    /** For tests: as the 4.4 build's IActivityManager.moveTaskToBack(taskId). */
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
    public boolean isInHomeStack(int taskId) {
        return false;
    }

    @Override
    public String getCallingPackage(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            return r == null || r.resultTo == null ? null : r.resultTo.info.packageName;
        }
    }

    @Override
    public ComponentName getCallingActivity(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            return r == null || r.resultTo == null ? null : r.resultTo.realActivity;
        }
    }

    @Override
    public String getLaunchedFromPackage(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            return r == null ? null : r.launchedFromPackage;
        }
    }

    @Override
    public int getLaunchedFromUid(IBinder token) {
        return Process.myUid();
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
     * The display turns for |r| if it wants another orientation (topic
     * rotation, as the 4.4 build's): a landscape activity on the portrait
     * display (a game; home again after it), and back for the next one. Then
     * the configuration follows, the app hears of it
     * (scheduleConfigurationChanged) and the windows lay out on the turned
     * display. ActivityStack.ensureActivityConfigurationLocked would relaunch
     * activities that do not handle the change; here they get the new
     * configuration only.
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
        for (ProcessRecord app : mProcesses.values()) {
            if (app.thread != null && !app.dead) {
                try {
                    app.thread.scheduleConfigurationChanged(config);
                } catch (RemoteException e) {
                    // In process: not thrown.
                }
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
            int exitAnim) {}

    @Override
    public void reportActivityFullyDrawn(IBinder token) {}

    @Override
    public void notifyActivityDrawn(IBinder token) {}

    @Override
    public void setTaskDescription(IBinder token, ActivityManager.TaskDescription values) {}

    @Override
    public int getActivityDisplayId(IBinder activityToken) {
        return Display.DEFAULT_DISPLAY;
    }

    @Override
    public android.app.ActivityOptions getActivityOptions(IBinder token) {
        return null;
    }

    @Override
    public boolean convertFromTranslucent(IBinder token) {
        return false;
    }

    @Override
    public boolean convertToTranslucent(IBinder token, android.app.ActivityOptions options) {
        return false;
    }

    @Override
    public boolean requestVisibleBehind(IBinder token, boolean visible) {
        return false;
    }

    @Override
    public boolean isBackgroundVisibleBehind(IBinder token) {
        return false;
    }

    @Override
    public void backgroundResourcesReleased(IBinder token) {}

    @Override
    public void releaseSomeActivities(IApplicationThread app) {}

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

    @Override
    public boolean shouldUpRecreateTask(IBinder token, String destAffinity) {
        return false;
    }

    @Override
    public boolean navigateUpTo(IBinder token, Intent destIntent, int resultCode,
            Intent resultData) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r == null) {
                return false;
            }
            // ActivityStack.navigateUpToLocked, for a parent below in the same task.
            List<ActivityRecord> list = r.task.activities;
            int i = list.indexOf(r);
            ComponentName dest = destIntent.getComponent();
            int parent = -1;
            for (int j = i - 1; j >= 0; j--) {
                if (list.get(j).realActivity.equals(dest)) {
                    parent = j;
                    break;
                }
            }
            if (parent < 0) {
                return false;
            }
            for (int j = i; j > parent; j--) {
                finishActivityLocked(list.get(j), j == i ? resultCode : 0,
                        j == i ? resultData : null);
            }
            return true;
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
    public ConfigurationInfo getDeviceConfigurationInfo() {
        ConfigurationInfo config = new ConfigurationInfo();
        synchronized (this) {
            config.reqTouchScreen = mConfiguration.touchscreen;
            config.reqKeyboardType = mConfiguration.keyboard;
            config.reqNavigation = mConfiguration.navigation;
            config.reqInputFeatures |= ConfigurationInfo.INPUT_FEATURE_HARD_KEYBOARD;
        }
        config.reqGlEsVersion = 0x20000;  // GLES 2.0 over WebGL2 (graphics_translation)
        return config;
    }

    @Override
    public List<ActivityManager.RunningTaskInfo> getTasks(int maxNum, int flags) {
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
    public List<ActivityManager.RecentTaskInfo> getRecentTasks(int maxNum, int flags, int userId) {
        synchronized (this) {
            List<ActivityManager.RecentTaskInfo> list = new ArrayList<ActivityManager.RecentTaskInfo>();
            for (int i = mTasks.size() - 1; i >= 0 && list.size() < maxNum; i--) {
                TaskRecord t = mTasks.get(i);
                ActivityManager.RecentTaskInfo ti = new ActivityManager.RecentTaskInfo();
                ti.id = ti.persistentId = t.taskId;
                ti.baseIntent = new Intent(t.intent);
                ti.origActivity = t.activities.get(0).realActivity;
                ti.baseActivity = ti.origActivity;
                ActivityRecord top = t.topRunning();
                ti.topActivity = top != null ? top.realActivity : ti.origActivity;
                ti.numActivities = t.activities.size();
                ti.userId = 0;
                // What SystemUI's recents read (topic userspace): no label/icon
                // overrides, the task its own affiliate.
                ti.taskDescription = new ActivityManager.TaskDescription();
                ti.affiliatedTaskId = t.taskId;
                ti.stackId = 0;
                ti.firstActiveTime = ti.lastActiveTime = SystemClock.elapsedRealtime();
                list.add(ti);
            }
            return list;
        }
    }

    @Override
    public List<android.app.IAppTask> getAppTasks(String callingPackage) {
        return new ArrayList<android.app.IAppTask>();
    }

    @Override
    public Point getAppTaskThumbnailSize() {
        return new Point(192, 108);
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
                        app.processName, Process.myPid(), new String[] {app.info.packageName});
                pi.uid = app.info.uid;
                boolean top = mResumedActivity != null && mResumedActivity.app == app;
                pi.importance = top ? ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND
                        : ActivityManager.RunningAppProcessInfo.IMPORTANCE_BACKGROUND;
                pi.processState = top ? ActivityManager.PROCESS_STATE_TOP
                        : ActivityManager.PROCESS_STATE_CACHED_ACTIVITY;
                list.add(pi);
            }
            return list;
        }
    }

    @Override
    public void getMyMemoryState(ActivityManager.RunningAppProcessInfo outInfo) {
        synchronized (this) {
            boolean top = mResumedActivity != null;
            outInfo.pid = Process.myPid();
            outInfo.uid = Process.myUid();
            outInfo.importance = top ? ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND
                    : ActivityManager.RunningAppProcessInfo.IMPORTANCE_BACKGROUND;
            outInfo.processState = top ? ActivityManager.PROCESS_STATE_TOP
                    : ActivityManager.PROCESS_STATE_CACHED_ACTIVITY;
            outInfo.lastTrimLevel = 0;
        }
    }

    @Override
    public void getMemoryInfo(ActivityManager.MemoryInfo outInfo) {
        // wasm32: 4 GB of address space for everything.
        Runtime rt = Runtime.getRuntime();
        outInfo.totalMem = 2048L << 20;
        outInfo.availMem = Math.max(outInfo.totalMem - rt.totalMemory() + rt.freeMemory(), 0);
        outInfo.threshold = 64L << 20;
        outInfo.lowMemory = false;
    }

    @Override
    public Debug.MemoryInfo[] getProcessMemoryInfo(int[] pids) {
        Debug.MemoryInfo[] out = new Debug.MemoryInfo[pids.length];
        for (int i = 0; i < pids.length; i++) {
            out[i] = new Debug.MemoryInfo();
        }
        return out;
    }

    @Override
    public int getProcessLimit() {
        return 1;
    }

    @Override
    public int getPackageProcessState(String packageName, String callingPackage) {
        synchronized (this) {
            for (ProcessRecord app : mProcesses.values()) {
                if (app.thread != null && app.info.packageName.equals(packageName)) {
                    return mResumedActivity != null && mResumedActivity.app == app
                            ? ActivityManager.PROCESS_STATE_TOP
                            : ActivityManager.PROCESS_STATE_CACHED_ACTIVITY;
                }
            }
        }
        return ActivityManager.PROCESS_STATE_NONEXISTENT;
    }

    @Override
    public boolean isUserAMonkey() {
        return false;
    }

    @Override
    public int getFrontActivityScreenCompatMode() {
        return ActivityManager.COMPAT_MODE_NEVER;
    }

    // --- users: the one owner ---------------------------------------------------------

    @Override
    public UserInfo getCurrentUser() {
        return new UserInfo(UserHandle.USER_OWNER, "Owner",
                UserInfo.FLAG_ADMIN | UserInfo.FLAG_PRIMARY | UserInfo.FLAG_INITIALIZED);
    }

    @Override
    public boolean isUserRunning(int userid, boolean orStopping) {
        return userid == UserHandle.USER_OWNER;
    }

    @Override
    public int[] getRunningUserIds() {
        return new int[] {UserHandle.USER_OWNER};
    }

    // --- permissions: the package manager's grants ------------------------------------

    @Override
    public int checkPermission(String permission, int pid, int uid) {
        // ActivityManagerService.checkComponentPermission: root and system
        // have everything; the rest is the package manager's grant. The
        // whole VM runs as one uid, so the app's own checks land here as root.
        if (uid == 0 || uid == Process.SYSTEM_UID || uid == Process.myUid()) {
            return PackageManager.PERMISSION_GRANTED;
        }
        try {
            return mPackageManager.checkUidPermission(permission, uid);
        } catch (RemoteException e) {
            return PackageManager.PERMISSION_DENIED;
        }
    }

    @Override
    public int checkUriPermission(android.net.Uri uri, int pid, int uid, int mode, int userId,
            IBinder callerToken) {
        return PackageManager.PERMISSION_GRANTED;
    }

    @Override
    public void grantUriPermission(IApplicationThread caller, String targetPkg,
            android.net.Uri uri, int mode, int userId) {}

    @Override
    public android.content.pm.ParceledListSlice<android.content.UriPermission>
            getPersistedUriPermissions(String packageName, boolean incoming) {
        return new android.content.pm.ParceledListSlice<android.content.UriPermission>(
                new ArrayList<android.content.UriPermission>());
    }

    @Override
    public void addPackageDependency(String packageName) {}

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
    public boolean handleApplicationWtf(IBinder app, String tag, boolean system,
            android.app.ApplicationErrorReport.CrashInfo crashInfo) {
        Slog.e(TAG, "wtf from app, " + tag + ": " + crashInfo.exceptionMessage);
        return false;
    }

    @Override
    public void unhandledBack() {}

    // --- broadcasts: to receivers registered by the app ---------------------------------

    @Override
    public Intent registerReceiver(IApplicationThread caller, String callerPackage,
            IIntentReceiver receiver, IntentFilter filter, String permission, int userId) {
        synchronized (this) {
            // The sticky broadcast that matches, if any (ActivityManagerService
            // returns the first and delivers them all; here there is one).
            Intent sticky = null;
            for (Intent i : mStickyBroadcasts.values()) {
                if (filter.match(null, i, false, TAG) >= 0) {
                    sticky = i;
                    break;
                }
            }
            if (receiver != null) {
                mReceivers.add(new Receiver(receiver, filter, caller));
            }
            return sticky != null ? new Intent(sticky) : null;
        }
    }

    @Override
    public void unregisterReceiver(IIntentReceiver receiver) {
        synchronized (this) {
            for (Iterator<Receiver> it = mReceivers.iterator(); it.hasNext();) {
                if (it.next().receiver.asBinder() == receiver.asBinder()) {
                    it.remove();
                }
            }
        }
    }

    @Override
    public int broadcastIntent(IApplicationThread caller, Intent intent, String resolvedType,
            IIntentReceiver resultTo, int resultCode, String resultData, Bundle map,
            String[] requiredPermissions, int appOp, Bundle options, boolean serialized,
            boolean sticky, int userId) {
        List<Receiver> targets = new ArrayList<Receiver>();
        synchronized (this) {
            if (sticky) {
                mStickyBroadcasts.put(intent.getAction(), new Intent(intent));
            }
            for (Receiver r : mReceivers) {
                if (r.filter.match(null, intent, false, TAG) >= 0) {
                    targets.add(r);
                }
            }
        }
        // Delivered as registered receivers get them: oneway performReceive
        // on the receiver's binder (its LoadedApk.ReceiverDispatcher posts to
        // the app's main thread). Ordered broadcasts are delivered unordered.
        for (Receiver r : targets) {
            try {
                r.receiver.performReceive(new Intent(intent), resultCode, resultData, map,
                        false, sticky, 0);
            } catch (RemoteException e) {
                Slog.w(TAG, "broadcast to " + r.receiver, e);
            }
        }
        if (resultTo != null) {
            try {
                resultTo.performReceive(new Intent(intent), resultCode, resultData, map,
                        false, false, 0);
            } catch (RemoteException e) {
                Slog.w(TAG, "broadcast result", e);
            }
        }
        if (targets.isEmpty()) {
            Slog.i(TAG, "broadcast not delivered (no receivers): " + intent);
        }
        return ActivityManager.BROADCAST_SUCCESS;
    }

    @Override
    public void unbroadcastIntent(IApplicationThread caller, Intent intent, int userId) {
        synchronized (this) {
            mStickyBroadcasts.remove(intent.getAction());
        }
    }

    @Override
    public void finishReceiver(IBinder who, int resultCode, String resultData, Bundle map,
            boolean abortBroadcast, int flags) {}

    // --- PendingIntent: intent senders ---------------------------------------------------
    // What ActivityManagerService's PendingIntentRecord does, without users,
    // permissions or result receivers: one sender per (type, package,
    // requestCode, intent) key, as PendingIntent.equals() relies on.
    // send() starts the activity, delivers the broadcast or starts the service.

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
                IIntentReceiver finishedReceiver, String requiredPermission, Bundle options) {
            if (canceled) {
                return ActivityManager.START_CANCELED;
            }
            Intent intent = new Intent(intents[intents.length - 1]);
            if (fillIn != null) {
                intent.fillIn(fillIn, 0);
            }
            switch (type) {
                case ActivityManager.INTENT_SENDER_ACTIVITY:
                    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                    return startActivity(null, packageName, intent, resolvedType, null, null, -1,
                            0, null, options);
                case ActivityManager.INTENT_SENDER_BROADCAST:
                    return broadcastIntent(null, intent, resolvedType, finishedReceiver, code,
                            null, null, requiredPermission == null ? null
                                    : new String[] {requiredPermission},
                            0, options, finishedReceiver != null, false, 0);
                case ActivityManager.INTENT_SENDER_SERVICE:
                    startService(null, intent, resolvedType, packageName, 0);
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

    // --- services: those of the app, in its process ----------------------------------------
    // ActiveServices, reduced: a service of the running app's package (or one
    // whose app process this service starts) is created, started, bound and
    // destroyed in the app through IApplicationThread, as on a device.
    // Services of other packages are not run: startService answers null and
    // bindService 0, what Android returns for a service it cannot find (an
    // app without Google Play services gets that from GMS's).

    private ServiceRecord retrieveServiceLocked(Intent service, String resolvedType) {
        ResolveInfo ri;
        try {
            ri = mPackageManager.resolveService(service, resolvedType, STOCK_PM_FLAGS, 0);
        } catch (RemoteException e) {
            ri = null;
        }
        if (ri == null || ri.serviceInfo == null) {
            return null;
        }
        ServiceInfo si = ri.serviceInfo;
        if (Gms.isUserGoogle(si)) {
            return null;  // the user's Google package (topic gms): no process of its own here
        }
        ComponentName name = new ComponentName(si.packageName, si.name);
        ServiceRecord s = mServices.get(name);
        if (s != null) {
            return s;
        }
        ProcessRecord app = hostLocked(si.processName);
        if (app == null || app.dead) {
            // Only into the one app process (started for an activity, or now).
            if (mAppProcessStarted) {
                return null;
            }
            app = startProcessLocked(si.processName, si.applicationInfo);
        }
        s = new ServiceRecord(si, app);
        mServices.put(name, s);
        return s;
    }

    /** ActiveServices.bringUpServiceLocked: create, then pending starts and binds. */
    private void bringUpServiceLocked(ServiceRecord s) {
        if (s.app.thread == null) {
            if (!s.app.pendingServices.contains(s)) {
                s.app.pendingServices.add(s);
            }
            return;
        }
        try {
            if (!s.created) {
                installPackageLocked(s.app, s.info);
                Slog.i(TAG, "create service " + s);
                s.created = true;
                s.app.thread.scheduleCreateService(s, s.info,
                        CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                        ActivityManager.PROCESS_STATE_SERVICE);
            }
            for (Intent i : s.pendingStarts) {
                s.app.thread.scheduleServiceArgs(s, false, ++s.lastStartId, 0, i);
            }
            s.pendingStarts.clear();
            for (IntentBinding b : s.bindings.values()) {
                if (!b.requested && !b.connections.isEmpty()) {
                    b.requested = true;
                    s.app.thread.scheduleBindService(s, b.intent, false,
                            ActivityManager.PROCESS_STATE_SERVICE);
                }
            }
        } catch (RemoteException e) {
            Slog.w(TAG, "service " + s, e);
        }
    }

    @Override
    public ComponentName startService(IApplicationThread caller, Intent service,
            String resolvedType, String callingPackage, int userId) {
        synchronized (this) {
            ServiceRecord s = retrieveServiceLocked(service, resolvedType);
            if (s == null) {
                Slog.w(TAG, "Unable to start service " + service + ": not found");
                return null;
            }
            s.startRequested = true;
            s.pendingStarts.add(new Intent(service));
            bringUpServiceLocked(s);
            return s.name;
        }
    }

    @Override
    public int stopService(IApplicationThread caller, Intent service, String resolvedType,
            int userId) {
        synchronized (this) {
            ServiceRecord s = retrieveServiceLocked(service, resolvedType);
            if (s == null || !s.startRequested) {
                return 0;
            }
            s.startRequested = false;
            maybeStopServiceLocked(s);
            return 1;
        }
    }

    @Override
    public boolean stopServiceToken(ComponentName className, IBinder token, int startId) {
        synchronized (this) {
            ServiceRecord s = token instanceof ServiceRecord ? (ServiceRecord) token : null;
            if (s == null || (startId >= 0 && startId != s.lastStartId)) {
                return false;
            }
            s.startRequested = false;
            maybeStopServiceLocked(s);
            return true;
        }
    }

    /** ActiveServices.bringDownServiceIfNeededLocked. */
    private void maybeStopServiceLocked(ServiceRecord s) {
        if (s.startRequested) {
            return;
        }
        for (IntentBinding b : s.bindings.values()) {
            if (!b.connections.isEmpty()) {
                return;
            }
        }
        mServices.remove(s.name);
        if (s.created && s.app.thread != null) {
            Slog.i(TAG, "stop service " + s);
            try {
                s.app.thread.scheduleStopService(s);
            } catch (RemoteException e) {
                Slog.w(TAG, "stop " + s, e);
            }
        }
    }

    @Override
    public int bindService(IApplicationThread caller, IBinder token, Intent service,
            String resolvedType, IServiceConnection connection, int flags,
            String callingPackage, int userId) {
        synchronized (this) {
            ServiceRecord s = retrieveServiceLocked(service, resolvedType);
            if (s == null) {
                Slog.w(TAG, "Unable to bind service " + service + ": not found");
                return 0;
            }
            Intent.FilterComparison key = new Intent.FilterComparison(service);
            String k = key.getIntent().filterHashCode() + "|" + service.toUri(0);
            IntentBinding b = s.bindings.get(k);
            if (b == null) {
                b = new IntentBinding(new Intent(service));
                s.bindings.put(k, b);
            }
            b.connections.add(connection);
            List<ServiceRecord> list = mServiceConnections.get(connection.asBinder());
            if (list == null) {
                list = new ArrayList<ServiceRecord>();
                mServiceConnections.put(connection.asBinder(), list);
            }
            list.add(s);
            if (b.received) {
                try {
                    connection.connected(s.name, b.binder);
                } catch (RemoteException e) {
                    Slog.w(TAG, "connected " + s, e);
                }
            } else if ((flags & android.content.Context.BIND_AUTO_CREATE) != 0 || s.created) {
                bringUpServiceLocked(s);
            }
            return 1;
        }
    }

    @Override
    public void publishService(IBinder token, Intent intent, IBinder service) {
        synchronized (this) {
            ServiceRecord s = token instanceof ServiceRecord ? (ServiceRecord) token : null;
            if (s == null) {
                return;
            }
            Intent.FilterComparison fc = new Intent.FilterComparison(intent);
            for (IntentBinding b : s.bindings.values()) {
                if (new Intent.FilterComparison(b.intent).equals(fc)) {
                    b.binder = service;
                    b.received = true;
                    for (IServiceConnection c : b.connections) {
                        try {
                            c.connected(s.name, service);
                        } catch (RemoteException e) {
                            Slog.w(TAG, "connected " + s, e);
                        }
                    }
                }
            }
        }
    }

    @Override
    public boolean unbindService(IServiceConnection connection) {
        synchronized (this) {
            List<ServiceRecord> list = mServiceConnections.remove(connection.asBinder());
            if (list == null) {
                return false;
            }
            for (ServiceRecord s : list) {
                for (IntentBinding b : s.bindings.values()) {
                    for (Iterator<IServiceConnection> it = b.connections.iterator(); it.hasNext();) {
                        if (it.next().asBinder() == connection.asBinder()) {
                            it.remove();
                        }
                    }
                    if (b.connections.isEmpty() && b.requested && s.app.thread != null) {
                        b.requested = false;
                        b.received = false;
                        try {
                            s.app.thread.scheduleUnbindService(s, b.intent);
                        } catch (RemoteException e) {
                            Slog.w(TAG, "unbind " + s, e);
                        }
                    }
                }
                maybeStopServiceLocked(s);
            }
            return true;
        }
    }

    @Override
    public void unbindFinished(IBinder token, Intent intent, boolean doRebind) {}

    @Override
    public void serviceDoneExecuting(IBinder token, int type, int startId, int res) {}

    @Override
    public void setServiceForeground(ComponentName className, IBinder token, int id,
            android.app.Notification notification, boolean removeNotification) {}

    @Override
    public IBinder peekService(Intent service, String resolvedType, String callingPackage) {
        synchronized (this) {
            for (ServiceRecord s : mServices.values()) {
                if (service.getComponent() != null && service.getComponent().equals(s.name)) {
                    for (IntentBinding b : s.bindings.values()) {
                        if (b.received) {
                            return b.binder;
                        }
                    }
                }
            }
            return null;
        }
    }

    @Override
    public android.app.PendingIntent getRunningServiceControlPanel(ComponentName service) {
        return null;
    }

    // --- content providers: the system's "settings" and the app's own ---------------------

    @Override
    public IActivityManager.ContentProviderHolder getContentProvider(IApplicationThread caller,
            String name, int userId, boolean stable) {
        android.content.IContentProvider system = systemProvider(name);
        if (system != null) {
            ProviderInfo info = new ProviderInfo();
            info.authority = name;
            info.name = system.getClass().getName();
            info.packageName = "android";
            info.applicationInfo = new ApplicationInfo();
            info.applicationInfo.packageName = "android";
            info.applicationInfo.uid = Process.SYSTEM_UID;
            info.exported = true;
            IActivityManager.ContentProviderHolder holder =
                    new IActivityManager.ContentProviderHolder(info);
            holder.provider = system;
            holder.noReleaseNeeded = true;
            return holder;
        }
        synchronized (this) {
            for (ProcessRecord app : mProcesses.values()) {
                IActivityManager.ContentProviderHolder h = app.providers.get(name);
                if (h != null) {
                    IActivityManager.ContentProviderHolder holder =
                            new IActivityManager.ContentProviderHolder(h.info);
                    holder.provider = h.provider;
                    holder.noReleaseNeeded = true;
                    return holder;
                }
            }
        }
        Slog.w(TAG, "Failed to find provider info for " + name);
        return null;
    }

    @Override
    public void publishContentProviders(IApplicationThread caller,
            List<IActivityManager.ContentProviderHolder> providers) {
        synchronized (this) {
            ProcessRecord app = null;
            for (ProcessRecord p : mProcesses.values()) {
                if (p.thread != null && !p.dead) {
                    app = p;
                }
            }
            if (app == null || providers == null) {
                return;
            }
            for (IActivityManager.ContentProviderHolder h : providers) {
                if (h == null || h.info == null || h.info.authority == null) {
                    continue;
                }
                for (String authority : h.info.authority.split(";")) {
                    app.providers.put(authority, h);
                }
                Slog.i(TAG, "published provider " + h.info.authority);
            }
        }
    }

    @Override
    public void removeContentProvider(IBinder connection, boolean stable) {}

    @Override
    public boolean refContentProvider(IBinder connection, int stable, int unstable) {
        return true;
    }

    @Override
    public String getProviderMimeType(android.net.Uri uri, int userId) {
        IActivityManager.ContentProviderHolder h = getContentProvider(null, uri.getAuthority(),
                userId, false);
        if (h == null || h.provider == null) {
            return null;
        }
        try {
            return h.provider.getType(uri);
        } catch (RemoteException e) {
            return null;
        }
    }

    private final FilesProvider mFiles = new FilesProvider();

    /** Where PageFiles sends a start's result later; null if none was asked for. */
    private PageFiles.Result resultTo(final IBinder resultTo, final String resultWho,
            final int requestCode) {
        if (requestCode < 0) {
            return null;
        }
        return new PageFiles.Result() {
            @Override
            public void deliver(int resultCode, Intent data) {
                synchronized (ActivityManagerService.this) {
                    ActivityRecord source = forToken(resultTo);
                    if (source != null) {
                        sendResultLocked(source, resultWho, requestCode, resultCode, data);
                    }
                }
            }
        };
    }

    /** The providers of the system's own (on a device: system apps'). */
    private android.content.IContentProvider systemProvider(String authority) {
        if (SettingsProvider.AUTHORITY.equals(authority)) {
            return mSettings;
        }
        if (UserDictionaryProvider.AUTHORITY.equals(authority)) {
            return mUserDictionary;
        }
        if (FilesProvider.AUTHORITY.equals(authority)) {
            return mFiles;  // the page's files (topic app-files)
        }
        return null;
    }

    SettingsProvider settings() {
        return mSettings;
    }
}
