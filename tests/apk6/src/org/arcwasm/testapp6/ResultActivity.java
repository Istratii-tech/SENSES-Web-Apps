package org.arcwasm.testapp6;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/** Answers MainActivity's startActivityForResult: RESULT_OK with "answer". */
public class ResultActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        App6.event("ResultActivity.onCreate");
        App6.LIVE.put("ResultActivity", this);
        setResult(RESULT_OK, new Intent().putExtra("answer",
                getIntent().getIntExtra("question", 0)));
    }

    @Override
    protected void onResume() {
        super.onResume();
        App6.event("ResultActivity.onResume");
        finish();
    }

    @Override
    protected void onPause() {
        App6.event("ResultActivity.onPause");
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        App6.event("ResultActivity.onDestroy");
        App6.LIVE.remove("ResultActivity");
        super.onDestroy();
    }
}
