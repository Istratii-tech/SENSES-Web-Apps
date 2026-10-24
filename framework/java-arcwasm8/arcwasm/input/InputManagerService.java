// arc-wasm: the "input" system service in this process, on the Android
// 8.1 framework: IInputManager of 8.1 (6.0's, plus enabling devices,
// keyboard layouts per input method, pointer icons and capture) over the
// same natives as the 4.4
// build's (framework/java-arcwasm/arcwasm/input/InputManagerService.java):
// the browser's input devices built from InputDeviceInfo, with key
// character maps from /system/usr/keychars, and which keys they have.
// Started by libarcwasm_graphics8.so's registration
// (shim/input/register_input.cc, register_arcwasm_input_natives).
package arcwasm.input;

import android.app.IInputForwarder;
import android.content.Context;
import android.hardware.input.IInputDevicesChangedListener;
import android.hardware.input.IInputManager;
import android.hardware.input.ITabletModeChangedListener;
import android.hardware.input.InputDeviceIdentifier;
import android.hardware.input.TouchCalibration;
import android.hardware.input.KeyboardLayout;
import android.os.IBinder;
import android.os.ServiceManager;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.PointerIcon;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodSubtype;

public final class InputManagerService extends IInputManager.Stub {
    private InputDevice[] mInputDevices;

    private InputManagerService() {}

    static void start() {
        ServiceManager.addService(Context.INPUT_SERVICE, new InputManagerService());
    }

    private synchronized InputDevice[] getDevices() {
        if (mInputDevices == null) {
            mInputDevices = nativeCreateInputDevices();
        }
        return mInputDevices;
    }

    @Override
    public InputDevice getInputDevice(int deviceId) {
        for (InputDevice device : getDevices()) {
            if (device.getId() == deviceId) {
                return device;
            }
        }
        return null;
    }

    @Override
    public int[] getInputDeviceIds() {
        InputDevice[] devices = getDevices();
        int[] ids = new int[devices.length];
        for (int i = 0; i < devices.length; i++) {
            ids[i] = devices[i].getId();
        }
        return ids;
    }

    @Override
    public boolean hasKeys(int deviceId, int sourceMask, int[] keyCodes, boolean[] keyExists) {
        if (keyCodes == null || keyExists == null || keyExists.length < keyCodes.length) {
            throw new IllegalArgumentException("keyExists must not be null and must be at "
                    + "least as large as keyCodes.");
        }
        if (deviceId == -1) {
            // Any device (InputManager.deviceHasKeys without a device).
            deviceId = BrowserInput.DEVICE_ID_KEYBOARD;
        }
        return nativeHasKeys(deviceId, sourceMask, keyCodes, keyExists);
    }

    @Override
    public void tryPointerSpeed(int speed) {}

    @Override
    public boolean injectInputEvent(InputEvent ev, int mode) {
        return false;
    }

    @Override
    public KeyboardLayout[] getKeyboardLayouts() {
        return new KeyboardLayout[0];
    }

    @Override
    public KeyboardLayout getKeyboardLayout(String keyboardLayoutDescriptor) {
        return null;
    }

    @Override
    public TouchCalibration getTouchCalibrationForInputDevice(String inputDeviceDescriptor,
            int rotation) {
        return TouchCalibration.IDENTITY;
    }

    @Override
    public void setTouchCalibrationForInputDevice(String inputDeviceDescriptor, int rotation,
            TouchCalibration calibration) {}

    @Override
    public String getCurrentKeyboardLayoutForInputDevice(InputDeviceIdentifier identifier) {
        return null;
    }

    @Override
    public void setCurrentKeyboardLayoutForInputDevice(InputDeviceIdentifier identifier,
            String keyboardLayoutDescriptor) {}

    @Override
    public KeyboardLayout[] getKeyboardLayoutsForInputDevice(InputDeviceIdentifier identifier) {
        return new KeyboardLayout[0];
    }

    @Override
    public String[] getEnabledKeyboardLayoutsForInputDevice(InputDeviceIdentifier identifier) {
        return new String[0];
    }

    @Override
    public KeyboardLayout getKeyboardLayoutForInputDevice(InputDeviceIdentifier identifier,
            InputMethodInfo imeInfo, InputMethodSubtype imeSubtype) {
        return null;
    }

    @Override
    public void setKeyboardLayoutForInputDevice(InputDeviceIdentifier identifier,
            InputMethodInfo imeInfo, InputMethodSubtype imeSubtype,
            String keyboardLayoutDescriptor) {}

    @Override
    public boolean isInputDeviceEnabled(int deviceId) {
        return getInputDevice(deviceId) != null;
    }

    @Override
    public void enableInputDevice(int deviceId) {}

    @Override
    public void disableInputDevice(int deviceId) {}

    @Override
    public void setPointerIconType(int typeId) {}

    @Override
    public void setCustomPointerIcon(PointerIcon icon) {}

    @Override
    public void requestPointerCapture(IBinder windowToken, boolean enabled) {}

    @Override
    public IInputForwarder createInputForwarder(int displayId) {
        return null;
    }

    @Override
    public void addKeyboardLayoutForInputDevice(InputDeviceIdentifier identifier,
            String keyboardLayoutDescriptor) {}

    @Override
    public void removeKeyboardLayoutForInputDevice(InputDeviceIdentifier identifier,
            String keyboardLayoutDescriptor) {}

    @Override
    public int isInTabletMode() {
        return -1;  // InputManagerService.SWITCH_STATE_UNKNOWN: no tablet mode switch
    }

    @Override
    public void registerTabletModeChangedListener(ITabletModeChangedListener listener) {}

    @Override
    public void registerInputDevicesChangedListener(IInputDevicesChangedListener listener) {
        // The browser's devices do not change.
    }

    @Override
    public void vibrate(int deviceId, long[] pattern, int repeat, IBinder token) {}

    @Override
    public void cancelVibrate(int deviceId, IBinder token) {}

    private static native InputDevice[] nativeCreateInputDevices();
    private static native boolean nativeHasKeys(int deviceId, int sourceMask, int[] keyCodes,
            boolean[] keyExists);
}
