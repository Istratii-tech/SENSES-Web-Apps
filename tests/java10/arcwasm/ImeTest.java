package arcwasm;

import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.WindowManager;
import android.view.inputmethod.CompletionInfo;
import android.view.inputmethod.CorrectionInfo;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.InputContentInfo;

import arcwasm.input.BrowserIme;
import arcwasm.input.ImeHost;

import com.android.internal.view.IInputContext;
import com.android.internal.view.IInputContextCallback;

import java.util.ArrayList;
import java.util.List;

/**
 * Checks the page's keyboard as Android's input method (topic
 * ios-audio-keyboard, docs/topics/ios-audio-keyboard.md) without a page or
 * windows: arcwasm.input.ImeHost, which the "input_method" service of 6.0.1,
 * 8.1 and 10 drives, gets an editor (EditorInfo and an IInputContext that
 * records what it is told, as InputMethodManager's would pass it on to the
 * InputConnection); records are queued as the page queues them
 * (BrowserIme.inject: the same queue as arcwasm_ime_op) and must arrive at
 * the editor as an input method's calls. Also the state the page reads
 * (BrowserIme.getState), the client made active, the soft input state of a
 * window, and the keyboard's height for the window manager. Prints one
 * line per check and exits with the number of failures.
 */
public class ImeTest {
    private static final long WAIT_MS = 5000;

    private static int passed;
    private static int failed;

    interface Check {
        void run() throws Exception;
    }

    private static void check(String name, Check c) {
        try {
            c.run();
            passed++;
            System.out.println("[ OK ] " + name);
        } catch (Throwable t) {
            failed++;
            System.out.println("[FAIL] " + name + ": " + t);
            t.printStackTrace(System.out);
        }
    }

    private static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    private static void expectEq(Object want, Object got, String what) {
        if (want == null ? got != null : !want.equals(got)) {
            throw new AssertionError(what + ": want <" + want + ">, got <" + got + ">");
        }
    }

    /** An editor's InputConnection as the service gets it: records the calls. */
    static final class Editor extends IInputContext.Stub {
        final List<String> calls = new ArrayList<String>();
        String before = "Hello. ";

        synchronized void add(String call) {
            calls.add(call);
            notifyAll();
        }

        synchronized List<String> await(int n) throws InterruptedException {
            long until = System.currentTimeMillis() + WAIT_MS;
            while (calls.size() < n && System.currentTimeMillis() < until) {
                wait(50);
            }
            List<String> got = new ArrayList<String>(calls);
            calls.clear();
            return got;
        }

        @Override public void getTextBeforeCursor(int length, int flags, int seq,
                IInputContextCallback callback) throws RemoteException {
            add("getTextBeforeCursor " + length);
            callback.setTextBeforeCursor(before, seq);
        }
        @Override public void getTextAfterCursor(int length, int flags, int seq,
                IInputContextCallback callback) {}
        @Override public void getCursorCapsMode(int reqModes, int seq,
                IInputContextCallback callback) {}
        @Override public void getExtractedText(ExtractedTextRequest request, int flags, int seq,
                IInputContextCallback callback) {}
        @Override public void deleteSurroundingText(int before, int after) {
            add("delete " + before + " " + after);
        }
        @Override public void deleteSurroundingTextInCodePoints(int before, int after) {}
        @Override public void setComposingText(CharSequence text, int pos) {
            add("compose " + text + " " + pos);
        }
        @Override public void finishComposingText() {
            add("finish");
        }
        @Override public void commitText(CharSequence text, int pos) {
            add("commit " + text + " " + pos);
        }
        @Override public void commitCompletion(CompletionInfo completion) {}
        @Override public void commitCorrection(CorrectionInfo correction) {}
        @Override public void setSelection(int start, int end) {}
        @Override public void performEditorAction(int actionCode) {
            add("action " + actionCode);
        }
        @Override public void performContextMenuAction(int id) {}
        @Override public void beginBatchEdit() {
            add("begin");
        }
        @Override public void endBatchEdit() {
            add("end");
        }
        @Override public void sendKeyEvent(KeyEvent event) {
            add("key " + (event.getAction() == KeyEvent.ACTION_DOWN ? "down " : "up ")
                    + event.getKeyCode() + " soft="
                    + ((event.getFlags() & KeyEvent.FLAG_SOFT_KEYBOARD) != 0));
        }
        @Override public void clearMetaKeyStates(int states) {}
        @Override public void performPrivateCommand(String action, Bundle data) {}
        @Override public void setComposingRegion(int start, int end) {}
        @Override public void getSelectedText(int flags, int seq, IInputContextCallback callback) {}
        @Override public void requestUpdateCursorAnchorInfo(int cursorUpdateMode, int seq,
                IInputContextCallback callback) {}
        @Override public void commitContent(InputContentInfo inputContentInfo, int flags,
                Bundle opts, int sec, IInputContextCallback callback) {}
    }

    /** The service's side: which client is active, the keyboard's height. */
    static final class Host implements ImeHost.Host {
        final List<String> events = new ArrayList<String>();
        int height;

        @Override public synchronized void setClientActive(Object client, boolean active) {
            events.add(client + (active ? " active" : " inactive"));
        }

        @Override public synchronized void setKeyboardHeight(int h) {
            height = h;
            notifyAll();
        }

        synchronized boolean awaitHeight(int h) throws InterruptedException {
            long until = System.currentTimeMillis() + WAIT_MS;
            while (height != h && System.currentTimeMillis() < until) {
                wait(50);
            }
            return height == h;
        }

        synchronized List<String> take() {
            List<String> got = new ArrayList<String>(events);
            events.clear();
            return got;
        }
    }

    /** The page's view: {serial, editor, show, inputType, imeOptions, actionId} and seed. */
    static int[] state = new int[6];
    static String seed;

    static void readState() {
        seed = BrowserIme.getState(state);
    }

    /** Waits until the page's state has the seed |want| (the editor answers on a binder call). */
    static void awaitSeed(String want) throws InterruptedException {
        long until = System.currentTimeMillis() + WAIT_MS;
        do {
            readState();
            if (want.equals(seed)) return;
            Thread.sleep(20);
        } while (System.currentTimeMillis() < until);
    }

    public static void main(String[] args) throws Exception {
        long t0 = System.currentTimeMillis();
        final Host host = new Host();
        final ImeHost ime = new ImeHost(host);
        final Editor search = new Editor();
        final Editor note = new Editor();
        final IBinder clientA = new Binder();
        final IBinder clientB = new Binder();
        final IBinder window1 = new Binder();
        final IBinder window2 = new Binder();
        final EditorInfo searchInfo = new EditorInfo();
        searchInfo.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES;
        searchInfo.imeOptions = EditorInfo.IME_ACTION_SEARCH;
        final EditorInfo noteInfo = new EditorInfo();
        noteInfo.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE;
        noteInfo.imeOptions = EditorInfo.IME_FLAG_NO_ENTER_ACTION;
        noteInfo.actionLabel = "Go";
        noteInfo.actionId = 42;

        check("the page's text input is there (graphics library)", () -> {
            expect(BrowserIme.available(), "BrowserIme.available()");
            expect(ImeHost.available(), "ImeHost.available()");
        });

        check("a window's editor: client active, state for the page, text before the cursor", () -> {
            ime.startInput("A", clientA, window1,
                    WindowManager.LayoutParams.SOFT_INPUT_STATE_UNSPECIFIED, searchInfo, search);
            expectEq("[A active]", host.take().toString(), "client");
            expectEq("[getTextBeforeCursor 200]", search.await(1).toString(), "seed asked");
            awaitSeed("Hello. ");
            expectEq(1, state[1], "editor");
            expectEq(0, state[2], "keyboard not wanted (unspecified state)");
            expectEq(searchInfo.inputType, state[3], "inputType");
            expectEq(searchInfo.imeOptions, state[4], "imeOptions");
            expectEq(0, state[5], "no custom action without its label");
        });

        check("showSoftInput: only from the client with the input; then wanted", () -> {
            expectEq(-1, ime.showSoftInput(clientB), "other client");
            expectEq(0, ime.showSoftInput(clientA), "first");
            expectEq(1, ime.showSoftInput(clientA), "again");
            readState();
            expectEq(1, state[2], "keyboard wanted");
        });

        check("typing: commit, delete, compose, finish, as an input method", () -> {
            BrowserIme.inject(BrowserIme.OP_COMMIT, 0, 0, "мир 😀");
            BrowserIme.inject(BrowserIme.OP_BATCH, 1, 0, null);
            BrowserIme.inject(BrowserIme.OP_DELETE, 3, 0, null);
            BrowserIme.inject(BrowserIme.OP_COMMIT, 0, 0, "the ");
            BrowserIme.inject(BrowserIme.OP_BATCH, 0, 0, null);
            BrowserIme.inject(BrowserIme.OP_COMPOSE, 0, 0, "ка");
            BrowserIme.inject(BrowserIme.OP_COMPOSE, 0, 0, "кап");
            BrowserIme.inject(BrowserIme.OP_FINISH, 0, 0, null);
            expectEq("[commit мир 😀 1, begin, delete 3 0, commit the  1, end, compose ка 1, "
                    + "compose кап 1, finish]", search.await(8).toString(), "calls");
        });

        check("keys and the editor's action", () -> {
            BrowserIme.inject(BrowserIme.OP_KEY, KeyEvent.KEYCODE_DEL, 0, null);
            BrowserIme.inject(BrowserIme.OP_ACTION, EditorInfo.IME_ACTION_SEARCH, 0, null);
            expectEq("[key down 67 soft=true, key up 67 soft=true, action 3]",
                    search.await(3).toString(), "calls");
        });

        check("the page lost track: the text before the cursor again", () -> {
            search.before = "Hello. мир the кап";
            BrowserIme.inject(BrowserIme.OP_RESEED, 0, 0, null);
            expectEq("[getTextBeforeCursor 200]", search.await(1).toString(), "seed asked");
            awaitSeed("Hello. мир the кап");
            expectEq("Hello. мир the кап", seed, "seed");
        });

        check("the keyboard's height goes to the window manager; put away: wish forgotten", () -> {
            BrowserIme.inject(BrowserIme.OP_KEYBOARD, 230, 1, null);
            expect(host.awaitHeight(230), "height 230, got " + host.height);
            expectEq(230, ime.keyboardHeight(), "getInputMethodWindowVisibleHeight");
            BrowserIme.inject(BrowserIme.OP_KEYBOARD, 0, 2, null);
            expect(host.awaitHeight(0), "height 0, got " + host.height);
            expectEq(0, ime.showSoftInput(clientA), "not wanted any more");
        });

        check("hideSoftInput", () -> {
            expectEq(1, ime.hideSoftInput(), "was wanted");
            expectEq(0, ime.hideSoftInput(), "again");
            readState();
            expectEq(0, state[2], "keyboard not wanted");
        });

        check("another window (always visible): clients switch, its editor, keyboard wanted", () -> {
            ime.startInput("B", clientB, window2,
                    WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE, noteInfo, note);
            expectEq("[A inactive, B active]", host.take().toString(), "clients");
            note.await(1);
            readState();
            expectEq(1, state[2], "keyboard wanted");
            expectEq(noteInfo.inputType, state[3], "inputType");
            expectEq(42, state[5], "custom action (it has a label)");
            BrowserIme.inject(BrowserIme.OP_COMMIT, 0, 0, "x");
            expectEq("[commit x 1]", note.await(1).toString(), "to the new editor");
            expect(search.await(0).isEmpty(), "nothing for the old one");
        });

        check("a password field: no text before the cursor for the page", () -> {
            EditorInfo password = new EditorInfo();
            password.inputType = InputType.TYPE_CLASS_TEXT
                    | InputType.TYPE_TEXT_VARIATION_PASSWORD;
            Editor secret = new Editor();
            ime.startInput("B", clientB, null, 0, password, secret);
            Thread.sleep(200);
            expect(secret.await(0).isEmpty(), "not asked");
            readState();
            expectEq("", seed, "seed");
        });

        check("no editor: nothing served, records dropped, keyboard not wanted", () -> {
            ime.startInput("B", clientB, null, 0, null, null);
            readState();
            expectEq(0, state[1], "editor");
            expectEq(0, state[2], "keyboard");
            expectEq(-1, ime.showSoftInput(clientB), "showSoftInput without an editor");
            BrowserIme.inject(BrowserIme.OP_COMMIT, 0, 0, "lost");
            Thread.sleep(200);
            expect(note.await(0).isEmpty(), "no calls");
        });

        long ms = System.currentTimeMillis() - t0;
        System.out.println("ImeTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
