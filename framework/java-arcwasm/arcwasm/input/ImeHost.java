// arc-wasm: the soft keyboard of Android 6.0.1, 8.1 and 10 is the page's
// (topic ios-audio-keyboard, docs/topics/ios-audio-keyboard.md). The
// "input_method" service of each (arcwasm.server.InputMethodManagerService,
// a device without an installed input method) hands this class what it
// hears from InputMethodManager -- the window that has the input, its served
// editor's EditorInfo and IInputContext (the InputConnection, as an input
// method gets it), showSoftInput/hideSoftInput -- and this class:
//   - tells the page the editor and whether the keyboard is wanted
//     (BrowserIme.setState), with the editor's text before the cursor;
//   - applies what the page's keyboard typed (BrowserIme records) to that
//     IInputContext on a thread of its own: commitText, setComposingText,
//     deleteSurroundingText, sendKeyEvent, performEditorAction, batch edits;
//   - makes the window's client active (the service's setActive, which
//     differs between versions) -- an inactive one drops the calls;
//   - passes on how much of the display the page's keyboard covers (the
//     window manager gives windows that much as insets).
// Without a page (headless VM, no BrowserIme natives) it does nothing and the
// service stays a device without an input method.
package arcwasm.input;

import android.os.IBinder;
import android.os.RemoteException;
import android.os.SystemClock;
import android.text.InputType;
import android.util.Slog;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ExtractedText;

import com.android.internal.view.IInputContext;
import com.android.internal.view.IInputContextCallback;

public final class ImeHost {
    private static final String TAG = "InputMethodManager";
    // The editor's text the page gets before the cursor (its keyboard's
    // capitals and suggestions go by it).
    private static final int SEED_LENGTH = 200;

    /** What differs between the versions' services. */
    public interface Host {
        /** IInputMethodClient.setActive on |client| (the object given to startInput). */
        void setClientActive(Object client, boolean active);

        /** The page's keyboard now covers |height| px at the bottom of the display. */
        void setKeyboardHeight(int height);
    }

    private final Host mHost;
    private final Object mLock = new Object();
    // The client of the window that has the input (its binder: the identity),
    // that window, and its served editor.
    private Object mClient;
    private IBinder mClientBinder;
    private IBinder mWindow;
    private IInputContext mContext;
    private EditorInfo mAttribute;
    // The app wants the keyboard (showSoftInput, the window's soft input state).
    private boolean mShowRequested;
    // Display pixels the page's keyboard covers.
    private int mHeight;
    // getTextBeforeCursor's sequence: answers for an older editor are dropped.
    private int mSeedSeq;
    private boolean mStarted;

    public ImeHost(Host host) {
        mHost = host;
    }

    public static boolean available() {
        return BrowserIme.available();
    }

    /**
     * startInput / windowGainedFocus: |client| (its binder |clientBinder|)
     * has the input; |windowToken| is the window that gained focus (null:
     * a view's focus within it), |attribute| and |inputContext| its editor
     * (null: none). The window's soft input state says whether the keyboard
     * shows, as InputMethodManagerService decides.
     */
    public void startInput(Object client, IBinder clientBinder, IBinder windowToken,
            int softInputMode, EditorInfo attribute, IInputContext inputContext) {
        if (!available() || clientBinder == null) {
            return;
        }
        startThread();
        Object deactivate = null;
        boolean activate = false;
        synchronized (mLock) {
            if (clientBinder != mClientBinder) {
                deactivate = mClient;
                mClient = client;
                mClientBinder = clientBinder;
                mWindow = null;
                mShowRequested = false;
                activate = true;
            }
            // (TYPE_NULL too: a terminal's view; what is committed reaches it as
            // key events, BaseInputConnection's dummy mode)
            final boolean editor = attribute != null && inputContext != null;
            if (windowToken != null && windowToken != mWindow) {
                mWindow = windowToken;
                final boolean forward = (softInputMode
                        & WindowManager.LayoutParams.SOFT_INPUT_IS_FORWARD_NAVIGATION) != 0;
                switch (softInputMode & WindowManager.LayoutParams.SOFT_INPUT_MASK_STATE) {
                    case WindowManager.LayoutParams.SOFT_INPUT_STATE_UNCHANGED:
                        break;
                    case WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE:
                        mShowRequested = forward && editor;
                        break;
                    case WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE:
                        mShowRequested = editor;
                        break;
                    default:  // unspecified, hidden, always hidden
                        mShowRequested = false;
                        break;
                }
            }
            mContext = editor ? inputContext : null;
            mAttribute = editor ? attribute : null;
            if (!editor) {
                mShowRequested = false;
            }
            publishLocked(null);
            if (editor) {
                requestSeedLocked();
            }
        }
        if (deactivate != null) {
            mHost.setClientActive(deactivate, false);
        }
        if (activate) {
            mHost.setClientActive(client, true);
        }
    }

    /**
     * showSoftInput from |clientBinder|: -1 refused (not the client with
     * the input, no editor), else 1 if the keyboard was wanted already, 0 not.
     */
    public int showSoftInput(IBinder clientBinder) {
        synchronized (mLock) {
            if (!available() || mContext == null || clientBinder == null
                    || clientBinder != mClientBinder) {
                return -1;
            }
            final boolean was = mShowRequested;
            mShowRequested = true;
            publishLocked(null);
            return was ? 1 : 0;
        }
    }

    /** hideSoftInput: -1 no page, else 1 if the keyboard was wanted, 0 not. */
    public int hideSoftInput() {
        synchronized (mLock) {
            if (!available()) {
                return -1;
            }
            final boolean was = mShowRequested;
            mShowRequested = false;
            publishLocked(null);
            return was ? 1 : 0;
        }
    }

    /** Display pixels the keyboard covers (getInputMethodWindowVisibleHeight). */
    public int keyboardHeight() {
        synchronized (mLock) {
            return mHeight;
        }
    }

    // --- the page ----------------------------------------------------------------------

    /** Tells the page the editor and whether the keyboard is wanted. */
    private void publishLocked(CharSequence seed) {
        final EditorInfo a = mAttribute;
        // (the custom action only with its label, as LatinIME takes it)
        BrowserIme.setState(a != null, a != null && mShowRequested,
                a != null ? a.inputType : 0, a != null ? a.imeOptions : 0,
                a != null && a.actionLabel != null ? a.actionId : 0, seed);
    }

    /** Asks the editor for its text before the cursor (for the page). */
    private void requestSeedLocked() {
        final IInputContext context = mContext;
        final int seq = ++mSeedSeq;
        if (context == null || mAttribute == null || isPassword(mAttribute.inputType)) {
            return;
        }
        try {
            context.getTextBeforeCursor(SEED_LENGTH, 0, seq, new SeedCallback(context));
        } catch (RemoteException e) {
            // In process: not thrown.
        }
    }

    private static boolean isPassword(int inputType) {
        final int variation = inputType & InputType.TYPE_MASK_VARIATION;
        final int klass = inputType & InputType.TYPE_MASK_CLASS;
        return (klass == InputType.TYPE_CLASS_TEXT
                && (variation == InputType.TYPE_TEXT_VARIATION_PASSWORD
                    || variation == InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                    || variation == InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD))
                || (klass == InputType.TYPE_CLASS_NUMBER
                    && variation == InputType.TYPE_NUMBER_VARIATION_PASSWORD);
    }

    // (no @Override: the callback's methods differ between 6.0.1, 8.1 and 10;
    // the extra ones are plain methods where the interface lacks them)
    private final class SeedCallback extends IInputContextCallback.Stub {
        private final IInputContext mFor;

        SeedCallback(IInputContext context) {
            mFor = context;
        }

        public void setTextBeforeCursor(CharSequence text, int seq) {
            synchronized (mLock) {
                if (seq == mSeedSeq && mContext == mFor && text != null) {
                    publishLocked(text);
                }
            }
        }

        public void setTextAfterCursor(CharSequence text, int seq) {}
        public void setCursorCapsMode(int capsMode, int seq) {}
        public void setExtractedText(ExtractedText extractedText, int seq) {}
        public void setSelectedText(CharSequence selectedText, int seq) {}
        public void setRequestUpdateCursorAnchorInfoResult(boolean result, int seq) {}
        public void setCommitContentResult(boolean result, int seq) {}
    }

    private void startThread() {
        synchronized (mLock) {
            if (mStarted) {
                return;
            }
            mStarted = true;
        }
        Thread thread = new Thread("BrowserIme") {
            @Override
            public void run() {
                for (;;) {
                    BrowserIme.Record r = BrowserIme.take(-1);
                    if (r != null) {
                        apply(r);
                    }
                }
            }
        };
        thread.setDaemon(true);
        thread.start();
    }

    /** One record from the page, on the served editor (as an input method would). */
    private void apply(BrowserIme.Record r) {
        if (r.op == BrowserIme.OP_KEYBOARD) {
            keyboard(r.a, r.b);
            return;
        }
        final IInputContext ic;
        synchronized (mLock) {
            ic = mContext;
            if (r.op == BrowserIme.OP_RESEED) {
                // The page lost track of the text (a tap, a key that moves the
                // cursor): the text before the cursor again.
                requestSeedLocked();
                return;
            }
        }
        if (ic == null) {
            return;
        }
        try {
            switch (r.op) {
                case BrowserIme.OP_COMMIT:
                    ic.commitText(r.text, 1);
                    break;
                case BrowserIme.OP_COMPOSE:
                    ic.setComposingText(r.text, 1);
                    break;
                case BrowserIme.OP_FINISH:
                    ic.finishComposingText();
                    break;
                case BrowserIme.OP_DELETE:
                    ic.deleteSurroundingText(r.a, r.b);
                    break;
                case BrowserIme.OP_KEY: {
                    // InputMethodService.sendDownUpKeyEvents.
                    final long now = SystemClock.uptimeMillis();
                    final int flags = KeyEvent.FLAG_SOFT_KEYBOARD | KeyEvent.FLAG_KEEP_TOUCH_MODE;
                    ic.sendKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, r.a, 0, r.b,
                            KeyCharacterMap.VIRTUAL_KEYBOARD, 0, flags));
                    ic.sendKeyEvent(new KeyEvent(now, SystemClock.uptimeMillis(),
                            KeyEvent.ACTION_UP, r.a, 0, r.b, KeyCharacterMap.VIRTUAL_KEYBOARD, 0,
                            flags));
                    break;
                }
                case BrowserIme.OP_ACTION:
                    ic.performEditorAction(r.a);
                    break;
                case BrowserIme.OP_BATCH:
                    if (r.a != 0) {
                        ic.beginBatchEdit();
                    } else {
                        ic.endBatchEdit();
                    }
                    break;
                default:
                    break;
            }
        } catch (RemoteException e) {
            // In process: not thrown.
        }
    }

    /**
     * The page's keyboard: |height| display pixels covered, |state| 1 shown,
     * 0 hidden, 2 put away by the user (the app's wish is then forgotten,
     * as when the back key closes an input method).
     */
    private void keyboard(int height, int state) {
        synchronized (mLock) {
            if (state == 2) {
                mShowRequested = false;
            }
            if (height == mHeight) {
                return;
            }
            mHeight = height;
        }
        Slog.i(TAG, "keyboard " + (height > 0 ? "covers " + height + " px" : "hidden"));
        mHost.setKeyboardHeight(height);
    }
}
