package org.arcwasm.multiproc;

import android.app.Application;
import android.os.Process;

import java.io.FileInputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * The multiproc test app's Application (arc-wasm topic multiproc). It is
 * created once in each process of the app, as on a device: its statics are
 * per process (each process has its own class loader). arcwasm.MultiProcessTest
 * reads the main process's EVENTS by reflection and asks the other
 * processes through their binders (Codes).
 */
public class App extends Application {
    public static final List<String> EVENTS = new ArrayList<String>();
    /** Bumped by Codes.BUMP and the provider's insert: per process. */
    public static int sCounter;
    /** Application.onCreate calls in this process. */
    public static int sCreates;
    /** RemoteReceiver broadcasts received in this process. */
    public static int sPings;

    public static void event(String what) {
        synchronized (EVENTS) {
            EVENTS.add(what);
        }
        System.out.println("multiproc: " + what);
    }

    /** /proc/self/cmdline: the process's name, as apps read it. */
    public static String cmdline() {
        try (FileInputStream in = new FileInputStream("/proc/self/cmdline")) {
            byte[] b = new byte[256];
            int n = in.read(b);
            int end = 0;
            while (end < Math.max(n, 0) && b[end] != 0) {
                end++;
            }
            return new String(b, 0, end, "UTF-8");
        } catch (Exception e) {
            return "? " + e;
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        sCreates++;
        event("App.onCreate " + cmdline() + " pid " + Process.myPid() + " creates " + sCreates);
    }
}
