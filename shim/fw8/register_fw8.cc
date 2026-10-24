// The framework's JNI registrations, as gRegJNI in AOSP 8.1's
// frameworks/base/core/jni/AndroidRuntime.cpp (startReg), in its order:
// registering looks classes up, and the order is the one the framework's
// static initializers were written for. Generated once from that file, as
// shim/fw6/register_fw6.cc was for 6.0.1.
//
// Every register_* is declared weak: a topic that builds some of
// libandroid_runtime links its objects in, and registrations that are not
// linked are skipped (ARC_FW8_JNI_VERBOSE=1 lists them). A topic links its
// JNI objects with --whole-archive (FW8_JNI_LIBS in mk/topic-fw8-core.mk):
// nothing else refers to them strongly, and wasm-ld does not pull archive
// members in for weak references. Natives of classes that are not linked
// stay unregistered and throw UnsatisfiedLinkError when called.
//
// The entries after gRegJNI's are arc-wasm's (in-process servicemanager and
// binder thread pool; the hooks of the fw8 topics).
// Part of the arc-wasm shim layer.

#include <jni.h>
#include <stdio.h>
#include <stdlib.h>

#define WEAK __attribute__((weak))

// Declared outside namespace android in AndroidRuntime.cpp, as defined.

int register_android_os_Binder(JNIEnv*) WEAK;
int register_android_os_Process(JNIEnv*) WEAK;
int register_android_graphics_Bitmap(JNIEnv*) WEAK;
int register_android_graphics_BitmapFactory(JNIEnv*) WEAK;
int register_android_graphics_BitmapRegionDecoder(JNIEnv*) WEAK;
int register_android_graphics_Camera(JNIEnv*) WEAK;
int register_android_graphics_CreateJavaOutputStreamAdaptor(JNIEnv*) WEAK;
int register_android_graphics_GraphicBuffer(JNIEnv*) WEAK;
int register_android_graphics_Graphics(JNIEnv*) WEAK;
int register_android_graphics_Interpolator(JNIEnv*) WEAK;
int register_android_graphics_MaskFilter(JNIEnv*) WEAK;
int register_android_graphics_Movie(JNIEnv*) WEAK;
int register_android_graphics_NinePatch(JNIEnv*) WEAK;
int register_android_graphics_PathEffect(JNIEnv*) WEAK;
int register_android_graphics_Shader(JNIEnv*) WEAK;
int register_android_graphics_Typeface(JNIEnv*) WEAK;
int register_android_graphics_YuvImage(JNIEnv*) WEAK;
int register_com_google_android_gles_jni_EGLImpl(JNIEnv*) WEAK;
int register_com_google_android_gles_jni_GLImpl(JNIEnv*) WEAK;
int register_android_opengl_jni_EGL14(JNIEnv*) WEAK;
int register_android_opengl_jni_EGLExt(JNIEnv*) WEAK;
int register_android_opengl_jni_GLES10(JNIEnv*) WEAK;
int register_android_opengl_jni_GLES10Ext(JNIEnv*) WEAK;
int register_android_opengl_jni_GLES11(JNIEnv*) WEAK;
int register_android_opengl_jni_GLES11Ext(JNIEnv*) WEAK;
int register_android_opengl_jni_GLES20(JNIEnv*) WEAK;
int register_android_opengl_jni_GLES30(JNIEnv*) WEAK;
int register_android_opengl_jni_GLES31(JNIEnv*) WEAK;
int register_android_opengl_jni_GLES31Ext(JNIEnv*) WEAK;
int register_android_opengl_jni_GLES32(JNIEnv*) WEAK;
int register_android_hardware_Camera(JNIEnv*) WEAK;
int register_android_hardware_camera2_CameraMetadata(JNIEnv*) WEAK;
int register_android_hardware_camera2_legacy_LegacyCameraDevice(JNIEnv*) WEAK;
int register_android_hardware_camera2_legacy_PerfMeasurement(JNIEnv*) WEAK;
int register_android_hardware_camera2_DngCreator(JNIEnv*) WEAK;
int register_android_hardware_HardwareBuffer(JNIEnv*) WEAK;
int register_android_hardware_SensorManager(JNIEnv*) WEAK;
int register_android_hardware_SerialPort(JNIEnv*) WEAK;
int register_android_hardware_SoundTrigger(JNIEnv*) WEAK;
int register_android_hardware_UsbDevice(JNIEnv*) WEAK;
int register_android_hardware_UsbDeviceConnection(JNIEnv*) WEAK;
int register_android_hardware_UsbRequest(JNIEnv*) WEAK;
int register_android_hardware_location_ActivityRecognitionHardware(JNIEnv*) WEAK;
int register_android_media_AudioRecord(JNIEnv*) WEAK;
int register_android_media_AudioSystem(JNIEnv*) WEAK;
int register_android_media_AudioTrack(JNIEnv*) WEAK;
int register_android_media_JetPlayer(JNIEnv*) WEAK;
int register_android_media_ToneGenerator(JNIEnv*) WEAK;

namespace android {

// The frame's own (shim/fw8/android_runtime8.cc).
int register_com_android_internal_os_RuntimeInit(JNIEnv*);
int register_com_android_internal_os_ZygoteInit_nativeZygoteInit(JNIEnv*);

int register_android_app_admin_SecurityLog(JNIEnv*) WEAK;
int register_android_content_AssetManager(JNIEnv*) WEAK;
int register_android_util_EventLog(JNIEnv*) WEAK;
int register_android_util_Log(JNIEnv*) WEAK;
int register_android_util_MemoryIntArray(JNIEnv*) WEAK;
int register_android_util_PathParser(JNIEnv*) WEAK;
int register_android_content_StringBlock(JNIEnv*) WEAK;
int register_android_content_XmlBlock(JNIEnv*) WEAK;
int register_android_graphics_Canvas(JNIEnv*) WEAK;
int register_android_graphics_CanvasProperty(JNIEnv*) WEAK;
int register_android_graphics_ColorFilter(JNIEnv*) WEAK;
int register_android_graphics_DrawFilter(JNIEnv*) WEAK;
int register_android_graphics_FontFamily(JNIEnv*) WEAK;
int register_android_graphics_Matrix(JNIEnv*) WEAK;
int register_android_graphics_Paint(JNIEnv*) WEAK;
int register_android_graphics_Path(JNIEnv*) WEAK;
int register_android_graphics_PathMeasure(JNIEnv*) WEAK;
int register_android_graphics_Picture(JNIEnv*) WEAK;
int register_android_graphics_Region(JNIEnv*) WEAK;
int register_android_graphics_SurfaceTexture(JNIEnv*) WEAK;
int register_android_graphics_drawable_AnimatedVectorDrawable(JNIEnv*) WEAK;
int register_android_graphics_drawable_VectorDrawable(JNIEnv*) WEAK;
int register_android_graphics_pdf_PdfDocument(JNIEnv*) WEAK;
int register_android_graphics_pdf_PdfEditor(JNIEnv*) WEAK;
int register_android_graphics_pdf_PdfRenderer(JNIEnv*) WEAK;
int register_android_view_DisplayEventReceiver(JNIEnv*) WEAK;
int register_android_view_DisplayListCanvas(JNIEnv*) WEAK;
int register_android_view_HardwareLayer(JNIEnv*) WEAK;
int register_android_view_RenderNode(JNIEnv*) WEAK;
int register_android_view_RenderNodeAnimator(JNIEnv*) WEAK;
int register_android_view_Surface(JNIEnv*) WEAK;
int register_android_view_SurfaceControl(JNIEnv*) WEAK;
int register_android_view_SurfaceSession(JNIEnv*) WEAK;
int register_android_view_TextureView(JNIEnv*) WEAK;
int register_android_view_ThreadedRenderer(JNIEnv*) WEAK;
int register_com_android_internal_view_animation_NativeInterpolatorFactoryHelper(JNIEnv*) WEAK;
int register_android_database_CursorWindow(JNIEnv*) WEAK;
int register_android_database_SQLiteConnection(JNIEnv*) WEAK;
int register_android_database_SQLiteGlobal(JNIEnv*) WEAK;
int register_android_database_SQLiteDebug(JNIEnv*) WEAK;
int register_android_nio_utils(JNIEnv*) WEAK;
int register_android_os_Debug(JNIEnv*) WEAK;
int register_android_os_GraphicsEnvironment(JNIEnv*) WEAK;
int register_android_os_HwBinder(JNIEnv*) WEAK;
int register_android_os_HwBlob(JNIEnv*) WEAK;
int register_android_os_HwParcel(JNIEnv*) WEAK;
int register_android_os_HwRemoteBinder(JNIEnv*) WEAK;
int register_android_os_MessageQueue(JNIEnv*) WEAK;
int register_android_os_Parcel(JNIEnv*) WEAK;
int register_android_os_SELinux(JNIEnv*) WEAK;
int register_android_os_VintfObject(JNIEnv*) WEAK;
int register_android_os_VintfRuntimeInfo(JNIEnv*) WEAK;
int register_android_os_seccomp(JNIEnv*) WEAK;
int register_android_os_SystemProperties(JNIEnv*) WEAK;
int register_android_os_SystemClock(JNIEnv*) WEAK;
int register_android_os_Trace(JNIEnv*) WEAK;
int register_android_os_FileObserver(JNIEnv*) WEAK;
int register_android_os_UEventObserver(JNIEnv*) WEAK;
int register_android_os_MemoryFile(JNIEnv*) WEAK;
int register_android_os_SharedMemory(JNIEnv*) WEAK;
int register_android_net_LocalSocketImpl(JNIEnv*) WEAK;
int register_android_net_NetworkUtils(JNIEnv*) WEAK;
int register_android_net_TrafficStats(JNIEnv*) WEAK;
int register_android_text_AndroidCharacter(JNIEnv*) WEAK;
int register_android_text_StaticLayout(JNIEnv*) WEAK;
int register_android_text_AndroidBidi(JNIEnv*) WEAK;
int register_android_opengl_classes(JNIEnv*) WEAK;
int register_android_ddm_DdmHandleNativeHeap(JNIEnv*) WEAK;
int register_android_server_NetworkManagementSocketTagger(JNIEnv*) WEAK;
int register_android_backup_BackupDataInput(JNIEnv*) WEAK;
int register_android_backup_BackupDataOutput(JNIEnv*) WEAK;
int register_android_backup_FileBackupHelperBase(JNIEnv*) WEAK;
int register_android_backup_BackupHelperDispatcher(JNIEnv*) WEAK;
int register_android_app_backup_FullBackup(JNIEnv*) WEAK;
int register_android_app_Activity(JNIEnv*) WEAK;
int register_android_app_ActivityThread(JNIEnv*) WEAK;
int register_android_app_ApplicationLoaders(JNIEnv*) WEAK;
int register_android_app_NativeActivity(JNIEnv*) WEAK;
int register_android_media_RemoteDisplay(JNIEnv*) WEAK;
int register_android_util_jar_StrictJarFile(JNIEnv*) WEAK;
int register_android_view_InputChannel(JNIEnv*) WEAK;
int register_android_view_InputDevice(JNIEnv*) WEAK;
int register_android_view_InputEventReceiver(JNIEnv*) WEAK;
int register_android_view_InputEventSender(JNIEnv*) WEAK;
int register_android_view_InputQueue(JNIEnv*) WEAK;
int register_android_view_KeyCharacterMap(JNIEnv*) WEAK;
int register_android_view_KeyEvent(JNIEnv*) WEAK;
int register_android_view_MotionEvent(JNIEnv*) WEAK;
int register_android_view_PointerIcon(JNIEnv*) WEAK;
int register_android_view_VelocityTracker(JNIEnv*) WEAK;
int register_android_content_res_ObbScanner(JNIEnv*) WEAK;
int register_android_content_res_Configuration(JNIEnv*) WEAK;
int register_android_animation_PropertyValuesHolder(JNIEnv*) WEAK;
int register_com_android_internal_content_NativeLibraryHelper(JNIEnv*) WEAK;
int register_com_android_internal_net_NetworkStatsFactory(JNIEnv*) WEAK;
int register_com_android_internal_os_ClassLoaderFactory(JNIEnv*) WEAK;
int register_com_android_internal_os_FuseAppLoop(JNIEnv*) WEAK;
int register_com_android_internal_os_Zygote(JNIEnv*) WEAK;
int register_com_android_internal_os_ZygoteInit(JNIEnv*) WEAK;
int register_com_android_internal_util_VirtualRefBasePtr(JNIEnv*) WEAK;

// arc-wasm: servicemanager and the binder thread pool (shim/binder/
// binder_runtime.cc); what topics add beyond gRegJNI (each defines its own).
int register_arc_wasm_binder(JNIEnv*) WEAK;
// Light processes (shim/multiproc/lproc_jni.cc, topic multiproc).
int register_arc_wasm_multiproc(JNIEnv*) WEAK;
int register_fw8_graphics(JNIEnv*) WEAK;
int register_fw8_crypto(JNIEnv*) WEAK;
int register_fw8_system(JNIEnv*) WEAK;

namespace {

struct RegJNIRec {
  int (*proc)(JNIEnv*);
  const char* name;
};
#define REG_JNI(name) {name, #name}

const RegJNIRec kRegJNI[] = {
    REG_JNI(register_com_android_internal_os_RuntimeInit),
    REG_JNI(register_com_android_internal_os_ZygoteInit_nativeZygoteInit),
    REG_JNI(register_android_os_SystemClock),
    REG_JNI(register_android_util_EventLog),
    REG_JNI(register_android_util_Log),
    REG_JNI(register_android_util_MemoryIntArray),
    REG_JNI(register_android_util_PathParser),
    REG_JNI(register_android_app_admin_SecurityLog),
    REG_JNI(register_android_content_AssetManager),
    REG_JNI(register_android_content_StringBlock),
    REG_JNI(register_android_content_XmlBlock),
    REG_JNI(register_android_text_AndroidCharacter),
    REG_JNI(register_android_text_StaticLayout),
    REG_JNI(register_android_text_AndroidBidi),
    REG_JNI(register_android_view_InputDevice),
    REG_JNI(register_android_view_KeyCharacterMap),
    REG_JNI(register_android_os_Process),
    REG_JNI(register_android_os_SystemProperties),
    REG_JNI(register_android_os_Binder),
    REG_JNI(register_android_os_Parcel),
    REG_JNI(register_android_os_HwBinder),
    REG_JNI(register_android_os_HwBlob),
    REG_JNI(register_android_os_HwParcel),
    REG_JNI(register_android_os_HwRemoteBinder),
    REG_JNI(register_android_os_VintfObject),
    REG_JNI(register_android_os_VintfRuntimeInfo),
    REG_JNI(register_android_nio_utils),
    REG_JNI(register_android_graphics_Canvas),
    REG_JNI(register_android_graphics_Graphics),
    REG_JNI(register_android_view_DisplayEventReceiver),
    REG_JNI(register_android_view_RenderNode),
    REG_JNI(register_android_view_RenderNodeAnimator),
    REG_JNI(register_android_view_DisplayListCanvas),
    REG_JNI(register_android_view_HardwareLayer),
    REG_JNI(register_android_view_ThreadedRenderer),
    REG_JNI(register_android_view_Surface),
    REG_JNI(register_android_view_SurfaceControl),
    REG_JNI(register_android_view_SurfaceSession),
    REG_JNI(register_android_view_TextureView),
    REG_JNI(register_com_android_internal_view_animation_NativeInterpolatorFactoryHelper),
    REG_JNI(register_com_google_android_gles_jni_EGLImpl),
    REG_JNI(register_com_google_android_gles_jni_GLImpl),
    REG_JNI(register_android_opengl_jni_EGL14),
    REG_JNI(register_android_opengl_jni_EGLExt),
    REG_JNI(register_android_opengl_jni_GLES10),
    REG_JNI(register_android_opengl_jni_GLES10Ext),
    REG_JNI(register_android_opengl_jni_GLES11),
    REG_JNI(register_android_opengl_jni_GLES11Ext),
    REG_JNI(register_android_opengl_jni_GLES20),
    REG_JNI(register_android_opengl_jni_GLES30),
    REG_JNI(register_android_opengl_jni_GLES31),
    REG_JNI(register_android_opengl_jni_GLES31Ext),
    REG_JNI(register_android_opengl_jni_GLES32),
    REG_JNI(register_android_graphics_Bitmap),
    REG_JNI(register_android_graphics_BitmapFactory),
    REG_JNI(register_android_graphics_BitmapRegionDecoder),
    REG_JNI(register_android_graphics_Camera),
    REG_JNI(register_android_graphics_CreateJavaOutputStreamAdaptor),
    REG_JNI(register_android_graphics_CanvasProperty),
    REG_JNI(register_android_graphics_ColorFilter),
    REG_JNI(register_android_graphics_DrawFilter),
    REG_JNI(register_android_graphics_FontFamily),
    REG_JNI(register_android_graphics_GraphicBuffer),
    REG_JNI(register_android_graphics_Interpolator),
    REG_JNI(register_android_graphics_MaskFilter),
    REG_JNI(register_android_graphics_Matrix),
    REG_JNI(register_android_graphics_Movie),
    REG_JNI(register_android_graphics_NinePatch),
    REG_JNI(register_android_graphics_Paint),
    REG_JNI(register_android_graphics_Path),
    REG_JNI(register_android_graphics_PathMeasure),
    REG_JNI(register_android_graphics_PathEffect),
    REG_JNI(register_android_graphics_Picture),
    REG_JNI(register_android_graphics_Region),
    REG_JNI(register_android_graphics_Shader),
    REG_JNI(register_android_graphics_SurfaceTexture),
    REG_JNI(register_android_graphics_Typeface),
    REG_JNI(register_android_graphics_YuvImage),
    REG_JNI(register_android_graphics_drawable_AnimatedVectorDrawable),
    REG_JNI(register_android_graphics_drawable_VectorDrawable),
    REG_JNI(register_android_graphics_pdf_PdfDocument),
    REG_JNI(register_android_graphics_pdf_PdfEditor),
    REG_JNI(register_android_graphics_pdf_PdfRenderer),
    REG_JNI(register_android_database_CursorWindow),
    REG_JNI(register_android_database_SQLiteConnection),
    REG_JNI(register_android_database_SQLiteGlobal),
    REG_JNI(register_android_database_SQLiteDebug),
    REG_JNI(register_android_os_Debug),
    REG_JNI(register_android_os_FileObserver),
    REG_JNI(register_android_os_GraphicsEnvironment),
    REG_JNI(register_android_os_MessageQueue),
    REG_JNI(register_android_os_SELinux),
    REG_JNI(register_android_os_seccomp),
    REG_JNI(register_android_os_Trace),
    REG_JNI(register_android_os_UEventObserver),
    REG_JNI(register_android_net_LocalSocketImpl),
    REG_JNI(register_android_net_NetworkUtils),
    REG_JNI(register_android_net_TrafficStats),
    REG_JNI(register_android_os_MemoryFile),
    REG_JNI(register_android_os_SharedMemory),
    REG_JNI(register_com_android_internal_os_ClassLoaderFactory),
    REG_JNI(register_com_android_internal_os_Zygote),
    REG_JNI(register_com_android_internal_os_ZygoteInit),
    REG_JNI(register_com_android_internal_util_VirtualRefBasePtr),
    REG_JNI(register_android_hardware_Camera),
    REG_JNI(register_android_hardware_camera2_CameraMetadata),
    REG_JNI(register_android_hardware_camera2_legacy_LegacyCameraDevice),
    REG_JNI(register_android_hardware_camera2_legacy_PerfMeasurement),
    REG_JNI(register_android_hardware_camera2_DngCreator),
    REG_JNI(register_android_hardware_HardwareBuffer),
    REG_JNI(register_android_hardware_SensorManager),
    REG_JNI(register_android_hardware_SerialPort),
    REG_JNI(register_android_hardware_SoundTrigger),
    REG_JNI(register_android_hardware_UsbDevice),
    REG_JNI(register_android_hardware_UsbDeviceConnection),
    REG_JNI(register_android_hardware_UsbRequest),
    REG_JNI(register_android_hardware_location_ActivityRecognitionHardware),
    REG_JNI(register_android_media_AudioRecord),
    REG_JNI(register_android_media_AudioSystem),
    REG_JNI(register_android_media_AudioTrack),
    REG_JNI(register_android_media_JetPlayer),
    REG_JNI(register_android_media_RemoteDisplay),
    REG_JNI(register_android_media_ToneGenerator),
    REG_JNI(register_android_opengl_classes),
    REG_JNI(register_android_server_NetworkManagementSocketTagger),
    REG_JNI(register_android_ddm_DdmHandleNativeHeap),
    REG_JNI(register_android_backup_BackupDataInput),
    REG_JNI(register_android_backup_BackupDataOutput),
    REG_JNI(register_android_backup_FileBackupHelperBase),
    REG_JNI(register_android_backup_BackupHelperDispatcher),
    REG_JNI(register_android_app_backup_FullBackup),
    REG_JNI(register_android_app_Activity),
    REG_JNI(register_android_app_ActivityThread),
    REG_JNI(register_android_app_ApplicationLoaders),
    REG_JNI(register_android_app_NativeActivity),
    REG_JNI(register_android_util_jar_StrictJarFile),
    REG_JNI(register_android_view_InputChannel),
    REG_JNI(register_android_view_InputEventReceiver),
    REG_JNI(register_android_view_InputEventSender),
    REG_JNI(register_android_view_InputQueue),
    REG_JNI(register_android_view_KeyEvent),
    REG_JNI(register_android_view_MotionEvent),
    REG_JNI(register_android_view_PointerIcon),
    REG_JNI(register_android_view_VelocityTracker),
    REG_JNI(register_android_content_res_ObbScanner),
    REG_JNI(register_android_content_res_Configuration),
    REG_JNI(register_android_animation_PropertyValuesHolder),
    REG_JNI(register_com_android_internal_content_NativeLibraryHelper),
    REG_JNI(register_com_android_internal_net_NetworkStatsFactory),
    REG_JNI(register_com_android_internal_os_FuseAppLoop),
    // arc-wasm
    REG_JNI(register_arc_wasm_binder),
    REG_JNI(register_arc_wasm_multiproc),
    REG_JNI(register_fw8_graphics),
    REG_JNI(register_fw8_crypto),
    REG_JNI(register_fw8_system),
};

}  // namespace

// Registers every linked-in native; called by arc_wasm_fw8_start
// (android_runtime8.cc). Returns -1 if a registration failed.
int arc_wasm_fw8_register_natives(JNIEnv* env) {
  const char* verbose = getenv("ARC_FW8_JNI_VERBOSE");
  const bool list_missing = verbose && verbose[0] == '1';
  int failed = 0, registered = 0, missing = 0;
  for (const RegJNIRec& rec : kRegJNI) {
    if (rec.proc == nullptr) {
      ++missing;
      if (list_missing)
        fprintf(stderr, "artvm8: %s not linked in\n", rec.name);
      continue;
    }
    // Every registration makes local references; one frame each.
    env->PushLocalFrame(200);
    if (rec.proc(env) < 0) {
      fprintf(stderr, "artvm8: %s failed\n", rec.name);
      if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
      }
      ++failed;
    } else {
      ++registered;
    }
    env->PopLocalFrame(nullptr);
  }
  if (list_missing)
    fprintf(stderr, "artvm8: %d JNI registrations, %d not linked in, %d failed\n",
            registered, missing, failed);
  return failed ? -1 : 0;
}

// For tests and topics: how many registrations are linked in (strong
// definitions), and the name of each entry.
int arc_wasm_fw8_jni_count(int* linked) {
  int n = 0;
  for (const RegJNIRec& rec : kRegJNI)
    n += rec.proc != nullptr;
  if (linked) *linked = n;
  return static_cast<int>(sizeof(kRegJNI) / sizeof(kRegJNI[0]));
}

const char* arc_wasm_fw8_jni_name(int i, bool* linked) {
  if (i < 0 || i >= static_cast<int>(sizeof(kRegJNI) / sizeof(kRegJNI[0]))) return nullptr;
  if (linked) *linked = kRegJNI[i].proc != nullptr;
  return kRegJNI[i].name;
}

}  // namespace android
