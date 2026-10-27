# Topic "fw6-system": apps on the Android 6.0.1 framework on ART
# (docs/topics/fw6-system.md): the in-process system_server for API 23,
# ActivityThread launching APKs from /system/app, their windows on the page
# and input, and a test APK with minSdkVersion 23.
#
#   services.jar          framework/java-arcwasm6 (arcwasm.server: lean AMS, PMS,
#                         WMS, display, power, ... and the 6.0 arcwasm.input /
#                         arcwasm.graphics of topic fw6-graphics), with bases
#                         generated from the 6.0.1 binder interfaces
#                         (tools/build_services_jar.sh services6); on ART's
#                         BOOTCLASSPATH, last (art/artvm.cc)
#   libarcwasm_system6.so natives of the window manager (input windows) and the
#                         compositor: shim/fw6-system, linked into artvm
#   ArcWasmTestApp6.apk   tests/apk6: minSdk/targetSdk 23, Theme.Material,
#                         runtime permissions, a service, a result
#   app.html              fw6-graphics-web/app.html: an APK's launcher activity
#                         on the page (?pkg=, ?selftest=1)
#
#   make fw6-system-jars    # services.jar and the test APK
#   make fw6-system-test    # ActivityTest and WindowTest on ART in Node
#   make fw6-system-web     # fw6-graphics-web with app.html
#
# Included by the Makefile after mk/topic-fw6-graphics.mk (alphabetical
# order), so FW6_* (fw6-core) and FG* (fw6-graphics) are defined.

FS     := $(BUILD)/fw6s
FSOBJ  := $(FS)/obj
FSLIB  := $(FS)/lib
fsobj   = $(patsubst %,$(FSOBJ)/%.o,$(1))

# --- services.jar --------------------------------------------------------------------
FS_SERVICES_JAR := $(F6FW)/services.jar
FS_SERVICES_CLASSES := $(F6FW)/services-java/services-classes.jar
FS_JAVA := $(shell find framework/java-arcwasm6 -name '*.java') \
           framework/java-arcwasm/arcwasm/server/Unimplemented.java \
           framework/java-arcwasm/arcwasm/server/Trace.java \
           framework/java-arcwasm/arcwasm/audio/AudioStats.java \
           framework/java-arcwasm/arcwasm/server/SystemKeys.java \
           framework/java-arcwasm/arcwasm/server/HangWatchdog.java \
           framework/java-arcwasm/arcwasm/input/BrowserIme.java \
           framework/java-arcwasm/arcwasm/input/ImeHost.java
$(FS_SERVICES_JAR): tools/build_services_jar.sh tools/gen_binder_base.py tools/gen_webview_stub.py $(FS_JAVA) \
                    $(F6FW)/framework.jar $(F6FW)/fw6-libs.stamp
	tools/build_services_jar.sh $(F6FW) services6 $(F6_CLASSES)
$(FS_SERVICES_CLASSES): $(FS_SERVICES_JAR) ;
ASYSTEM_ADD += --add $(FS_SERVICES_JAR):system/framework/services.jar
$(ASYSTEM_IMAGE): $(FS_SERVICES_JAR)
# The page's files (topic app-files).
$(FS_SERVICES_JAR): $(addprefix framework/java-arcwasm/arcwasm/server/,PageFiles.java FilesProvider.java)

# The 6.0 arcwasm.input/arcwasm.graphics classes are in services.jar (on the
# boot class path) now: not again in arc-tests.jar. Tests compile against it.
ART_JAVA_TESTS := $(filter-out $(FG_JAVA6),$(ART_JAVA_TESTS))
ART_TESTS_CLASSPATH += $(FS_SERVICES_CLASSES)
$(AFW)/arc-tests.jar: $(FS_SERVICES_CLASSES)

# --- libarcwasm_system6.so ---------------------------------------------------------------
$(FSOBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(FG_CXXFLAGS) $(FSFLAGS) -c $< -o $@
$(FSOBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(FG_CXXFLAGS) $(FSFLAGS) -c $< -o $@
-include $(shell find $(FSOBJ) -name '*.d' 2>/dev/null)
# (layer_target6.cc: the frame destination of the NDK's ANativeWindow and EGL
# window surfaces, for the bridge below.)
FS_JNI_SRCS := shim/fw6-system/arcwasm_system6_jni.cc shim/fw6-system/layer_target6.cc
$(call fsobj,$(FS_JNI_SRCS)): FSFLAGS := $(FG_FW_INCLUDES_BASE) -Ishim/input -Ishim/fw6-graphics \
    -Ishim/gles -I$(F6FWB)/core/jni -DLOG_TAG=\"SystemServer\"
$(FSLIB)/libarcwasm_system6.a: $(call fsobj,$(FS_JNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
FW6_JNI_LIBS += $(FSLIB)/libarcwasm_system6.a
$(ABLD)/artvm.js $(AWEB)/artvm.js $(FGWEB)/artvm.js: $(FSLIB)/libarcwasm_system6.a

# --- the test APK with minSdkVersion 23 (tests/apk6) ---------------------------------
FS_TEST_APK := $(FS)/apk6/ArcWasmTestApp6.apk
FS_TEST_APK_FILES := $(shell find tests/apk6 -type f)
$(FS_TEST_APK): $(FS_TEST_APK_FILES) tools/build_test_apk.sh
	MIN_SDK=23 TARGET_SDK=23 AAPT="$(AAPT)" D8="$(D8)" ANDROID_JAR="$(ANDROID_JAR)" \
	    tools/build_test_apk.sh tests/apk6 $(FS)/apk6 $@
ASYSTEM_ADD += --add $(FS_TEST_APK):system/app/ArcWasmTestApp6.apk
$(ASYSTEM_IMAGE): $(FS_TEST_APK)

# Extra APKs for experiments (not committed), as mk/topic-app.mk does for
# Dalvik: every build/extra-apks/NAME.apk also goes into ART's /system/app.
ASYSTEM_ADD += $(foreach a,$(EXTRA_APKS),--add $(a):system/app/$(notdir $(a)))
$(ASYSTEM_IMAGE): $(EXTRA_APKS)

# --- tests on ART ------------------------------------------------------------------------
# tests/java6/arcwasm/ActivityTest.java: the ART version of ActivityTest (the
# minSdk 23 app through the 6.0 server); WindowTest (and WindowDemo) of the
# 4.4 build, unchanged, on the 6.0 window manager.
FS_TESTS6 := ActivityTest WindowTest
FS_TEST_SRCS := tests/java6/arcwasm/ActivityTest.java \
                $(addprefix tests/java/arcwasm/,WindowTest.java WindowEnv.java WindowDemo.java)
ART_JAVA_TESTS += $(FS_TEST_SRCS)
$(AFW)/arc-tests.jar: $(FS_TEST_SRCS)

.PHONY: fw6-system-jars fw6-system-test fw6-system-web
fw6-system-jars: $(FS_SERVICES_JAR) $(FS_TEST_APK)
fw6-system-test: art
	VM=art tools/run_java_tests.sh node $(FS_TESTS6)

# --- the page: an APK on ART, on the page's canvas ----------------------------------------
$(FGWEB)/app.html: demo/app6/index.html
	@mkdir -p $(dir $@)
	cp $< $@
fw6-system-web: fw6-graphics-web $(FGWEB)/app.html

# --- /data in Node --------------------------------------------------------------------
# Node has no OPFS: the in-memory one of topic activity (shim/opfs/memory_opfs.js),
# so that /data (the app's data directory, its code cache) is mounted under
# Node as in the browser.
$(ABLD)/artvm.js: ART_LDFLAGS += --pre-js shim/opfs/memory_opfs.js
$(ABLD)/artvm.js: shim/opfs/memory_opfs.js mk/topic-fw6-system.mk

# --- the NDK bridge on ART -----------------------------------------------------------
# ARM libraries of APKs (System.loadLibrary from /data/app-lib/PACKAGE) run on
# the processor of topic arm-cpu (build/libndk_cpu.a) through the bridge of
# topics ndk-bridge and ndk-imports (shim/ndk), as under Dalvik: the same
# sources, built here against ART's headers into build/fw6s, with ART's side
# (shim/fw6-system/ndk_art.cc: dlopen, JNI_OnLoad, arc_ndk_art_call_native)
# instead of Dalvik's ndk_dalvik.cc, and the framework half of libandroid
# (assets, ALooper, ANativeWindow) against the 6.0 framework. (The lists are
# spelled out: mk/topic-ndk-*.mk are included after this file.) OpenSL ES
# players play through the audio half (ndk_audio_fw.cc) on topic gles-audio's
# lean AudioTrack and in-process mixer (shim/audio: its own libmedia headers),
# built here against system/core 6.0; the page's sink is its AudioWorklet
# (shim/audio/web_audio.js).
FS_NDK_SRCS := $(addprefix shim/ndk/,ndk_core.cc ndk_linker.cc ndk_libc.cc ndk_stdio.cc \
                 ndk_pthread.cc ndk_green.cc ndk_libm.cc ndk_misc.cc ndk_gles.cc ndk_jni.cc \
                 ndk_bionic.cc ndk_fd.cc ndk_locale.cc ndk_syscall.cc ndk_zlib.cc ndk_egl.cc \
                 ndk_glsl.cc ndk_android.cc ndk_opensles.cc ndk_libc8.cc ndk_android8.cc)
$(call fsobj,$(FS_NDK_SRCS)): FSFLAGS := -std=gnu++17 $(A_INCLUDES) -Ishim/ndk -Ishim/gles \
    -Wno-c++17-extensions
$(call fsobj,shim/ndk/ndk_android_fw.cc): FSFLAGS := $(FG_FW_INCLUDES_BASE) -std=gnu++17 \
    -I$(F6FWB)/core/jni -DLOG_NDEBUG=1 -Ishim/ndk \
    -include android/looper.h
$(AOBJ)/shim/fw6-system/ndk_art.cc.o: AFLAGS := $(ART_FLAGS) -Ishim/ndk -std=gnu++17
FS_AUDIO_SRCS := shim/audio/audio_flinger.cc shim/audio/audio_track.cc shim/ndk/ndk_audio_fw.cc
# ARC_WASM_FW6: the lean AudioTrack with its 6.0 API (in every file that
# includes its header: the class layout depends on it).
FS_AUDIO_FLAGS := -Ishim/audio/include $(FW6_INCLUDES) -I$(A60)/system/media/audio/include \
    -I$(A60)/system/media/audio_utils/include -I$(A60)/frameworks/av/include -DARC_WASM_FW6=1
$(call fsobj,$(FS_AUDIO_SRCS)): FSFLAGS := $(FS_AUDIO_FLAGS) -DLOG_NDEBUG=1
FS_NDK_OBJS := $(call fsobj,$(FS_NDK_SRCS) shim/ndk/ndk_android_fw.cc $(FS_AUDIO_SRCS)) \
               $(call aobj,shim/fw6-system/ndk_art.cc)
$(FSLIB)/libndk_bridge_art.a: $(FS_NDK_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
FS_NDK_LIBS := $(FSLIB)/libndk_bridge_art.a $(BUILD)/libndk_cpu.a
FW6_LIBS += $(FS_NDK_LIBS)
$(ABLD)/artvm.js $(AWEB)/artvm.js $(FGWEB)/artvm.js: $(FS_NDK_LIBS)
# The bridge is reached through weak references (static_jni_libs_art.cc,
# jni_call_wasm.cc, ndk_android.cc): -u pulls its members in. Guest GLES 3.0
# goes to WebGL2 (ndk_egl.cc, as in the Dalvik build).
ART_LDFLAGS += -Wl,-u,ndk_dalvik_dlopen -Wl,-u,arc_ndk_art_call_native \
               -Wl,-u,ndk_android_fw_linked -Wl,-u,ndk_audio_fw_linked -sALLOW_TABLE_GROWTH \
               -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
# NdkTest and Ndk6Test (topics ndk-bridge, ndk-imports) on ART: their test
# APKs with ARM libraries in ART's /system/app too (paths spelled out: the
# rules are in the later mk/topic-ndk-*.mk).
FS_NDK_APKS := $(BUILD)/dalvik/apk-ndk/ArcWasmNdkTest.apk $(BUILD)/dalvik/apk-ndk/ArcWasmNdkTestV5.apk \
               $(BUILD)/dalvik/apk-ndk6/ArcWasmNdk6Test.apk
ASYSTEM_ADD += $(foreach a,$(FS_NDK_APKS),--add $(a):system/app/$(notdir $(a)))
$(ASYSTEM_IMAGE): $(FS_NDK_APKS)
# GlesTest (topic gles-audio): GLSurfaceView, EGL10/EGL14, GLES20 and
# AudioTrack of the 4.4 build's test, unchanged, on ART (below).
FS_NDK_TESTS := NdkTest Ndk6Test GlesTest
FS_NDK_TEST_SRCS := $(addprefix tests/java/arcwasm/,$(addsuffix .java,$(FS_NDK_TESTS) AudioChecks))
ART_JAVA_TESTS += $(FS_NDK_TEST_SRCS)
$(AFW)/arc-tests.jar: $(FS_NDK_TEST_SRCS)
# The audio sink on the page (as the Dalvik browser builds have it).
$(FGWEB)/artvm.js $(AWEB)/artvm.js: FGWEB_LDFLAGS += --pre-js shim/audio/web_audio.js
$(AWEB)/artvm.js: ART_LDFLAGS += --pre-js shim/audio/web_audio.js
$(FGWEB)/artvm.js $(AWEB)/artvm.js: shim/audio/web_audio.js

# --- OpenGL ES for Java apps: GLSurfaceView, EGL10/EGL14, GLES10/11/20 --------------
# The GL JNI of AOSP 6.0.1 (unchanged) over graphics_translation's EGL and
# GLES 1.1/2.0 on WebGL2, as topic gles-audio does for 4.4: each GL thread
# has its own WebGL2 context (OffscreenCanvas); an EGL window surface on an
# android.view.Surface (libgui) is a layer surface whose frames go into the
# Surface's buffers (shim/fw6-system/surface_window6.cc, layer_target6.cc,
# shim/gles/egl_layer_surface.cc). GLES30/GLES31 (ES 3.x for Java) are not
# built: graphics_translation is ES 2.0. In Node eglInitialize fails cleanly.
FS_GL_JNI := com_google_android_gles_jni_EGLImpl com_google_android_gles_jni_GLImpl \
             android_opengl_EGL14 android_opengl_EGLExt android_opengl_GLES10 \
             android_opengl_GLES10Ext android_opengl_GLES11 android_opengl_GLES11Ext \
             android_opengl_GLES20
FS_GL_JNI_SRCS := $(addprefix $(F6FWB)/core/jni/,$(addsuffix .cpp,$(FS_GL_JNI))) \
                  $(F6FWB)/core/jni/android/opengl/util.cpp $(F6FWB)/core/jni/android/opengl/poly_clip.cpp
$(call fsobj,$(FS_GL_JNI_SRCS)): FSFLAGS := -Ishim/fw6-system/include $(FG_FW_INCLUDES) -I$(F6FWB)/core/jni \
    -I$(F6FWB)/core/jni/android/graphics -I$(F6FWB)/libs/hwui -DGL_GLEXT_PROTOTYPES \
    -DEGL_EGLEXT_PROTOTYPES -DLOG_TAG=\"GLJNI\" \
    -DeglInitialize=arcwasm_eglInitialize -DeglCreateWindowSurface=arcwasm_eglCreateWindowSurface
$(call fsobj,shim/fw6-system/surface_window6.cc): FSFLAGS := -Ishim/fw6-system/include $(FG_FW_INCLUDES_BASE) -Ishim/gles \
    -Ishim/fw6-graphics -DEGL_EGLEXT_PROTOTYPES
FS_GL_GT_OBJ := $(BUILD)/gobj/shim/gles/egl_layer_surface.cc.o
$(FSLIB)/libandroid_runtime_gl6.a: $(call fsobj,$(FS_GL_JNI_SRCS) shim/fw6-system/surface_window6.cc) \
                                   $(FS_GL_GT_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
FW6_JNI_LIBS += $(FSLIB)/libandroid_runtime_gl6.a
FW6_LIBS += $(BUILD)/libgraphics_translation.a
$(ABLD)/artvm.js $(AWEB)/artvm.js $(FGWEB)/artvm.js: $(FSLIB)/libandroid_runtime_gl6.a \
    $(BUILD)/libgraphics_translation.a

# --- sound for Java apps: AudioTrack and AudioSystem ------------------------------------
# AOSP 6.0.1's AudioTrack JNI (unchanged) over topic gles-audio's lean
# AudioTrack (shim/audio, with its 6.0 API under ARC_WASM_FW6) and the
# in-process mixer, whose sink is the page's AudioWorklet (Node: a clock);
# AudioSystem's natives are this topic's (shim/fw6-system/audio_system6_jni.cc).
# libmedia_jni (MediaPlayer, MediaCodec...): topic mediacodec
# (mk/topic-mediacodec.mk), statically linked; System.loadLibrary("media_jni")
# wants a file in /system/lib.
FS_MEDIA_SRCS := $(F6FWB)/core/jni/android_media_AudioTrack.cpp \
                 $(F6FWB)/core/jni/android_media_DeviceCallback.cpp \
                 shim/fw6-system/audio_system6_jni.cc
$(call fsobj,$(FS_MEDIA_SRCS)): FSFLAGS := $(FS_AUDIO_FLAGS) -I$(F6FWB)/core/jni -I$(F6FWB)/media/jni \
    -Ishim/audio -DLOG_NDEBUG=1
$(FS)/empty.so:
	@mkdir -p $(dir $@)
	@: > $@
ASYSTEM_ADD += --add $(FS)/empty.so:system/lib/libmedia_jni.so
$(ASYSTEM_IMAGE): $(FS)/empty.so
$(FSLIB)/libandroid_runtime_media6.a: $(call fsobj,$(FS_MEDIA_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
FW6_JNI_LIBS += $(FSLIB)/libandroid_runtime_media6.a
$(ABLD)/artvm.js $(AWEB)/artvm.js $(FGWEB)/artvm.js: $(FSLIB)/libandroid_runtime_media6.a
