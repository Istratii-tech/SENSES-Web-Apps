// arc-wasm: the page's text input on Android 6.0.1, 8.1 and 10 (topic
// ios-audio-keyboard, docs/topics/ios-audio-keyboard.md).
//
// The system keyboard of the browser (an iPhone's, or a computer's with its
// layouts and input methods) types into a hidden <textarea> of the page
// (shim/input/browser_input.js); the page turns what changed there into
// records -- commit, compose, delete, key, editor action, batch -- that the
// "input_method" service applies to the served editor's InputConnection
// (ImeHost), as an input method does. The other way, the service tells the
// page which editor is served and whether the keyboard is wanted (setState).
// Natives: shim/input/register_input.cc, in the graphics library, which
// arcwasm.input.BrowserInput loads (none in a headless VM: available() is
// false).
package arcwasm.input;

public final class BrowserIme {
    public static final int OP_COMMIT = 'c';
    public static final int OP_COMPOSE = 'p';
    public static final int OP_FINISH = 'f';
    public static final int OP_DELETE = 'd';
    public static final int OP_KEY = 'k';
    public static final int OP_ACTION = 'a';
    public static final int OP_BATCH = 'b';
    public static final int OP_KEYBOARD = 'h';
    public static final int OP_RESEED = 'r';

    /** One record from the page. */
    public static final class Record {
        public int op;
        public int a;
        public int b;
        public String text;
    }

    private static final boolean sAvailable;
    static {
        boolean available = false;
        try {
            // Loads the graphics library, which registers our natives too.
            Class.forName("arcwasm.input.BrowserInput");
            available = nativeAvailable();
        } catch (ClassNotFoundException | LinkageError e) {
            // Headless: no page.
        }
        sAvailable = available;
    }

    private BrowserIme() {}

    public static boolean available() {
        return sAvailable;
    }

    /** The next record; waits up to timeoutMillis (< 0: forever). Null on timeout. */
    public static Record take(int timeoutMillis) {
        int[] args = new int[3];
        String text = nativeTake(args, timeoutMillis);
        if (text == null) {
            return null;
        }
        Record r = new Record();
        r.op = args[0];
        r.a = args[1];
        r.b = args[2];
        r.text = text;
        return r;
    }

    /** As the page queues a record (tests). */
    public static void inject(int op, int a, int b, String text) {
        nativeInject(op, a, b, text);
    }

    /**
     * What the page shows: the served editor (none: editor false), whether
     * the keyboard is wanted, and the editor's text before the cursor.
     */
    public static void setState(boolean editor, boolean show, int inputType, int imeOptions,
            int actionId, CharSequence textBeforeCursor) {
        nativeSetState(editor, show, inputType, imeOptions, actionId,
                textBeforeCursor != null ? textBeforeCursor.toString() : null);
    }

    /**
     * The state as the page reads it (tests): serial, editor, show,
     * inputType, imeOptions, actionId into out[0..5]; returns the text
     * before the cursor.
     */
    public static String getState(int[] out) {
        return nativeGetState(out);
    }

    private static native boolean nativeAvailable();
    private static native String nativeGetState(int[] out);
    private static native String nativeTake(int[] args, int timeoutMillis);
    private static native void nativeInject(int op, int a, int b, String text);
    private static native void nativeSetState(boolean editor, boolean show, int inputType,
            int imeOptions, int actionId, String seed);
}
