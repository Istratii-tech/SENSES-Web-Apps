package org.arcwasm.testapp10;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;

/** Theme.NoDisplay: answers its caller and finishes in onCreate, as such activities must. */
public class ResultActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        App10.event("ResultActivity.onCreate " + getIntent().getStringExtra("question")
                + " from " + (getCallingActivity() == null ? null
                        : getCallingActivity().getShortClassName()));
        setResult(RESULT_OK, new Intent().putExtra("answer", "sdk " + Build.VERSION.SDK_INT));
        finish();
    }

    @Override
    protected void onDestroy() {
        App10.event("ResultActivity.onDestroy");
        super.onDestroy();
    }
}
