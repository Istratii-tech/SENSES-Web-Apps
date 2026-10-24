package arcwasm;

import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.PixelFormat;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.view.Gravity;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;

import arcwasm.server.Compositor;
import arcwasm.server.SystemServer;
import arcwasm.vulkan.VulkanSupport;

import java.io.File;
import java.util.concurrent.Callable;

/**
 * Vulkan on WebGPU (topic vulkan, docs/topics/vulkan.md), on ART 8.1 and 10:
 * libvktest.so (armeabi-v7a, /system/lib) and libvktest64.so (arm64-v8a,
 * /system/lib64), tests/vulkan/vktest.c, use the bridge's libvulkan.so as
 * engines do.
 *
 *   1. what the system reports: android.hardware.vulkan.* exactly when there
 *      is a device (the page's WebGPU adapter);
 *   2. both ABIs, the real path: with WebGPU (Safari 26, Chromium) a device
 *      whose results are checked (copies, a compute shader, a render pass);
 *      without (Node, older browsers) "no device": no instance extensions,
 *      vkCreateInstance = VK_ERROR_INCOMPATIBLE_DRIVER;
 *   3. a swapchain on a window (a SurfaceView's ANativeWindow): three frames
 *      cleared and presented; with a device the display shows the color --
 *      the frames go straight into the page's canvas over the window (topic
 *      vulkan-present), the last one stays in the window's layer when the
 *      swapchain goes, and where a view covers part of the window they are
 *      read back into the layer as before;
 *   4. both ABIs on the null device (debug.vulkan.null_device=1): the whole
 *      API without WebGPU -- objects, struct conversion for arm64, command
 *      recording, submits, fences and the swapchain.
 *
 * Prints one line per check and exits with the number of failures.
 */
public class VulkanTest {
    static int passed;
    static int failed;

    static native String nativeProbe32(int mode);
    static native String nativeProbe64(int mode);
    static native String nativeSwapchain32(Surface surface, int mode);
    static native String nativeSwapchain64(Surface surface, int mode);
    static native String nativeSwapchainEnd32();
    static native String nativeSwapchainEnd64();

    /** A SurfaceView whose surface tells when it exists. */
    static final class VkSurfaceView extends SurfaceView implements SurfaceHolder.Callback {
        volatile Surface surface;

        VkSurfaceView(Context c) {
            super(c);
            getHolder().addCallback(this);
        }

        public void surfaceCreated(SurfaceHolder h) {
            surface = h.getSurface();
        }

        public void surfaceChanged(SurfaceHolder h, int format, int w, int height) {}

        public void surfaceDestroyed(SurfaceHolder h) {
            surface = null;
        }
    }

    static WindowEnv sEnv;
    static FrameLayout sRoot;

    /** Two SurfaceViews (one per ABI) in a window; their surfaces. */
    static VkSurfaceView[] window() throws Exception {
        final WindowEnv env = WindowEnv.start();
        sEnv = env;
        final Context ctx = env.context;
        final WindowManager wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
        final VkSurfaceView[] views = new VkSurfaceView[2];
        env.onUi(new Callable<Void>() {
            public Void call() {
                FrameLayout root = new FrameLayout(ctx);
                sRoot = root;
                root.setBackgroundColor(0xff808080);
                for (int i = 0; i < 2; i++) {
                    views[i] = new VkSurfaceView(ctx);
                    FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(96, 64, Gravity.TOP | Gravity.LEFT);
                    lp.leftMargin = 20 + i * 140;
                    lp.topMargin = 40;
                    root.addView(views[i], lp);
                }
                WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                        WindowManager.LayoutParams.MATCH_PARENT, WindowManager.LayoutParams.MATCH_PARENT,
                        WindowManager.LayoutParams.TYPE_APPLICATION, 0, PixelFormat.OPAQUE);
                lp.setTitle("VulkanTest");
                wm.addView(root, lp);
                return null;
            }
        });
        long deadline = SystemClock.uptimeMillis() + 10000;
        while ((views[0].surface == null || views[1].surface == null) && SystemClock.uptimeMillis() < deadline) {
            Thread.sleep(10);
        }
        return views;
    }

    /** The display's pixel at the center of |v| (ARGB). */
    static int center(VkSurfaceView v) {
        return pixel(v, v.getWidth() / 2, v.getHeight() / 2);
    }

    /** The display's pixel at (x, y) of |v| (ARGB). */
    static int pixel(VkSurfaceView v, int x, int y) {
        int[] at = new int[2];
        v.getLocationOnScreen(at);
        return Compositor.readDisplay(at[0] + x, at[1] + y, 1, 1)[0];
    }

    /** An opaque view over the top left corner of |v| (null: removes it). */
    static View cover(final VkSurfaceView v, final View old) throws Exception {
        return sEnv.onUi(new Callable<View>() {
            public View call() {
                if (old != null) {
                    sRoot.removeView(old);
                    return null;
                }
                View c = new View(v.getContext());
                c.setBackgroundColor(0xff00c000);
                FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(24, 16, Gravity.TOP | Gravity.LEFT);
                FrameLayout.LayoutParams at = (FrameLayout.LayoutParams) v.getLayoutParams();
                lp.leftMargin = at.leftMargin;
                lp.topMargin = at.topMargin;
                sRoot.addView(c, lp);
                return c;
            }
        });
    }

    static boolean close(int a, int b, int tolerance) {
        for (int shift = 0; shift < 32; shift += 8) {
            if (Math.abs(((a >>> shift) & 0xff) - ((b >>> shift) & 0xff)) > tolerance) return false;
        }
        return true;
    }

    static void count(String abi, String report) {
        for (String line : report.split("\n")) {
            if (line.isEmpty()) continue;
            System.out.println("[" + abi + "] " + line);
            if (line.startsWith("ok:")) passed++;
            else if (line.startsWith("FAIL:")) failed++;
        }
    }

    static void expect(boolean ok, String what) {
        System.out.println((ok ? "ok: " : "FAIL: ") + what);
        if (ok) passed++; else failed++;
    }

    public static void main(String[] args) throws Exception {
        final long start = System.nanoTime();
        SystemServer.start();
        int version = VulkanSupport.version();
        boolean device = version != 0;
        System.out.println("VulkanTest: " + (device ? "a WebGPU device, Vulkan " + (version >>> 22) + "."
                + ((version >>> 12) & 0x3ff) : "no WebGPU here (no device)"));
        expect(SystemServer.packageManager().hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_VERSION,
                0x400000) == device, "android.hardware.vulkan.version 1.0 is reported exactly with a device");
        expect(SystemServer.packageManager().hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_LEVEL, 0)
                == device, "android.hardware.vulkan.level 0 likewise");
        expect(!SystemServer.packageManager().hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_VERSION,
                0x403000), "no Vulkan 1.3");
        boolean has32 = new File("/system/lib/libvktest.so").exists();
        boolean has64 = new File("/system/lib64/libvktest64.so").exists();
        if (has32) System.load("/system/lib/libvktest.so");
        if (has64) System.load("/system/lib64/libvktest64.so");
        expect(has32, "libvktest.so (armeabi-v7a) is in the image");
        int mode = device ? 2 : 0;
        if (has32) count("armeabi-v7a", nativeProbe32(mode));
        if (has64) count("arm64-v8a", nativeProbe64(mode));
        // A swapchain on a window: the frames reach the display.
        VkSurfaceView[] views = window();
        expect(views[0].surface != null && views[1].surface != null, "a window with two SurfaceViews");
        for (int i = 0; i < 2 && views[i].surface != null; i++) {
            if (i == 0 ? !has32 : !has64) continue;
            String abi = i == 0 ? "armeabi-v7a" : "arm64-v8a";
            int overlaid = VulkanSupport.overlayPresents();
            count(abi, i == 0 ? nativeSwapchain32(views[i].surface, mode)
                    : nativeSwapchain64(views[i].surface, mode));
            if (device) {
                Thread.sleep(200);  // the next composition
                int c = center(views[i]);
                expect(close(c, 0xffff0040, 3), abi + ": the window shows the presented frame: "
                        + String.format("#%08x", c));
                // The first frame goes into the layer (not composed yet), the
                // others straight into the page's canvas over it (no read-back).
                overlaid = VulkanSupport.overlayPresents() - overlaid;
                expect(overlaid >= 1, abi + ": frames shown by the page's canvas without a read-back: "
                        + overlaid + " of 3");
            }
            count(abi, i == 0 ? nativeSwapchainEnd32() : nativeSwapchainEnd64());
            if (device) {
                Thread.sleep(200);
                int c = center(views[i]);
                expect(close(c, 0xffff0040, 3), abi + ": the last frame stays in the window after the swapchain: "
                        + String.format("#%08x", c));
            }
        }
        // A view over part of the window: nothing over the window may be
        // hidden by the page's canvas, so the frames are read back into the
        // window's layer.
        if (device && (has32 || has64) && views[has32 ? 0 : 1].surface != null) {
            int i = has32 ? 0 : 1;
            String abi = (i == 0 ? "armeabi-v7a" : "arm64-v8a") + " covered";
            View c = cover(views[i], null);
            Thread.sleep(200);  // drawn and composed
            int overlaid = VulkanSupport.overlayPresents();
            count(abi, i == 0 ? nativeSwapchain32(views[i].surface, mode) : nativeSwapchain64(views[i].surface, mode));
            Thread.sleep(200);
            overlaid = VulkanSupport.overlayPresents() - overlaid;
            expect(overlaid == 0, abi + ": a view over the window: no frame shown by the page's canvas ("
                    + overlaid + ")");
            int px = center(views[i]), pc = pixel(views[i], 4, 4);
            expect(close(px, 0xffff0040, 3) && close(pc, 0xff00c000, 3), abi + ": the frame read back, the view over it: "
                    + String.format("#%08x, #%08x", px, pc));
            count(abi, i == 0 ? nativeSwapchainEnd32() : nativeSwapchainEnd64());
            cover(views[i], c);
        }
        // The null device: everything but the GPU.
        SystemProperties.set("debug.vulkan.null_device", "1");
        try {
            if (has32) count("armeabi-v7a null", nativeProbe32(1));
            if (has64) count("arm64-v8a null", nativeProbe64(1));
            if (has32 && views[0].surface != null) {
                count("armeabi-v7a null", nativeSwapchain32(views[0].surface, 1));
                count("armeabi-v7a null", nativeSwapchainEnd32());
            }
            if (has64 && views[1].surface != null) {
                count("arm64-v8a null", nativeSwapchain64(views[1].surface, 1));
                count("arm64-v8a null", nativeSwapchainEnd64());
            }
        } finally {
            SystemProperties.set("debug.vulkan.null_device", "0");
        }
        long ms = (System.nanoTime() - start) / 1000000;
        System.out.println("VulkanTest: " + passed + " passed, " + failed + " failed (" + ms + " ms)");
        System.exit(failed);
    }
}
