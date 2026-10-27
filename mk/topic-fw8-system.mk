# Topic "fw8-system": apps on the Android 8.1 framework on ART 8.1
# (docs/topics/fw8-system.md), phase 2 of docs/android8-plan.md, as topic
# fw6-system did for 6.0.1 (mk/topic-fw6-system.mk):
#
#   services.jar   framework/java-arcwasm8 (arcwasm.server: the lean AMS, PMS,
#                  WMS, display, power, ... on the 8.1 interfaces, plus what
#                  API 26 apps want: notification channels, JobScheduler 26,
#                  shortcut, storagestats, autofill, trust, uimode, appwidget,
#                  account, wifi/phone/sensors/camera as "no such device"),
#                  bases generated from the 8.1 binder interfaces
#                  (tools/build_services_jar.sh services8); on artvm8's
#                  BOOTCLASSPATH, last (art8/artvm8.cc)
#
#   make fw8-system-jars    # services.jar and the test APK
#   make fw8-system-test    # the topic's tests on ART 8.1 in Node
#
# Included by the Makefile after mk/topic-fw8-core.mk and
# mk/topic-fw8-graphics.mk (alphabetical order): F8_* (fw8-core) and A8*,
# ART8_* (art8) are defined. Everything goes to build/fw8s.

F8S    := $(BUILD)/fw8s
F8SOBJ := $(F8S)/obj
F8SLIB := $(F8S)/lib
F8SFW  := $(F8S)/framework
f8sobj  = $(patsubst %,$(F8SOBJ)/%.o,$(1))

# --- services.jar ------------------------------------------------------------------
F8S_SERVICES_JAR := $(F8SFW)/services.jar
F8S_SERVICES_CLASSES := $(F8SFW)/services-java/services-classes.jar
F8S_JAVA := $(shell find framework/java-arcwasm8 -name '*.java') \
            framework/java-arcwasm/arcwasm/server/Unimplemented.java \
            framework/java-arcwasm/arcwasm/server/Trace.java \
            framework/java-arcwasm/arcwasm/audio/AudioStats.java \
            framework/java-arcwasm/arcwasm/input/BrowserIme.java \
            framework/java-arcwasm/arcwasm/input/ImeHost.java
$(F8S_SERVICES_JAR): tools/build_services_jar.sh tools/gen_binder_base.py tools/gen_webview_stub.py \
                     $(F8S_JAVA) $(F8FW)/framework.jar $(F8FW)/fw8-libs.stamp $(F8_CORE_CLASSES)
	@mkdir -p $(F8SFW)
	tools/build_services_jar.sh $(F8SFW) services8 $(F8_CLASSES)
$(F8S_SERVICES_CLASSES): $(F8S_SERVICES_JAR) ;
# The page's files (topic app-files).
$(F8S_SERVICES_JAR): $(addprefix framework/java-arcwasm/arcwasm/server/,PageFiles.java FilesProvider.java)

.PHONY: fw8-system-jars fw8-system-test
fw8-system-jars: $(F8S_SERVICES_JAR)

ifneq ($(A8LIB),)
ASYSTEM8_ADD += --add $(F8S_SERVICES_JAR):system/framework/services.jar
$(A8SYSTEM_IMAGE): $(F8S_SERVICES_JAR)
ART8_TESTS_CLASSPATH += $(F8S_SERVICES_CLASSES)
$(A8FW)/arc-tests.jar: $(F8S_SERVICES_CLASSES)
# The 8.1 arcwasm.input/arcwasm.graphics classes of topic fw8-graphics are
# in services.jar (on the boot class path) now: not again in arc-tests.jar,
# as for 6.0.1 (mk/topic-fw6-system.mk). Tests compile against it.
ART8_JAVA_TESTS := $(filter-out $(G8_JAVA8),$(ART8_JAVA_TESTS))
endif

# --- natives: objects in build/fw8s/obj, with fw8-core's flags ---------------------------
# (FW8_INCLUDES, F8_CXXFLAGS, F8_JNI_FLAGS of mk/topic-fw8-core.mk; F8SXFLAGS per file.)
F8S_FLAGS := $(FW8_INCLUDES) -DLOG_NDEBUG=1
$(F8SOBJ)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(EMCC) $(F8_CFLAGS) $(F8S_FLAGS) $(F8SXFLAGS) -c $< -o $@
$(F8SOBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(F8_CXXFLAGS) $(F8S_FLAGS) $(F8SXFLAGS) -c $< -o $@
$(F8SOBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(F8_CXXFLAGS) $(F8S_FLAGS) $(F8SXFLAGS) -c $< -o $@
-include $(shell find $(F8SOBJ) -name '*.d' 2>/dev/null)

# --- libandroid_runtime_system8.a: the framework's JNI that is this topic's -------------
# shim/fw8-system/system8_jni.cc: a device without sensors, cameras, netd,
# SELinux, uevents or Vulkan (SystemSensorManager, Camera, NetworkUtils,
# SELinux, Seccomp, UEventObserver, ApplicationLoaders, Activity,
# NetworkManagementSocketTagger); AOSP's ClassLoaderFactory (libnativeloader
# of topic art8: no linker namespaces off-device, as on a host); SQLite 8.1
# (external/sqlite, the device flags of dist/Android.bp; WAL's -shm index is a
# MAP_SHARED mmap shared by one process's connections) and the JNI of
# android.database (CursorWindow from libandroidfw8).
F8S_SQLITE := $(A81)/external/sqlite
F8S_SQLITE_FLAGS := -DNDEBUG=1 -DHAVE_USLEEP=1 -DSQLITE_HAVE_ISNAN \
    -DSQLITE_DEFAULT_JOURNAL_SIZE_LIMIT=1048576 -DSQLITE_THREADSAFE=2 -DSQLITE_TEMP_STORE=3 \
    -DSQLITE_POWERSAFE_OVERWRITE=1 -DSQLITE_DEFAULT_FILE_FORMAT=4 -DSQLITE_DEFAULT_AUTOVACUUM=1 \
    -DSQLITE_ENABLE_MEMORY_MANAGEMENT=1 -DSQLITE_ENABLE_FTS3 -DSQLITE_ENABLE_FTS3_BACKWARDS \
    -DSQLITE_ENABLE_FTS4 -DSQLITE_OMIT_BUILTIN_TEST -DSQLITE_OMIT_COMPILEOPTION_DIAGS \
    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_FILE_PERMISSIONS=0600 -DSQLITE_SECURE_DELETE \
    -DSQLITE_ENABLE_BATCH_ATOMIC_WRITE -DHAVE_POSIX_FALLOCATE=1 -DSQLITE_ENABLE_ICU \
    -include shim/app6/sqlite_config.h -Wno-everything
F8S_SQLITE_SRCS := $(F8S_SQLITE)/dist/sqlite3.c \
    $(addprefix $(F8S_SQLITE)/android/,sqlite3_android.cpp PhoneNumberUtils.cpp OldPhoneNumberUtils.cpp)
$(call f8sobj,$(F8S_SQLITE_SRCS)): mk/topic-fw8-system.mk shim/app6/sqlite_config.h
$(call f8sobj,$(F8S_SQLITE_SRCS)): F8SXFLAGS := $(F8S_SQLITE_FLAGS) -idirafter $(F8S_SQLITE)/dist \
    -I$(F8S_SQLITE)/android -I$(A81)/external/icu/icu4c/source/common \
    -I$(A81)/external/icu/icu4c/source/i18n -DU_USING_ICU_NAMESPACE=0 -DLOG_TAG=\"sqlite3_android\"
F8S_JNI_SRCS := $(addprefix $(F8_JNI)/,android_database_SQLiteConnection.cpp \
    android_database_SQLiteGlobal.cpp android_database_SQLiteDebug.cpp \
    android_database_SQLiteCommon.cpp android_database_CursorWindow.cpp \
    com_android_internal_os_ClassLoaderFactory.cpp) \
    shim/fw8-system/system8_jni.cc shim/fw8-system/register_fw8_system.cc
$(call f8sobj,$(F8S_JNI_SRCS)): F8SXFLAGS := $(F8_JNI_FLAGS) -idirafter $(F8S_SQLITE)/dist \
    -I$(F8S_SQLITE)/android -I$(A81)/system/core/libnativeloader/include
F8S_JNI_LIB := $(F8SLIB)/libandroid_runtime_system8.a
$(F8S_JNI_LIB): $(call f8sobj,$(F8S_SQLITE_SRCS) $(F8S_JNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

ifneq ($(A8LIB),)
ART8_LIBS += -Wl,--whole-archive $(F8S_JNI_LIB) -Wl,--no-whole-archive
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js: $(F8S_JNI_LIB)
endif

# --- the NDK bridge on ART 8.1 ------------------------------------------------------------
# ARM libraries of APKs (System.loadLibrary from /data/app-lib/PACKAGE) on
# the ARM processor of topic arm-cpu (build/libndk_cpu.a) through the bridge
# of topics ndk-bridge and ndk-imports (shim/ndk), as for 6.0.1
# (mk/topic-fw6-system.mk): the same sources, built here against the 8.1
# headers into build/fw8s, with ART's side (shim/fw6-system/ndk_art.cc:
# dlopen, guest JNI_OnLoad, arc_ndk_art_call_native; ART 8.1 keeps jmethodID
# = ArtMethod* and the calling convention of shim/art8/jni_call_wasm8.cc)
# compiled against ART 8.1. The dependencies of an app library are found
# next to it (the linker's AddSearchDirOf), so ART 8.1's library_path,
# which art8.patch leaves unused, is not needed. Host libraries other
# topics make real (libsync, libnativewindow, libmediandk) register
# themselves and replace the bridge's stand-ins (shim/ndk/ndk_host_libs.h).
F8S_NDK_SRCS := $(addprefix shim/ndk/,ndk_core.cc ndk_linker.cc ndk_libc.cc ndk_stdio.cc \
                  ndk_pthread.cc ndk_green.cc ndk_libm.cc ndk_misc.cc ndk_gles.cc ndk_jni.cc \
                  ndk_bionic.cc ndk_fd.cc ndk_locale.cc ndk_syscall.cc ndk_zlib.cc ndk_egl.cc \
                  ndk_glsl.cc ndk_android.cc ndk_opensles.cc ndk_libc8.cc ndk_android8.cc)
$(call f8sobj,$(F8S_NDK_SRCS)): F8SXFLAGS := -std=gnu++17 -Ishim/ndk -Ishim/gles \
    -Wno-c++17-extensions -UARC_WASM_FW6
F8S_NDK_ART_OBJ := $(call a8obj,shim/fw6-system/ndk_art.cc)
$(F8S_NDK_ART_OBJ): A8FLAGS := $(ART8_FLAGS) -Ishim/ndk -std=gnu++17
# (The framework half, ndk_android_fw.cc, with the window target of guest
# ANativeWindow_lock and EGL surfaces, libsync and AHardwareBuffer come from
# topic fw8-graphics: $(G8_NDK_LIBS), linked with $(G8_NDK_LDFLAGS).)
F8S_NDK_OBJS := $(call f8sobj,$(F8S_NDK_SRCS)) $(F8S_NDK_ART_OBJ)
$(F8SLIB)/libndk_bridge_art8.a: $(F8S_NDK_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
F8S_NDK_LIBS := $(F8SLIB)/libndk_bridge_art8.a $(BUILD)/libndk_cpu.a $(G8_NDK_LIBS)

ifneq ($(A8LIB),)
ART8_LIBS += $(F8S_NDK_LIBS)
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js: $(F8S_NDK_LIBS)
# The bridge is reached through weak references (static_jni_libs_art8.cc,
# jni_call_wasm8.cc, ndk_android.cc): -u pulls its members in. Guest GLES 3.0
# goes to WebGL2 (ndk_egl.cc).
ART8_LDFLAGS += -Wl,-u,ndk_dalvik_dlopen -Wl,-u,arc_ndk_art_call_native \
                $(G8_NDK_LDFLAGS) -Wl,-u,arcwasm_layer_target_for_window -sALLOW_TABLE_GROWTH \
                -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
endif

# --- tests on ART 8.1 -----------------------------------------------------------------------
# NdkTest and Ndk6Test (topics ndk-bridge, ndk-imports; unchanged sources):
# their test APKs with ARM libraries in artvm8's /system/app (the rules are
# in the later mk/topic-ndk-*.mk).
ifneq ($(A8LIB),)
F8S_NDK_APKS := $(BUILD)/dalvik/apk-ndk/ArcWasmNdkTest.apk $(BUILD)/dalvik/apk-ndk/ArcWasmNdkTestV5.apk \
                $(BUILD)/dalvik/apk-ndk6/ArcWasmNdk6Test.apk
ASYSTEM8_ADD += $(foreach a,$(F8S_NDK_APKS),--add $(a):system/app/$(notdir $(a)))
$(A8SYSTEM_IMAGE): $(F8S_NDK_APKS)
# WindowTest (4.4, unchanged) on the 8.1 window manager; Ndk6Test draws into
# windows (WindowEnv; ViewTest is topic fw8-graphics'). Sources an earlier
# topic added already are not added twice (javac would see two classes).
F8S_NDK_TESTS := NdkTest Ndk6Test WindowTest GlesTest
F8S_TEST_SRCS := $(addprefix tests/java/arcwasm/,$(addsuffix .java,$(F8S_NDK_TESTS) \
                   WindowEnv WindowDemo AudioChecks))
ART8_JAVA_TESTS += $(filter-out $(ART8_JAVA_TESTS),$(F8S_TEST_SRCS))
$(A8FW)/arc-tests.jar: $(F8S_TEST_SRCS)
endif

# --- Ndk8Test: the NDK of API 24-27 (tests/ndk8) ---------------------------------------
# libndk8test.so is built like topic ndk-imports' libndk6test.so (NDK r9's
# android-18 sysroot, what it lacks declared in the source), and links
# link-time stand-ins of libsync, libnativewindow and libmediandk
# (tests/ndk8/stubs.c with their sonames) so that it names them in
# DT_NEEDED, as Firefox's libxul does.
# (NDK18_SYSROOT, NDK6_CC: mk/topic-ndk-imports.mk, included later; hence $$.)
F8S_NDK8 := $(F8S)/ndk8
F8S_NDK8_STUBS := $(addprefix $(F8S_NDK8)/stubs/,libsync.so libnativewindow.so libmediandk.so)
$(F8S_NDK8)/stubs.c.o: tests/ndk8/stubs.c | $$(NDK18_SYSROOT)/include/jni.h
	@mkdir -p $(dir $@)
	$(NDK6_CC) -c $< -o $@
$(F8S_NDK8)/stubs/%.so: $(F8S_NDK8)/stubs.c.o
	@mkdir -p $(dir $@)
	$(NDKT_LD) -soname $*.so -o $@ $<
F8S_NDK8_LIB := $(F8S_NDK8)/libs/armeabi-v7a/libndk8test.so
$(F8S_NDK8)/ndk8test.c.o: tests/ndk8/ndk8test.c | $$(NDK18_SYSROOT)/include/jni.h
	@mkdir -p $(dir $@)
	$(NDK6_CC) -c $< -o $@
$(F8S_NDK8_LIB): $(F8S_NDK8)/ndk8test.c.o $(BUILD)/ndk/armeabi-v7a/libndkrt.a $(F8S_NDK8_STUBS)
	@mkdir -p $(dir $@)
	$(NDKT_LD) -soname libndk8test.so -o $@ $(NDK18_SYSROOT)/lib/crtbegin_so.o \
	    $(F8S_NDK8)/ndk8test.c.o $(BUILD)/ndk/armeabi-v7a/libndkrt.a $(F8S_NDK8_STUBS) \
	    -L$(NDK18_SYSROOT)/lib -llog -lc -lm -ldl $(NDK18_SYSROOT)/lib/crtend_so.o
F8S_NDK8_APK := $(F8S_NDK8)/ArcWasmNdk8Test.apk
$(F8S_NDK8_APK): $(shell find tests/ndk8/apk -type f) $(F8S_NDK8_LIB) tools/build_test_apk.sh
	MIN_SDK=26 TARGET_SDK=27 AAPT="$(AAPT)" D8="$(D8)" ANDROID_JAR="$(ANDROID_JAR)" \
	    EXTRA_ASSETS="$(F8S_NDK8_LIB):lib/armeabi-v7a/libndk8test.so" \
	    tools/build_test_apk.sh tests/ndk8/apk $(F8S_NDK8)/work $@
ifneq ($(A8LIB),)
ASYSTEM8_ADD += --add $(F8S_NDK8_APK):system/app/ArcWasmNdk8Test.apk
$(A8SYSTEM_IMAGE): $(F8S_NDK8_APK)
endif

# --- the test APK with minSdkVersion 26 (tests/apk8) ---------------------------------
# Java 8 (lambdas need java.lang.invoke's LambdaMetafactory, which the SDK's
# android.jar does not have: topic art8's libcore first, BOOTCP); d8
# --min-api 26 makes dex 038.
F8S_TEST_APK := $(F8S)/apk8/ArcWasmTestApp8.apk
$(F8S_TEST_APK): $(shell find tests/apk8 -type f) tools/build_test_apk.sh $(F8_CORE_CLASSES)
	BOOTCP="$(abspath $(F8_CORE_CLASSES))" MIN_SDK=26 TARGET_SDK=27 AAPT="$(AAPT)" D8="$(D8)" \
	    ANDROID_JAR="$(ANDROID_JAR)" tools/build_test_apk.sh tests/apk8 $(F8S)/apk8/work $@
fw8-system-jars: $(F8S_TEST_APK)
ifneq ($(A8LIB),)
ASYSTEM8_ADD += --add $(F8S_TEST_APK):system/app/ArcWasmTestApp8.apk
$(A8SYSTEM_IMAGE): $(F8S_TEST_APK)
endif

# Extra APKs for experiments (not committed): build/extra-apks8/NAME.apk goes
# into artvm8's /system/app only (apps for Android 8: Firefox, ...).
EXTRA_APKS8 := $(wildcard $(BUILD)/extra-apks8/*.apk)
ifneq ($(A8LIB),)
ASYSTEM8_ADD += $(foreach a,$(EXTRA_APKS8),--add $(a):system/app/$(notdir $(a)))
$(A8SYSTEM_IMAGE): $(EXTRA_APKS8)
endif

# The topic's tests on ART 8.1 in Node (tests/java8: ActivityTest8, Ndk8Test,
# AppSupportTest8 are in art8's arc-tests.jar by its wildcard).
F8S_TESTS := ActivityTest8 AppSupportTest8 WindowTest NdkTest Ndk6Test Ndk8Test
fw8-system-test: art8
	VM=art8 tools/run_java_tests.sh node $(F8S_TESTS)

# --- libarcwasm_system8.so: the window manager's and compositor's natives -----------------
# shim/fw8-system/arcwasm_system8_jni.cc: the input windows of
# WindowManagerService to topic input's dispatcher (shim/input, built by
# topic fw8-graphics into libarcwasm_graphics8.a) and arcwasm.server.Compositor
# over the in-process SurfaceFlinger 8.1 (topic fw8-graphics). It adds itself
# to artvm8's table of static JNI libraries; System.load wants a file in
# /system/lib (an empty one).
ifneq ($(A8LIB),)
F8S_SYSJNI_SRCS := shim/fw8-system/arcwasm_system8_jni.cc
$(call f8sobj,$(F8S_SYSJNI_SRCS)): F8SXFLAGS := $(G8_FW_INCLUDES_BASE) -DARC_WASM_FW6=1 \
    -DARC_WASM_FW8=1 -I$(F8_JNI) -I$(F8_JNI)/include -Ishim/input -Ishim/fw8-graphics \
    -DLOG_TAG=\"SystemServer\"
$(F8SLIB)/libarcwasm_system8.a: $(call f8sobj,$(F8S_SYSJNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART8_LIBS += -Wl,--whole-archive $(F8SLIB)/libarcwasm_system8.a -Wl,--no-whole-archive
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js: $(F8SLIB)/libarcwasm_system8.a
$(F8S)/empty.so:
	@mkdir -p $(dir $@)
	@: > $@
ASYSTEM8_ADD += --add $(F8S)/empty.so:system/lib/libarcwasm_system8.so
$(A8SYSTEM_IMAGE): $(F8S)/empty.so
endif

# --- the page: an APK on ART 8.1, on the page's canvas ---------------------------------
#   make fw8-system-web   # build/fw8-system-web/app.html: ?pkg=, ?selftest=1
# The VM is topic fw8-graphics' page build (build/fw8-graphics-web/artvm8.js,
# the display on the canvas through gralloc, DOM input), loaded from there.
ifneq ($(A8LIB),)
F8SWEB := $(BUILD)/fw8-system-web
$(F8SWEB)/app.html: demo/app8/index.html
	@mkdir -p $(dir $@)
	cp $< $@
$(F8SWEB)/report.js: shim/report.js
	@mkdir -p $(dir $@)
	cp $< $@
.PHONY: fw8-system-web
fw8-system-web: fw8-graphics-web $(F8SWEB)/app.html $(F8SWEB)/report.js
endif

# --- OpenGL ES for Java apps: GLSurfaceView, EGL10/EGL14, GLES10/11/20 --------------
# The GL JNI of AOSP 8.1 (unchanged) over graphics_translation's EGL and
# GLES 1.1/2.0 on WebGL2, as topic fw6-system did for 6.0.1: each GL thread
# has its own WebGL2 context (OffscreenCanvas); an EGL window surface on an
# android.view.Surface (libgui 8.1) is a layer surface whose frames go into
# the Surface's buffers (shim/fw8-system/surface_window8.cc, fw8-graphics'
# layer target, shim/gles/egl_layer_surface.cc). hwui's own GL and EGL are
# renamed (topic fw8-graphics), so these are the plain gl*/egl* of
# graphics_translation. OpenGL ES 3 contexts are made by the bridge's EGL,
# direct on WebGL2, and GLES30 is built (topic failing-apps,
# mk/topic-failing-apps.mk). In Node eglInitialize fails cleanly.
ifneq ($(A8LIB),)
F8S_GL_JNI := com_google_android_gles_jni_EGLImpl com_google_android_gles_jni_GLImpl \
              android_opengl_EGL14 android_opengl_EGLExt android_opengl_GLES10 \
              android_opengl_GLES10Ext android_opengl_GLES11 android_opengl_GLES11Ext \
              android_opengl_GLES20 android_opengl_GLES30
F8S_GL_JNI_SRCS := $(addprefix $(F8_JNI)/,$(addsuffix .cpp,$(F8S_GL_JNI))) \
                   $(F8_JNI)/android/opengl/util.cpp $(F8_JNI)/android/opengl/poly_clip.cpp
$(call f8sobj,$(F8S_GL_JNI_SRCS)): F8SXFLAGS := $(G8_FW_INCLUDES) \
    -I$(A81)/frameworks/native/opengl/libs/EGL/include \
    -I$(F8_JNI) -I$(F8_JNI)/include -I$(F8_JNI)/android/graphics -I$(G8_HWUI) \
    -I$(G8_SKIA)/include/private -DGL_GLEXT_PROTOTYPES -DEGL_EGLEXT_PROTOTYPES -DLOG_TAG=\"GLJNI\" \
    $(JAVA_GL_FLAGS)
$(call f8sobj,$(F8S_GL_JNI_SRCS)): shim/ndk/ndk_java_egl.h $(JAVA_GL_NAMES)
$(call f8sobj,$(JAVA_GL_SRCS)): F8SXFLAGS := -std=gnu++17 -Ishim/ndk -Ishim/gles
$(call f8sobj,shim/fw8-system/surface_window8.cc): F8SXFLAGS := $(G8_FW_INCLUDES_BASE) -Ishim/gles -Ishim/fw8-graphics -DEGL_EGLEXT_PROTOTYPES
# libgui's headers include the HIDL headers fw8-graphics generates: those
# first (in a fresh tree nothing else orders them before these objects).
$(call f8sobj,$(F8S_GL_JNI_SRCS) shim/fw8-system/surface_window8.cc): | \
    $(G8_HIDL_GEN)/hidl.stamp $(F8_HIDL_GENCPP)/hidl.stamp
F8S_GL_GT_OBJ := $(BUILD)/gobj/shim/gles/egl_layer_surface.cc.o
$(F8SLIB)/libandroid_runtime_gl8.a: $(call f8sobj,$(F8S_GL_JNI_SRCS) shim/fw8-system/surface_window8.cc) \
                                    $(call f8sobj,$(JAVA_GL_SRCS)) $(F8S_GL_GT_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART8_LIBS += -Wl,--whole-archive $(F8SLIB)/libandroid_runtime_gl8.a -Wl,--no-whole-archive \
             $(BUILD)/libgraphics_translation.a
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js: $(F8SLIB)/libandroid_runtime_gl8.a $(BUILD)/libgraphics_translation.a
endif
