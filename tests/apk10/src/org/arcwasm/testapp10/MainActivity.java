package org.arcwasm.testapp10;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.ViewTreeObserver;
import android.widget.Button;
import android.widget.TextView;

/**
 * The launcher activity: Theme.Material, a button that counts taps (a lambda
 * as its listener). Prints "MainActivity: ready button=X,Y" (the button's
 * centre on the display) once drawn and "MainActivity: clicks=N" per tap,
 * for the page's self-test (fw8-system-web/app.html). The count is the
 * activity's saved state: it survives the VM's restart for memory (topic
 * mem-budget), as on Android it survives the process being killed.
 */
public class MainActivity extends Activity {
    private int mClicks;
    private TextView mCounter;
    private Button mButton;
    private boolean mReadyReported;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState != null) {
            mClicks = savedInstanceState.getInt("clicks");
            App10.event("MainActivity.onCreate restored clicks=" + mClicks);
        } else {
            App10.event("MainActivity.onCreate");
        }
        App10.LIVE.put("MainActivity", this);
        setContentView(R.layout.main);
        ((TextView) findViewById(R.id.hello)).setText(
                getString(R.string.hello, Build.VERSION.SDK_INT) + " (" + getString(R.string.sdk_note) + ")");
        mCounter = findViewById(R.id.counter);  // API 26: generic findViewById
        mButton = findViewById(R.id.button);
        updateCounter();
        mButton.setOnClickListener(v -> {
            mClicks++;
            updateCounter();
            App10.event("MainActivity.onClick " + mClicks);
            System.out.println("MainActivity: clicks=" + mClicks);
        });
        final View decor = getWindow().getDecorView();
        decor.getViewTreeObserver().addOnPreDrawListener(new ViewTreeObserver.OnPreDrawListener() {
            @Override
            public boolean onPreDraw() {
                if (!mReadyReported && mButton.getWidth() > 0) {
                    mReadyReported = true;
                    decor.post(() -> {
                        int[] xy = new int[2];
                        mButton.getLocationOnScreen(xy);
                        System.out.println("MainActivity: ready button="
                                + (xy[0] + mButton.getWidth() / 2) + ","
                                + (xy[1] + mButton.getHeight() / 2));
                    });
                }
                return true;
            }
        });
    }

    private void updateCounter() {
        mCounter.setText(getString(R.string.clicks, mClicks));
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putInt("clicks", mClicks);
        App10.event("MainActivity.onSaveInstanceState clicks=" + mClicks);
    }

    @Override
    protected void onStop() {
        App10.event("MainActivity.onStop");
        super.onStop();
    }

    @Override
    protected void onResume() {
        super.onResume();
        App10.event("MainActivity.onResume");
    }

    @Override
    protected void onPause() {
        App10.event("MainActivity.onPause");
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        App10.event("MainActivity.onDestroy");
        App10.LIVE.remove("MainActivity");
        super.onDestroy();
    }
}
