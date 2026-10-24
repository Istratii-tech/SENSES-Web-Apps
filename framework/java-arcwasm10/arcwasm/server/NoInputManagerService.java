// arc-wasm: an "input" service for a VM without the browser input
// dispatcher (topic fw10-system): Android always has one, and the framework
// asks it things on the way (InputManager.getInstance(): devices, tablet
// mode, pointer icons) even when no event will ever come. A device with
// only the virtual keyboard (KeyCharacterMap.VIRTUAL_KEYBOARD, which every
// device has: PhoneWindow loads its key map for menus) answers; its key map
// has no keys. With the graphics and input of topic fw10-graphics,
// its arcwasm.input.InputManagerService is registered instead (first).
package arcwasm.server;

import android.hardware.input.IInputDevicesChangedListener;
import android.hardware.input.IInputManager;
import android.hardware.input.ITabletModeChangedListener;
import android.hardware.input.InputDeviceIdentifier;
import android.hardware.input.KeyboardLayout;
import android.hardware.input.TouchCalibration;
import android.os.IBinder;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.PointerIcon;

final class NoInputManagerService extends IInputManager.Stub {
    private InputDevice mVirtual;
    private boolean mTried;

    /**
     * The virtual keyboard, made the way the input service sends it: from a
     * Parcel (InputDevice.writeToParcel's layout; the key map as libinput's
     * KeyCharacterMap::writeToParcel, type FULL, no keys). Null when the key
     * map's natives are not there (they are fw10-graphics' input JNI).
     */
    private synchronized InputDevice virtualKeyboard() {
        if (!mTried) {
            mTried = true;
            android.os.Parcel p = android.os.Parcel.obtain();
            try {
                p.writeInt(android.view.KeyCharacterMap.VIRTUAL_KEYBOARD);  // id
                p.writeInt(1);  // generation
                p.writeInt(0);  // controller number
                p.writeString("Virtual");
                p.writeInt(0);  // vendor
                p.writeInt(0);  // product
                p.writeString("arcwasm-virtual-keyboard");
                p.writeInt(0);  // not external
                p.writeInt(InputDevice.SOURCE_KEYBOARD);
                p.writeInt(InputDevice.KEYBOARD_TYPE_ALPHABETIC);
                p.writeInt(android.view.KeyCharacterMap.VIRTUAL_KEYBOARD);  // the map's device
                p.writeInt(android.view.KeyCharacterMap.FULL);
                p.writeInt(0);  // no keys
                p.writeInt(0);  // no vibrator
                p.writeInt(0);  // no microphone
                p.writeInt(0);  // no button under the pad
                p.writeInt(0);  // no motion ranges
                p.setDataPosition(0);
                mVirtual = InputDevice.CREATOR.createFromParcel(p);
            } catch (RuntimeException | LinkageError e) {
                android.util.Slog.w("NoInputManager", "no virtual keyboard: " + e);
            } finally {
                p.recycle();
            }
        }
        return mVirtual;
    }

    @Override
    public InputDevice getInputDevice(int deviceId) {
        InputDevice v = deviceId == android.view.KeyCharacterMap.VIRTUAL_KEYBOARD
                ? virtualKeyboard() : null;
        return v;
    }

    @Override
    public int[] getInputDeviceIds() {
        return virtualKeyboard() != null
                ? new int[] {android.view.KeyCharacterMap.VIRTUAL_KEYBOARD} : new int[0];
    }
    @Override public boolean isInputDeviceEnabled(int deviceId) { return false; }
    @Override public void enableInputDevice(int deviceId) {}
    @Override public void disableInputDevice(int deviceId) {}

    @Override
    public boolean hasKeys(int deviceId, int sourceMask, int[] keyCodes, boolean[] keyExists) {
        return false;
    }

    @Override public void tryPointerSpeed(int speed) {}
    @Override public boolean injectInputEvent(InputEvent ev, int mode) { return false; }

    @Override
    public TouchCalibration getTouchCalibrationForInputDevice(String descriptor, int rotation) {
        return TouchCalibration.IDENTITY;
    }

    @Override
    public void setTouchCalibrationForInputDevice(String descriptor, int rotation,
            TouchCalibration calibration) {}

    @Override public KeyboardLayout[] getKeyboardLayouts() { return new KeyboardLayout[0]; }

    @Override
    public KeyboardLayout[] getKeyboardLayoutsForInputDevice(InputDeviceIdentifier identifier) {
        return new KeyboardLayout[0];
    }

    @Override public KeyboardLayout getKeyboardLayout(String descriptor) { return null; }

    @Override
    public String getCurrentKeyboardLayoutForInputDevice(InputDeviceIdentifier identifier) {
        return null;
    }

    @Override
    public void setCurrentKeyboardLayoutForInputDevice(InputDeviceIdentifier identifier,
            String descriptor) {}

    @Override
    public String[] getEnabledKeyboardLayoutsForInputDevice(InputDeviceIdentifier identifier) {
        return new String[0];
    }

    @Override
    public void addKeyboardLayoutForInputDevice(InputDeviceIdentifier identifier,
            String descriptor) {}

    @Override
    public void removeKeyboardLayoutForInputDevice(InputDeviceIdentifier identifier,
            String descriptor) {}

    @Override
    public void registerInputDevicesChangedListener(IInputDevicesChangedListener listener) {}

    /** InputManager.SWITCH_STATE_OFF: not a tablet folded into tablet mode. */
    @Override public int isInTabletMode() { return 0; }

    @Override
    public void registerTabletModeChangedListener(ITabletModeChangedListener listener) {}

    @Override public void vibrate(int deviceId, long[] pattern, int repeat, IBinder token) {}
    @Override public void cancelVibrate(int deviceId, IBinder token) {}
    @Override public void setPointerIconType(int typeId) {}
    @Override public void setCustomPointerIcon(PointerIcon icon) {}
    @Override public void requestPointerCapture(IBinder windowToken, boolean enabled) {}

    @Override
    public android.view.InputMonitor monitorGestureInput(String name, int displayId) {
        throw new UnsupportedOperationException("no input dispatcher in this VM");
    }
}
