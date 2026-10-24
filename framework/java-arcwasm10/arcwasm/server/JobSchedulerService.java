// arc-wasm: the "jobscheduler" service on Android 8.1 (IJobScheduler, API 26),
// lean, but jobs do run: WorkManager, Firebase JobDispatcher and apps' own
// JobServices schedule their work here.
//
// What JobSchedulerService + JobServiceContext do, reduced to one device
// state that never changes: always on mains power and charged, storage and
// battery not low, on an unmetered Wi-Fi network (ConnectivityService), and
// never idle (the page is in use). A job runs when its constraints are met
// and its minimum latency has passed, or at its deadline:
//
//   run    bindService to the job's JobService in the app process (through
//          the activity manager, as JobServiceContext does), then
//          IJobService.startJob(JobParameters) with an IJobCallback;
//          jobFinished/acknowledgeStartMessage(ongoing = false) unbind it
//   work   enqueue(job, JobWorkItem) of API 26: work is queued per job and
//          handed out by IJobCallback.dequeueWork, the job runs while there
//          is work
//   again  periodic jobs every interval; jobFinished(reschedule) after the
//          job's backoff (linear or exponential, as JobStatus computes it)
//   content TriggerContentUri: the job runs when ContentService reports a
//          change under one of its URIs (with the URIs and authorities)
//
// Jobs needing a metered (cellular) network or an idle device wait for their deadline
// (if any). No persistence across VM runs, no execution time limit.
package arcwasm.server;

import android.app.job.IJobCallback;
import android.app.job.IJobService;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobWorkItem;
import android.content.ComponentName;
import android.content.Context;
import android.content.IContentService;
import android.content.Intent;
import android.database.IContentObserver;
import android.net.Uri;
import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.util.Slog;

import arcwasm.server.base.JobSchedulerBase;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedList;
import java.util.List;
import java.util.Map;

public final class JobSchedulerService extends JobSchedulerBase {
    static final String TAG = "JobScheduler";
    /** JobInfo's minimum period (API 24: 15 min), as JobInfo.Builder enforces. */
    static final long MIN_PERIOD_MS = 15 * 60 * 1000L;

    /** JobStatus: a job, its work and where it is. */
    final class Job {
        final JobInfo info;
        final String callingPackage;
        final LinkedList<JobWorkItem> work = new LinkedList<JobWorkItem>();
        final List<JobWorkItem> delivered = new ArrayList<JobWorkItem>();
        int nextWorkId = 1;
        int failures;
        /** Earliest and latest run, SystemClock.uptimeMillis(); 0 = none. */
        long earliest, latest;
        boolean running;
        boolean canceled;
        IJobService service;
        Connection connection;
        Uri[] triggeredUris;
        String[] triggeredAuthorities;
        final List<IContentObserver> observers = new ArrayList<IContentObserver>();
        final Runnable runner = new Runnable() {
            public void run() {
                maybeRun(Job.this, false);
            }
        };
        final Runnable deadline = new Runnable() {
            public void run() {
                maybeRun(Job.this, true);
            }
        };

        Job(JobInfo info, String callingPackage) {
            this.info = info;
            this.callingPackage = callingPackage;
        }

        /** The device state above: does it meet this job's constraints? */
        boolean constraintsMet() {
            int net = info.getNetworkType();
            if (net == JobInfo.NETWORK_TYPE_METERED) {
                return false;  // unmetered Wi-Fi only
            }
            if (info.isRequireDeviceIdle()) {
                return false;  // never idle
            }
            if (info.getTriggerContentUris() != null && triggeredUris == null) {
                return false;  // no change seen yet
            }
            return true;
        }

        @Override
        public String toString() {
            return "job " + info.getId() + " (" + info.getService().flattenToShortString() + ")";
        }
    }

    /** By job id: one app process, so the ids of one uid. */
    private final Map<Integer, Job> mJobs = new LinkedHashMap<Integer, Job>();

    // --- IJobScheduler ----------------------------------------------------------------------

    @Override
    public int schedule(JobInfo job) {
        return scheduleAsPackage(job, job.getService().getPackageName(), 0, null);
    }

    @Override
    public int scheduleAsPackage(JobInfo info, String packageName, int userId, String tag) {
        synchronized (this) {
            Job old = mJobs.get(info.getId());
            Job j = new Job(info, packageName);
            if (old != null) {
                // JobSchedulerService.scheduleAsPackage: work enqueued on the
                // old job carries over when the job is the same.
                if (old.info.equals(info)) {
                    j.work.addAll(old.work);
                    j.nextWorkId = old.nextWorkId;
                }
                cancelLocked(old, false);
            }
            mJobs.put(info.getId(), j);
            Slog.i(TAG, "scheduled " + j);
            planLocked(j, 0);
            return JobScheduler.RESULT_SUCCESS;
        }
    }

    @Override
    public int enqueue(JobInfo info, JobWorkItem work) {
        synchronized (this) {
            Job j = mJobs.get(info.getId());
            if (j == null || !j.info.equals(info) || j.canceled) {
                if (j != null) {
                    cancelLocked(j, false);
                }
                j = new Job(info, info.getService().getPackageName());
                mJobs.put(info.getId(), j);
                planLocked(j, 0);
            }
            work.setWorkId(j.nextWorkId++);
            j.work.add(work);
            if (!j.running) {
                LeanServices.handler().post(j.runner);
            }
            return JobScheduler.RESULT_SUCCESS;
        }
    }

    @Override
    public void cancel(int jobId) {
        synchronized (this) {
            Job j = mJobs.get(jobId);
            if (j != null) {
                cancelLocked(j, true);
            }
        }
    }

    @Override
    public void cancelAll() {
        synchronized (this) {
            for (Job j : new ArrayList<Job>(mJobs.values())) {
                cancelLocked(j, true);
            }
        }
    }

    @Override
    public synchronized android.content.pm.ParceledListSlice getAllPendingJobs() {
        List<JobInfo> out = new ArrayList<JobInfo>();
        for (Job j : mJobs.values()) {
            out.add(j.info);
        }
        return new android.content.pm.ParceledListSlice<JobInfo>(out);
    }

    @Override
    public synchronized JobInfo getPendingJob(int jobId) {
        Job j = mJobs.get(jobId);
        return j != null ? j.info : null;
    }

    // --- planning -----------------------------------------------------------------------------

    /** When the job may and must run next (JobStatus's earliest/latest run times). */
    private void planLocked(final Job j, long backoffMs) {
        long now = SystemClock.uptimeMillis();
        JobInfo info = j.info;
        if (info.isPeriodic()) {
            long period = Math.max(info.getIntervalMillis(), MIN_PERIOD_MS);
            long flex = Math.min(info.getFlexMillis(), period);
            // The first run of a periodic job comes at once (its window is open).
            j.earliest = backoffMs > 0 ? now + backoffMs : now;
            j.latest = j.earliest + period + flex;
        } else {
            j.earliest = now + Math.max(info.getMinLatencyMillis(), backoffMs);
            j.latest = info.hasLateConstraint() ? now + info.getMaxExecutionDelayMillis() : 0;
        }
        if (info.getTriggerContentUris() != null && j.observers.isEmpty()) {
            observeContent(j);
        }
        android.os.Handler h = LeanServices.handler();
        h.removeCallbacks(j.runner);
        h.removeCallbacks(j.deadline);
        h.postAtTime(j.runner, Math.max(j.earliest, now));
        if (j.latest > 0) {
            h.postAtTime(j.deadline, j.latest);
        }
    }

    /** After a run: the next period, a backoff, or done. */
    private void rescheduleLocked(Job j, boolean wantsReschedule) {
        if (j.canceled) {
            return;
        }
        if (wantsReschedule) {
            j.failures++;
            long initial = j.info.getInitialBackoffMillis();
            long backoff = j.info.getBackoffPolicy() == JobInfo.BACKOFF_POLICY_LINEAR
                    ? initial * j.failures : initial << Math.min(j.failures - 1, 20);
            planLocked(j, Math.min(backoff, 5 * 60 * 60 * 1000L /* JobInfo.MAX_BACKOFF_DELAY */));
        } else if (j.info.isPeriodic()) {
            j.failures = 0;
            j.triggeredUris = null;
            planLocked(j, Math.max(j.info.getIntervalMillis(), MIN_PERIOD_MS));
        } else if (!j.work.isEmpty()) {
            LeanServices.handler().post(j.runner);
        } else if (j.info.getTriggerContentUris() != null) {
            j.triggeredUris = null;
            j.triggeredAuthorities = null;  // waits for the next change
        } else {
            mJobs.remove(j.info.getId());
            unobserveContent(j);
        }
    }

    private void cancelLocked(Job j, boolean stopRunning) {
        j.canceled = true;
        mJobs.remove(j.info.getId());
        LeanServices.handler().removeCallbacks(j.runner);
        LeanServices.handler().removeCallbacks(j.deadline);
        unobserveContent(j);
        if (j.running && stopRunning && j.service != null) {
            try {
                j.service.stopJob(params(j));
            } catch (RemoteException e) {
                // Gone.
            }
            finishLocked(j);
        }
    }

    // --- running --------------------------------------------------------------------------------

    // (The activity manager calls the connection back holding its own lock:
    // this service never calls it holding this one.)
    private void maybeRun(Job j, boolean deadlinePassed) {
        Connection c;
        synchronized (this) {
            if (j.canceled || j.running || mJobs.get(j.info.getId()) != j) {
                return;
            }
            boolean ready = SystemClock.uptimeMillis() >= j.earliest && j.constraintsMet();
            if (!ready && !deadlinePassed) {
                return;
            }
            j.running = true;
            c = j.connection = new Connection(j, deadlinePassed);
        }
        Intent intent = new Intent().setComponent(j.info.getService());
        // JobServiceContext.executeRunnableJob.
        int ok = SystemServer.activityManager().bindService(null, null, intent, null, c,
                Context.BIND_AUTO_CREATE | Context.BIND_NOT_FOREGROUND, "android", 0);
        if (ok == 0) {
            Slog.w(TAG, "cannot bind to " + j.info.getService().flattenToShortString()
                    + " for " + j);
            synchronized (this) {
                if (j.connection == c) {
                    j.running = false;
                    j.connection = null;
                }
            }
        }
    }

    private JobParameters params(Job j) {
        return new JobParameters(j.connection.callback, j.info.getId(), j.info.getExtras(),
                j.info.getTransientExtras(), j.info.getClipData(), j.info.getClipGrantFlags(),
                j.connection.deadlinePassed, j.triggeredUris, j.triggeredAuthorities,
                // API 28: the network the job may use (the one there is).
                j.info.getRequiredNetwork() != null
                        ? new android.net.Network(ConnectivityService.NET_ID) : null);
    }

    /** The binding to the JobService, and its IJobCallback. */
    private final class Connection extends android.app.IServiceConnection.Stub {
        final Job job;
        final boolean deadlinePassed;
        final Callback callback;

        Connection(Job job, boolean deadlinePassed) {
            this.job = job;
            this.deadlinePassed = deadlinePassed;
            this.callback = new Callback(job);
        }

        @Override
        public void connected(ComponentName name, IBinder service, boolean dead) {
            JobParameters params;
            synchronized (JobSchedulerService.this) {
                if (!job.running || job.connection != this || service == null) {
                    return;
                }
                job.service = IJobService.Stub.asInterface(service);
                params = params(job);
            }
            Slog.i(TAG, "starting " + job);
            try {
                job.service.startJob(params);
            } catch (RemoteException e) {
                synchronized (JobSchedulerService.this) {
                    finishLocked(job);
                }
            }
        }
    }

    private final class Callback extends IJobCallback.Stub {
        final Job job;

        Callback(Job job) {
            this.job = job;
        }

        @Override
        public void acknowledgeStartMessage(int jobId, boolean ongoing) {
            if (!ongoing) {
                jobFinished(jobId, false);
            }
        }

        @Override
        public void acknowledgeStopMessage(int jobId, boolean reschedule) {
            jobFinished(jobId, reschedule);
        }

        @Override
        public JobWorkItem dequeueWork(int jobId) {
            synchronized (JobSchedulerService.this) {
                JobWorkItem w = job.work.poll();
                if (w != null) {
                    w.bumpDeliveryCount();
                    job.delivered.add(w);
                }
                return w;
            }
        }

        @Override
        public boolean completeWork(int jobId, int workId) {
            synchronized (JobSchedulerService.this) {
                for (int i = 0; i < job.delivered.size(); i++) {
                    if (job.delivered.get(i).getWorkId() == workId) {
                        job.delivered.remove(i);
                        return true;
                    }
                }
                return false;
            }
        }

        @Override
        public void jobFinished(int jobId, boolean reschedule) {
            synchronized (JobSchedulerService.this) {
                if (!job.running) {
                    return;
                }
                Slog.i(TAG, "finished " + job + (reschedule ? ", reschedule" : ""));
                // Work handed out and not completed is delivered again.
                job.work.addAll(0, job.delivered);
                job.delivered.clear();
                finishLocked(job);
                rescheduleLocked(job, reschedule);
            }
        }
    }

    private void finishLocked(Job j) {
        j.running = false;
        j.service = null;
        final Connection c = j.connection;
        j.connection = null;
        if (c != null) {
            LeanServices.handler().post(new Runnable() {
                public void run() {
                    SystemServer.activityManager().unbindService(c);
                }
            });
        }
    }

    // --- content triggers ---------------------------------------------------------------------

    private void observeContent(final Job j) {
        IContentService cs = IContentService.Stub.asInterface(ServiceManager.getService("content"));
        if (cs == null) {
            return;
        }
        for (final JobInfo.TriggerContentUri t : j.info.getTriggerContentUris()) {
            IContentObserver o = new IContentObserver.Stub() {
                @Override
                public void onChange(boolean selfUpdate, Uri uri, int userId) {
                    synchronized (JobSchedulerService.this) {
                        if (j.canceled) {
                            return;
                        }
                        j.triggeredUris = new Uri[] {uri};
                        j.triggeredAuthorities = new String[] {uri.getAuthority()};
                        LeanServices.handler().postDelayed(j.runner,
                                j.info.getTriggerContentUpdateDelay());
                    }
                }
            };
            try {
                cs.registerContentObserver(t.getUri(),
                        (t.getFlags() & JobInfo.TriggerContentUri.FLAG_NOTIFY_FOR_DESCENDANTS) != 0,
                        o, 0, 26);
                j.observers.add(o);
            } catch (RemoteException e) {
                // In process: not thrown.
            }
        }
    }

    private void unobserveContent(Job j) {
        IContentService cs = IContentService.Stub.asInterface(ServiceManager.getService("content"));
        for (IContentObserver o : j.observers) {
            try {
                if (cs != null) {
                    cs.unregisterContentObserver(o);
                }
            } catch (RemoteException e) {
                // In process: not thrown.
            }
        }
        j.observers.clear();
    }

    /** For tests: the ids of the jobs waiting or running. */
    public synchronized List<Integer> jobIds() {
        return new ArrayList<Integer>(mJobs.keySet());
    }
}
