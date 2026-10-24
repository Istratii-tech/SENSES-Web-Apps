package org.arcwasm.testapp;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

/** Started by MainActivity's startActivity(): the view is built in code. */
public class SecondActivity extends Activity {
    private void event(String what) {
        TestApplication.event("SecondActivity." + what);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        event("onCreate");
        TestApplication.LIVE.put(getClass().getSimpleName(), this);
        TextView text = new TextView(this);
        text.setText(getTitle());
        setContentView(text);
    }

    @Override protected void onStart() { super.onStart(); event("onStart"); }
    @Override protected void onRestart() { super.onRestart(); event("onRestart"); }
    @Override protected void onResume() { super.onResume(); event("onResume"); }
    @Override protected void onPause() { super.onPause(); event("onPause"); }
    @Override protected void onStop() { super.onStop(); event("onStop"); }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        event("onDestroy");
        TestApplication.LIVE.remove(getClass().getSimpleName());
    }
}
