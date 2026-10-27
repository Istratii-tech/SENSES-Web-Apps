package org.arcwasm.testapp10;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/**
 * Starts ResultActivity for a result once resumed (as a trampoline or a
 * login flow does), takes the answer in onActivityResult and finishes: an
 * activity that never shows a window, so its lifecycle runs on a VM without
 * graphics too. Records its callbacks, onTopResumedActivityChanged (API 29)
 * among them.
 */
public class TrampolineActivity extends Activity {
    private boolean mAsked;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        App10.event("TrampolineActivity.onCreate " + getIntent().getStringExtra("why"));
    }

    @Override
    protected void onResume() {
        super.onResume();
        App10.event("TrampolineActivity.onResume");
        if (!mAsked) {
            mAsked = true;
            // From onResume: the window would be hidden for now (another
            // activity is starting), so none is made yet.
            Intent ask = getIntent().getParcelableExtra("ask");
            if (ask != null) {
                // A system screen of files (topic app-files): an open or a
                // save, answered by the page (AppFilesTest10).
                startActivityForResult(ask, 8);
                return;
            }
            startActivityForResult(new Intent(this, ResultActivity.class)
                    .putExtra("question", "sdk?"), 7);
        }
    }

    @Override
    public void onTopResumedActivityChanged(boolean top) {
        App10.event("TrampolineActivity.onTopResumedActivityChanged " + top);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == 8) {
            App10.event("TrampolineActivity.files " + resultCode + " " + Files.describe(this, data,
                    getIntent().getStringExtra("write")));
            finish();
            return;
        }
        App10.event("TrampolineActivity.onActivityResult " + requestCode + " " + resultCode
                + " " + (data == null ? null : data.getStringExtra("answer")));
        finish();
    }

    @Override
    protected void onPause() {
        App10.event("TrampolineActivity.onPause");
        super.onPause();
    }

    @Override
    protected void onStop() {
        App10.event("TrampolineActivity.onStop");
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        App10.event("TrampolineActivity.onDestroy");
        super.onDestroy();
    }
}
