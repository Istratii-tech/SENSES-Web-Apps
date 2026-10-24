package org.arcwasm.testapp8;

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
 * The API 26 test app's Application (arc-wasm topic fw8-system): records the
 * callbacks of the app's components (EVENTS) and the activities alive
 * (LIVE), which arcwasm.ActivityTest8 reads by reflection. On create it does
 * what apps targeting API 26 do first: Java 8 code (a lambda, a default and a
 * static interface method, a stream) and its notification channels. Memory
 * callbacks (onTrimMemory, onLowMemory: topic mem-budget) are events too.
 */
public class App8 extends Application {
    public static final List<String> EVENTS = new ArrayList<String>();
    public static final Map<String, Activity> LIVE = new HashMap<String, Activity>();
    public static final String CHANNEL = "updates";

    public static void event(String what) {
        synchronized (EVENTS) {
            EVENTS.add(what);
        }
        System.out.println("App8: " + what);
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
        event("App8.onCreate " + getPackageName());
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
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        event("App8.onTrimMemory " + level);
    }

    @Override
    public void onLowMemory() {
        super.onLowMemory();
        event("App8.onLowMemory");
    }
}
