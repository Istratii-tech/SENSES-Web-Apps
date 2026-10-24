// arc-wasm: the "input_method" system service, in this process: a device
// with no input method installed. InputMethodManager (made by every window
// session, WindowManagerGlobal.getWindowSession) and the views it serves
// work against it -- focus changes and startInput are accepted, nothing is
// bound, the soft keyboard never shows -- and key events reach the focused
// view from the browser keyboard directly (ViewRootImpl's ImeInputStage
// passes them on when there is no current method). Started with the window
// natives (shim/window/register_window.cc).
package arcwasm.server;

import android.os.IBinder;
import android.os.ResultReceiver;
import android.text.style.SuggestionSpan;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodSubtype;

import com.android.internal.view.IInputContext;
import com.android.internal.view.IInputMethodClient;
import com.android.internal.view.IInputMethodManager;
import com.android.internal.view.InputBindResult;

import java.util.ArrayList;
import java.util.List;

public final class InputMethodManagerService extends IInputMethodManager.Stub {
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

    @Override public void showInputMethodPickerFromClient(IInputMethodClient client) {}
    @Override public void showInputMethodAndSubtypeEnablerFromClient(IInputMethodClient client,
            String topId) {}
    @Override public void setInputMethod(IBinder token, String id) {}
    @Override public void setInputMethodAndSubtype(IBinder token, String id,
            InputMethodSubtype subtype) {}
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

    @Override public boolean switchToLastInputMethod(IBinder token) { return false; }

    @Override
    public boolean switchToNextInputMethod(IBinder token, boolean onlyCurrentIme) {
        return false;
    }

    @Override
    public boolean shouldOfferSwitchingToNextInputMethod(IBinder token) {
        return false;
    }

    @Override public boolean setInputMethodEnabled(String id, boolean enabled) { return false; }
    @Override public void setAdditionalInputMethodSubtypes(String id,
            InputMethodSubtype[] subtypes) {}
}
