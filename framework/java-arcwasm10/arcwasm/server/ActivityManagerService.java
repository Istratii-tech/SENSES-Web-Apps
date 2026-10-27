// arc-wasm: the "activity" service of the in-process system server on
// Android 8.1, a lean IActivityManager (docs/topics/fw8-system.md): the
// 6.0.1 one (docs/topics/fw6-system.md) on 8.1's AIDL interfaces. The
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
// Processes (topic multiproc, docs/topics/multiproc.md): the app's main
// process is a thread of this VM's process, as above; a component of
// another android:process (":remote", isolatedProcess services) runs in a
// light process of the same package (arcwasm.server.LightProcess): its own
// pid, ActivityThread, Application and class loader, its own binder
// process. It is started when one of its components is needed (an
// activity, a service, a provider -- the caller waits for its publication
// --, a manifest receiver), found again by the calling pid of its
// attachApplication/publish calls, and when it dies (Process.killProcess,
// exit, a crash, killBackgroundProcesses) its activities, providers and
// receivers go away and its services restart in a new one if they were
// started sticky or are bound with BIND_AUTO_CREATE (clients see
// onServiceDisconnected, then onServiceConnected again), as
// ActiveServices does. With LightProcess off (arcwasm.multiproc=0, other
// VMs) every component runs in the main process.
//
// Not done: providers and services of other packages, launch modes and task
// affinities beyond "same task unless FLAG_ACTIVITY_NEW_TASK", timeouts and
// ANRs, a home screen, configuration changes, users.
package arcwasm.server;

import android.app.ActivityManager;
import android.app.ContentProviderHolder;
import android.app.IActivityManager;
import android.app.IApplicationThread;
import android.app.IServiceConnection;
import android.app.ProfilerInfo;
import android.app.ResultInfo;
import android.app.servertransaction.ActivityLifecycleItem;
import android.app.servertransaction.ActivityResultItem;
import android.app.servertransaction.ClientTransaction;
import android.app.servertransaction.ClientTransactionItem;
import android.app.servertransaction.DestroyActivityItem;
import android.app.servertransaction.LaunchActivityItem;
import android.app.servertransaction.PauseActivityItem;
import android.app.servertransaction.ResumeActivityItem;
import android.app.servertransaction.StopActivityItem;
import android.app.servertransaction.TopResumedActivityChangeItem;
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


        @Override
        public String getName() {
            return record.toString();
        }

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
        /** Its pid: the VM's for the main process, else its light process's. */
        int pid;
        /** A light process (topic multiproc): not the app's main process. */
        boolean light;
        /** An isolatedProcess service's process (its own uid). */
        boolean isolated;
        int uid;
        final List<ActivityRecord> activities = new ArrayList<ActivityRecord>();
        /** Its content providers, by authority, once the app published them. */
        final Map<String, ContentProviderHolder> providers =
                new HashMap<String, ContentProviderHolder>();
        /** Services to create once the app is bound. */
        final List<ServiceRecord> pendingServices = new ArrayList<ServiceRecord>();
        /** Broadcasts for manifest receivers, to deliver once the app is bound. */
        final List<Object[]> pendingReceivers = new ArrayList<Object[]>();

        ProcessRecord(String processName, ApplicationInfo info) {
            this.processName = processName;
            this.info = info;
        }

        @Override
        public String toString() {
            return "ProcessRecord{" + pid + ":" + processName + (thread == null ? " starting" : "")
                    + (dead ? " dead" : "") + "}";
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
        /** ActivityRecord.assistToken (API 29). */
        final IBinder assistToken = new Binder();
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
        /** Its process (a new one after the old one died). */
        ProcessRecord app;
        boolean created;
        /** A restart after its process died is pending. */
        boolean restarting;
        boolean startRequested;
        int lastStartId;
        /** Service.startForeground's type (API 29), FOREGROUND_SERVICE_TYPE_NONE if not. */
        int foregroundServiceType;
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
        /** Some connection has BIND_AUTO_CREATE: the service restarts with its process. */
        boolean autoCreate;
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
        /** The registering process (its receivers go when it dies). */
        final int pid;

        Receiver(IIntentReceiver receiver, IntentFilter filter, IApplicationThread caller, int pid) {
            this.receiver = receiver;
            this.filter = filter;
            this.caller = caller;
            this.pid = pid;
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
    private final Map<ComponentName, ServiceRecord> mServices =
            new HashMap<ComponentName, ServiceRecord>();
    private final Map<IBinder, List<ServiceRecord>> mServiceConnections =
            new HashMap<IBinder, List<ServiceRecord>>();
    private final List<Receiver> mReceivers = new ArrayList<Receiver>();
    private final Map<String, Intent> mStickyBroadcasts = new HashMap<String, Intent>();
    private ActivityRecord mResumedActivity;
    private ActivityRecord mPausingActivity;
    /** The activity told it is the top resumed one (API 29). */
    private ActivityRecord mTopResumed;
    private int mNextTaskId = 1;
    private boolean mAppProcessStarted;
    /** The package whose processes run in this VM (one app per VM). */
    private String mAppPackage;
    /** Lifecycle requests sent to the app, "STATE activity", for tests and dumps. */
    private final List<String> mHistory = new ArrayList<String>();

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
            // The window's bounds as the activity task manager gives them: a
            // full-screen app. androidx.window (Compose's LocalWindowInfo on
            // API 29) reads them by reflection; empty bounds gave Freebloks a
            // window 0 px wide and a RadialGradient of radius 0.
            mConfiguration.windowConfiguration.setWindowingMode(
                    android.app.WindowConfiguration.WINDOWING_MODE_FULLSCREEN);
            mConfiguration.windowConfiguration.setActivityType(
                    android.app.WindowConfiguration.ACTIVITY_TYPE_STANDARD);
            mConfiguration.windowConfiguration.setBounds(
                    new android.graphics.Rect(0, 0, di.logicalWidth, di.logicalHeight));
            mConfiguration.windowConfiguration.setAppBounds(0, 0, di.appWidth, di.appHeight);
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

    /** The window manager, called directly for app tokens (not in 8.1's IWindowManager). */
    private static WindowManagerService wms() {
        return SystemServer.windowManager();
    }

    private static IWindowManager windowManager() {
        return IWindowManager.Stub.asInterface(ServiceManager.getService("window"));
    }

    /** The app is called as from another process: through IApplicationThread.Stub.Proxy. */
    private static IApplicationThread proxyFor(IApplicationThread thread) {
        try {
            Class<?> c = Class.forName("android.app.IApplicationThread$Stub$Proxy");
            Constructor<?> ctor = c.getDeclaredConstructor(IBinder.class);
            ctor.setAccessible(true);
            return (IApplicationThread) ctor.newInstance(thread.asBinder());
        } catch (Exception e) {
            throw new IllegalStateException("ApplicationThreadProxy", e);
        }
    }

    /**
     * ClientLifecycleManager.scheduleTransaction: in 10 the activity
     * lifecycle reaches the app as one ClientTransaction (callbacks, then the
     * state to end in), parcelled through the IApplicationThread proxy.
     */
    private static void schedule(ActivityRecord r, ClientTransactionItem callback,
            ActivityLifecycleItem state) throws RemoteException {
        ClientTransaction t = ClientTransaction.obtain(r.app.thread, r.appToken);
        if (callback != null) {
            t.addCallback(callback);
        }
        if (state != null) {
            t.setLifecycleStateRequest(state);
        }
        t.schedule();
        t.recycle();
    }

    /**
     * An app's callback interface, called as from another process: its AIDL
     * Stub.Proxy (|proxyClass|) on its binder. A light process's objects
     * reach this service as objects when it is called directly; through
     * the proxy the call goes to that process's binder threads (Binder's
     * multiproc patch) instead of running on this service's thread.
     */
    static <T> T asProxy(Object stub, String proxyClass) {
        if (stub == null) {
            return null;
        }
        try {
            Class<?> c = Class.forName(proxyClass);
            Constructor<?> ctor = c.getDeclaredConstructor(IBinder.class);
            ctor.setAccessible(true);
            @SuppressWarnings("unchecked")
            T t = (T) ctor.newInstance(((android.os.IInterface) stub).asBinder());
            return t;
        } catch (Exception e) {
            throw new IllegalStateException(proxyClass, e);
        }
    }

    /** (Only once light processes exist: before, everything is one process, as it was.) */
    private static IServiceConnection proxyFor(IServiceConnection c) {
        return c == null || !Binder.sArcLight || !(c.asBinder() instanceof Binder) ? c
                : ActivityManagerService.<IServiceConnection>asProxy(c,
                        "android.app.IServiceConnection$Stub$Proxy");
    }

    private static IIntentReceiver proxyFor(IIntentReceiver r) {
        return r == null || !Binder.sArcLight || !(r.asBinder() instanceof Binder) ? r
                : ActivityManagerService.<IIntentReceiver>asProxy(r,
                        "android.content.IIntentReceiver$Stub$Proxy");
    }

    /**
     * A provider published by process |owner|, for a caller in another
     * process: ContentProviderProxy on its binder (see asProxy).
     */
    private static android.content.IContentProvider providerFor(
            android.content.IContentProvider provider, ProcessRecord owner) {
        if (provider == null || owner == null || !owner.light
                || owner.pid == Binder.getCallingPid() || !(provider.asBinder() instanceof Binder)) {
            return provider;
        }
        return ActivityManagerService.<android.content.IContentProvider>asProxy(provider,
                "android.content.ContentProviderProxy");
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
            if (intent.getComponent() == null && Senses.openUrl(intent)) {
                return ActivityManager.START_SUCCESS;  // the page's browser shows it
            }
            // Files to open, save or share: the page is the phone's side
            // (topic app-files, PageFiles).
            if (PageFiles.start(intent, resolvedType, resultTo(resultTo, resultWho, requestCode))) {
                return ActivityManager.START_SUCCESS;
            }
            // A screen of the Settings app (default apps, notification channels,
            // app details), a document picker ...: there is none, so as if the
            // user left it at once (Senses.leftAtOnce). A result asked for comes
            // back cancelled.
            String action = intent.getAction();
            if (Senses.leftAtOnce(intent)) {
                Slog.i(TAG, "no system app for " + action + ": left at once");
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
            // 8.1: an AppWindowContainerController for the activity.
            WindowManagerService wms = SystemServer.windowManager();
            if (wms != null) {
                wms.addAppToken(r.appToken, aInfo.screenOrientation);
            }
            resumeTopActivityLocked();
            return ActivityManager.START_SUCCESS;
        }
    }

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
                schedule(r, ActivityResultItem.obtain(list), null);
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
        wms().setAppVisibility(next.appToken, true);
        wms().setFocusedApp(next.appToken, true);
        next.visible = true;
        mStopping.remove(next);
        if (next.state != State.INITIALIZING && next.app != null && next.app.thread != null) {
            // Already launched (paused or stopped): its results, then resume it.
            history("resume", next);
            next.state = State.RESUMED;
            next.idle = false;
            mResumedActivity = next;
            try {
                ClientTransactionItem results = null;
                if (!next.results.isEmpty()) {
                    results = ActivityResultItem.obtain(new ArrayList<ResultInfo>(next.results));
                    next.results.clear();
                }
                schedule(next, results,
                        ResumeActivityItem.obtain(ActivityManager.PROCESS_STATE_TOP, true));
                topResumedLocked(next);
                wm.executeAppTransition();
            } catch (RemoteException e) {
                Slog.w(TAG, "resume " + next, e);
            }
            return;
        }
        startSpecificActivityLocked(next);
    }

    /**
     * The process of a component: its android:process (topic multiproc: a
     * light process of the app) when LightProcess is on for the package,
     * else -- one process per app, as before -- its application's main
     * process, so that the app's "am I the main process?" checks
     * (Application.getProcessName, ActivityManager.getRunningAppProcesses)
     * see the main one (GeckoView's, analytics SDKs' ":remote" services).
     */
    private static String processNameOf(android.content.pm.ComponentInfo ci) {
        String main = ci.applicationInfo != null && ci.applicationInfo.processName != null
                ? ci.applicationInfo.processName : ci.packageName;
        if (ci.processName == null || ci.processName.equals(main)) {
            return main;
        }
        if (LightProcess.enabledFor(ci.packageName)) {
            return ci.processName;
        }
        Slog.i(TAG, ci.name + ": " + ci.processName + " runs in " + main + " (one process per app)");
        return main;
    }

    /** Whether |ci| is an isolatedProcess service run in a light process of its own. */
    private static boolean isolatedOf(android.content.pm.ComponentInfo ci) {
        return ci instanceof ServiceInfo
                && (((ServiceInfo) ci).flags & ServiceInfo.FLAG_ISOLATED_PROCESS) != 0
                && LightProcess.enabledFor(ci.packageName);
    }

    /** ActivityStackSupervisor.startSpecificActivityLocked. */
    private void startSpecificActivityLocked(ActivityRecord r) {
        String processName = processNameOf(r.info);
        ProcessRecord app = mProcesses.get(processName);
        if (app != null && app.thread != null && !app.dead) {
            realStartActivityLocked(r, app);
            return;
        }
        if (app == null || app.dead) {
            app = startProcessLocked(processName, r.info.applicationInfo);
        }
        // The launch happens in attachApplication.
        r.app = app;
    }

    /** ActivityStackSupervisor.realStartActivityLocked (andResume = true). */
    private void realStartActivityLocked(ActivityRecord r, ProcessRecord app) {
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
            // ActivityStackSupervisor.realStartActivityLocked of 10.
            schedule(r, LaunchActivityItem.obtain(new Intent(r.intent),
                    System.identityHashCode(r), r.info, new Configuration(mConfiguration),
                    new Configuration() /* overrideConfig */,
                    CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO, r.launchedFromPackage,
                    null /* voiceInteractor */, ActivityManager.PROCESS_STATE_TOP, r.icicle,
                    r.persistentState, results, null /* newIntents */, true /* isForward */,
                    null /* profilerInfo */, r.assistToken),
                    ResumeActivityItem.obtain(ActivityManager.PROCESS_STATE_TOP, true));
            topResumedLocked(r);
            windowManager().executeAppTransition();
        } catch (RemoteException e) {
            Slog.w(TAG, "launch " + r, e);
        }
    }

    /**
     * ActivityStackSupervisor.updateTopResumedActivityIfNeeded: the resumed
     * activity is the top resumed one (Activity.onTopResumedActivityChanged,
     * API 29); the previous one no longer is.
     */
    private void topResumedLocked(ActivityRecord r) {
        if (mTopResumed == r) {
            return;
        }
        ActivityRecord prev = mTopResumed;
        mTopResumed = r;
        try {
            if (prev != null && prev.app != null && prev.app.thread != null
                    && prev.state != State.DESTROYED && prev.state != State.INITIALIZING) {
                schedule(prev, TopResumedActivityChangeItem.obtain(false), null);
            }
            schedule(r, TopResumedActivityChangeItem.obtain(true), null);
        } catch (RemoteException e) {
            Slog.w(TAG, "top resumed " + r, e);
        }
    }

    /**
     * Process.start() of ActivityManagerService.startProcessLocked. There is
     * no zygote and no fork: the app's main process is a thread of this VM
     * running ActivityThread.main(), with its own main Looper; its other
     * processes are light processes (startLightProcessLocked). One app
     * (package) per VM; its main process starts once.
     */
    private ProcessRecord startProcessLocked(String processName, ApplicationInfo info) {
        return startProcessLocked(processName, info, false);
    }

    private ProcessRecord startProcessLocked(String processName, ApplicationInfo info,
            boolean isolated) {
        final ProcessRecord app = new ProcessRecord(processName, info);
        app.uid = info.uid;
        boolean google = isGoogleProcess(info);
        boolean main = !google && processName.equals(info.processName != null
                ? info.processName : info.packageName);
        if (!main || isolated) {
            if (!google) {
                if (mAppPackage != null && !mAppPackage.equals(info.packageName)) {
                    Slog.e(TAG, "one app per VM: not starting " + processName);
                    app.dead = true;
                    return app;
                }
                mAppPackage = info.packageName;
            }
            if (!isolated) {
                mProcesses.put(processName, app);
            }
            startLightProcessLocked(app, isolated);
            return app;
        }
        mProcesses.put(processName, app);
        if (mAppProcessStarted) {
            Slog.e(TAG, "one app process per VM: not starting " + processName);
            app.dead = true;
            return app;
        }
        mAppProcessStarted = true;
        mAppPackage = info.packageName;
        app.pid = Process.myPid();
        if (SystemServer.packageManager() != null) {
            SystemServer.packageManager().setAppProcessPackage(info.packageName);
        }
        Slog.i(TAG, "Start proc " + processName + " for activity: uid=" + info.uid);
        app.mainThread = new Thread(new Runnable() {
            public void run() {
                try {
                    android.app.ActivityThread.main(new String[0]);
                } catch (Throwable t) {
                    Senses.crash(app.info.packageName, "process died: " + Senses.reason(t));
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
        // (Burstly's ads in Angry Birds 2.1: "not in UI thread" otherwise).
        }, "main");
        app.mainThread.start();
        return app;
    }

    /**
     * A light process for |app| (topic multiproc): ActivityThread.main on
     * the main thread of a new pid, which attaches like the main process.
     * Its death (by anyone) comes back through LightProcess's reaper.
     */
    private void startLightProcessLocked(final ProcessRecord app, boolean isolated) {
        app.light = true;
        app.isolated = isolated;
        if (isolated) {
            app.uid = LightProcess.newIsolatedUid();
        }
        LightProcess.startReaper(new LightProcess.DeathListener() {
            public void onDied(int pid) {
                lightProcessDied(pid);
            }
        });
        // The app's processes run as the VM's uid (0, which the package
        // manager answers with the app's package); a Google package's as
        // its own, so that the app's calls to it and its calls back are
        // told apart by uid (Binder.getCallingUid), as microG does.
        int uid = isolated ? app.uid : isGoogleProcess(app.info) ? app.info.uid : 0;
        app.pid = LightProcess.start(app.processName, uid, new Runnable() {
            public void run() {
                android.app.ActivityThread.main(new String[0]);
            }
        }, new LightProcess.ExitListener() {
            public void onExit(int pid, Throwable error) {
                // Its main thread ended: an uncaught exception there, or its
                // Looper quit. The process is gone either way.
                Slog.e(TAG, "process " + app.processName + " (pid " + pid + ") ended"
                        + (error == null ? "" : ": " + Senses.reason(error)));
                if (error != null) {
                    for (String line : android.util.Log.getStackTraceString(error).split("\n")) {
                        Slog.e(TAG, line);
                    }
                }
                LightProcess.kill(pid);
            }
        });
        Slog.i(TAG, "Start proc " + app.pid + ":" + app.processName + "/u0a"
                + (app.info.uid % 100000 - 10000) + (isolated ? " (isolated, uid " + app.uid + ")" : "")
                + " in a light process");
    }

    /**
     * Whether process |processName| of |ai| may start: one app (package) per
     * VM, its main process once; and the processes of the user's Google
     * packages (isGoogleProcess) beside it.
     */
    private boolean canStartLocked(String processName, ApplicationInfo ai) {
        if (isGoogleProcess(ai)) {
            return true;
        }
        if (mAppPackage != null && !mAppPackage.equals(ai.packageName)) {
            return false;
        }
        String main = ai.processName != null ? ai.processName : ai.packageName;
        return !processName.equals(main) || !mAppProcessStarted;
    }

    /**
     * Topic gms x multiproc: the user's Google packages (microG's GmsCore
     * and companion, or Google's own, from /data/google: Gms.isUserGoogle)
     * run their components in light processes of their own -- every one of
     * them, its main process (com.google.android.gms) too, with the
     * package's uid -- beside the one app of this VM, started when an app
     * first needs one of their services, receivers or providers (never at
     * boot: android:persistent is not honoured). Without light processes
     * (another VM, arcwasm.multiproc=0) they are not run at all.
     */
    private static boolean isGoogleProcess(ApplicationInfo ai) {
        return ai != null && Gms.isUserGoogle(ai.packageName, ai.sourceDir)
                && LightProcess.enabledFor(ai.packageName);
    }

    /** A component of the user's Google packages that cannot run (no light processes). */
    private static boolean isStrandedGoogle(android.content.pm.ComponentInfo ci) {
        return Gms.isUserGoogle(ci) && !isGoogleProcess(ci.applicationInfo);
    }

    /** The record of the process with |pid| (the main one's is the VM's pid). */
    private ProcessRecord processForPidLocked(int pid) {
        for (ProcessRecord app : mProcesses.values()) {
            if (app.pid == pid && !app.dead) {
                return app;
            }
        }
        for (ServiceRecord s : mServices.values()) {
            if (s.app != null && s.app.isolated && s.app.pid == pid && !s.app.dead) {
                return s.app;
            }
        }
        return null;
    }

    /** A light process died (killed by anyone, or its main thread ended). */
    void lightProcessDied(int pid) {
        synchronized (this) {
            ProcessRecord app = processForPidLocked(pid);
            if (app == null) {
                return;
            }
            Slog.i(TAG, "Process " + app.processName + " (pid " + pid + ") has died");
            appDiedLocked(app);
        }
    }

    private void appDied(ProcessRecord app) {
        synchronized (this) {
            appDiedLocked(app);
        }
    }

    /**
     * ActivityManagerService.handleAppDiedLocked: what ran in |app| goes. Its
     * services restart in a new process when they were started (sticky, as
     * the default onStartCommand result) or are bound with
     * BIND_AUTO_CREATE (ActiveServices.killServicesLocked); clients of its
     * binders get their death notices from the driver.
     */
    private void appDiedLocked(ProcessRecord app) {
        if (app.dead && app.thread == null && !mProcesses.containsValue(app)) {
            return;
        }
        app.dead = true;
        app.thread = null;
        for (ActivityRecord r : new ArrayList<ActivityRecord>(app.activities)) {
            removeActivityLocked(r);
        }
        if (mResumedActivity != null && mResumedActivity.app == app) {
            mResumedActivity = null;
        }
        if (mPausingActivity != null && mPausingActivity.app == app) {
            mPausingActivity = null;
        }
        if (mProcesses.get(app.processName) == app) {
            mProcesses.remove(app.processName);
        }
        app.providers.clear();
        for (Iterator<Receiver> it = mReceivers.iterator(); it.hasNext();) {
            Receiver r = it.next();
            if (app.light && r.pid == app.pid) {
                it.remove();
            }
        }
        for (Iterator<ServiceRecord> it = mServices.values().iterator(); it.hasNext();) {
            final ServiceRecord s = it.next();
            if (s.app != app) {
                continue;
            }
            boolean restart = app.light && s.startRequested;
            for (IntentBinding b : s.bindings.values()) {
                b.binder = null;
                b.received = false;
                b.requested = false;
                restart |= app.light && b.autoCreate && !b.connections.isEmpty();
            }
            s.created = false;
            if (!restart) {
                it.remove();
                continue;
            }
            // ActiveServices.scheduleServiceRestartLocked: after a second, in
            // a new process.
            s.restarting = true;
            Slog.w(TAG, "Scheduling restart of crashed service " + s.name.flattenToShortString()
                    + " in 1000ms");
            new Thread("ServiceRestart") {
                @Override
                public void run() {
                    SystemClock.sleep(1000);
                    synchronized (ActivityManagerService.this) {
                        if (!s.restarting || mServices.get(s.name) != s) {
                            return;
                        }
                        s.restarting = false;
                        ProcessRecord p = processForServiceLocked(s.info);
                        if (p == null) {
                            mServices.remove(s.name);
                            return;
                        }
                        s.app = p;
                        bringUpServiceLocked(s);
                    }
                }
            }.start();
        }
        notifyAll();  // getContentProvider waiting for a provider of this process
    }

    /** The process this service started and the app has not attached to yet. */
    private ProcessRecord pendingProcessLocked(int pid) {
        for (ProcessRecord app : mProcesses.values()) {
            if (app.thread == null && !app.dead && app.pid == pid) {
                return app;
            }
        }
        for (ServiceRecord s : mServices.values()) {
            if (s.app != null && s.app.isolated && s.app.thread == null && !s.app.dead
                    && s.app.pid == pid) {
                return s.app;
            }
        }
        return null;
    }

    @Override
    public void attachApplication(IApplicationThread thread, long startSeq) {
        synchronized (this) {
            // The calling pid: the main process's is the VM's, a light
            // process's its own (directly or through the driver).
            ProcessRecord app = pendingProcessLocked(Binder.getCallingPid());
            if (app == null) {
                Slog.w(TAG, "attachApplication from an unknown process "
                        + Binder.getCallingPid());
                return;
            }
            app.thread = proxyFor(thread);
            // ActivityManagerService.attachApplicationLocked: the services the
            // app caches (getCommonServicesLocked) and its content providers.
            Map<String, IBinder> services = new HashMap<String, IBinder>();
            for (String name : new String[] {"package", "window", "alarm"}) {
                IBinder b = ServiceManager.getService(name);
                if (b != null) {
                    services.put(name, b);
                }
            }
            List<ProviderInfo> providers = app.isolated ? new ArrayList<ProviderInfo>()
                    : generateApplicationProvidersLocked(app);
            try {
                app.thread.bindApplication(app.processName, app.info,
                        providers.isEmpty() ? null : providers, null, null, null, null, null,
                        0, false, false, false, false /* persistent */,
                        new Configuration(mConfiguration),
                        CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO, services, new Bundle(),
                        android.os.Build.SERIAL, null /* autofillOptions */,
                        null /* contentCaptureOptions */);
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
            for (Object[] b : app.pendingReceivers) {
                deliverToManifestReceiverLocked(app, (Intent) b[0], (ActivityInfo) b[1]);
            }
            app.pendingReceivers.clear();
        }
    }

    private List<ProviderInfo> generateApplicationProvidersLocked(ProcessRecord app) {
        List<ProviderInfo> out = new ArrayList<ProviderInfo>();
        try {
            android.content.pm.ParceledListSlice<ProviderInfo> slice = mPackageManager
                    .queryContentProviders(LightProcess.enabledFor(app.info.packageName)
                            ? app.processName : null /* one process: all of them */,
                            app.info.uid, STOCK_PM_FLAGS, null);
            if (slice != null) {
                out.addAll(slice.getList());
            }
        } catch (RemoteException e) {
            // In process: not thrown.
        }
        return out;
    }

    // --- callbacks from the app ----------------------------------------------------

    public void activityResumed(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r != null) {
                Slog.i(TAG, "resumed " + r);
            }
        }
    }

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
            wms().setAppVisibility(r.appToken, false);
            schedule(r, null, StopActivityItem.obtain(false, r.info.configChanges));
        } catch (RemoteException e) {
            Slog.w(TAG, "stop " + r, e);
        }
    }

    private void destroyActivityLocked(ActivityRecord r) {
        history("destroy", r);
        r.state = State.DESTROYING;
        r.visible = false;
        try {
            wms().setAppVisibility(r.appToken, false);
            schedule(r, null, DestroyActivityItem.obtain(r.finishing, r.info.configChanges));
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
            schedule(r, null, PauseActivityItem.obtain(r.finishing, userLeaving,
                    r.info.configChanges, false));
        } catch (RemoteException e) {
            Slog.w(TAG, "pause " + r, e);
        }
    }

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

    public void activitySlept(IBinder token) {}

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
        if (mTopResumed == r) {
            mTopResumed = null;
        }
        if (r.app != null) {
            r.app.activities.remove(r);
        }
        TaskRecord task = r.task;
        task.activities.remove(r);
        if (task.activities.isEmpty()) {
            mTasks.remove(task);
        }
        wms().removeAppToken(r.appToken);
    }

    @Override
    public boolean finishActivity(IBinder token, int resultCode, Intent resultData,
            int finishTask) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r == null) {
                return false;
            }
            // Activity.FINISH_TASK_WITH_ROOT_ACTIVITY (1) / FINISH_TASK_WITH_ACTIVITY (2).
            if (finishTask != 0 && r.task.activities.indexOf(r) == 0) {
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

    public boolean willActivityBeVisible(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            // A headless VM (no fw10-graphics): no window will ever show, and
            // ActivityThread then makes none (no DecorView, whose RenderNode
            // needs hwui) for an activity that started another.
            return r != null && r == topRunningActivity() && WindowManagerService.hasGraphics();
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
    public void moveTaskToFront(IApplicationThread caller, String callingPackage, int taskId,
            int flags, Bundle options) {
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

    public boolean isInHomeStack(int taskId) {
        return false;
    }

    public String getCallingPackage(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            return r == null || r.resultTo == null ? null : r.resultTo.info.packageName;
        }
    }

    public ComponentName getCallingActivity(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            return r == null || r.resultTo == null ? null : r.resultTo.realActivity;
        }
    }

    public ComponentName getActivityClassForToken(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            return r == null ? null : r.realActivity;
        }
    }

    /** Activity.finishActivity(requestCode): the activities it started for that result. */
    public void finishSubActivity(IBinder token, String resultWho, int requestCode) {
        synchronized (this) {
            ActivityRecord self = forToken(token);
            if (self == null) {
                return;
            }
            for (TaskRecord t : new ArrayList<TaskRecord>(mTasks)) {
                for (ActivityRecord r : new ArrayList<ActivityRecord>(t.activities)) {
                    if (r.resultTo == self && r.requestCode == requestCode
                            && (r.resultWho == null ? resultWho == null
                                : r.resultWho.equals(resultWho))) {
                        finishActivityLocked(r, android.app.Activity.RESULT_CANCELED, null);
                    }
                }
            }
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
                wms().setAppOrientation(r.appToken, requestedOrientation);
                if (r == mResumedActivity) {
                    updateRotationLocked(r);
                }
            }
        }
    }

    /**
     * The display turns for |r| if it wants another orientation (topic
     * rotation, as the 4.4 build's): a landscape activity on the portrait
     * display, and back for the next one. Then the configuration follows,
     * the app hears of it (a ConfigurationChangeItem, as
     * ActivityTaskManagerService.updateGlobalConfigurationLocked sends) and
     * the windows lay out on the turned display. ensureActivityConfiguration
     * would relaunch activities that do not handle the change; here they get
     * the new configuration only.
     */
    private void updateRotationLocked(ActivityRecord r) {
        WindowManagerService wm = wms();
        if (wm == null || !wm.updateRotationFor(r.appToken)) {
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
                    ClientTransaction t = ClientTransaction.obtain(app.thread, null);
                    t.addCallback(android.app.servertransaction.ConfigurationChangeItem.obtain(
                            new Configuration(config)));
                    t.schedule();
                    t.recycle();
                } catch (RemoteException e) {
                    // In process: not thrown.
                }
            }
        }
        wm.relayoutAll(config);
    }

    public int getRequestedOrientation(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            return r == null ? ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED : r.info.screenOrientation;
        }
    }

    public void overridePendingTransition(IBinder token, String packageName, int enterAnim,
            int exitAnim) {}

    public void reportActivityFullyDrawn(IBinder token, boolean restoredFromBundle) {}

    public void notifyActivityDrawn(IBinder token) {}

    public void setTaskDescription(IBinder token, ActivityManager.TaskDescription values) {}

    public int getActivityDisplayId(IBinder activityToken) {
        return Display.DEFAULT_DISPLAY;
    }

    public Bundle getActivityOptions(IBinder token) {
        return null;
    }

    public boolean convertFromTranslucent(IBinder token) {
        return false;
    }

    public boolean convertToTranslucent(IBinder token, Bundle options) {
        return false;
    }

    @Override
    public void backgroundWhitelistUid(int uid) {}

    public boolean enterPictureInPictureMode(IBinder token,
            android.app.PictureInPictureParams params) {
        return false;  // no picture-in-picture (API 26)
    }

    public void setPictureInPictureParams(IBinder token,
            android.app.PictureInPictureParams params) {}

    public int getMaxNumPictureInPictureActions(IBinder token) {
        return 0;
    }

    public boolean isInMultiWindowMode(IBinder token) {
        return false;
    }

    public boolean isInPictureInPictureMode(IBinder token) {
        return false;
    }

    public void setShowWhenLocked(IBinder token, boolean showWhenLocked) {}

    public void setTurnScreenOn(IBinder token, boolean turnScreenOn) {}

    public int setVrMode(IBinder token, boolean enabled, ComponentName packageName) {
        return -1;  // no VR
    }

    public void setDisablePreviewScreenshots(IBinder token, boolean disable) {}

    public void reportSizeConfigurations(IBinder token, int[] horizontalSizeConfiguration,
            int[] verticalSizeConfigurations, int[] smallestWidthConfigurations) {}

    public void releaseSomeActivities(IApplicationThread app) {}

    public boolean isImmersive(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r == null) {
                throw new IllegalArgumentException();
            }
            return (r.info.flags & ActivityInfo.FLAG_IMMERSIVE) != 0;
        }
    }

    public void setImmersive(IBinder token, boolean immersive) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r == null) {
                throw new IllegalArgumentException();
            }
            if (immersive) {
                r.info.flags |= ActivityInfo.FLAG_IMMERSIVE;
            } else {
                r.info.flags &= ~ActivityInfo.FLAG_IMMERSIVE;
            }
        }
    }

    // --- asked by 8.1's Activity, DecorView and ActivityThread ------------------------------

    /** ActivityManager.StackId.FULLSCREEN_WORKSPACE_STACK_ID: one stack, full screen. */
    public int getActivityStackId(IBinder token) {
        return 1;
    }

    @Override
    public boolean isTopOfTask(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            return r != null && r.task != null && r.task.topRunning() == r;
        }
    }

    /** Activity.finishAffinity: this activity and those below it in its task. */
    public boolean finishActivityAffinity(IBinder token) {
        synchronized (this) {
            ActivityRecord r = forToken(token);
            if (r == null || r.task == null) {
                return false;
            }
            List<ActivityRecord> list = r.task.activities;
            for (int i = list.indexOf(r); i >= 0; i--) {
                ActivityRecord a = list.get(i);
                if (!a.finishing) {
                    finishActivityLocked(a, android.app.Activity.RESULT_CANCELED, null);
                }
            }
            return true;
        }
    }

    public void activityRelaunched(IBinder token) {}
    public boolean isRootVoiceInteraction(IBinder token) { return false; }
    public boolean supportsLocalVoiceInteraction() { return false; }
    public boolean releaseActivityInstance(IBinder token) { return false; }
    @Override public void setRenderThread(int tid) {}
    @Override public void unstableProviderDied(IBinder connection) {}
    @Override public void waitForNetworkStateUpdate(long procStateSeq) {}
    public void exitFreeformMode(IBinder token) {}
    public void startLockTaskModeByToken(IBinder token) {}
    public void stopLockTaskMode() {}

    @Override
    public void closeSystemDialogs(String reason) {
        WindowManagerService wm = SystemServer.windowManager();
        if (wm != null) {
            wm.closeSystemDialogs(reason);
        }
    }

    @Override
    public int checkPermissionWithToken(String permission, int pid, int uid, IBinder callerToken) {
        return checkPermission(permission, pid, uid);
    }

    @Override
    public void revokeUriPermission(IApplicationThread caller, String targetPackage,
            android.net.Uri uri, int mode, int userId) {}

    /** PendingIntent.send (8.1 goes through the activity manager). */
    @Override
    public int sendIntentSender(IIntentSender target, IBinder whitelistToken, int code,
            Intent intent, String resolvedType, IIntentReceiver finishedReceiver,
            String requiredPermission, Bundle options) {
        if (target instanceof IntentSender) {
            return ((IntentSender) target).sendInner(code, intent, resolvedType, finishedReceiver,
                    requiredPermission, options);
        }
        try {
            target.send(code, intent, resolvedType, whitelistToken, finishedReceiver,
                    requiredPermission, options);
        } catch (RemoteException e) {
            return ActivityManager.START_CANCELED;
        }
        return 0;
    }

    @Override
    public boolean isTopActivityImmersive() {
        synchronized (this) {
            ActivityRecord r = topRunningActivity();
            return r != null && (r.info.flags & ActivityInfo.FLAG_IMMERSIVE) != 0;
        }
    }

    public boolean shouldUpRecreateTask(IBinder token, String destAffinity) {
        return false;
    }

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
    public List<ActivityManager.RunningTaskInfo> getTasks(int maxNum) {
        synchronized (this) {
            List<ActivityManager.RunningTaskInfo> list = new ArrayList<ActivityManager.RunningTaskInfo>();
            for (int i = mTasks.size() - 1; i >= 0 && list.size() < maxNum; i--) {
                TaskRecord t = mTasks.get(i);
                ActivityManager.RunningTaskInfo ti = new ActivityManager.RunningTaskInfo();
                ti.id = ti.taskId = t.taskId;
                ti.isRunning = true;
                ti.baseIntent = t.intent;
                ti.baseActivity = ti.realActivity = t.activities.get(0).realActivity;
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
    public android.content.pm.ParceledListSlice getRecentTasks(int maxNum, int flags, int userId) {
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
                ti.lastActiveTime = SystemClock.elapsedRealtime();
                list.add(ti);
            }
            return new android.content.pm.ParceledListSlice<ActivityManager.RecentTaskInfo>(list);
        }
    }

    public List<IBinder> getAppTasks(String callingPackage) {
        return new ArrayList<IBinder>();
    }

    public Point getAppTaskThumbnailSize() {
        return new Point(192, 108);
    }

    @Override
    public List<ActivityManager.RunningAppProcessInfo> getRunningAppProcesses() {
        synchronized (this) {
            List<ActivityManager.RunningAppProcessInfo> list =
                    new ArrayList<ActivityManager.RunningAppProcessInfo>();
            for (ProcessRecord app : mProcesses.values()) {
                if (app.thread == null || app.dead) {
                    continue;
                }
                ActivityManager.RunningAppProcessInfo pi = new ActivityManager.RunningAppProcessInfo(
                        app.processName, app.pid, new String[] {app.info.packageName});
                pi.uid = app.isolated ? app.uid : app.info.uid;
                boolean top = mResumedActivity != null && mResumedActivity.app == app;
                pi.importance = top ? ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND
                        : ActivityManager.RunningAppProcessInfo.IMPORTANCE_BACKGROUND;
                pi.processState = top ? ActivityManager.PROCESS_STATE_TOP
                        : ActivityManager.PROCESS_STATE_CACHED_ACTIVITY;
                pi.lastTrimLevel = lastTrimLevel();
                list.add(pi);
            }
            return list;
        }
    }

    @Override
    public void getMyMemoryState(ActivityManager.RunningAppProcessInfo outInfo) {
        synchronized (this) {
            ProcessRecord app = processForPidLocked(Binder.getCallingPid());
            boolean top = mResumedActivity != null
                    && (app == null || mResumedActivity.app == app);
            outInfo.pid = Binder.getCallingPid();
            outInfo.uid = Binder.getCallingUid();
            outInfo.importance = top ? ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND
                    : ActivityManager.RunningAppProcessInfo.IMPORTANCE_BACKGROUND;
            outInfo.processState = top ? ActivityManager.PROCESS_STATE_TOP
                    : ActivityManager.PROCESS_STATE_CACHED_ACTIVITY;
            outInfo.lastTrimLevel = lastTrimLevel();
        }
    }

    @Override
    public void getMemoryInfo(ActivityManager.MemoryInfo outInfo) {
        // The memory budget is the device's RAM (topic mem-budget).
        MemoryMonitor m = MemoryMonitor.get();
        if (m != null) {
            m.fillMemoryInfo(outInfo);
            return;
        }
        Runtime rt = Runtime.getRuntime();
        outInfo.totalMem = (long) MemoryBudget.config().budgetMb << 20;
        outInfo.availMem = Math.max(outInfo.totalMem - rt.totalMemory() + rt.freeMemory(), 0);
        outInfo.threshold = outInfo.totalMem / 5;
        outInfo.lowMemory = false;
    }

    private static int lastTrimLevel() {
        MemoryMonitor m = MemoryMonitor.get();
        return m != null ? m.lastTrimLevel() : 0;
    }

    // --- memory (topic mem-budget, MemoryMonitor) ------------------------------------

    /** Where the front task's state goes before a restart or when the page is hidden. */
    static final java.io.File TASK_STATE = new java.io.File("/data/system/arcwasm/task-state.bin");
    private static final int TASK_STATE_MAGIC = 0x41574b31;  // "AWK1"
    /** The task moved to the back because the page was hidden. */
    private TaskRecord mHiddenTask;

    /** ComponentCallbacks2.onTrimMemory(level) in every app process (oneway calls). */
    public void trimApps(int level) {
        synchronized (this) {
            for (ProcessRecord app : mProcesses.values()) {
                if (app.thread == null || app.dead) {
                    continue;
                }
                try {
                    app.thread.scheduleTrimMemory(level);
                } catch (RemoteException e) {
                    Slog.w(TAG, "trim " + app, e);
                }
            }
        }
    }

    /** ComponentCallbacks.onLowMemory() in every app process. */
    public void lowMemoryApps() {
        synchronized (this) {
            for (ProcessRecord app : mProcesses.values()) {
                if (app.thread == null || app.dead) {
                    continue;
                }
                try {
                    app.thread.scheduleLowMemory();
                } catch (RemoteException e) {
                    Slog.w(TAG, "low memory " + app, e);
                }
            }
        }
    }

    /** The task in front (shown), else the one the hidden page sent back. */
    private TaskRecord frontTaskLocked() {
        for (int i = mTasks.size() - 1; i >= 0; i--) {
            TaskRecord t = mTasks.get(i);
            if (!t.inBackground && t.topRunning() != null) {
                return t;
            }
        }
        return mHiddenTask != null && mTasks.contains(mHiddenTask) ? mHiddenTask : null;
    }

    /**
     * The page was hidden (as the Home key: the front task goes to the back,
     * its activities are stopped and save their state, which is written to
     * /data, so that a discarded tab comes back where it was) or shown again
     * (the task comes back to the front; the written state is dropped).
     */
    public void onPageVisibility(boolean hidden) {
        TaskRecord t;
        synchronized (this) {
            if (!hidden) {
                t = mHiddenTask;
                mHiddenTask = null;
                if (t != null && mTasks.contains(t) && t.inBackground) {
                    Slog.i(TAG, "page shown: task " + t.taskId + " to the front");
                    moveTaskToFrontLocked(t);
                }
                TASK_STATE.delete();
                return;
            }
            t = frontTaskLocked();
            if (t == null || t.inBackground) {
                return;
            }
            Slog.i(TAG, "page hidden: task " + t.taskId + " to the back");
            mHiddenTask = t;
            moveTaskToBackLocked(t);
        }
        writeTaskState(t, 5000);
    }

    /**
     * lowmemorykiller's step, before the VM restarts: the front task is
     * stopped (onSaveInstanceState) and written to /data with its activities'
     * state. Returns whether the state was written.
     */
    public boolean saveTaskForRestart(long timeoutMs) {
        TaskRecord t;
        synchronized (this) {
            t = frontTaskLocked();
            if (t == null) {
                return false;
            }
            if (!t.inBackground) {
                moveTaskToBackLocked(t);
            }
        }
        return writeTaskState(t, timeoutMs);
    }

    /** Waits until the task's launched activities are stopped, then writes their state. */
    private boolean writeTaskState(TaskRecord t, long timeoutMs) {
        long until = SystemClock.uptimeMillis() + timeoutMs;
        while (true) {
            synchronized (this) {
                boolean stopped = true;
                for (ActivityRecord r : t.activities) {
                    if (!r.finishing && r.state != State.STOPPED && r.state != State.INITIALIZING) {
                        stopped = false;
                    }
                }
                if (stopped || SystemClock.uptimeMillis() >= until) {
                    if (!stopped) {
                        Slog.w(TAG, "task " + t.taskId + " not stopped in time: " + t.activities);
                    }
                    return writeTaskStateLocked(t);
                }
            }
            SystemClock.sleep(20);
        }
    }

    private boolean writeTaskStateLocked(TaskRecord t) {
        android.os.Parcel p = android.os.Parcel.obtain();
        try {
            List<ActivityRecord> saved = new ArrayList<ActivityRecord>();
            for (ActivityRecord r : t.activities) {
                if (!r.finishing && r.state != State.DESTROYED) {
                    saved.add(r);
                }
            }
            if (saved.isEmpty()) {
                return false;
            }
            p.writeInt(TASK_STATE_MAGIC);
            p.writeLong(System.currentTimeMillis());
            p.writeString(saved.get(0).info.packageName);
            p.writeInt(saved.size());
            for (ActivityRecord r : saved) {
                p.writeString(r.realActivity.flattenToString());
                r.intent.writeToParcel(p, 0);
                p.writeBundle(r.icicle);
                p.writePersistableBundle(r.persistentState);
            }
            byte[] bytes = p.marshall();
            java.io.File dir = TASK_STATE.getParentFile();
            dir.mkdirs();
            java.io.File tmp = new java.io.File(dir, TASK_STATE.getName() + ".tmp");
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(tmp)) {
                out.write(bytes);
                out.getFD().sync();
            }
            if (!tmp.renameTo(TASK_STATE)) {
                throw new java.io.IOException("rename " + tmp);
            }
            Slog.i(TAG, "task " + t.taskId + " saved: " + saved.size() + " activities, "
                    + bytes.length + " bytes");
            return true;
        } catch (Exception e) {
            Slog.w(TAG, "cannot save task " + t.taskId, e);
            return false;
        } finally {
            p.recycle();
        }
    }

    /**
     * After a restart (or a discarded tab): the task saved by
     * saveTaskForRestart/onPageVisibility comes back, its activities with
     * their saved state, the top one launched (onCreate(savedInstanceState),
     * onRestoreInstanceState) and the others when they come to the top, as
     * Android brings back the task of a process it killed. Returns the top
     * activity, or null when there is no saved task (or it is stale, or of
     * another package than |packageName| when that is not null).
     */
    public ComponentName restoreSavedTask(String packageName) {
        if (!TASK_STATE.isFile()) {
            return null;
        }
        byte[] bytes;
        try {
            bytes = java.nio.file.Files.readAllBytes(TASK_STATE.toPath());
        } catch (java.io.IOException e) {
            Slog.w(TAG, "cannot read " + TASK_STATE, e);
            return null;
        } finally {
            // Once: a state that crashes the app must not come back forever.
            TASK_STATE.delete();
        }
        android.os.Parcel p = android.os.Parcel.obtain();
        try {
            p.unmarshall(bytes, 0, bytes.length);
            p.setDataPosition(0);
            if (p.readInt() != TASK_STATE_MAGIC) {
                return null;
            }
            long time = p.readLong();
            if (System.currentTimeMillis() - time > 24L * 3600 * 1000) {
                Slog.i(TAG, "saved task is older than a day: not restored");
                return null;
            }
            String pkg = p.readString();
            if (packageName != null && !packageName.equals(pkg)) {
                Slog.i(TAG, "saved task of " + pkg + ", not " + packageName + ": not restored");
                return null;
            }
            int n = p.readInt();
            synchronized (this) {
                TaskRecord task = null;
                ActivityRecord top = null;
                for (int i = 0; i < n; i++) {
                    ComponentName component = ComponentName.unflattenFromString(p.readString());
                    Intent intent = Intent.CREATOR.createFromParcel(p);
                    Bundle icicle = p.readBundle();
                    PersistableBundle persistent = p.readPersistableBundle();
                    ActivityInfo aInfo = mPackageManager.getActivityInfo(component, STOCK_PM_FLAGS, 0);
                    if (aInfo == null) {
                        Slog.w(TAG, "saved task: no " + component);
                        continue;
                    }
                    if (task == null) {
                        task = new TaskRecord(mNextTaskId++, intent);
                        mTasks.add(task);
                    }
                    ActivityRecord r = new ActivityRecord(aInfo, intent, pkg, null, null, -1);
                    r.icicle = icicle;
                    r.persistentState = persistent;
                    r.task = task;
                    task.activities.add(r);
                    WindowManagerService wms = SystemServer.windowManager();
                    if (wms != null) {
                        wms.addAppToken(r.appToken, aInfo.screenOrientation);
                    }
                    top = r;
                }
                if (top == null) {
                    return null;
                }
                Slog.i(TAG, "restoring task " + task.taskId + " of " + pkg + ": " + task.activities);
                history("restore", top);
                resumeTopActivityLocked();
                return top.realActivity;
            }
        } catch (Exception e) {
            Slog.w(TAG, "cannot restore the saved task", e);
            return null;
        } finally {
            p.recycle();
        }
    }

    // --- killing processes (topic multiproc): light processes only; the main
    // process is the VM, which the page ends ----------------------------------------

    /** Kills the app's light processes |which| accepts (outside the lock: the reaper reports). */
    private interface ProcessFilter {
        boolean accept(ProcessRecord app);
    }

    private int killLightProcesses(ProcessFilter which, String reason) {
        List<Integer> pids = new ArrayList<Integer>();
        synchronized (this) {
            for (ProcessRecord app : mProcesses.values()) {
                if (app.light && !app.dead && which.accept(app)) {
                    pids.add(app.pid);
                }
            }
            for (ServiceRecord s : mServices.values()) {
                if (s.app != null && s.app.isolated && !s.app.dead && which.accept(s.app)
                        && !pids.contains(s.app.pid)) {
                    pids.add(s.app.pid);
                }
            }
        }
        for (int pid : pids) {
            Slog.i(TAG, "Killing " + pid + ": " + reason);
            LightProcess.kill(pid);
        }
        return pids.size();
    }

    /** ActivityManager.killBackgroundProcesses: its light processes not in front. */
    @Override
    public void killBackgroundProcesses(final String packageName, int userId) {
        killLightProcesses(new ProcessFilter() {
            public boolean accept(ProcessRecord app) {
                return app.info.packageName.equals(packageName)
                        && (mResumedActivity == null || mResumedActivity.app != app);
            }
        }, "kill background");
    }

    @Override
    public void killApplicationProcess(final String processName, int uid) {
        killLightProcesses(new ProcessFilter() {
            public boolean accept(ProcessRecord app) {
                return app.processName.equals(processName);
            }
        }, "kill application process");
    }

    @Override
    public boolean killPids(final int[] pids, String reason, boolean secure) {
        return killLightProcesses(new ProcessFilter() {
            public boolean accept(ProcessRecord app) {
                for (int pid : pids) {
                    if (pid == app.pid) {
                        return true;
                    }
                }
                return false;
            }
        }, reason) > 0;
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
    public boolean isUserRunning(int userid, int flags) {
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
    public void addPackageDependency(String packageName) {}

    // --- crashes and StrictMode reports: logged (no DropBox, no crash dialog) ----

    @Override
    public void handleApplicationStrictModeViolation(IBinder app, int violationMask,
            android.os.StrictMode.ViolationInfo info) {
        Slog.w(TAG, "StrictMode violation 0x" + Integer.toHexString(violationMask) + ": "
                + (info == null ? null : info.getStackTrace()));
    }

    @Override
    public void handleApplicationCrash(IBinder app,
            android.app.ApplicationErrorReport.ParcelableCrashInfo crashInfo) {
        Slog.e(TAG, "app crashed: " + crashInfo.exceptionClassName + ": "
                + crashInfo.exceptionMessage + "\n" + crashInfo.stackTrace);
        synchronized (this) {
            ProcessRecord p = processForPidLocked(Binder.getCallingPid());
            if (p != null && p.light) {
                // A light process: RuntimeInit kills it next
                // (Process.killProcess), the app's other processes go on.
                Slog.e(TAG, "process " + p + " crashed");
                return;
            }
        }
        // One-app mode: the page goes back to its library (RuntimeInit kills
        // the process, i.e. this VM, next).
        Senses.crash(crashedPackage(app),
                crashInfo.exceptionClassName + ": " + crashInfo.exceptionMessage);
    }

    /** The package of the process whose ApplicationThread is |app| (else the page's app). */
    private String crashedPackage(IBinder app) {
        synchronized (this) {
            for (ProcessRecord p : mProcesses.values()) {
                if (p.thread != null && app != null && p.thread.asBinder() == app) {
                    return p.info.packageName;
                }
            }
        }
        return Senses.app();
    }

    // --- SENSES Web Apps' one-app mode (topic install8, arcwasm.server.Senses) ----------

    /**
     * HOME (SystemKeys). No home screen here; in one-app mode the page goes
     * back to its library: the task is stopped and its state written, as
     * for a restart for memory (saveTaskForRestart), so that the app's next
     * start comes back where it was, as after its process was killed in the
     * background. Off the input thread: the task's activities stop meanwhile.
     */
    public void startHome() {
        if (!Senses.enabled()) {
            Slog.i(TAG, "HOME: there is no home screen");
            return;
        }
        new Thread("SensesHome") {
            @Override
            public void run() {
                String pkg = Senses.app();
                synchronized (ActivityManagerService.this) {
                    TaskRecord t = frontTaskLocked();
                    if (t != null && !t.activities.isEmpty()) {
                        pkg = t.activities.get(0).info.packageName;
                    }
                }
                boolean saved = saveTaskForRestart(5000);
                Slog.i(TAG, "HOME: task of " + pkg + (saved ? " saved" : " not saved"));
                Senses.exit(pkg, "home");
            }
        }.start();
    }

    /** APP_SWITCH: no recent apps here (the page has its own). */
    public void showRecentApps() {
        Slog.i(TAG, "APP_SWITCH: no recent apps");
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
                SystemClock.sleep(700);
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

    @Override
    public boolean handleApplicationWtf(IBinder app, String tag, boolean system,
            android.app.ApplicationErrorReport.ParcelableCrashInfo crashInfo) {
        Slog.e(TAG, "wtf from app, " + tag + ": " + crashInfo.exceptionMessage);
        return false;
    }

    @Override
    public void unhandledBack() {}

    // --- broadcasts: to receivers registered by the app ---------------------------------

    @Override
    public Intent registerReceiver(IApplicationThread caller, String callerPackage,
            IIntentReceiver receiver, IntentFilter filter, String permission, int userId,
            int flags) {
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
                mReceivers.add(new Receiver(receiver, filter, caller, Binder.getCallingPid()));
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
                proxyFor(r.receiver).performReceive(new Intent(intent), resultCode, resultData,
                        map, false, sticky, 0);
            } catch (RemoteException e) {
                Slog.w(TAG, "broadcast to " + r.receiver, e);
            }
        }
        int manifest = deliverToManifestReceivers(intent, resolvedType);
        if (resultTo != null) {
            try {
                proxyFor(resultTo).performReceive(new Intent(intent), resultCode, resultData, map,
                        false, false, 0);
            } catch (RemoteException e) {
                Slog.w(TAG, "broadcast result", e);
            }
        }
        if (targets.isEmpty() && manifest == 0) {
            Slog.i(TAG, "broadcast not delivered (no receivers): " + intent);
        }
        return ActivityManager.BROADCAST_SUCCESS;
    }

    /** Implicit broadcasts API 26 apps still get in their manifest (BroadcastQueue's exemptions). */
    private static final java.util.Set<String> IMPLICIT_EXEMPT = new java.util.HashSet<String>(
            java.util.Arrays.asList(Intent.ACTION_BOOT_COMPLETED, Intent.ACTION_LOCKED_BOOT_COMPLETED,
                    Intent.ACTION_LOCALE_CHANGED, Intent.ACTION_TIMEZONE_CHANGED,
                    Intent.ACTION_TIME_CHANGED, Intent.ACTION_MY_PACKAGE_REPLACED,
                    "android.appwidget.action.APPWIDGET_UPDATE"));

    /**
     * Manifest receivers (BroadcastQueue.processNextBroadcast, unordered):
     * the receivers the package manager finds for the intent, in the app
     * process (started for them if it is not running yet), through
     * IApplicationThread.scheduleReceiver. As 8.1 does for apps targeting
     * API 26, implicit broadcasts reach manifest receivers only when the
     * action is exempt; explicit ones (component or package) always do.
     */
    private int deliverToManifestReceivers(Intent intent, String resolvedType) {
        List<ResolveInfo> receivers;
        try {
            receivers = mPackageManager.queryIntentReceivers(intent, resolvedType,
                    STOCK_PM_FLAGS, 0).getList();
        } catch (RemoteException e) {
            return 0;
        }
        boolean explicit = intent.getComponent() != null || intent.getPackage() != null;
        int n = 0;
        synchronized (this) {
            for (ResolveInfo ri : receivers) {
                ActivityInfo ai = ri.activityInfo;
                if (isStrandedGoogle(ai)) {
                    continue;  // the user's Google package (topic gms): no light processes here
                }
                if (!explicit && ai.applicationInfo.targetSdkVersion >= 26
                        && !IMPLICIT_EXEMPT.contains(intent.getAction())) {
                    continue;
                }
                String name = processNameOf(ai);
                ProcessRecord app = mProcesses.get(name);
                if (app == null || app.dead) {
                    if (!canStartLocked(name, ai.applicationInfo)) {
                        continue;  // another package: no second app in this VM
                    }
                    app = startProcessLocked(name, ai.applicationInfo);
                    if (app.dead) {
                        continue;
                    }
                }
                if (app.thread == null) {
                    app.pendingReceivers.add(new Object[] {new Intent(intent), ai});
                } else {
                    deliverToManifestReceiverLocked(app, intent, ai);
                }
                n++;
            }
        }
        return n;
    }

    private void deliverToManifestReceiverLocked(ProcessRecord app, Intent intent,
            ActivityInfo ai) {
        Intent i = new Intent(intent);
        i.setComponent(new ComponentName(ai.packageName, ai.name));
        Slog.i(TAG, "broadcast to " + i.getComponent().flattenToShortString() + ": "
                + intent.getAction());
        try {
            app.thread.scheduleReceiver(i, ai, CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                    android.app.Activity.RESULT_OK, null, intent.getExtras(), false, 0,
                    ActivityManager.PROCESS_STATE_RECEIVER);
        } catch (RemoteException e) {
            Slog.w(TAG, "broadcast to " + ai.name, e);
        }
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
        public void send(int code, Intent fillIn, String resolvedType, IBinder whitelistToken,
                IIntentReceiver finishedReceiver, String requiredPermission, Bundle options) {
            sendInner(code, fillIn, resolvedType, finishedReceiver, requiredPermission, options);
        }

        int sendInner(int code, Intent fillIn, String resolvedType,
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
                case ActivityManager.INTENT_SENDER_FOREGROUND_SERVICE:
                    startService(null, intent, resolvedType,
                            type == ActivityManager.INTENT_SENDER_FOREGROUND_SERVICE, packageName, 0);
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
        if (isStrandedGoogle(si)) {
            return null;  // the user's Google package (topic gms): no light processes here
        }
        ComponentName name = new ComponentName(si.packageName, si.name);
        ServiceRecord s = mServices.get(name);
        if (s != null) {
            return s;
        }
        ProcessRecord app = processForServiceLocked(si);
        if (app == null) {
            return null;
        }
        s = new ServiceRecord(si, app);
        mServices.put(name, s);
        return s;
    }

    /**
     * The process a service runs in: its running process, else one started
     * now (into the one app of this VM only). An isolatedProcess service
     * gets a light process of its own, with an isolated uid.
     */
    private ProcessRecord processForServiceLocked(ServiceInfo si) {
        String processName = processNameOf(si);
        if (isolatedOf(si)) {
            if (!canStartLocked(processName, si.applicationInfo)) {
                return null;
            }
            ProcessRecord app = startProcessLocked(processName, si.applicationInfo, true);
            return app.dead ? null : app;
        }
        ProcessRecord app = mProcesses.get(processName);
        if (app == null || app.dead) {
            // Only into the one app (started for an activity, or now).
            if (!canStartLocked(processName, si.applicationInfo)) {
                return null;
            }
            app = startProcessLocked(processName, si.applicationInfo);
            if (app.dead) {
                return null;
            }
        }
        return app;
    }

    /** ActiveServices.bringUpServiceLocked: create, then pending starts and binds. */
    private void bringUpServiceLocked(ServiceRecord s) {
        if (s.app.dead) {
            ProcessRecord p = processForServiceLocked(s.info);
            if (p == null) {
                return;
            }
            s.app = p;
        }
        if (s.app.thread == null) {
            if (!s.app.pendingServices.contains(s)) {
                s.app.pendingServices.add(s);
            }
            return;
        }
        try {
            if (!s.created) {
                Slog.i(TAG, "create service " + s);
                s.created = true;
                s.app.thread.scheduleCreateService(s, s.info,
                        CompatibilityInfo.DEFAULT_COMPATIBILITY_INFO,
                        ActivityManager.PROCESS_STATE_SERVICE);
            }
            if (!s.pendingStarts.isEmpty()) {
                List<android.app.ServiceStartArgs> args = new ArrayList<android.app.ServiceStartArgs>();
                for (Intent i : s.pendingStarts) {
                    args.add(new android.app.ServiceStartArgs(false, ++s.lastStartId, 0, i));
                }
                s.app.thread.scheduleServiceArgs(s,
                        new android.content.pm.ParceledListSlice<android.app.ServiceStartArgs>(args));
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
            String resolvedType, boolean requireForeground, String callingPackage, int userId) {
        // API 26 startForegroundService: the service is started as any other;
        // its startForeground (setServiceForeground) is accepted whenever it
        // comes, so there is no ANR for a late one.
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

    /** Context.bindService of 10 (instanceName: API 29's isolated instances; one process). */
    @Override
    public int bindIsolatedService(IApplicationThread caller, IBinder token, Intent service,
            String resolvedType, IServiceConnection connection, int flags, String instanceName,
            String callingPackage, int userId) {
        return bindService(caller, token, service, resolvedType, connection, flags,
                callingPackage, userId);
    }

    @Override
    public void updateServiceGroup(IServiceConnection connection, int group, int importance) {}

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
            b.connections.add(proxyFor(connection));
            if ((flags & android.content.Context.BIND_AUTO_CREATE) != 0) {
                b.autoCreate = true;
            }
            List<ServiceRecord> list = mServiceConnections.get(connection.asBinder());
            if (list == null) {
                list = new ArrayList<ServiceRecord>();
                mServiceConnections.put(connection.asBinder(), list);
            }
            list.add(s);
            if (b.received) {
                try {
                    proxyFor(connection).connected(s.name, b.binder, false);
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
            if (s.app != null && s.app.light) {
                Slog.i(TAG, "published " + s + " from " + s.app + ": "
                        + (service == null ? null : service.getClass().getSimpleName()));
            }
            for (IntentBinding b : s.bindings.values()) {
                if (new Intent.FilterComparison(b.intent).equals(fc)) {
                    b.binder = service;
                    b.received = true;
                    for (IServiceConnection c : b.connections) {
                        try {
                            c.connected(s.name, service, false);
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
                    if (b.connections.isEmpty()) {
                        b.autoCreate = false;
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
            android.app.Notification notification, int flags, int foregroundServiceType) {
        synchronized (this) {
            ServiceRecord s = mServices.get(className);
            if (s != null && s == token) {
                // ServiceRecord.foregroundServiceType: the manifest's type when
                // FOREGROUND_SERVICE_TYPE_MANIFEST (-1) is asked for.
                s.foregroundServiceType = notification == null ? 0
                        : foregroundServiceType == -1 ? s.info.getForegroundServiceType()
                        : foregroundServiceType;
            }
        }
        if (notification != null) {
            Slog.i(TAG, "foreground service " + className.flattenToShortString()
                    + " (notification " + id + ", not shown: no status bar)");
        }
    }

    @Override
    public int getForegroundServiceType(ComponentName className, IBinder token) {
        synchronized (this) {
            ServiceRecord s = mServices.get(className);
            return s != null && s == token ? s.foregroundServiceType : 0;
        }
    }

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
    public ContentProviderHolder getContentProvider(IApplicationThread caller,
            String callingPackage, String name, int userId, boolean stable) {
        if (SettingsProvider.AUTHORITY.equals(name)) {
            ProviderInfo info = new ProviderInfo();
            info.authority = name;
            info.name = SettingsProvider.class.getName();
            info.packageName = "android";
            info.applicationInfo = new ApplicationInfo();
            info.applicationInfo.packageName = "android";
            info.applicationInfo.uid = Process.SYSTEM_UID;
            info.exported = true;
            ContentProviderHolder holder =
                    new ContentProviderHolder(info);
            holder.provider = mSettings;
            holder.noReleaseNeeded = true;
            return holder;
        }
        if (FilesProvider.AUTHORITY.equals(name)) {
            return systemProvider(name, FilesProvider.class.getName(), mFiles);
        }
        synchronized (this) {
            ContentProviderHolder h = publishedProviderLocked(name);
            if (h != null) {
                return h;
            }
            // ActivityManagerService.getContentProviderImpl: a provider of a
            // process that is not running (or not yet published): start the
            // process and wait for it to publish (CONTENT_PROVIDER_PUBLISH_TIMEOUT).
            ProviderInfo pi;
            try {
                pi = mPackageManager.resolveContentProvider(name, STOCK_PM_FLAGS, userId);
            } catch (RemoteException e) {
                pi = null;
            }
            if (pi == null || isStrandedGoogle(pi) || (!isGoogleProcess(pi.applicationInfo)
                    && !Binder.sArcLight && !LightProcess.enabledFor(mAppPackage))) {
                // (One process per app: its providers come with its binding.
                // The user's Google packages' -- microG's Gservices, settings,
                // chimera -- in a light process of theirs.)
                Slog.w(TAG, "Failed to find provider info for " + name);
                return null;
            }
            String processName = processNameOf(pi);
            if (pi.multiprocess && pi.packageName.equals(mAppPackage)
                    && processForPidLocked(Binder.getCallingPid()) != null) {
                // android:multiprocess: the caller makes its own instance.
                ContentProviderHolder holder = new ContentProviderHolder(pi);
                holder.noReleaseNeeded = true;
                return holder;
            }
            ProcessRecord app = mProcesses.get(processName);
            if (app == null || app.dead) {
                if (!canStartLocked(processName, pi.applicationInfo)) {
                    Slog.w(TAG, "provider " + name + ": not starting " + processName);
                    return null;
                }
                app = startProcessLocked(processName, pi.applicationInfo);
            }
            if (app.pid == Binder.getCallingPid()) {
                // Its own process asks before publishing it (ContentProviderRecord.
                // canRunHere): the caller makes the instance itself.
                ContentProviderHolder holder = new ContentProviderHolder(pi);
                holder.noReleaseNeeded = true;
                return holder;
            }
            long until = SystemClock.uptimeMillis() + 20000;
            while (!app.dead && app.providers.get(name) == null) {
                long left = until - SystemClock.uptimeMillis();
                if (left <= 0) {
                    Slog.w(TAG, "Timeout waiting for provider " + name + " in " + app);
                    return null;
                }
                try {
                    wait(left);
                } catch (InterruptedException e) {
                    return null;
                }
            }
            return publishedProviderLocked(name);
        }
    }

    /** The page's files (topic app-files): content://arcwasm.files. */
    private final FilesProvider mFiles = new FilesProvider();

    /** A holder of a provider of the system's ("android"). */
    private static ContentProviderHolder systemProvider(String authority, String className,
            android.content.IContentProvider provider) {
        ProviderInfo info = new ProviderInfo();
        info.authority = authority;
        info.name = className;
        info.packageName = "android";
        info.applicationInfo = new ApplicationInfo();
        info.applicationInfo.packageName = "android";
        info.applicationInfo.uid = Process.SYSTEM_UID;
        info.exported = true;
        info.grantUriPermissions = true;
        ContentProviderHolder holder = new ContentProviderHolder(info);
        holder.provider = provider;
        holder.noReleaseNeeded = true;
        return holder;
    }

    /** A holder of published provider |name| for the calling process. */
    private ContentProviderHolder publishedProviderLocked(String name) {
        for (ProcessRecord app : mProcesses.values()) {
            ContentProviderHolder h = app.providers.get(name);
            if (h != null && !app.dead) {
                ContentProviderHolder holder = new ContentProviderHolder(h.info);
                holder.provider = providerFor(h.provider, app);
                holder.noReleaseNeeded = true;
                return holder;
            }
        }
        return null;
    }

    @Override
    public void publishContentProviders(IApplicationThread caller,
            List<ContentProviderHolder> providers) {
        synchronized (this) {
            // The publishing process: by the calling pid (the main process's
            // is the VM's own).
            ProcessRecord app = processForPidLocked(Binder.getCallingPid());
            if (app == null) {
                for (ProcessRecord p : mProcesses.values()) {
                    if (p.thread != null && !p.dead && !p.light) {
                        app = p;
                    }
                }
            }
            if (app == null || providers == null) {
                return;
            }
            for (ContentProviderHolder h : providers) {
                if (h == null || h.info == null || h.info.authority == null) {
                    continue;
                }
                for (String authority : h.info.authority.split(";")) {
                    app.providers.put(authority, h);
                }
                Slog.i(TAG, "published provider " + h.info.authority
                        + (app.light ? " in " + app : ""));
            }
            notifyAll();
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
        ContentProviderHolder h = getContentProvider(null, "android", uri.getAuthority(),
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

    /**
     * The one global configuration, for the window manager's relayout. Not under
     * this service's lock: relayout holds the window manager's, and a start
     * from another thread holds this one and then wants the window manager's
     * (addAppToken) -- F-Droid's main thread and its pool thread deadlocked.
     * mConfiguration is set once, in the constructor.
     */
    Configuration globalConfiguration() {
        return new Configuration(mConfiguration);
    }

    SettingsProvider settings() {
        return mSettings;
    }
}
