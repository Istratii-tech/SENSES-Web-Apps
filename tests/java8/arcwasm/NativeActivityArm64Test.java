package arcwasm;

/**
 * NativeActivityTest with the arm64-v8a build of the app
 * (/system/app/ArcWasmNativeActivityTestArm64.apk, org.arcwasm.natest.arm64):
 * a VM of its own, as a VM runs one app process (topic native-activity,
 * docs/topics/native-activity.md).
 */
public class NativeActivityArm64Test {
    public static void main(String[] args) throws Exception {
        NativeActivityTest.main(new String[] {"arm64-v8a"});
    }
}
