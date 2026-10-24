package org.arcwasm.gmstest;

import android.app.Application;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** GmsTest's app: records what happened in EVENTS (read by the test by reflection). */
public class GmsApp extends Application {
    public static final List<String> EVENTS = Collections.synchronizedList(new ArrayList<String>());

    @Override
    public void onCreate() {
        super.onCreate();
        EVENTS.add("GmsApp.onCreate " + getPackageName());
    }
}
