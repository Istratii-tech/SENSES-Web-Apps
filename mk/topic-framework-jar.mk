# Topic "framework-jar" (docs/topics/framework-jar.md). The jars themselves
# (framework.jar, framework2.jar and the libraries) are built in
# mk/framework.mk; here: the JNI of frameworks/base/core/jni that builds
# without libgui, hwui, media, camera, sqlite, selinux or the input stack
# (that is topic "input"), registered by shim/framework/register_framework_jar.cc,
# and arcwasm.ViewTest. Also not here: the JNI of services.jar classes
# (android_server_*: com.android.server is not on the boot class path).
# Included from the Makefile after mk/dalvik.mk and mk/framework.mk (and
# before the other topics: recursive '=' for what uses their variables).

FWJ_JNI := android_util_Process android_text_AndroidBidi android_text_AndroidCharacter \
  android_os_Debug android_util_FileObserver android_net_LocalSocketImpl \
  android_net_TrafficStats android_os_MemoryFile android_ddm_DdmHandleNativeHeap \
  android_debug_JNITest android_backup_BackupDataInput android_backup_BackupDataOutput \
  android_backup_BackupHelperDispatcher android_backup_FileBackupHelperBase \
  android_app_backup_FullBackup android_content_res_ObbScanner \
  android_content_res_Configuration android_animation_PropertyValuesHolder \
  com_android_internal_content_NativeLibraryHelper com_android_internal_net_NetworkStatsFactory
# With the android.graphics headers (topic graphics): GLES20Canvas without
# USE_OPENGL_RENDERER is just nIsAvailable() = false (no hwui), which
# HardwareRenderer.isAvailable() asks; ActivityThread's dumpGraphicsInfo is
# in the same file. EmojiFactory (android.text.Layout's <clinit> asks it)
# dlopens libemoji.so, which is not there: no emoji factory, as on devices
# without it. Not built: GLES20DisplayList (hwui headers even without
# USE_OPENGL_RENDERER).
FWJ_GJNI := android_view_GLES20Canvas android_emoji_EmojiFactory
FWJ_JNI_SRCS := $(addprefix $(FWB)/core/jni/,$(addsuffix .cpp,$(FWJ_JNI) $(FWJ_GJNI))) \
                $(addprefix $(FWB)/libs/androidfw/,BackupData.cpp BackupHelpers.cpp) \
                $(addprefix $(A44)/system/core/libcutils/,socket_local_client.c socket_local_server.c process_name.c \
                  debugger.c) \
                $(A44)/system/core/libmemtrack/memtrack.c $(A44)/hardware/libhardware/hardware.c \
                $(addprefix shim/framework/,register_framework_jar.cc thread_priority.cc debug_support.cc \
                  load_properties.cc)
$(call dobj,$(addprefix $(FWB)/core/jni/,$(addsuffix .cpp,$(FWJ_JNI)))): \
    XFLAGS = $(FW_INCLUDES) $(BINDER_INC) $(ICU_INCLUDES) -DLOG_NDEBUG=1
$(call dobj,$(addprefix $(FWB)/core/jni/,$(addsuffix .cpp,$(FWJ_GJNI)))): \
    XFLAGS = $(GJNI_XFLAGS) $(BINDER_INC) -I$(A44)/frameworks/opt/emoji
# The backup helpers are libandroidfw's device-only sources (with libbinder).
$(call dobj,$(addprefix $(FWB)/libs/androidfw/,BackupData.cpp BackupHelpers.cpp)): \
    XFLAGS = $(APK_CFLAGS) $(BINDER_INC) -DLOG_TAG=\"backup_data\"
# libmemtrack with libhardware's hw_get_module: there are no HAL modules in
# /system/lib/hw, so memtrack_init() fails and Debug reports no graphics
# memory, as on a device without a memtrack HAL.
$(DOBJ)/$(A44)/system/core/libmemtrack/%: XFLAGS = $(D_INCLUDES) -I$(A44)/hardware/libhardware/include -include string.h \
    -DLOG_TAG=\"memtrack\"
$(DOBJ)/$(A44)/hardware/libhardware/%: XFLAGS = $(D_INCLUDES) -I$(A44)/hardware/libhardware/include \
    -DLOG_TAG=\"HAL\"
$(call dobj,$(addprefix shim/framework/,register_framework_jar.cc thread_priority.cc debug_support.cc \
                  load_properties.cc)): \
    XFLAGS = $(FW_INCLUDES) -DLOG_TAG=\"AndroidRuntime\"
$(BUILD)/libandroid_runtime_fwj.a: $(call dobj,$(FWJ_JNI_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

FWJ_LIBS := $(BUILD)/libandroid_runtime_fwj.a
DALVIK_LIBS += $(FWJ_LIBS)
$(BUILD)/dalvik/dalvikvm.js $(DWEB)/dalvikvm.js: $(FWJ_LIBS)

# /system/build.prop, loaded at startup by dalvik/launcher/dalvikvm.cc.
DSYSTEM_ADD += --add framework/build.prop:system/build.prop
$(DSYSTEM_IMAGE): framework/build.prop

.PHONY: framework-jar-libs view-test
framework-jar-libs: $(FWJ_LIBS) $(FW_JARS)
view-test: dalvik
	cd $(BUILD)/dalvik && $(NODE) dalvikvm.js -cp /system/framework/arc-tests.jar arcwasm.ViewTest
