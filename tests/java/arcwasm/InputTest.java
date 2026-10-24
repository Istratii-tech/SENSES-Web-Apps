package arcwasm;

import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.Parcel;
import android.os.SystemClock;
import android.view.InputChannel;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.InputEventReceiver;
import android.view.InputEventSender;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;

import arcwasm.input.BrowserInput;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * Checks input the way Android delivers it to a window: an InputChannel
 * pair over posix_translation's unix sockets, the server end registered
 * with the browser input dispatcher (arcwasm.input.BrowserInput, like
 * InputDispatcher for the focused window), the client end read by an
 * InputEventReceiver on a Looper thread. Events are injected as the page
 * reports DOM events (the same queue and dispatcher thread), so this is
 * the browser path minus the DOM. Also: the "input" service with the
 * browser's devices and key character maps, InputEventSender, MotionEvent
 * and InputDevice through Parcel. Prints one line per check and exits with
 * the number of failures.
 */
public class InputTest {
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

    private static void expectEq(Object expected, Object actual, String what) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError(what + ": expected <" + expected + "> but was <" + actual
                    + ">");
        }
    }

    /** Copies what it gets (the receiver recycles the originals) and finishes them. */
    static final class Receiver extends InputEventReceiver {
        final LinkedBlockingQueue<InputEvent> events = new LinkedBlockingQueue<InputEvent>();
        volatile boolean handled = true;
        volatile int batchesPending;

        Receiver(InputChannel channel, Looper looper) {
            super(channel, looper);
        }

        // (No @Override: 4.4 and 6.0 call onInputEvent(event), 8.x
        // onInputEvent(event, displayId); the tests run on all three.)
        public void onInputEvent(InputEvent event) {
            events.add(event instanceof MotionEvent ? MotionEvent.obtain((MotionEvent) event)
                    : new KeyEvent((KeyEvent) event));
            finishInputEvent(event, handled);
        }

        public void onInputEvent(InputEvent event, int displayId) {
            onInputEvent(event);
        }

        @Override
        public void onBatchedInputEventPending() {
            batchesPending++;
            super.onBatchedInputEventPending();
        }

        InputEvent next() throws InterruptedException {
            InputEvent event = events.poll(WAIT_MS, TimeUnit.MILLISECONDS);
            expect(event != null, "timed out waiting for an input event");
            return event;
        }

        MotionEvent nextMotion() throws InterruptedException {
            InputEvent event = next();
            expect(event instanceof MotionEvent, "expected a MotionEvent, got " + event);
            return (MotionEvent) event;
        }

        KeyEvent nextKey() throws InterruptedException {
            InputEvent event = next();
            expect(event instanceof KeyEvent, "expected a KeyEvent, got " + event);
            return (KeyEvent) event;
        }

        void expectNothingMore() throws InterruptedException {
            InputEvent event = events.poll(50, TimeUnit.MILLISECONDS);
            expect(event == null, "unexpected event " + event);
        }
    }

    private static Handler sHandler;
    private static Receiver sReceiver;
    private static InputChannel sServerChannel;

    /**
     * Runs the injections on the receiver's own Looper thread and waits there
     * until the dispatcher published them all, so they are all in the socket
     * when the receiver next reads it (deterministic batching).
     */
    private static void injectOnLooper(final Runnable injections) throws InterruptedException {
        final CountDownLatch done = new CountDownLatch(1);
        sHandler.post(new Runnable() {
            @Override
            public void run() {
                injections.run();
                done.countDown();
            }
        });
        expect(done.await(WAIT_MS, TimeUnit.MILLISECONDS), "injection timed out");
    }

    private static int pointer(int type, int id, float x, float y, long time) {
        return BrowserInput.injectPointer(type, id, BrowserInput.POINTER_TYPE_TOUCH, x, y, 1.0f,
                time);
    }

    private static long now() {
        return SystemClock.uptimeMillis() * 1000000L;
    }

    public static void main(String[] args) throws Exception {
        long start = SystemClock.uptimeMillis();
        final int[] display = BrowserInput.getDisplaySize();

        check("input service: devices of the browser", new Check() {
            public void run() {
                int[] ids = InputDevice.getDeviceIds();
                expectEq(3, ids.length, "device count");
                InputDevice touch = InputDevice.getDevice(BrowserInput.DEVICE_ID_TOUCHSCREEN);
                expect(touch != null, "no touchscreen");
                expectEq(InputDevice.SOURCE_TOUCHSCREEN, touch.getSources(), "touchscreen source");
                InputDevice.MotionRange x = touch.getMotionRange(MotionEvent.AXIS_X);
                expect(x != null, "no X range");
                expectEq((float) (display[0] - 1), x.getMax(), "X max");
                InputDevice keyboard = InputDevice.getDevice(BrowserInput.DEVICE_ID_KEYBOARD);
                expectEq(InputDevice.KEYBOARD_TYPE_ALPHABETIC, keyboard.getKeyboardType(),
                        "keyboard type");
                expect((keyboard.getSources() & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD, "keyboard source");
                expectEq("Browser keyboard", keyboard.getName(), "name");
                expect(InputDevice.getDevice(KeyCharacterMap.VIRTUAL_KEYBOARD) != null,
                        "no virtual keyboard");
                boolean[] has = InputManager.getInstance().deviceHasKeys(new int[] {
                        KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_BUTTON_A});
                expect(has[0] && has[1] && !has[2], "deviceHasKeys: A, ENTER, not BUTTON_A");
                expect(KeyCharacterMap.deviceHasKey(KeyEvent.KEYCODE_SPACE), "deviceHasKey SPACE");
            }
        });

        check("KeyCharacterMap: Generic.kcm and Virtual.kcm", new Check() {
            public void run() {
                KeyCharacterMap kcm = KeyCharacterMap.load(BrowserInput.DEVICE_ID_KEYBOARD);
                expectEq(KeyCharacterMap.FULL, kcm.getKeyboardType(), "keyboard type");
                expectEq((int) 'a', kcm.get(KeyEvent.KEYCODE_A, 0), "a");
                expectEq((int) 'A', kcm.get(KeyEvent.KEYCODE_A, KeyEvent.META_SHIFT_ON), "A");
                expectEq((int) '!', kcm.get(KeyEvent.KEYCODE_1, KeyEvent.META_SHIFT_ON), "!");
                expectEq('A', kcm.getDisplayLabel(KeyEvent.KEYCODE_A), "label");
                expectEq('7', kcm.getNumber(KeyEvent.KEYCODE_7), "number");
                KeyEvent[] events = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD)
                        .getEvents("Hi".toCharArray());
                expect(events != null && events.length >= 4, "getEvents(\"Hi\")");
                StringBuilder typed = new StringBuilder();
                for (KeyEvent e : events) {
                    if (e.getAction() == KeyEvent.ACTION_DOWN && e.getUnicodeChar() != 0) {
                        typed.append((char) e.getUnicodeChar());
                    }
                }
                expectEq("Hi", typed.toString(), "typed back");
            }
        });

        // The window: a channel pair, the server end with the dispatcher, the
        // client end read on a Looper thread.
        HandlerThread thread = new HandlerThread("input-receiver");
        thread.start();
        sHandler = new Handler(thread.getLooper());
        InputChannel[] pair = InputChannel.openInputChannelPair("InputTest");
        sServerChannel = pair[0];
        BrowserInput.registerInputChannel(sServerChannel);
        sReceiver = new Receiver(pair[1], thread.getLooper());
        final int[] stats0 = BrowserInput.getStats();

        check("tap: DOWN and UP on the touchscreen", new Check() {
            public void run() throws Exception {
                long t = now();
                pointer(BrowserInput.POINTER_DOWN, 1, 100.5f, 200.25f, t);
                pointer(BrowserInput.POINTER_UP, 1, 101f, 201f, t + 30000000L);
                MotionEvent down = sReceiver.nextMotion();
                expectEq(MotionEvent.ACTION_DOWN, down.getAction(), "action");
                expectEq(InputDevice.SOURCE_TOUCHSCREEN, down.getSource(), "source");
                expectEq(BrowserInput.DEVICE_ID_TOUCHSCREEN, down.getDeviceId(), "device");
                expectEq(100.5f, down.getX(), "x");
                expectEq(200.25f, down.getY(), "y");
                expectEq(1.0f, down.getPressure(), "pressure");
                expectEq(1, down.getPointerCount(), "pointers");
                expectEq(0, down.getPointerId(0), "pointer id");
                expectEq(MotionEvent.TOOL_TYPE_FINGER, down.getToolType(0), "tool");
                expectEq(t, down.getEventTimeNano(), "event time");
                expectEq(down.getEventTime(), down.getDownTime(), "down time");
                long age = SystemClock.uptimeMillis() - down.getEventTime();
                expect(age >= 0 && age < 2000, "event time on the uptime clock (age " + age + " ms)");
                MotionEvent up = sReceiver.nextMotion();
                expectEq(MotionEvent.ACTION_UP, up.getAction(), "up action");
                expectEq(101f, up.getX(), "up x");
                expectEq(down.getDownTime(), up.getDownTime(), "up down time");
                expectEq(30L, up.getEventTime() - up.getDownTime(), "duration");
                sReceiver.expectNothingMore();
            }
        });

        check("drag: moves batched into one MotionEvent with history", new Check() {
            public void run() throws Exception {
                final long t = now();
                final int batches = sReceiver.batchesPending;
                injectOnLooper(new Runnable() {
                    public void run() {
                        int ticket = pointer(BrowserInput.POINTER_DOWN, 7, 10, 10, t);
                        for (int i = 1; i <= 5; i++) {
                            ticket = pointer(BrowserInput.POINTER_MOVE, 7, 10 + 10 * i, 10 + i,
                                    t + i * 8000000L);
                        }
                        ticket = pointer(BrowserInput.POINTER_UP, 7, 60, 15, t + 48000000L);
                        BrowserInput.waitUntilDispatched(ticket);
                    }
                });
                expectEq(MotionEvent.ACTION_DOWN, sReceiver.nextMotion().getAction(), "down");
                MotionEvent move = sReceiver.nextMotion();
                expectEq(MotionEvent.ACTION_MOVE, move.getAction(), "move");
                expectEq(4, move.getHistorySize(), "history size");
                for (int h = 0; h < 4; h++) {
                    expectEq(20f + 10 * h, move.getHistoricalX(h), "historical x " + h);
                    expectEq((t + (h + 1) * 8000000L) / 1000000L, move.getHistoricalEventTime(h),
                            "historical time " + h);
                }
                expectEq(60f, move.getX(), "x");
                expectEq(15f, move.getY(), "y");
                expectEq(MotionEvent.ACTION_UP, sReceiver.nextMotion().getAction(), "up");
                sReceiver.expectNothingMore();
                // Only moves are batched; the UP flushed this batch, so no
                // onBatchedInputEventPending was needed.
                expectEq(batches, sReceiver.batchesPending, "batch callbacks");
            }
        });

        check("moves without an UP: onBatchedInputEventPending, consumeBatchedInputEvents", new Check() {
            public void run() throws Exception {
                final long t = now();
                final int batches = sReceiver.batchesPending;
                injectOnLooper(new Runnable() {
                    public void run() {
                        pointer(BrowserInput.POINTER_DOWN, 3, 5, 5, t);
                        pointer(BrowserInput.POINTER_MOVE, 3, 6, 5, t + 1000000L);
                        pointer(BrowserInput.POINTER_MOVE, 3, 7, 5, t + 2000000L);
                        BrowserInput.waitUntilDispatched(
                                pointer(BrowserInput.POINTER_MOVE, 3, 8, 5, t + 3000000L));
                    }
                });
                expectEq(MotionEvent.ACTION_DOWN, sReceiver.nextMotion().getAction(), "down");
                MotionEvent move = sReceiver.nextMotion();
                expectEq(MotionEvent.ACTION_MOVE, move.getAction(), "move");
                expectEq(2, move.getHistorySize(), "history size");
                expectEq(8f, move.getX(), "x");
                expect(sReceiver.batchesPending > batches, "onBatchedInputEventPending called");
                pointer(BrowserInput.POINTER_CANCEL, 3, 8, 5, 0);
                expectEq(MotionEvent.ACTION_CANCEL, sReceiver.nextMotion().getAction(), "cancel");
            }
        });

        check("multi-touch: POINTER_DOWN/POINTER_UP with indices and ids", new Check() {
            public void run() throws Exception {
                long t = now();
                pointer(BrowserInput.POINTER_DOWN, 10, 10, 10, t);
                pointer(BrowserInput.POINTER_DOWN, 11, 100, 100, t + 1000000L);
                pointer(BrowserInput.POINTER_UP, 10, 12, 12, t + 3000000L);
                pointer(BrowserInput.POINTER_DOWN, 12, 50, 50, t + 4000000L);
                pointer(BrowserInput.POINTER_UP, 11, 105, 105, t + 5000000L);
                pointer(BrowserInput.POINTER_UP, 12, 50, 50, t + 6000000L);

                MotionEvent e = sReceiver.nextMotion();
                expectEq(MotionEvent.ACTION_DOWN, e.getActionMasked(), "first down");
                e = sReceiver.nextMotion();
                expectEq(MotionEvent.ACTION_POINTER_DOWN, e.getActionMasked(), "second down");
                expectEq(1, e.getActionIndex(), "second down index");
                expectEq(2, e.getPointerCount(), "two pointers");
                expectEq(1, e.getPointerId(1), "second id");
                expectEq(100f, e.getX(1), "second x");
                expectEq(10f, e.getX(0), "first x");
                e = sReceiver.nextMotion();
                expectEq(MotionEvent.ACTION_POINTER_UP, e.getActionMasked(), "first up");
                expectEq(0, e.getActionIndex(), "first up index");
                expectEq(0, e.getPointerId(e.getActionIndex()), "first up id");
                expectEq(12f, e.getX(0), "first up x");
                e = sReceiver.nextMotion();
                // The new finger takes the lowest free id, 0, and sorts first.
                expectEq(MotionEvent.ACTION_POINTER_DOWN, e.getActionMasked(), "third down");
                expectEq(0, e.getActionIndex(), "third down index");
                expectEq(0, e.getPointerId(0), "third id");
                expectEq(1, e.findPointerIndex(1) , "id 1 at index 1");
                e = sReceiver.nextMotion();
                expectEq(MotionEvent.ACTION_POINTER_UP, e.getActionMasked(), "second up");
                expectEq(1, e.getPointerId(e.getActionIndex()), "second up id");
                expectEq(105f, e.getX(e.getActionIndex()), "second up x");
                e = sReceiver.nextMotion();
                expectEq(MotionEvent.ACTION_UP, e.getActionMasked(), "last up");
                expectEq(1, e.getPointerCount(), "one pointer left");
                expectEq(0, e.getPointerId(0), "last id");
                sReceiver.expectNothingMore();
            }
        });

        check("keys: KeyboardEvent.code through Generic.kl, meta state, getUnicodeChar", new Check() {
            public void run() throws Exception {
                int shift = BrowserInput.MOD_SHIFT;
                long t = now();
                BrowserInput.injectKey(true, "ShiftLeft", shift, false, t);
                BrowserInput.injectKey(true, "KeyH", shift, false, t + 1000000L);
                BrowserInput.injectKey(false, "KeyH", shift, false, t + 2000000L);
                BrowserInput.injectKey(false, "ShiftLeft", 0, false, t + 3000000L);
                BrowserInput.injectKey(true, "KeyI", 0, false, t + 4000000L);
                BrowserInput.injectKey(false, "KeyI", 0, false, t + 5000000L);

                KeyEvent e = sReceiver.nextKey();
                expectEq(KeyEvent.KEYCODE_SHIFT_LEFT, e.getKeyCode(), "shift key code");
                expect(e.isShiftPressed(), "shift pressed on its own down");
                e = sReceiver.nextKey();
                expectEq(KeyEvent.ACTION_DOWN, e.getAction(), "H down");
                expectEq(KeyEvent.KEYCODE_H, e.getKeyCode(), "H key code");
                expectEq(35, e.getScanCode(), "H scan code (KEY_H)");
                expectEq(BrowserInput.DEVICE_ID_KEYBOARD, e.getDeviceId(), "device");
                expectEq(InputDevice.SOURCE_KEYBOARD, e.getSource(), "source");
                expectEq(KeyEvent.META_SHIFT_ON | KeyEvent.META_SHIFT_LEFT_ON, e.getMetaState(),
                        "meta state");
                expectEq((int) 'H', e.getUnicodeChar(), "H");
                expectEq((int) 'h', e.getUnicodeChar(0), "h without meta");
                expectEq((t + 1000000L) / 1000000L, e.getDownTime(), "down time");
                e = sReceiver.nextKey();
                expectEq(KeyEvent.ACTION_UP, e.getAction(), "H up");
                expectEq((t + 1000000L) / 1000000L, e.getDownTime(), "up keeps down time");
                e = sReceiver.nextKey();
                expectEq(KeyEvent.ACTION_UP, e.getAction(), "shift up");
                expect(!e.isShiftPressed(), "shift released");
                e = sReceiver.nextKey();
                expectEq(KeyEvent.KEYCODE_I, e.getKeyCode(), "I");
                expectEq(0, e.getMetaState(), "no meta");
                expectEq((int) 'i', e.getUnicodeChar(), "i");
                sReceiver.nextKey();
                sReceiver.expectNothingMore();
            }
        });

        check("keys: repeat, caps lock, Enter, Digit1+Shift, unknown code dropped", new Check() {
            public void run() throws Exception {
                int dropped = BrowserInput.getStats()[3];
                BrowserInput.injectKey(true, "KeyA", BrowserInput.MOD_CAPS_LOCK, false, 0);
                BrowserInput.injectKey(true, "KeyA", BrowserInput.MOD_CAPS_LOCK, true, 0);
                BrowserInput.injectKey(true, "KeyA", BrowserInput.MOD_CAPS_LOCK, true, 0);
                BrowserInput.injectKey(false, "KeyA", BrowserInput.MOD_CAPS_LOCK, false, 0);
                BrowserInput.injectKey(true, "NoSuchKey", 0, false, 0);
                BrowserInput.injectKey(true, "Enter", 0, false, 0);
                BrowserInput.injectKey(true, "Digit1", BrowserInput.MOD_SHIFT, false, 0);
                BrowserInput.injectKey(false, "Digit1", BrowserInput.MOD_SHIFT, false, 0);
                BrowserInput.injectKey(false, "Enter", 0, false, 0);
                KeyEvent first = sReceiver.nextKey();
                expectEq(0, first.getRepeatCount(), "first repeat count");
                expect(first.isCapsLockOn(), "caps lock on");
                expectEq((int) 'A', first.getUnicodeChar(), "caps A");
                KeyEvent r1 = sReceiver.nextKey();
                KeyEvent r2 = sReceiver.nextKey();
                expectEq(1, r1.getRepeatCount(), "repeat 1");
                expectEq(2, r2.getRepeatCount(), "repeat 2");
                expectEq(first.getDownTime(), r2.getDownTime(), "repeats keep down time");
                KeyEvent up = sReceiver.nextKey();
                expectEq(KeyEvent.ACTION_UP, up.getAction(), "A up");
                expectEq(0, up.getRepeatCount(), "up repeat count");
                KeyEvent enter = sReceiver.nextKey();
                expectEq(KeyEvent.KEYCODE_ENTER, enter.getKeyCode(), "Enter");
                expectEq((int) '\n', enter.getUnicodeChar(), "newline");
                KeyEvent one = sReceiver.nextKey();
                expectEq(KeyEvent.KEYCODE_1, one.getKeyCode(), "1");
                expectEq((int) '!', one.getUnicodeChar(), "!");
                sReceiver.nextKey();
                sReceiver.nextKey();
                sReceiver.expectNothingMore();
                expectEq(dropped + 1, BrowserInput.getStats()[3], "dropped");
                expectEq(KeyEvent.KEYCODE_DPAD_LEFT, BrowserInput.keyCodeForDomCode("ArrowLeft"),
                        "ArrowLeft");
                expectEq(KeyEvent.KEYCODE_BACK, BrowserInput.keyCodeForDomCode("BrowserBack"),
                        "BrowserBack");
                expectEq(KeyEvent.KEYCODE_ESCAPE, BrowserInput.keyCodeForDomCode("Escape"),
                        "Escape");
            }
        });

        check("finishInputEvent: finished signals back at the dispatcher", new Check() {
            public void run() throws Exception {
                int[] before = BrowserInput.getStats();
                // Everything so far was finished (handled = true).
                int published = before[0] - stats0[0];
                expect(published >= 30, "published " + published);
                expectEq(before[0], BrowserInput.waitForFinished(before[0], (int) WAIT_MS),
                        "all finished");
                expectEq(before[0], BrowserInput.getStats()[2], "all handled");
                sReceiver.handled = false;
                pointer(BrowserInput.POINTER_DOWN, 1, 1, 1, 0);
                pointer(BrowserInput.POINTER_UP, 1, 1, 1, 0);
                sReceiver.nextMotion();
                sReceiver.nextMotion();
                expectEq(before[0] + 2, BrowserInput.waitForFinished(before[0] + 2, (int) WAIT_MS),
                        "two more finished");
                int[] after = BrowserInput.getStats();
                expectEq(before[0] + 2, after[0], "published");
                expectEq(before[2], after[2], "not handled");
                sReceiver.handled = true;
            }
        });

        check("unregisterInputChannel: input goes nowhere", new Check() {
            public void run() throws Exception {
                BrowserInput.unregisterInputChannel(sServerChannel);
                int dropped = BrowserInput.getStats()[3];
                BrowserInput.waitUntilDispatched(pointer(BrowserInput.POINTER_DOWN, 1, 1, 1, 0));
                expectEq(dropped + 1, BrowserInput.getStats()[3], "dropped");
                sReceiver.expectNothingMore();
                BrowserInput.registerInputChannel(sServerChannel);
                pointer(BrowserInput.POINTER_UP, 1, 1, 1, 0);  // the down went nowhere
                pointer(BrowserInput.POINTER_DOWN, 1, 2, 2, 0);
                expectEq(MotionEvent.ACTION_DOWN, sReceiver.nextMotion().getAction(), "down");
                pointer(BrowserInput.POINTER_UP, 1, 2, 2, 0);
                expectEq(MotionEvent.ACTION_UP, sReceiver.nextMotion().getAction(), "up");
            }
        });

        check("InputEventSender to InputEventReceiver over a channel pair", new Check() {
            public void run() throws Exception {
                InputChannel[] pair = InputChannel.openInputChannelPair("sender");
                final LinkedBlockingQueue<int[]> finished = new LinkedBlockingQueue<int[]>();
                InputEventSender sender = new InputEventSender(pair[0], sHandler.getLooper()) {
                    @Override
                    public void onInputEventFinished(int seq, boolean handled) {
                        finished.add(new int[] {seq, handled ? 1 : 0});
                    }
                };
                Receiver receiver = new Receiver(pair[1], sHandler.getLooper());
                long t = SystemClock.uptimeMillis();
                KeyEvent key = new KeyEvent(t, t, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_Z, 0,
                        KeyEvent.META_SHIFT_ON);
                expect(sender.sendInputEvent(42, key), "sendInputEvent");
                KeyEvent got = receiver.nextKey();
                expectEq(KeyEvent.KEYCODE_Z, got.getKeyCode(), "key code");
                expectEq((int) 'Z', got.getUnicodeChar(), "Z (virtual keyboard map)");
                int[] f = finished.poll(WAIT_MS, TimeUnit.MILLISECONDS);
                expect(f != null, "no finished signal");
                expectEq(42, f[0], "seq");
                expectEq(1, f[1], "handled");
                receiver.dispose();
                sender.dispose();
                pair[0].dispose();
                pair[1].dispose();
            }
        });

        check("Parcel: MotionEvent with history and InputDevice with its key map", new Check() {
            public void run() {
                MotionEvent e = MotionEvent.obtain(100, 100, MotionEvent.ACTION_MOVE, 3f, 4f, 0);
                e.addBatch(116, 5f, 6f, 1f, 1f, 0);
                Parcel p = Parcel.obtain();
                e.writeToParcel(p, 0);
                p.setDataPosition(0);
                MotionEvent back = MotionEvent.CREATOR.createFromParcel(p);
                expectEq(1, back.getHistorySize(), "history");
                expectEq(3f, back.getHistoricalX(0), "historical x");
                expectEq(5f, back.getX(), "x");
                expectEq(116L, back.getEventTime(), "time");
                p.setDataPosition(0);
                InputDevice.getDevice(BrowserInput.DEVICE_ID_KEYBOARD).writeToParcel(p, 0);
                p.setDataPosition(0);
                InputDevice device = InputDevice.CREATOR.createFromParcel(p);
                expectEq("Browser keyboard", device.getName(), "name");
                expectEq((int) 'q', device.getKeyCharacterMap().get(KeyEvent.KEYCODE_Q, 0), "q");
                p.recycle();
            }
        });

        sReceiver.dispose();
        thread.quit();
        long ms = SystemClock.uptimeMillis() - start;
        System.out.println("InputTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
