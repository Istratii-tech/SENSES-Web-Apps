// arc-wasm: the "activity_task" service of the in-process system server on
// Android 10 (docs/topics/fw10-system.md). In 10 activities and tasks moved
// from IActivityManager to IActivityTaskManager (ActivityTaskManagerService
// in system_server, which shares the activity manager's lock); the app's
// Activity, Instrumentation and ActivityThread call it for starts,
// lifecycle reports and task queries. Here it is a front for the lean
// activity manager, which keeps activities, tasks and processes in one
// place, as the 8.1 one did.
package arcwasm.server;

import android.app.ActivityManager;
import android.app.IApplicationThread;
import android.app.IRequestFinishCallback;
import android.app.ITaskStackListener;
import android.app.PictureInPictureParams;
import android.app.ProfilerInfo;
import android.content.ComponentName;
import android.content.IIntentSender;
import android.content.Intent;
import android.content.pm.ConfigurationInfo;
import android.content.pm.ParceledListSlice;
import android.content.res.Configuration;
import android.graphics.Point;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.IBinder;
import android.os.PersistableBundle;
import android.os.RemoteException;
import android.util.Slog;

import arcwasm.server.base.ActivityTaskManagerBase;

import java.util.List;

public final class ActivityTaskManagerService extends ActivityTaskManagerBase {
    static final String TAG = "ActivityTaskManager";

    private final ActivityManagerService mAm;

    ActivityTaskManagerService(ActivityManagerService am) {
        mAm = am;
    }

    // --- starting activities ----------------------------------------------------------

    @Override
    public int startActivity(IApplicationThread caller, String callingPackage, Intent intent,
            String resolvedType, IBinder resultTo, String resultWho, int requestCode,
            int flags, ProfilerInfo profilerInfo, Bundle options) {
        return mAm.startActivity(caller, callingPackage, intent, resolvedType, resultTo,
                resultWho, requestCode, flags, profilerInfo, options);
    }

    @Override
    public int startActivityAsUser(IApplicationThread caller, String callingPackage,
            Intent intent, String resolvedType, IBinder resultTo, String resultWho,
            int requestCode, int flags, ProfilerInfo profilerInfo, Bundle options, int userId) {
        return mAm.startActivityAsUser(caller, callingPackage, intent, resolvedType, resultTo,
                resultWho, requestCode, flags, profilerInfo, options, userId);
    }

    /** TaskStackBuilder, PendingIntent.getActivities: one after the other, the last on top. */
    @Override
    public int startActivities(IApplicationThread caller, String callingPackage,
            Intent[] intents, String[] resolvedTypes, IBinder resultTo, Bundle options,
            int userId) {
        int res = ActivityManager.START_SUCCESS;
        for (int i = 0; i < intents.length; i++) {
            res = mAm.startActivityAsUser(caller, callingPackage, intents[i],
                    resolvedTypes == null ? null : resolvedTypes[i], resultTo, null, -1, 0,
                    null, i == intents.length - 1 ? options : null, userId);
            if (res < 0) {
                break;
            }
        }
        return res;
    }

    @Override
    public boolean isActivityStartAllowedOnDisplay(int displayId, Intent intent,
            String resolvedType, int userId) {
        return displayId == 0;
    }

    @Override
    public void unhandledBack() {
        mAm.unhandledBack();
    }

    @Override
    public boolean finishActivity(IBinder token, int resultCode, Intent resultData,
            int finishTask) {
        return mAm.finishActivity(token, resultCode, resultData, finishTask);
    }

    @Override
    public boolean finishActivityAffinity(IBinder token) {
        return mAm.finishActivityAffinity(token);
    }

    @Override
    public void finishSubActivity(IBinder token, String resultWho, int requestCode) {
        mAm.finishSubActivity(token, resultWho, requestCode);
    }

    /**
     * Activity.onBackPressed on the root of its task (API 29): the system
     * decides; it asks the activity to finish, as 10 does for a task that is
     * not the home task's.
     */
    @Override
    public void onBackPressedOnTaskRoot(IBinder token, IRequestFinishCallback callback) {
        try {
            callback.requestFinish();
        } catch (RemoteException e) {
            Slog.w(TAG, "requestFinish", e);
        }
    }

    // --- lifecycle reports from the app ----------------------------------------------

    @Override
    public void activityIdle(IBinder token, Configuration config, boolean stopProfiling) {
        mAm.activityIdle(token, config, stopProfiling);
    }

    @Override
    public void activityResumed(IBinder token) {
        mAm.activityResumed(token);
    }

    @Override
    public void activityPaused(IBinder token) {
        mAm.activityPaused(token);
    }

    @Override
    public void activityStopped(IBinder token, Bundle icicle, PersistableBundle persistentState,
            CharSequence description) {
        mAm.activityStopped(token, icicle, persistentState, description);
    }

    @Override
    public void activityDestroyed(IBinder token) {
        mAm.activityDestroyed(token);
    }

    @Override
    public void activityRelaunched(IBinder token) {}

    /** The top resumed activity lost it (onTopResumedActivityChanged(false) ran): nothing waits. */
    @Override
    public void activityTopResumedStateLost() {}

    @Override
    public void activitySlept(IBinder token) {}

    @Override
    public void notifyActivityDrawn(IBinder token) {}

    @Override
    public void reportActivityFullyDrawn(IBinder token, boolean restoredFromBundle) {}

    @Override
    public void reportSizeConfigurations(IBinder token, int[] horizontal, int[] vertical,
            int[] smallest) {}

    @Override
    public boolean willActivityBeVisible(IBinder token) {
        return mAm.willActivityBeVisible(token);
    }

    // --- one activity --------------------------------------------------------------------

    @Override
    public String getCallingPackage(IBinder token) {
        return mAm.getCallingPackage(token);
    }

    @Override
    public ComponentName getCallingActivity(IBinder token) {
        return mAm.getCallingActivity(token);
    }

    @Override
    public ComponentName getActivityClassForToken(IBinder token) {
        return mAm.getActivityClassForToken(token);
    }

    @Override
    public String getPackageForToken(IBinder token) {
        ComponentName c = mAm.getActivityClassForToken(token);
        return c == null ? null : c.getPackageName();
    }

    @Override
    public String getLaunchedFromPackage(IBinder token) {
        return mAm.getLaunchedFromPackage(token);
    }

    @Override
    public int getLaunchedFromUid(IBinder token) {
        return mAm.getLaunchedFromUid(token);
    }

    @Override
    public void setRequestedOrientation(IBinder token, int requestedOrientation) {
        mAm.setRequestedOrientation(token, requestedOrientation);
    }

    @Override
    public int getRequestedOrientation(IBinder token) {
        return mAm.getRequestedOrientation(token);
    }

    @Override
    public void overridePendingTransition(IBinder token, String packageName, int enterAnim,
            int exitAnim) {}

    @Override
    public int getActivityDisplayId(IBinder token) {
        return mAm.getActivityDisplayId(token);
    }

    @Override
    public Bundle getActivityOptions(IBinder token) {
        return mAm.getActivityOptions(token);
    }

    @Override
    public boolean convertFromTranslucent(IBinder token) {
        return mAm.convertFromTranslucent(token);
    }

    @Override
    public boolean convertToTranslucent(IBinder token, Bundle options) {
        return mAm.convertToTranslucent(token, options);
    }

    @Override
    public boolean isImmersive(IBinder token) {
        return mAm.isImmersive(token);
    }

    @Override
    public void setImmersive(IBinder token, boolean immersive) {
        mAm.setImmersive(token, immersive);
    }

    @Override
    public boolean isTopActivityImmersive() {
        return mAm.isTopActivityImmersive();
    }

    @Override
    public boolean isTopOfTask(IBinder token) {
        return mAm.isTopOfTask(token);
    }

    @Override
    public boolean isInMultiWindowMode(IBinder token) {
        return false;
    }

    @Override
    public boolean isInPictureInPictureMode(IBinder token) {
        return false;
    }

    @Override
    public boolean enterPictureInPictureMode(IBinder token, PictureInPictureParams params) {
        return false;
    }

    @Override
    public void setPictureInPictureParams(IBinder token, PictureInPictureParams params) {}

    @Override
    public int getMaxNumPictureInPictureActions(IBinder token) {
        return 0;
    }

    @Override
    public int setVrMode(IBinder token, boolean enabled, ComponentName packageName) {
        return mAm.setVrMode(token, enabled, packageName);
    }

    @Override
    public boolean isRootVoiceInteraction(IBinder token) {
        return false;
    }

    @Override
    public boolean supportsLocalVoiceInteraction() {
        return false;
    }

    @Override
    public boolean releaseActivityInstance(IBinder token) {
        return false;
    }

    @Override
    public void releaseSomeActivities(IApplicationThread app) {}

    @Override
    public void setShowWhenLocked(IBinder token, boolean showWhenLocked) {}

    @Override
    public void setTurnScreenOn(IBinder token, boolean turnScreenOn) {}

    @Override
    public void setDisablePreviewScreenshots(IBinder token, boolean disable) {}

    @Override
    public void setTaskDescription(IBinder token, ActivityManager.TaskDescription values) {}

    @Override
    public void startLockTaskModeByToken(IBinder token) {}

    @Override
    public void stopLockTaskModeByToken(IBinder token) {}

    @Override
    public void showLockTaskEscapeMessage(IBinder token) {}

    @Override
    public void setInheritShowWhenLocked(IBinder token, boolean inherit) {}

    @Override
    public void toggleFreeformWindowingMode(IBinder token) {}

    @Override
    public void startLocalVoiceInteraction(IBinder token, Bundle options) {}

    @Override
    public void stopLocalVoiceInteraction(IBinder token) {}

    @Override
    public boolean showAssistFromActivity(IBinder token, Bundle args) {
        return false;  // no assistant
    }

    /** Activity.dismissKeyguard (API 26): there is no keyguard, so it is dismissed. */
    @Override
    public void dismissKeyguard(IBinder token,
            com.android.internal.policy.IKeyguardDismissCallback callback, CharSequence message) {
        if (callback != null) {
            try {
                callback.onDismissSucceeded();
            } catch (RemoteException e) {
                // The app is gone.
            }
        }
    }

    @Override
    public void stopSystemLockTaskMode() {}

    @Override
    public int getLockTaskModeState() {
        return ActivityManager.LOCK_TASK_MODE_NONE;
    }

    @Override
    public boolean shouldUpRecreateTask(IBinder token, String destAffinity) {
        return mAm.shouldUpRecreateTask(token, destAffinity);
    }

    @Override
    public boolean navigateUpTo(IBinder token, Intent destIntent, int resultCode,
            Intent resultData) {
        return mAm.navigateUpTo(token, destIntent, resultCode, resultData);
    }

    @Override
    public boolean requestAutofillData(android.app.IAssistDataReceiver receiver,
            Bundle receiverExtras, IBinder activityToken, int flags) {
        return false;
    }

    @Override
    public boolean isAssistDataAllowedOnCurrentActivity() {
        return false;
    }

    @Override
    public void keyguardGoingAway(int flags) {}

    @Override
    public void registerRemoteAnimations(IBinder token,
            android.view.RemoteAnimationDefinition definition) {}

    // --- tasks ---------------------------------------------------------------------------

    @Override
    public boolean moveActivityTaskToBack(IBinder token, boolean nonRoot) {
        return mAm.moveActivityTaskToBack(token, nonRoot);
    }

    @Override
    public void moveTaskToFront(IApplicationThread caller, String callingPackage, int taskId,
            int flags, Bundle options) {
        mAm.moveTaskToFront(caller, callingPackage, taskId, flags, options);
    }

    @Override
    public int getTaskForActivity(IBinder token, boolean onlyRoot) {
        return mAm.getTaskForActivity(token, onlyRoot);
    }

    @Override
    public List<ActivityManager.RunningTaskInfo> getTasks(int maxNum) {
        return mAm.getTasks(maxNum);
    }

    @Override
    public ParceledListSlice getRecentTasks(int maxNum, int flags, int userId) {
        return mAm.getRecentTasks(maxNum, flags, userId);
    }

    @Override
    public List<IBinder> getAppTasks(String callingPackage) {
        return mAm.getAppTasks(callingPackage);
    }

    @Override
    public Point getAppTaskThumbnailSize() {
        return mAm.getAppTaskThumbnailSize();
    }

    @Override
    public ActivityManager.TaskDescription getTaskDescription(int taskId) {
        return new ActivityManager.TaskDescription();
    }

    @Override
    public Rect getTaskBounds(int taskId) {
        return new Rect();
    }

    @Override
    public void setFocusedTask(int taskId) {
        mAm.moveTaskToFront(null, null, taskId, 0, null);
    }

    @Override
    public void registerTaskStackListener(ITaskStackListener listener) {}

    @Override
    public void unregisterTaskStackListener(ITaskStackListener listener) {}

    // --- device ----------------------------------------------------------------------------

    @Override
    public ConfigurationInfo getDeviceConfigurationInfo() {
        return mAm.getDeviceConfigurationInfo();
    }

    @Override
    public int getFrontActivityScreenCompatMode() {
        return mAm.getFrontActivityScreenCompatMode();
    }

    @Override
    public void setFrontActivityScreenCompatMode(int mode) {}

    @Override
    public boolean getPackageAskScreenCompat(String packageName) {
        return false;
    }

    @Override
    public int getPackageScreenCompatMode(String packageName) {
        return ActivityManager.COMPAT_MODE_NEVER;
    }

    @Override
    public boolean updateConfiguration(Configuration values) {
        return false;
    }
}
