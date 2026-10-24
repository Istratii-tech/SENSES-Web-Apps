package org.arcwasm.splittest;

import android.app.Activity;
import android.content.Context;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.Bundle;
import android.util.TypedValue;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;

/**
 * Reports what it got from its parts (arcwasm.SplitInstallTest reads it):
 * files/report, one "key=value" line each, written whole at each start:
 *   version   its versionCode (the package manager's)
 *   density   R.string.density_tag: "mdpi"/"xhdpi" from a density part,
 *             "base" if no part's resources were loaded
 *   dot       the file R.drawable.dot comes from and its size in bytes
 *             (read through the asset manager: the part's zip)
 *   icon      the same for the launcher icon (only in the density parts)
 *   ru        R.string.greeting in Russian (the language part)
 *   feature   a class of split_feature.apk's dex, or the error
 *   lib       System.loadLibrary("ndkhelper") from the ABI part
 * and appends its versionCode to files/launches (what the test waits for).
 */
public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView text = new TextView(this);
        text.setText(R.string.app_name);
        setContentView(text);
        StringBuilder r = new StringBuilder();
        int version = -1;
        try {
            version = getPackageManager().getPackageInfo(getPackageName(), 0).versionCode;
        } catch (Exception e) {
            // -1
        }
        r.append("version=").append(version).append('\n');
        r.append("density=").append(getString(R.string.density_tag)).append('\n');
        r.append("dot=").append(file(R.drawable.dot)).append('\n');
        r.append("icon=").append(file(R.drawable.ic_launcher)).append('\n');
        try {
            Configuration ru = new Configuration(getResources().getConfiguration());
            ru.setLocale(new Locale("ru"));
            Context c = createConfigurationContext(ru);
            r.append("ru=").append(c.getString(R.string.greeting)).append('\n');
        } catch (Throwable t) {
            r.append("ru=").append(t).append('\n');
        }
        try {
            Class<?> f = Class.forName("org.arcwasm.splittest.feature.Feature");
            r.append("feature=").append(f.getMethod("hello").invoke(null)).append('\n');
        } catch (Throwable t) {
            r.append("feature=").append(t).append('\n');
        }
        try {
            System.loadLibrary("ndkhelper");
            r.append("lib=loaded\n");
        } catch (Throwable t) {
            r.append("lib=").append(t).append('\n');
        }
        try {
            write("report", r.toString(), false);
            write("launches", version + "\n", true);
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
    }

    /** "res/drawable-mdpi-v4/dot.png 73": a resource's file and its size. */
    private String file(int id) {
        try {
            Resources res = getResources();
            TypedValue v = new TypedValue();
            res.getValue(id, v, true);
            InputStream in = res.openRawResource(id);
            int n = 0;
            try {
                byte[] buf = new byte[4096];
                for (int k; (k = in.read(buf)) > 0;) {
                    n += k;
                }
            } finally {
                in.close();
            }
            return v.string + " " + n;
        } catch (Throwable t) {
            return t.toString();
        }
    }

    private void write(String name, String text, boolean append) throws IOException {
        FileOutputStream out = new FileOutputStream(new File(getFilesDir(), name), append);
        try {
            out.write(text.getBytes("UTF-8"));
        } finally {
            out.close();
        }
    }
}
