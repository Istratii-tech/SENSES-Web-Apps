package org.arcwasm.testapp;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.ViewTreeObserver;
import android.widget.Button;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;

/**
 * The launcher activity: its layout is res/layout/main.xml, the title gets
 * a formatted string from resources, a file is written to getFilesDir(),
 * the button counts clicks. Every lifecycle callback is recorded
 * (TestApplication.EVENTS).
 */
public class MainActivity extends Activity implements View.OnClickListener {
    public int clicks;

    public static String title() {
        return "MainActivity of " + MainActivity.class.getPackage().getName();
    }

    private void event(String what) {
        TestApplication.event("MainActivity." + what);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        event("onCreate");
        TestApplication.LIVE.put(getClass().getSimpleName(), this);
        setContentView(R.layout.main);
        TextView title = (TextView) findViewById(R.id.title);
        title.setText(getString(R.string.greeting, getPackageName(), 3));
        final Button button = (Button) findViewById(R.id.button);
        button.setOnClickListener(this);
        // Where the button is on the screen, once laid out: the browser
        // page's self-test taps there (graphics-web/app.html?selftest=1).
        button.getViewTreeObserver().addOnGlobalLayoutListener(
                new ViewTreeObserver.OnGlobalLayoutListener() {
                    public void onGlobalLayout() {
                        if (button.getWidth() == 0) return;
                        button.getViewTreeObserver().removeGlobalOnLayoutListener(this);
                        int[] at = new int[2];
                        button.getLocationOnScreen(at);
                        Log.i("MainActivity", "ready button=" + (at[0] + button.getWidth() / 2)
                                + "," + (at[1] + button.getHeight() / 2));
                    }
                });
        try {
            FileOutputStream out = new FileOutputStream(new File(getFilesDir(), "hello.txt"));
            out.write(("written by " + title()).getBytes("UTF-8"));
            out.close();
        } catch (Exception e) {
            event("files: " + e);
        }
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

    @Override
    public void onClick(View v) {
        clicks++;
        Log.i("MainActivity", "clicks=" + clicks);
        ((TextView) findViewById(R.id.title)).setText(getResources().getQuantityString(
                R.plurals.apples, clicks, clicks));
    }
}
