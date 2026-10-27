// arc-wasm: the "input_method" service on Android 10. No input method is
// installed; InputMethodManager (made by every window session) and the views
// it serves work against this service. With a page it is the soft keyboard
// (topic ios-audio-keyboard, docs/topics/ios-audio-keyboard.md): the
// browser's keyboard types into the page, and arcwasm.input.ImeHost applies
// that to the served editor's InputConnection, as an input method does. The
// bind result is "no IME" either way; without a page key events reach the
// focused view from the browser keyboard directly (ViewRootImpl's
// ImeInputStage passes them on when no method is bound).
package arcwasm.server;

import android.os.IBinder;
import android.os.RemoteException;
import android.os.ResultReceiver;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodManager;
import android.view.inputmethod.InputMethodSubtype;

import arcwasm.input.ImeHost;
import arcwasm.server.base.InputMethodManagerBase;

import com.android.internal.view.IInputContext;
import com.android.internal.view.IInputMethodClient;
import com.android.internal.view.InputBindResult;

import java.util.ArrayList;
import java.util.List;

public final class InputMethodManagerService extends InputMethodManagerBase {
    private final ImeHost mIme = new ImeHost(new ImeHost.Host() {
        @Override
        public void setClientActive(Object client, boolean active) {
            try {
                ((IInputMethodClient) client).setActive(active, false);
            } catch (RemoteException e) {
                // In process: not thrown.
            }
        }

        @Override
        public void setKeyboardHeight(int height) {
            WindowManagerService.getInstance().setImeHeight(height);
        }
    });

    @Override
    public List<InputMethodInfo> getInputMethodList(int userId) {
        return new ArrayList<InputMethodInfo>();
    }

    @Override
    public List<InputMethodInfo> getEnabledInputMethodList(int userId) {
        return new ArrayList<InputMethodInfo>();
    }

    @Override
    public List<InputMethodSubtype> getEnabledInputMethodSubtypeList(String imiId,
            boolean allowsImplicitlySelectedSubtypes) {
        return new ArrayList<InputMethodSubtype>();
    }

    @Override public InputMethodSubtype getLastInputMethodSubtype() { return null; }

    @Override public void addClient(IInputMethodClient client, IInputContext inputContext,
            int untrustedDisplayId) {}

    /**
     * startInput and windowGainedFocus in one: the page's keyboard gets the
     * window and its editor (ImeHost). 10's InputMethodManager wants a
     * result (null is a wtf): "no IME".
     */
    @Override
    public InputBindResult startInputOrWindowGainedFocus(int startInputReason,
            IInputMethodClient client, IBinder windowToken, int startInputFlags,
            int softInputMode, int windowFlags, EditorInfo attribute, IInputContext inputContext,
            int missingMethodFlags, int unverifiedTargetSdkVersion) {
        if (client != null) {
            mIme.startInput(client, client.asBinder(), windowToken, softInputMode, attribute,
                    inputContext);
        }
        return InputBindResult.NO_IME;
    }

    @Override
    public boolean showSoftInput(IInputMethodClient client, int flags,
            ResultReceiver resultReceiver) {
        final int was = mIme.showSoftInput(client != null ? client.asBinder() : null);
        if (was < 0) {
            return false;
        }
        if (resultReceiver != null) {
            resultReceiver.send(was > 0 ? InputMethodManager.RESULT_UNCHANGED_SHOWN
                    : InputMethodManager.RESULT_SHOWN, null);
        }
        return true;
    }

    @Override
    public boolean hideSoftInput(IInputMethodClient client, int flags,
            ResultReceiver resultReceiver) {
        final int was = mIme.hideSoftInput();
        if (was < 0) {
            return false;
        }
        if (resultReceiver != null) {
            resultReceiver.send(was > 0 ? InputMethodManager.RESULT_HIDDEN
                    : InputMethodManager.RESULT_UNCHANGED_HIDDEN, null);
        }
        return was > 0;
    }

    @Override public void showInputMethodPickerFromClient(IInputMethodClient client,
            int auxiliarySubtypeMode) {}

    @Override public InputMethodSubtype getCurrentInputMethodSubtype() { return null; }

    @Override
    public int getInputMethodWindowVisibleHeight() {
        return mIme.keyboardHeight();
    }

    @Override
    public void reportActivityView(IInputMethodClient parentClient, int childDisplayId,
            float[] matrixValues) {}
}
