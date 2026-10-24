package org.arcwasm.testapp;

import android.app.Activity;
import android.app.Application;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The APK's Application (android:name in the manifest). Records the
 * lifecycle callbacks of the app's components in order, for
 * arcwasm.ActivityTest, which reads EVENTS and LIVE by reflection.
 */
public class TestApplication extends Application {
    public static final List<String> EVENTS = Collections.synchronizedList(new ArrayList<String>());
    /** The live activities by class name (onCreate to onDestroy). */
    public static final Map<String, Activity> LIVE =
            Collections.synchronizedMap(new HashMap<String, Activity>());

    public static void event(String what) {
        EVENTS.add(what);
        Log.i("TestApp", what);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        event("TestApplication.onCreate " + getPackageName());
    }
}
