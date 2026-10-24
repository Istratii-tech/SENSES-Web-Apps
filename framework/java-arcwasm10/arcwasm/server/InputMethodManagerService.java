// arc-wasm: the "input_method" service on Android 10: a device with no
// input method installed. InputMethodManager (made by every window session)
// and the views it serves work against it -- focus changes and startInput
// are accepted, nothing is bound, the soft keyboard never shows -- and key
// events reach the focused view from the browser keyboard directly
// (ViewRootImpl's ImeInputStage passes them on when no method is bound).
package arcwasm.server;

import android.os.IBinder;
import android.os.ResultReceiver;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodSubtype;

import arcwasm.server.base.InputMethodManagerBase;

import com.android.internal.view.IInputContext;
import com.android.internal.view.IInputMethodClient;
import com.android.internal.view.InputBindResult;

import java.util.ArrayList;
import java.util.List;

public final class InputMethodManagerService extends InputMethodManagerBase {
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
     * startInput and windowGainedFocus in one: no method to bind. 10's
     * InputMethodManager wants a result (null is a wtf): "no IME".
     */
    @Override
    public InputBindResult startInputOrWindowGainedFocus(int startInputReason,
            IInputMethodClient client, IBinder windowToken, int startInputFlags,
            int softInputMode, int windowFlags, EditorInfo attribute, IInputContext inputContext,
            int missingMethodFlags, int unverifiedTargetSdkVersion) {
        return InputBindResult.NO_IME;
    }

    @Override
    public boolean showSoftInput(IInputMethodClient client, int flags,
            ResultReceiver resultReceiver) {
        return false;
    }

    @Override
    public boolean hideSoftInput(IInputMethodClient client, int flags,
            ResultReceiver resultReceiver) {
        return false;
    }

    @Override public void showInputMethodPickerFromClient(IInputMethodClient client,
            int auxiliarySubtypeMode) {}

    @Override public InputMethodSubtype getCurrentInputMethodSubtype() { return null; }

    @Override
    public int getInputMethodWindowVisibleHeight() {
        return 0;
    }

    @Override
    public void reportActivityView(IInputMethodClient parentClient, int childDisplayId,
            float[] matrixValues) {}
}
