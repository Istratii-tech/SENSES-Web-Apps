// arc-wasm: the "input" system service, in this process.
//
// What com.android.server.input.InputManagerService answers through
// IInputManager for apps (android.hardware.input.InputManager): the input
// devices -- built natively from InputDeviceInfo like the real service's
// devices, with their key character maps from /system/usr/keychars -- and
// which keys they have. Keyboard layouts, vibrators, pointer speed and event
// injection are not there yet. Started from the input natives' registration
// (shim/input/register_input.cc), as SystemServer starts the real one.
package arcwasm.input;

import android.content.Context;
import android.hardware.input.IInputDevicesChangedListener;
import android.hardware.input.IInputManager;
import android.hardware.input.KeyboardLayout;
import android.os.IBinder;
import android.os.ServiceManager;
import android.view.InputDevice;
import android.view.InputEvent;

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
    public String getCurrentKeyboardLayoutForInputDevice(String inputDeviceDescriptor) {
        return null;
    }

    @Override
    public void setCurrentKeyboardLayoutForInputDevice(String inputDeviceDescriptor,
            String keyboardLayoutDescriptor) {}

    @Override
    public String[] getKeyboardLayoutsForInputDevice(String inputDeviceDescriptor) {
        return new String[0];
    }

    @Override
    public void addKeyboardLayoutForInputDevice(String inputDeviceDescriptor,
            String keyboardLayoutDescriptor) {}

    @Override
    public void removeKeyboardLayoutForInputDevice(String inputDeviceDescriptor,
            String keyboardLayoutDescriptor) {}

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
