package org.arcwasm.installtest;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

/**
 * Appends its package's versionCode to files/launches at each start (what
 * arcwasm.InstallTest waits for: the data directory outlives updates), and
 * finishes at once when files/finish-on-launch exists (as Back on the last
 * activity: one-app mode's "exit").
 */
public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView text = new TextView(this);
        text.setText(R.string.app_name);
        setContentView(text);
        int version = -1;
        try {
            version = getPackageManager().getPackageInfo(getPackageName(), 0).versionCode;
        } catch (Exception e) {
            // -1
        }
        try {
            FileOutputStream out = new FileOutputStream(new File(getFilesDir(), "launches"), true);
            try {
                out.write((version + "\n").getBytes("UTF-8"));
            } finally {
                out.close();
            }
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
        if (new File(getFilesDir(), "finish-on-launch").exists()) {
            finish();
        }
    }
}
