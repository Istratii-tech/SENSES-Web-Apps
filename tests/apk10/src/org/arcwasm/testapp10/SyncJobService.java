package org.arcwasm.testapp10;

import android.app.job.JobParameters;
import android.app.job.JobService;
import android.app.job.JobWorkItem;

/**
 * A JobService (JobScheduler, API 21; work items, API 26): job 1 finishes at
 * once, job 2 takes its enqueued work items one by one on a thread.
 */
public class SyncJobService extends JobService {
    @Override
    public boolean onStartJob(final JobParameters params) {
        App10.event("SyncJobService.onStartJob " + params.getJobId() + " "
                + params.getExtras().getString("what", ""));
        if (params.getJobId() != 2) {
            return false;  // done
        }
        new Thread(() -> {
            JobWorkItem w;
            while ((w = params.dequeueWork()) != null) {
                App10.event("work " + w.getIntent().getStringExtra("item"));
                params.completeWork(w);
            }
            App10.event("SyncJobService.jobFinished 2");
            jobFinished(params, false);
        }).start();
        return true;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        App10.event("SyncJobService.onStopJob " + params.getJobId());
        return false;
    }
}
