# Topic "fw10-system": apps on the Android 10 framework on ART 10
# (docs/topics/fw10-system.md), phase 2 of docs/android10-plan.md, as topic
# fw8-system did for 8.1 (mk/topic-fw8-system.mk):
#
#   services.jar   framework/java-arcwasm10 (arcwasm.server: the lean AMS and
#                  ActivityTaskManager, PMS with split APKs and shared
#                  libraries, WMS, display, power, ... on the 10 interfaces),
#                  bases generated from the 10 binder interfaces
#                  (tools/build_services_jar.sh services10); on artvm10's
#                  BOOTCLASSPATH, last (art10/artvm10.cc)
#   natives        the framework JNI that is this topic's (StatsLog, SQLite,
#                  ClassLoaderFactory, "no such device"), the NDK bridge (ARM
#                  and arm64-v8a) on ART 10
#
#   make fw10-system-jars   # services.jar and the test APKs
#   make fw10-system-test   # the topic's tests on ART 10 in Node
#
# Included by the Makefile after mk/topic-fw10-core.mk (alphabetical order):
# F10_*, FW10_* (fw10-core) and A10*, ART10_* (art10) are defined.
# Everything goes to build/fw10s. Graphics (Skia, hwui, libgui, SurfaceFlinger,
# input) is topic fw10-graphics': without it the VM is headless (see
# "Швы с fw10-graphics" in the topic note).

F10S    := $(BUILD)/fw10s
F10SOBJ := $(F10S)/obj
F10SLIB := $(F10S)/lib
F10SFW  := $(F10S)/framework
f10sobj  = $(patsubst %,$(F10SOBJ)/%.o,$(1))

# --- services.jar ------------------------------------------------------------------
F10S_SERVICES_JAR := $(F10SFW)/services.jar
F10S_SERVICES_CLASSES := $(F10SFW)/services-java/services-classes.jar
F10S_JAVA := $(shell find framework/java-arcwasm10 -name '*.java') \
             $(addprefix framework/java-arcwasm/arcwasm/server/,Unimplemented.java Trace.java \
               AppLibrary.java Senses.java) \
             framework/java-arcwasm/arcwasm/audio/AudioStats.java \
             framework/java-arcwasm/arcwasm/input/BrowserIme.java \
             framework/java-arcwasm/arcwasm/input/ImeHost.java
$(F10S_SERVICES_JAR): tools/build_services_jar.sh tools/gen_binder_base.py tools/gen_webview_stub.py \
                      $(F10S_JAVA) $(F10FW)/framework.jar $(F10FW)/fw10-libs.stamp $(A10_CORE_CLASSES)
	@mkdir -p $(F10SFW)
	CORE10_CLASSES="$$PWD/$(A10_CORE_CLASSES)" tools/build_services_jar.sh $(F10SFW) services10 $(F10_CLASSES)
$(F10S_SERVICES_CLASSES): $(F10S_SERVICES_JAR) ;
# The page's files (topic app-files).
$(F10S_SERVICES_JAR): $(addprefix framework/java-arcwasm/arcwasm/server/,PageFiles.java FilesProvider.java)

.PHONY: fw10-system-jars fw10-system-test
fw10-system-jars: $(F10S_SERVICES_JAR)

ASYSTEM10_ADD += --add $(F10S_SERVICES_JAR):system/framework/services.jar
$(A10SYSTEM_IMAGE): $(F10S_SERVICES_JAR)
ART10_TESTS_CLASSPATH += $(F10S_SERVICES_CLASSES)
$(A10FW)/arc-tests.jar: $(F10S_SERVICES_CLASSES)

# --- the memory budget on ART 10 (topic mem-budget's natives) ----------------------------
# MemoryMonitor (the system server's view of the budget) wants the natives of
# arcwasm.server.MemoryBudget: topic mem-budget's mem_budget_jni.cc adds
# itself to artvm10's table of static JNI libraries too, art_maps8.cc reads
# ART 10's MemMaps (libartbase). libmembudget.a (the budget, the sample,
# /proc/meminfo; artvm10.cc calls it weakly) is shared with 6.0.1 and 8.1.
# (mk/topic-mem-budget.mk is included after this file: its MBB spelled out.)
F10S_MBB := $(BUILD)/membudget
MB10_OBJS := $(call a10obj,shim/membudget/mem_budget_jni.cc shim/membudget/art_maps8.cc)
$(A10OBJ)/shim/membudget/mem_budget_jni.cc.o: A10FLAGS := $(A10_SYS_INCLUDES) -Ishim
$(A10OBJ)/shim/membudget/art_maps8.cc.o: A10FLAGS := $(ART10_FLAGS)
$(MB10_OBJS): shim/membudget/mem_budget.h
$(F10S_MBB)/libmembudget10.a: $(MB10_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART10_LIBS += -Wl,--whole-archive $(F10S_MBB)/libmembudget.a $(F10S_MBB)/libmembudget10.a -Wl,--no-whole-archive
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js: $(F10S_MBB)/libmembudget.a $(F10S_MBB)/libmembudget10.a
ASYSTEM10_ADD += --add $(F10S_MBB)/empty.so:system/lib/libarcwasm_membudget.so
$(A10SYSTEM_IMAGE): $(F10S_MBB)/empty.so

# --- natives: objects in build/fw10s/obj, with fw10-core's flags ------------------------
# (FW10_INCLUDES, F10_CXXFLAGS, F10_JNI_FLAGS of mk/topic-fw10-core.mk; F10SXFLAGS per file.)
F10S_FLAGS := $(FW10_INCLUDES) -DLOG_NDEBUG=1
$(F10SOBJ)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(EMCC) $(F10_CFLAGS) $(F10S_FLAGS) $(F10SXFLAGS) -MMD -MP -c $< -o $@
$(F10SOBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(F10_CXXFLAGS) $(F10S_FLAGS) $(F10SXFLAGS) -MMD -MP -c $< -o $@
$(F10SOBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(F10_CXXFLAGS) $(F10S_FLAGS) $(F10SXFLAGS) -MMD -MP -c $< -o $@
-include $(shell find $(F10SOBJ) -name '*.d' 2>/dev/null)

# --- libandroid_runtime_system10.a: the framework JNI that is this topic's -------------
# shim/fw10-system/system10_jni.cc: a device without sensors, cameras, netd,
# SELinux, uevents, VINTF manifests or GPU drivers; register_fw10_system.cc:
# headless stand-ins for ColorSpace and HardwareRenderer's statics when
# fw10-graphics is not linked. AOSP's ClassLoaderFactory (libnativeloader 10
# of topic art10: no linker namespaces off-device). (SQLite 10 and the JNI of
# android.database are topic fw10-crypto-media's.) StatsLog: AOSP 10's writeRaw and the
# JNI stats-log-api-gen writes for StatsLogInternal over libstatssocket 10
# (system/core/libstats), whose socket (/dev/socket/statsdw) is not there:
# atoms are dropped, as on a device whose statsd is down.
# libstatssocket 10 and the StatsLogInternal JNI (generated).
F10S_STATS_GEN := $(F10S)/gen/statslog
$(F10S_STATS_GEN)/statslog.stamp: $(F10H)/stats-log-api-gen
	@mkdir -p $(dir $@)
	$(F10H)/stats-log-api-gen --header $(F10S_STATS_GEN)/statslog.h --cpp $(F10S_STATS_GEN)/statslog.cpp \
	    --jni $(F10S_STATS_GEN)/android_util_StatsLogInternal.cpp --importHeader statslog.h
	@touch $@
$(F10S_STATS_GEN)/statslog.cpp $(F10S_STATS_GEN)/android_util_StatsLogInternal.cpp: $(F10S_STATS_GEN)/statslog.stamp ;
F10S_STATS_SRCS := $(addprefix $(A10)/system/core/libstats/,stats_event_list.c statsd_writer.c) \
    $(F10S_STATS_GEN)/statslog.cpp $(F10S_STATS_GEN)/android_util_StatsLogInternal.cpp \
    $(F10_JNI)/android_util_StatsLog.cpp
$(call f10sobj,$(F10S_STATS_SRCS)): $(F10S_STATS_GEN)/statslog.stamp
$(call f10sobj,$(F10S_STATS_SRCS)): F10SXFLAGS := $(F10_JNI_FLAGS) -I$(F10S_STATS_GEN) \
    -I$(A10)/system/core/libstats/include -DLIBLOG_LOG_TAG=1006 -DWRITE_TO_STATSD=1 \
    -DWRITE_TO_LOGD=0 -DLOG_TAG=\"StatsLog\"
F10S_JNI_SRCS := $(F10_JNI)/com_android_internal_os_ClassLoaderFactory.cpp \
    shim/fw10-system/system10_jni.cc shim/fw10-system/register_fw10_system.cc
$(call f10sobj,$(F10S_JNI_SRCS)): F10SXFLAGS := $(F10_JNI_FLAGS) -I$(A10)/system/core/libnativeloader/include
F10S_JNI_LIB := $(F10SLIB)/libandroid_runtime_system10.a
$(F10S_JNI_LIB): $(call f10sobj,$(F10S_STATS_SRCS) $(F10S_JNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
.PHONY: fw10-system-libs
fw10-system-libs: $(F10S_JNI_LIB)

ART10_LIBS += -Wl,--whole-archive $(F10S_JNI_LIB) -Wl,--no-whole-archive
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js: $(F10S_JNI_LIB)

# --- the test APK with minSdkVersion 29 (tests/apk10) ---------------------------------
# Java 8 (lambdas want java.lang.invoke's LambdaMetafactory: topic art10's
# libcore first, BOOTCP); d8 --min-api 29 makes dex 039.
F10S_TEST_APK := $(F10S)/apk10/ArcWasmTestApp10.apk
$(F10S_TEST_APK): $(shell find tests/apk10 -type f) tools/build_test_apk.sh $(A10_CORE_CLASSES)
	BOOTCP="$$PWD/$(A10_CORE_CLASSES)" MIN_SDK=29 TARGET_SDK=29 AAPT="$(AAPT)" D8="$(D8)" \
	    ANDROID_JAR="$(ANDROID_JAR)" tools/build_test_apk.sh tests/apk10 $(F10S)/apk10/work $@
fw10-system-jars: $(F10S_TEST_APK)
ASYSTEM10_ADD += --add $(F10S_TEST_APK):system/app/ArcWasmTestApp10.apk
$(A10SYSTEM_IMAGE): $(F10S_TEST_APK)

# --- the topic's tests on ART 10 (tests/java10) ------------------------------------------
F10S_TEST_SRCS := tests/java10/arcwasm/ActivityTest10.java
ART10_JAVA_TESTS += $(F10S_TEST_SRCS)
$(A10FW)/arc-tests.jar: $(F10S_TEST_SRCS)
F10S_TESTS := ActivityTest10
fw10-system-test: art10
	VM=art10 tools/run_java_tests.sh node $(F10S_TESTS)

# --- the NDK bridge on ART 10 (ARM and arm64-v8a) -----------------------------------------
# ARM and AArch64 libraries of APKs (System.loadLibrary from /data/app-lib/
# PACKAGE) on the processors of topics arm-cpu and ndk-arm64
# (build/libndk_cpu.a, build/libndk_cpu64.a) through the bridge of topics
# ndk-bridge, ndk-imports and ndk-arm64 (shim/ndk), as for 8.1
# (mk/topic-fw8-system.mk, mk/topic-ndk-arm64.mk): the same sources built
# here against the 10 headers into build/fw10s, with ART's side
# (shim/fw6-system/ndk_art.cc: dlopen, guest JNI_OnLoad,
# arc_ndk_art_call_native; jmethodID = ArtMethod* in 10 too) compiled by
# topic art10 against ART 10 ($(ART10_NDK_ART_OBJ)). The framework half of
# libandroid (ndk_android_fw.cc: assets from 10's AssetManager2, ALooper,
# ANativeWindow_fromSurface when fw10-graphics links Surfaces) is built here
# with ARC_FW10; libsync and AHardwareBuffer of 10 would be fw10-graphics'
# (the bridge's "no gralloc" stand-ins otherwise, ndk_android8.cc).
F10S_NDK_SRCS := $(addprefix shim/ndk/,ndk_core.cc ndk_linker.cc ndk_libc.cc ndk_stdio.cc \
                   ndk_pthread.cc ndk_green.cc ndk_libm.cc ndk_misc.cc ndk_gles.cc ndk_jni.cc \
                   ndk_bionic.cc ndk_fd.cc ndk_locale.cc ndk_syscall.cc ndk_zlib.cc ndk_egl.cc \
                   ndk_glsl.cc ndk_android.cc ndk_opensles.cc ndk_libc8.cc ndk_android8.cc \
                   ndk_linker64.cc ndk64_libc.cc ndk64_stdio.cc ndk64_jni.cc)
$(call f10sobj,$(F10S_NDK_SRCS)): F10SXFLAGS := -std=gnu++17 -Ishim/ndk -Ishim/gles \
    -Wno-c++17-extensions -UARC_WASM_FW6
F10S_NDK_FW_SRCS := shim/ndk/ndk_android_fw.cc
$(call f10sobj,$(F10S_NDK_FW_SRCS)): F10SXFLAGS := -std=gnu++17 $(F10_JNI_FLAGS) -Ishim/ndk \
    -I$(A10)/frameworks/native/libs/nativewindow/include -I$(A10)/frameworks/native/libs/arect/include \
    -I$(A10)/frameworks/native/libs/nativebase/include -DARC_FW10=1 -include android/looper.h
# The frame destination of an app's ANativeWindow (arcwasm_layer_target_for_window,
# shim/fw6-system/layer_target6.cc, as fw8-graphics builds it for 8.1): guest
# ANativeWindow_lock/unlockAndPost (ndk_android.cc), EGL window surfaces
# (ndk_egl.cc) and Vulkan swapchains draw into buffers of libgui 10's Surface
# for SurfaceFlinger 10 (the ANativeWindow API is the same in 10). The bridge
# refers to it weakly (-u below).
F10S_NDK_GFX_SRCS := shim/fw6-system/layer_target6.cc
$(call f10sobj,$(F10S_NDK_GFX_SRCS)): F10SXFLAGS := $(G10_FW_INCLUDES_BASE) -Ishim/gles
F10S_NDK_OBJS := $(call f10sobj,$(F10S_NDK_SRCS) $(F10S_NDK_FW_SRCS) $(F10S_NDK_GFX_SRCS)) \
                 $(ART10_NDK_ART_OBJ)
$(F10SLIB)/libndk_bridge_art10.a: $(F10S_NDK_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
# (topic ndk-arm64's mk is included after this one: its library spelled out.)
F10S_NDK_LIBS := $(F10SLIB)/libndk_bridge_art10.a $(BUILD)/libndk_cpu.a $(BUILD)/libndk_cpu64.a
.PHONY: fw10-system-ndk
fw10-system-ndk: $(F10S_NDK_LIBS)
ART10_LIBS += $(F10S_NDK_LIBS)
# (and fw10-graphics-web's, which SENSES Web Apps' a10 is linked from: it did
# not relink after a change in shim/ndk; topic heavy-apps-mem)
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js $(G10WEB)/artvm10.js: $(F10S_NDK_LIBS)
# The guest's libmediandk/libaaudio table of topic fw10-crypto-media (its
# host functions are in artvm10 already), when that topic is there.
ifneq ($(F10C_NDK_TABLE_LIBS),)
ART10_LIBS += -Wl,--whole-archive $(F10C_NDK_TABLE_LIBS) -Wl,--no-whole-archive
ART10_LDFLAGS += -Wl,-u,ndk_host_library_fw8cm_media
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js: $(F10C_NDK_TABLE_LIBS)
endif
# The bridge is reached through weak references (static_jni_libs_art10.cc,
# jni_call_wasm10.cc, ndk_android.cc, ndk_egl.cc): -u pulls its members in.
# Guest GLES 3.0 goes to WebGL2 (ndk_egl.cc); arm64: the AArch64 CPU and the
# LP64 thunks.
ART10_LDFLAGS += -Wl,-u,ndk_dalvik_dlopen -Wl,-u,arc_ndk_art_call_native \
                 -Wl,-u,ndk_android_fw_linked -Wl,-u,arcwasm_layer_target_for_window \
                 -sALLOW_TABLE_GROWTH \
                 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
                 -Wl,-u,ndk64_call_guest_args -Wl,-u,ndk64_host_stub -Wl,-u,ndk64_jit_exec_one \
                 -Wl,-u,ndk64_cpu_report_trap -Wl,-u,ndk64_linker_linked -Wl,-u,ndk64_libc_linked \
                 -Wl,-u,ndk64_stdio_linked -Wl,-u,ndk64_jni_linked

# NdkTest, Ndk6Test (topics ndk-bridge, ndk-imports) and NdkArm64Test (topic
# ndk-arm64), unchanged sources, on ART 10: their test APKs with ARM and arm64
# libraries in artvm10's /system/app (the rules are in the later
# mk/topic-ndk-*.mk). Ndk6Test draws into windows (ANativeWindow_lock, EGL:
# the layer target above; WindowEnv comes with mk/topic-webview10.mk).
F10S_NDK_APKS := $(BUILD)/dalvik/apk-ndk/ArcWasmNdkTest.apk $(BUILD)/dalvik/apk-ndk/ArcWasmNdkTestV5.apk \
                 $(BUILD)/dalvik/apk-ndk6/ArcWasmNdk6Test.apk $(BUILD)/ndk64/ArcWasmNdkTestArm64.apk
ASYSTEM10_ADD += $(foreach a,$(F10S_NDK_APKS),--add $(a):system/app/$(notdir $(a)))
$(A10SYSTEM_IMAGE): $(F10S_NDK_APKS)
F10S_NDK_TEST_SRCS := $(addprefix tests/java/arcwasm/,NdkTest.java Ndk6Test.java NdkArm64Test.java)
ART10_JAVA_TESTS += $(F10S_NDK_TEST_SRCS)
$(A10FW)/arc-tests.jar: $(F10S_NDK_TEST_SRCS)
F10S_TESTS += NdkTest Ndk6Test NdkArm64Test
# GlesTest (topic gles-audio; OpenGL ES 3.0 from Java, topic failing-apps): its GL
# checks run in browsers (run_java_tests.sh chrome GlesTest), in Node it says
# EGL is unavailable and checks audio.
F10S_GLES_TEST_SRCS := $(addprefix tests/java/arcwasm/,GlesTest.java WindowTest.java \
                         WindowDemo.java AudioChecks.java)
ART10_JAVA_TESTS += $(filter-out $(ART10_JAVA_TESTS),$(F10S_GLES_TEST_SRCS))
$(A10FW)/arc-tests.jar: $(F10S_GLES_TEST_SRCS)

# --- SENSES Web Apps on ART 10: InstallTest, SplitInstallTest ------------------------------
# Their apps (topics install8 and apks-split; the rules are in the later
# mk/topic-install8.mk and mk/topic-senses-apks-split.mk, paths spelled out)
# in artvm10's image where the tests, playing the shell, copy them from.
F10S_INSTALL_APKS := $(BUILD)/install8/v1.apk $(BUILD)/install8/v2.apk
F10S_SPLIT_PARTS := base.apk split_config.armeabi_v7a.apk split_config.mdpi.apk \
                    split_config.ru.apk split_config.xhdpi.apk split_feature.apk
ASYSTEM10_ADD += $(foreach v,1 2,--add $(BUILD)/install8/v$(v).apk:system/etc/arcwasm/install-test/v$(v).apk) \
                 $(foreach v,1 2,$(foreach p,$(F10S_SPLIT_PARTS),\
                   --add $(BUILD)/apks-split/v$(v)/$(p):system/etc/arcwasm/split-test/v$(v)/$(p)))
$(A10SYSTEM_IMAGE): $(F10S_INSTALL_APKS) $(BUILD)/apks-split/v1/.built $(BUILD)/apks-split/v2/.built
F10S_INSTALL_TEST_SRCS := tests/java/arcwasm/InstallTest.java tests/java/arcwasm/SplitInstallTest.java
ART10_JAVA_TESTS += $(F10S_INSTALL_TEST_SRCS)
$(A10FW)/arc-tests.jar: $(F10S_INSTALL_TEST_SRCS)
F10S_TESTS += InstallTest SplitInstallTest
# AppSupportTest10 (tests/java10): AppSupportTest8 on 10.
ART10_JAVA_TESTS += tests/java10/arcwasm/AppSupportTest10.java
$(A10FW)/arc-tests.jar: tests/java10/arcwasm/AppSupportTest10.java
F10S_TESTS += AppSupportTest10
# AppFilesTest10 (tests/java10): files between an app and the page (topic app-files).
ART10_JAVA_TESTS += tests/java10/arcwasm/AppFilesTest10.java
$(A10FW)/arc-tests.jar: tests/java10/arcwasm/AppFilesTest10.java
F10S_TESTS += AppFilesTest10

# --- libarcwasm_system10.so: the window manager's and compositor's natives -----------------
# shim/fw10-system/arcwasm_system10_jni.cc: arcwasm.server.Compositor over
# fw10-graphics' in-process SurfaceFlinger 10 and the input windows of
# WindowManagerService to the browser input dispatcher (when linked). It
# adds itself to artvm10's table of static JNI libraries; System.load wants a
# file in /system/lib (an empty one). Only with fw10-graphics (G10_FW_INCLUDES):
# without it WindowManagerService.hasGraphics() is false.
ifneq ($(G10_FW_INCLUDES),)
F10S_SYSJNI_SRCS := shim/fw10-system/arcwasm_system10_jni.cc
$(call f10sobj,$(F10S_SYSJNI_SRCS)): F10SXFLAGS := $(G10_FW_INCLUDES) -I$(F10_JNI) -I$(F10_JNI)/include \
    -Ishim/input -Ishim/fw10-graphics -DLOG_TAG=\"SystemServer\"
$(call f10sobj,$(F10S_SYSJNI_SRCS)): | $(G10)/gen/hidl-cpp/hidl.stamp $(F10_HIDL_GENCPP)/hidl.stamp
$(F10SLIB)/libarcwasm_system10.a: $(call f10sobj,$(F10S_SYSJNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART10_LIBS += -Wl,--whole-archive $(F10SLIB)/libarcwasm_system10.a -Wl,--no-whole-archive
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js: $(F10SLIB)/libarcwasm_system10.a
$(F10S)/empty.so:
	@mkdir -p $(dir $@)
	@: > $@
ASYSTEM10_ADD += --add $(F10S)/empty.so:system/lib/libarcwasm_system10.so
$(A10SYSTEM_IMAGE): $(F10S)/empty.so
endif

# --- OpenGL ES for Java apps: GLSurfaceView, EGL10/EGL14, GLES10/11/20 --------------
# (topic a10-default) The GL JNI of AOSP 10 (unchanged) over graphics_translation's
# EGL and GLES 1.1/2.0 on WebGL2, as fw8-system does for 8.1: an EGL window
# surface on an android.view.Surface (libgui 10) is a layer surface whose
# frames go into the Surface's buffers (shim/fw10-system/surface_window10.cc,
# the layer target above, shim/gles/egl_layer_surface.cc). hwui's own GL and
# EGL are renamed (fw10-graphics), so these are the plain gl*/egl* of
# graphics_translation. Without them android.opengl/EGLImpl had no natives:
# GLSurfaceView's thread died on EGLImpl._nativeClassInit (MapLibre's map,
# Godot). OpenGL ES 3 contexts are made by the bridge's EGL, direct on WebGL2,
# and GLES30 is built (topic failing-apps, mk/topic-failing-apps.mk);
# GLES31/32 and EGL15 are not (WebGL2 is ES 3.0). In Node eglInitialize fails
# cleanly.
F10S_GL_JNI := com_google_android_gles_jni_EGLImpl com_google_android_gles_jni_GLImpl \
               android_opengl_EGL14 android_opengl_EGLExt android_opengl_GLES10 \
               android_opengl_GLES10Ext android_opengl_GLES11 android_opengl_GLES11Ext \
               android_opengl_GLES20 android_opengl_GLES30
F10S_GL_JNI_SRCS := $(addprefix $(F10_JNI)/,$(addsuffix .cpp,$(F10S_GL_JNI))) \
                    $(F10_JNI)/android/opengl/util.cpp $(F10_JNI)/android/opengl/poly_clip.cpp
$(call f10sobj,$(F10S_GL_JNI_SRCS)): F10SXFLAGS := $(G10_GJNI_BASE) -DLOG_TAG=\"GLJNI\" \
    $(JAVA_GL_FLAGS)
$(call f10sobj,$(F10S_GL_JNI_SRCS)): shim/ndk/ndk_java_egl.h $(JAVA_GL_NAMES)
$(call f10sobj,$(JAVA_GL_SRCS)): F10SXFLAGS := -std=gnu++17 -Ishim/ndk -Ishim/gles
$(call f10sobj,shim/fw10-system/surface_window10.cc): F10SXFLAGS := $(G10_FW_INCLUDES_BASE) \
    -Ishim/gles -Ishim/fw10-graphics
$(call f10sobj,$(F10S_GL_JNI_SRCS) shim/fw10-system/surface_window10.cc): \
    $(G10_PROTO_SRCS) $(G10_HIDL_GEN)/hidl.stamp
F10S_GL_GT_OBJ := $(BUILD)/gobj/shim/gles/egl_layer_surface.cc.o
$(F10SLIB)/libandroid_runtime_gl10.a: $(call f10sobj,$(F10S_GL_JNI_SRCS) shim/fw10-system/surface_window10.cc) \
                                      $(call f10sobj,$(JAVA_GL_SRCS)) $(F10S_GL_GT_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART10_LIBS += -Wl,--whole-archive $(F10SLIB)/libandroid_runtime_gl10.a -Wl,--no-whole-archive \
              $(BUILD)/libgraphics_translation.a
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js $(G10WEB)/artvm10.js: $(F10SLIB)/libandroid_runtime_gl10.a $(BUILD)/libgraphics_translation.a
