# Topic "fw6-core": the base of the Android 6.0.1 framework on ART
# (docs/topics/fw6-core.md). Builds into build/art (the ART build of
# mk/topic-art.mk) and links into artvm:
#
#   Java     the boot class path jars of AOSP 6.0.1 (tools/build_framework6_jars.sh),
#            framework-res.apk 6.0.1 and its R classes, /system/build.prop (SDK 23)
#   native   system/core 6.0 (libcutils, libutils with Looper, liblog pieces),
#            libbinder 6.0 and servicemanager 6.0 on the /dev/binder of
#            shim/binder, libandroidfw 6.0, and libandroid_runtime 6.0: the
#            AndroidRuntime frame (shim/fw6/android_runtime.cc) with the core
#            JNI and the gRegJNI table (shim/fw6/register_fw6.cc) other topics
#            extend (fw6-graphics: see docs/topics/fw6-core.md)
#
#   make fw6-jars                # the jars and framework-res.apk only
#   make art                     # artvm with all of it
#   make fw6-test                # LooperTest BinderTest ApkTest FrameworkResTest on ART in Node
#
# Included by the Makefile after mk/topic-art.mk (alphabetical order).

F6FW := $(ABLD)/framework
F6FWB := $(A60)/frameworks/base
SDK ?= $(HOME)/Library/Android/sdk
AAPT ?= $(lastword $(sort $(wildcard $(SDK)/build-tools/*/aapt)))
ZIPALIGN ?= $(lastword $(sort $(wildcard $(SDK)/build-tools/*/zipalign)))

# --- framework-res.apk 6.0.1 and android.R / com.android.internal.R ------------
# As mk/topic-framework-res.mk does for 4.4.4 (same flags, same reasons):
# -x is package id 0x01 (core/res/Android.mk), one aapt run makes the apk and
# the R classes, English and mdpi only by default (FRAMEWORK6_RES_LOCALES=,
# FRAMEWORK6_RES_DENSITY= keep everything).
F6RES := $(F6FWB)/core/res
F6RES_OUT := $(ABLD)/framework-res
F6RES_APK := $(F6RES_OUT)/framework-res.apk
FRAMEWORK6_RES_LOCALES ?= en
FRAMEWORK6_RES_DENSITY ?= mdpi
F6RES_FLAGS := -x --min-sdk-version 23 --target-sdk-version 23 \
    --version-code 23 --version-name 6.0.1 \
    $(if $(FRAMEWORK6_RES_LOCALES),-c $(FRAMEWORK6_RES_LOCALES)) \
    $(if $(FRAMEWORK6_RES_DENSITY),--preferred-density $(FRAMEWORK6_RES_DENSITY))
F6RES_GEN := $(F6RES_OUT)/gen
# "-S DIR" resource overlays before the framework's own (topic userspace pins
# the resource ids with one); empty by default.
F6RES_OVERLAY ?=
F6RES_JAVA := $(F6RES_GEN)/android/R.java $(F6RES_GEN)/android/Manifest.java \
    $(F6RES_GEN)/com/android/internal/R.java
$(F6RES_OUT)/aapt-flags: FORCE
	@mkdir -p $(dir $@)
	@echo '$(F6RES_FLAGS)' | cmp -s - $@ || echo '$(F6RES_FLAGS)' > $@
F6RES_FILES := $(F6RES)/AndroidManifest.xml $(shell find $(F6RES)/res $(F6RES)/assets -type f)
$(F6RES_OUT)/aapt.stamp: $(F6RES_FILES) $(F6RES_OUT)/aapt-flags
	@rm -rf $(F6RES_GEN) && mkdir -p $(F6RES_GEN)
	"$(AAPT)" package -f $(F6RES_FLAGS) -M $(F6RES)/AndroidManifest.xml \
	    $(F6RES_OVERLAY) -S $(F6RES)/res $(if $(F6RES_OVERLAY),--auto-add-overlay) \
	    -A $(F6RES)/assets -F $(F6RES_APK).unaligned \
	    -m -J $(F6RES_GEN) --private-symbols com.android.internal 2> $(F6RES_OUT)/aapt.log \
	    || { tail -20 $(F6RES_OUT)/aapt.log; exit 1; }
	"$(ZIPALIGN)" -f 4 $(F6RES_APK).unaligned $(F6RES_APK)
	@rm -f $(F6RES_APK).unaligned
	@echo "built $(F6RES_APK) ($$(wc -c < $(F6RES_APK) | tr -d ' ') bytes)"
	@touch $@
$(F6RES_APK) $(F6RES_JAVA): $(F6RES_OUT)/aapt.stamp ;

# --- the boot class path jars ---------------------------------------------------
# BOOTCLASSPATH of AOSP 6.0.1 (system/core/rootdir/init.environ.rc), in order;
# the same list is the default in art/artvm.cc.
F6_BOOT_JARS := core-libart conscrypt okhttp core-junit bouncycastle ext framework \
                telephony-common voip-common ims-common apache-xml org.apache.http.legacy.boot
F6_LIB_JARS := conscrypt okhttp core-junit bouncycastle ext apache-xml
F6_OPT_JARS := voip-common ims-common telephony-common org.apache.http.legacy.boot
F6_JARS := $(addprefix $(F6FW)/,$(addsuffix .jar,$(filter-out core-libart,$(F6_BOOT_JARS))))
# javac output of the framework and what it is compiled against, for tests.
F6_CLASSES := $(foreach j,$(F6_LIB_JARS) framework,$(F6FW)/$(j)-java/$(j)-classes.jar)
F6_LIB_SRCS := $(shell find $(A60)/external/conscrypt/src/main/java $(A60)/external/conscrypt/src/platform/java \
                 $(A60)/external/okhttp $(A60)/external/junit/src $(A60)/external/bouncycastle/bcprov/src/main \
                 $(A60)/external/nist-sip/java $(A60)/external/tagsoup/src $(A60)/external/libphonenumber \
                 $(A60)/external/apache-xml/src/main/java -type f -name '*.java')
$(F6FW)/fw6-libs.stamp: tools/build_framework6_jars.sh $(F6FW)/core-libart.jar $(F6_LIB_SRCS)
	tools/build_framework6_jars.sh $(F6FW) libs
	@touch $@
$(foreach j,$(F6_LIB_JARS),$(F6FW)/$(j).jar $(F6FW)/$(j)-java/$(j)-classes.jar): $(F6FW)/fw6-libs.stamp ;

F6_SRC_DIRS := $(addprefix $(F6FWB)/,$(addsuffix /java,core graphics location media media/mca/effect \
                 media/mca/filterfw media/mca/filterpacks drm opengl sax telecomm telephony wifi keystore rs))
F6_SRCS := $(shell find $(F6_SRC_DIRS) $(F6FWB)/packages/services -name '*.java' -o -name '*.aidl' \
             -o -name '*.logtags') $(F6FWB)/Android.mk $(F6FWB)/preloaded-classes
$(F6FW)/framework.jar: tools/build_framework6_jars.sh tools/java_event_log_tags.py \
                       $(F6FW)/fw6-libs.stamp $(F6_SRCS) $(F6RES_JAVA)
	tools/build_framework6_jars.sh $(F6FW) framework $(F6RES_JAVA)
$(F6FW)/framework-java/framework-classes.jar: $(F6FW)/framework.jar ;

F6_OPT_SRCS := $(shell find $(A60)/frameworks/opt/telephony/src $(A60)/frameworks/opt/net/voip/src \
                 $(A60)/frameworks/opt/net/ims/src $(A60)/external/apache-http/src \
                 $(A60)/external/apache-http/android -type f)
$(F6FW)/fw6-opt.stamp: tools/build_framework6_jars.sh $(F6FW)/framework.jar $(F6_OPT_SRCS)
	tools/build_framework6_jars.sh $(F6FW) opt
	@touch $@
$(foreach j,$(F6_OPT_JARS),$(F6FW)/$(j).jar): $(F6FW)/fw6-opt.stamp ;

# --- /system ---------------------------------------------------------------------
ASYSTEM_ADD += $(foreach j,$(F6_JARS),--add $(j):system/framework/$(notdir $(j))) \
               --add $(F6RES_APK):system/framework/framework-res.apk \
               --add framework/build6.prop:system/build.prop
$(ASYSTEM_IMAGE): $(F6_JARS) $(F6RES_APK) framework/build6.prop

.PHONY: fw6-jars FORCE
fw6-jars: $(F6_JARS) $(F6RES_APK)

# --- native: shared flags and object rules -------------------------------------
# Objects in build/art/fw6obj, apart from topic art's build/art/obj: the same
# system/core files are compiled there with ART's flags (libartsys.a).
F6OBJ := $(ABLD)/fw6obj
f6obj = $(patsubst %,$(F6OBJ)/%.o,$(1))
# FW6_INCLUDES: what code built against the 6.0 framework's native headers
# needs (topics fw6-graphics, fw6-crypto... compile with these).
# HAVE_ANDROID_OS=1: the device variant of system/core and the framework,
# as hwui, libgui and libinput need (utils/Trace.h, Input.h's Parcel code).
# It changes class layouts (utils/Thread.h: mTid, Mutex.h: timedLock), so
# everything linked against these libutils/libbinder must be built with it.
#   shim/fw6/include   linux/binder.h 6.0 (32-bit protocol), selinux/, sys/system_properties.h
#   shim/binder/include  arc_binder.h (the in-process driver's extensions)
FW6_INCLUDES := -Ishim/fw6/include -Ishim/binder/include $(A_INCLUDES) \
    -I$(A60)/system/core/base/include -I$(A60)/frameworks/native/include \
    -I$(A60)/system/core/libprocessgroup/include \
    -I$(F6FWB)/include -I$(F6FWB)/core/jni $(ICU55_INCLUDES) $(A_UAPI) \
    -DBINDER_IPC_32BIT=1 -DU_USING_ICU_NAMESPACE=0 -include shim/fw6/fw6_compat.h \
    -DHAVE_ANDROID_OS=1
# -DLOG_NDEBUG=1 as in a release build: ALOGV and LOG_FATAL_IF compile out
# (libbinder would log every command, see docs/topics/binder.md).
F6_FLAGS := $(FW6_INCLUDES) -DLOG_NDEBUG=1
$(F6OBJ)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(EMCC) $(A_CFLAGS) $(F6_FLAGS) $(F6XFLAGS) -c $< -o $@
$(F6OBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(A_CXXFLAGS) $(F6_FLAGS) $(F6XFLAGS) -c $< -o $@
$(F6OBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(A_CXXFLAGS) $(F6_FLAGS) $(F6XFLAGS) -c $< -o $@
-include $(shell find $(F6OBJ) -name '*.d' 2>/dev/null)

# --- system/core 6.0: libcutils, liblog, libutils --------------------------------
# libartsys.a (topic art) already has libbase, libziparchive, libutils'
# FileMap and libcutils' trace-host, ashmem-dev, process_name; these are the
# rest the framework uses. Properties are bionic's API, kept in-process
# (shim/fw6/system_properties.cc); scheduling groups are stubs.
F6_CUTILS_SRCS := $(addprefix $(A60)/system/core/libcutils/,properties.c threads.c \
                    native_handle.c multiuser.c socket_local_client.c socket_local_server.c \
                    debugger.c) \
                  shim/fw6/system_properties.cc shim/fw6/cutils_fw6.cc
$(ALIB)/libcutils.a: $(call f6obj,$(F6_CUTILS_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
# liblog: text goes through shim/common/alog_shim.cc and
# shim/dalvik/android_log.cc (libposix_translation, libart_shim); here the
# real log_is_loggable.c (log.tag.* properties) and the event log.
F6_LIBLOG_SRCS := $(A60)/system/core/liblog/log_is_loggable.c shim/fw6/liblog_fw6.cc
$(ALIB)/liblog.a: $(call f6obj,$(F6_LIBLOG_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
# libutils: the host list (commonSources) with Looper (patched: a pipe
# instead of eventfd); CallStack/ProcessCallStack need libbacktrace (an
# empty CallStack: shim/fw6/callstack_stub.cc).
F6_UTILS := $(A60)/system/core/libutils
F6_UTILS_SRCS := $(filter-out %/CallStack.cpp %/ProcessCallStack.cpp %/FileMap.cpp, \
                   $(shell tools/mk_srcs.py $(F6_UTILS)/Android.mk commonSources $(F6_UTILS)/)) \
                 $(F6_UTILS)/Looper.cpp shim/fw6/thread_priority.cc shim/fw6/callstack_stub.cc
$(call f6obj,$(F6_UTILS_SRCS)): F6XFLAGS := -DLIBUTILS_NATIVE=1
# /dev/alarm's UAPI header (shim/linux/android_alarm.h), as in mk/framework.mk.
$(call f6obj,$(F6_UTILS)/SystemClock.cpp): F6XFLAGS := -DLIBUTILS_NATIVE=1 -Ishim
$(ALIB)/libutils.a: $(call f6obj,$(F6_UTILS_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libbinder 6.0, servicemanager 6.0 ----------------------------------------------
# On /dev/binder of shim/binder/dev_binder.cc (libdev_binder.a, mk/topic-binder.mk),
# mounted by art/vfs_setup.cc.
F6_BINDER := $(A60)/frameworks/native/libs/binder
F6_BINDER_SRCS := $(shell tools/mk_srcs.py $(F6_BINDER)/Android.mk sources $(F6_BINDER)/)
$(ALIB)/libbinder.a: $(call f6obj,$(F6_BINDER_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
F6_SVCMGR := $(A60)/frameworks/native/cmds/servicemanager
F6_SVCMGR_SRCS := $(F6_SVCMGR)/service_manager.c $(F6_SVCMGR)/binder.c \
                  shim/fw6/servicemanager6_main.c shim/fw6/selinux_stub.c
# (bionic's headers bring stdbool.h in.)
$(call f6obj,$(F6_SVCMGR)/service_manager.c): F6XFLAGS := -Dmain=arc_servicemanager_unused_main \
    -include stdbool.h
$(call f6obj,shim/fw6/servicemanager6_main.c): F6XFLAGS := -I$(F6_SVCMGR)
$(ALIB)/libservicemanager.a: $(call f6obj,$(F6_SVCMGR_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libandroidfw 6.0 ------------------------------------------------------------------
F6_ANDROIDFW := $(F6FWB)/libs/androidfw
# deviceSources: commonSources and the backup helpers and CursorWindow.
F6_ANDROIDFW_SRCS := $(shell tools/mk_srcs.py $(F6_ANDROIDFW)/Android.mk commonSources $(F6_ANDROIDFW)/) \
                     $(addprefix $(F6_ANDROIDFW)/,BackupData.cpp BackupHelpers.cpp CursorWindow.cpp)
$(ALIB)/libandroidfw.a: $(call f6obj,$(F6_ANDROIDFW_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libandroid_runtime 6.0: the frame and the core JNI -------------------------------
# Beyond the core: the rest of core/jni that needs neither graphics, media,
# input, sqlite nor hardware (what topic framework-jar built for 4.4;
# EmojiFactory, which android.text.Layout needs, includes Skia's headers:
# topic fw6-graphics).
F6_JNI_EXTRA_SRCS := $(addprefix $(F6FWB)/core/jni/,android_os_Debug.cpp android_util_FileObserver.cpp \
                 android_net_LocalSocketImpl.cpp android_net_TrafficStats.cpp android_os_MemoryFile.cpp \
                 android_ddm_DdmHandleNativeHeap.cpp android_backup_BackupDataInput.cpp \
                 android_backup_BackupDataOutput.cpp android_backup_BackupHelperDispatcher.cpp \
                 android_backup_FileBackupHelperBase.cpp android_app_backup_FullBackup.cpp \
                 android_content_res_ObbScanner.cpp android_animation_PropertyValuesHolder.cpp \
                 com_android_internal_content_NativeLibraryHelper.cpp \
                 com_android_internal_net_NetworkStatsFactory.cpp) \
                     shim/fw6/memtrack_stub.c shim/framework/debug_support.cc
# AndroidRuntime.cpp itself is replaced by shim/fw6/android_runtime.cc (the
# frame, RuntimeInit) and shim/fw6/register_fw6.cc (gRegJNI, weak entries).
F6_JNI := $(F6FWB)/core/jni
F6_JNI_SRCS := $(addprefix $(F6_JNI)/,android_util_Log.cpp android_os_SystemClock.cpp \
                 android_util_EventLog.cpp android_os_Trace.cpp android_os_MessageQueue.cpp \
                 android_util_Binder.cpp android_os_Parcel.cpp android_util_AssetManager.cpp \
                 android_util_StringBlock.cpp android_util_XmlBlock.cpp android_util_Process.cpp \
                 android_os_SystemProperties.cpp android_nio_utils.cpp \
                 android_content_res_Configuration.cpp android_text_AndroidCharacter.cpp \
                 android_text_AndroidBidi.cpp com_android_internal_util_VirtualRefBasePtr.cpp) \
               $(F6_JNI_EXTRA_SRCS) \
               shim/fw6/android_runtime.cc shim/fw6/register_fw6.cc shim/binder/binder_runtime.cc
$(call f6obj,shim/fw6/android_runtime.cc shim/binder/binder_runtime.cc): \
    F6XFLAGS := -DLOG_TAG=\"AndroidRuntime\"
$(ALIB)/libandroid_runtime.a: $(call f6obj,$(F6_JNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- linking into artvm ------------------------------------------------------------------
# FW6_JNI_LIBS: libraries of JNI objects (each defines register_* functions
# of gRegJNI). Linked whole: register_fw6.cc refers to register_* weakly,
# and a weak reference does not pull an archive member in. Topics append
# theirs (FW6_JNI_LIBS += build/art/lib/libandroid_runtime_graphics.a) and
# the libraries those need to FW6_LIBS.
FW6_JNI_LIBS := $(ALIB)/libandroid_runtime.a
FW6_LIBS := $(ALIB)/libandroidfw.a $(ALIB)/libbinder.a $(ALIB)/libservicemanager.a \
            $(ALIB)/libutils.a $(ALIB)/libcutils.a $(ALIB)/liblog.a $(BUILD)/libdev_binder.a
# ART_LIBS is expanded in artvm's link recipes (topic art). Redefined here as
# a recursive variable, so that what later topics append to FW6_JNI_LIBS and
# FW6_LIBS (FW6_JNI_LIBS += ...) still reaches the link; the prerequisites
# are added separately (each topic adds its own:
# $(ABLD)/artvm.js $(AWEB)/artvm.js: its libraries).
ART_LIBS_ART := $(ART_LIBS)
ART_LIBS = $(ART_LIBS_ART) -Wl,--whole-archive $(FW6_JNI_LIBS) -Wl,--no-whole-archive $(FW6_LIBS)
$(ABLD)/artvm.js $(AWEB)/artvm.js: $(FW6_JNI_LIBS) $(FW6_LIBS)

.PHONY: fw6-libs
fw6-libs: $(FW6_JNI_LIBS) $(FW6_LIBS)
$(ART_VFS_OBJ): shim/binder/dev_binder.h

# --- tests on ART (arc-tests.jar of mk/topic-art.mk) -------------------------------
# The Android 4.4 framework tests, unchanged sources, compiled against the
# 6.0.1 framework: Looper/Handler/MessageQueue, Binder/Parcel/servicemanager,
# APK code and resources, framework-res. The test APK (mk/topic-apk.mk) goes
# into the image as for Dalvik; BinderTest's natives are linked in.
FW6_TESTS := LooperTest BinderTest ApkTest FrameworkResTest
ART_JAVA_TESTS += $(addprefix tests/java/arcwasm/,$(addsuffix .java,$(FW6_TESTS)))
ART_TESTS_CLASSPATH += $(F6_CLASSES)
$(F6FW)/arc-tests.jar: $(addprefix tests/java/arcwasm/,$(addsuffix .java,$(FW6_TESTS))) $(F6_CLASSES)
ASYSTEM_ADD += --add $(TEST_APK):system/app/ArcWasmTestApp.apk \
               --add $(BUILD)/dalvik/apk/dex/classes.dex:system/app/ArcWasmTestApp.dex
$(ASYSTEM_IMAGE): $(TEST_APK)
# tests/native/binder_test_jni.cc: BinderTest's natives (System.load of
# libarcwasm_binder_test.so, shim/art/static_jni_libs_art.cc), against 6.0's
# libbinder and servicemanager's binder.c.
$(call f6obj,tests/native/binder_test_jni.cc): F6XFLAGS := -I$(F6_SVCMGR) -DLOG_TAG=\"BinderTest\"
$(ALIB)/libarcwasm_binder_test.a: $(call f6obj,tests/native/binder_test_jni.cc)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
FW6_JNI_LIBS += $(ALIB)/libarcwasm_binder_test.a
$(ABLD)/artvm.js $(AWEB)/artvm.js: $(ALIB)/libarcwasm_binder_test.a

.PHONY: fw6-test
fw6-test: art
	VM=art tools/run_java_tests.sh node VmTest $(FW6_TESTS)
