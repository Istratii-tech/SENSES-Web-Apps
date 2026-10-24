// arc-wasm: the "input_method" service on Android 6.0.1: a device with no
// input method installed. InputMethodManager (made by every window session)
// and the views it serves work against it -- focus changes and startInput
// are accepted, nothing is bound, the soft keyboard never shows -- and key
// events reach the focused view from the browser keyboard directly
// (ViewRootImpl's ImeInputStage passes them on when no method is bound).
package arcwasm.server;

import android.os.IBinder;
import android.os.ResultReceiver;
import android.text.style.SuggestionSpan;
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
    public List<InputMethodInfo> getInputMethodList() {
        return new ArrayList<InputMethodInfo>();
    }

    @Override
    public List<InputMethodInfo> getEnabledInputMethodList() {
        return new ArrayList<InputMethodInfo>();
    }

    @Override
    public List<InputMethodSubtype> getEnabledInputMethodSubtypeList(String imiId,
            boolean allowsImplicitlySelectedSubtypes) {
        return new ArrayList<InputMethodSubtype>();
    }

    @Override public InputMethodSubtype getLastInputMethodSubtype() { return null; }

    @Override
    public List getShortcutInputMethodsAndSubtypes() {
        return new ArrayList();
    }

    @Override public void addClient(IInputMethodClient client, IInputContext inputContext,
            int uid, int pid) {}
    @Override public void removeClient(IInputMethodClient client) {}

    @Override
    public InputBindResult startInput(IInputMethodClient client, IInputContext inputContext,
            EditorInfo attribute, int controlFlags) {
        return null;  // no method to bind
    }

    @Override public void finishInput(IInputMethodClient client) {}

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

    @Override
    public InputBindResult windowGainedFocus(IInputMethodClient client, IBinder windowToken,
            int controlFlags, int softInputMode, int windowFlags, EditorInfo attribute,
            IInputContext inputContext) {
        return null;
    }

    @Override public void showInputMethodPickerFromClient(IInputMethodClient client,
            int auxiliarySubtypeMode) {}
    @Override public void hideMySoftInput(IBinder token, int flags) {}
    @Override public void showMySoftInput(IBinder token, int flags) {}
    @Override public void updateStatusIcon(IBinder token, String packageName, int iconId) {}
    @Override public void setImeWindowStatus(IBinder token, int vis, int backDisposition) {}
    @Override public void registerSuggestionSpansForNotification(SuggestionSpan[] spans) {}

    @Override
    public boolean notifySuggestionPicked(SuggestionSpan span, String originalString,
            int index) {
        return false;
    }

    @Override public InputMethodSubtype getCurrentInputMethodSubtype() { return null; }

    @Override
    public boolean setCurrentInputMethodSubtype(InputMethodSubtype subtype) {
        return false;
    }

    @Override
    public boolean shouldOfferSwitchingToNextInputMethod(IBinder token) {
        return false;
    }

    @Override
    public int getInputMethodWindowVisibleHeight() {
        return 0;
    }

    @Override public void notifyUserAction(int sequenceNumber) {}
}
