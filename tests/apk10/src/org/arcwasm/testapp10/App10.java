package org.arcwasm.testapp10;

import android.app.Activity;
import android.app.Application;
import android.app.NotificationChannel;
import android.app.NotificationChannelGroup;
import android.app.NotificationManager;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.stream.Collectors;

/**
 * The API 29 test app's Application (arc-wasm topic fw10-system; the API 26
 * one of tests/apk8 grown): records the callbacks of the app's components
 * (EVENTS) and the activities alive (LIVE), which arcwasm.ActivityTest10
 * reads by reflection. On create it does what apps targeting API 26-29 do
 * first: Java 8 code (a lambda, a default and a static interface method, a
 * stream), its notification channels, and API 28-29 checks (the Apache HTTP
 * client of its <uses-library>, its runtime permissions, scoped storage).
 * Memory callbacks (onTrimMemory, onLowMemory: topic mem-budget) are events
 * too.
 */
public class App10 extends Application {
    public static final List<String> EVENTS = new ArrayList<String>();
    public static final Map<String, Activity> LIVE = new HashMap<String, Activity>();
    public static final String CHANNEL = "updates";

    public static void event(String what) {
        synchronized (EVENTS) {
            EVENTS.add(what);
        }
        System.out.println("App10: " + what);
    }

    /** Java 8 in an interface: a default and a static method. */
    interface Greeter {
        String name();

        default String greet() {
            return "hello " + name() + suffix();
        }

        static String suffix() {
            return "!";
        }
    }

    /** "hello java8! 3 words, sum 15" -- lambdas, a default method, a stream. */
    public static String java8() {
        Greeter g = () -> "java8";
        String words = Arrays.asList("a", "bb", "ccc").stream()
                .map(String::toUpperCase).collect(Collectors.joining(","));
        int sum = Arrays.asList(1, 2, 3, 4, 5).stream().mapToInt(Integer::intValue).sum();
        return g.greet() + " " + words.split(",").length + " words, sum " + sum;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        event("App10.onCreate " + getPackageName());
        event("java8 " + java8());
        NotificationManager nm = getSystemService(NotificationManager.class);
        nm.createNotificationChannelGroup(new NotificationChannelGroup("main", "Main"));
        NotificationChannel c = new NotificationChannel(CHANNEL, getString(R.string.channel_name),
                NotificationManager.IMPORTANCE_DEFAULT);
        c.setDescription("What changed");
        c.setGroup("main");
        nm.createNotificationChannel(c);
        event("channels " + nm.getNotificationChannels().stream()
                .map(NotificationChannel::getId).sorted().collect(Collectors.joining(",")));
        // API 28+: org.apache.http.legacy is no boot class path jar any more,
        // the app's class loader has it through its <uses-library>.
        try {
            Class<?> http = Class.forName("org.apache.http.impl.client.DefaultHttpClient", false,
                    getClassLoader());
            event("http legacy " + http.getSimpleName() + " from "
                    + (http.getClassLoader() == getClassLoader() ? "app" : "other") + " loader");
        } catch (ClassNotFoundException e) {
            event("http legacy missing");
        }
        // Runtime permissions (granted at install here, as after the user said yes).
        event("permissions camera=" + checkSelfPermission(android.Manifest.permission.CAMERA)
                + " location=" + checkSelfPermission(
                        android.Manifest.permission.ACCESS_FINE_LOCATION));
        // Scoped storage (API 29): the app's own directory; legacy view asked for.
        java.io.File ext = getExternalFilesDir(null);
        event("storage legacy=" + android.os.Environment.isExternalStorageLegacy()
                + " files=" + (ext != null && ext.isDirectory()));
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        event("App10.onTrimMemory " + level);
    }

    @Override
    public void onLowMemory() {
        super.onLowMemory();
        event("App10.onLowMemory");
    }
}
