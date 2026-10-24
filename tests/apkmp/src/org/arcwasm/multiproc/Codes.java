package org.arcwasm.multiproc;

/** Binder transaction codes of RemoteService (arcwasm.MultiProcessTest uses the same numbers). */
final class Codes {
    static final int PID = 1;            // -> int Process.myPid()
    static final int PROCESS = 2;        // -> String /proc/self/cmdline, String from ActivityManager
    static final int BUMP = 3;           // -> int ++App.sCounter
    static final int CREATES = 4;        // -> int App.sCreates
    static final int ADD = 5;            // int a, int b -> int a + b
    static final int CALLBACK = 6;       // IBinder cb -> int (cb's answer: its calling pid), int its pid
    static final int CALLING_PID = 7;    // -> int Binder.getCallingPid()
    static final int MAIN_LOOPER = 8;    // -> long main Looper's thread id, String its name, int same-as-own
    static final int KILL_SELF = 9;      // Process.killProcess(myPid()): no reply
    static final int EXIT = 10;          // System.exit(3): no reply
    static final int UID = 11;           // -> int myUid
    static final int LOADER = 12;        // -> int identityHashCode(App's class loader)
    static final int API_PROCESS_NAME = 13;  // -> String Application.getProcessName() (API 28+), else null
    static final int PINGS = 14;         // -> int App.sPings
    static final int CRASH = 15;         // throws on the main thread: an uncaught exception

    private Codes() {}
}
