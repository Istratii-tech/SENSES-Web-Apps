package org.arcwasm.testapp;

import java.util.ArrayList;
import java.util.List;

/**
 * Code in the test APK that arcwasm.ApkTest calls through a class loader:
 * instance and static methods, an interface, an inner class, exceptions,
 * and the resource ids aapt generated (R).
 */
public class Greeter implements Runnable {
    public static int created;

    private final String mName;
    private final List<String> mLog = new ArrayList<String>();

    public Greeter(String name) {
        mName = name;
        created++;
    }

    public String greet() {
        String s = "Hello, " + mName + " (from " + getClass().getClassLoader().getClass().getSimpleName() + ")";
        mLog.add(s);
        return s;
    }

    public static int fib(int n) {
        return n < 2 ? n : fib(n - 1) + fib(n - 2);
    }

    public static int helloId() {
        return R.string.hello;
    }

    public static int layoutId() {
        return R.layout.main;
    }

    @Override
    public void run() {
        mLog.add(new Inner().describe());
    }

    public List<String> log() {
        return mLog;
    }

    public void fail(String why) {
        throw new IllegalStateException("Greeter failed: " + why);
    }

    class Inner {
        String describe() {
            return "inner of " + mName;
        }
    }
}
