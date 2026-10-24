package arcwasm.vulkan;

import android.util.Slog;

/**
 * Whether this system has Vulkan (topic vulkan, docs/topics/vulkan.md):
 * libvulkan.so of the NDK bridge implements Vulkan 1.1 on the page's
 * WebGPU, so there is a device only where the browser has a WebGPU adapter
 * (Safari 26, Chromium). The package manager reports
 * android.hardware.vulkan.level/version/compute from this, and apps that
 * check them before trying Vulkan take GLES elsewhere. The natives are in
 * artvm's static JNI library libarcwasm_vulkan.so (shim/vulkan/vk_jni.cc).
 */
public final class VulkanSupport {
    private static final String TAG = "VulkanSupport";
    private static int sVersion = -1;

    private VulkanSupport() {}

    /** VK_MAKE_API_VERSION of the device, 0 if there is none. */
    public static synchronized int version() {
        if (sVersion < 0) {
            sVersion = 0;
            try {
                System.load("/system/lib/libarcwasm_vulkan.so");
                sVersion = nativeVersion();
            } catch (Throwable t) {
                Slog.i(TAG, "no Vulkan: " + t);
            }
            Slog.i(TAG, "Vulkan " + (sVersion == 0 ? "absent (no WebGPU)"
                    : (sVersion >>> 22) + "." + ((sVersion >>> 12) & 0x3ff)));
        }
        return sVersion;
    }

    /**
     * Swapchain presents that went straight into the page's canvas over the
     * window, without a read-back (topic vulkan-present; for tests).
     */
    public static int overlayPresents() {
        return version() == 0 ? 0 : nativeOverlayPresents();
    }

    private static native int nativeVersion();
    private static native int nativeOverlayPresents();
}
