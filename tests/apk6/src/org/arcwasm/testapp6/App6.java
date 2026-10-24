package org.arcwasm.testapp6;

import android.app.Activity;
import android.app.Application;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The test app's Application (arc-wasm topic fw6-system): records the
 * lifecycle callbacks of the app's components (EVENTS) and the activities
 * alive (LIVE), which arcwasm.ActivityTest reads by reflection.
 */
public class App6 extends Application {
    public static final List<String> EVENTS = new ArrayList<String>();
    public static final Map<String, Activity> LIVE = new HashMap<String, Activity>();

    public static void event(String what) {
        synchronized (EVENTS) {
            EVENTS.add(what);
        }
        System.out.println("App6: " + what);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        event("App6.onCreate " + getPackageName());
    }
}
