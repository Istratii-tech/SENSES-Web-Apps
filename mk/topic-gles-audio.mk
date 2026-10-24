# Topic "gles-audio": OpenGL ES and sound for Java apps (GLSurfaceView,
# EGL10/EGL14, GLES10/11/20, AudioTrack). See docs/topics/gles-audio.md.
#
#   JNI (AOSP 4.4, unchanged but for patches/aosp44/gles-audio.patch: no
#   SurfaceTexture)   com_google_android_gles_jni_{EGLImpl,GLImpl},
#                     android_opengl_{EGL14,EGLExt,GLES10,GLES10Ext,GLES11,
#                     GLES11Ext,GLES20}, android/opengl/util (GLUtils, Matrix,
#                     Visibility, ETC1), android_media_AudioTrack
#   shim/gles         EGL window surfaces on android.view.Surface (the window
#                     topic's SurfaceFlinger layers), registration
#   shim/audio        libmedia's AudioTrack/AudioSystem in lean form, the
#                     in-process mixer ("AudioFlinger"), its sinks (Web Audio
#                     in browsers, a clock in Node)
#   graphics_translation (EGL, GLES 1.1/2.0 on WebGL2) is now linked into
#   every dalvikvm, not only graphics-web: GL works on any thread of a
#   browser build (each GL thread has its own WebGL2 context); in Node,
#   eglInitialize fails cleanly.
#
# Included by the Makefile after mk/dalvik.mk and the topics before it in
# name order: GJNI_XFLAGS, WINDOW_XFLAGS (topics graphics, window) are used
# through recursive variables.

GLES_JNI := com_google_android_gles_jni_EGLImpl com_google_android_gles_jni_GLImpl \
            android_opengl_EGL14 android_opengl_EGLExt android_opengl_GLES10 \
            android_opengl_GLES10Ext android_opengl_GLES11 android_opengl_GLES11Ext \
            android_opengl_GLES20
GLES_JNI_SRCS := $(addprefix $(FWB)/core/jni/,$(addsuffix .cpp,$(GLES_JNI))) \
                 $(FWB)/core/jni/android/opengl/util.cpp $(FWB)/core/jni/android/opengl/poly_clip.cpp
# AOSP's GL headers (with GL_GLEXT_PROTOTYPES, as libandroid_runtime's
# Android.mk), the EGL platform types of graphics_translation, and the two
# EGL calls the shim takes over (shim/gles/surface_window.cc).
GLES_INCLUDES = -Ishim/gles/include -I$(A44)/frameworks/native/opengl/include \
    -I$(A44)/hardware/libhardware/include
GLES_JNI_XFLAGS = $(GLES_INCLUDES) $(GJNI_XFLAGS) $(BINDER_INC) -DGL_GLEXT_PROTOTYPES \
    -DEGL_EGLEXT_PROTOTYPES -DARC_WASM_NO_LIBGUI -DLOG_TAG=\"GLJNI\" \
    -DeglInitialize=arcwasm_eglInitialize -DeglCreateWindowSurface=arcwasm_eglCreateWindowSurface
$(call dobj,$(GLES_JNI_SRCS)): XFLAGS = $(GLES_JNI_XFLAGS)
$(call dobj,shim/gles/surface_window.cc): XFLAGS = $(GLES_INCLUDES) $(WINDOW_XFLAGS) \
    -DEGL_EGLEXT_PROTOTYPES
$(call dobj,shim/gles/register_gles.cc): XFLAGS = $(FW_INCLUDES)
# The graphics_translation half: its headers and flags (Makefile's GCXXFLAGS).
GLES_GT_OBJ := $(call gobj,shim/gles/egl_layer_surface.cc)
$(GLES_GT_OBJ): $(BUILD)/gobj/%.o: % shim/gles/gl_layer_target.h \
                shim/graphics_translation/egl/native_webgl.h
	@mkdir -p $(dir $@)
	$(EMXX) $(GCXXFLAGS) -c $< -o $@
$(BUILD)/libgles_jni.a: $(call dobj,$(GLES_JNI_SRCS) shim/gles/surface_window.cc \
                          shim/gles/register_gles.cc) $(GLES_GT_OBJ)
	@rm -f $@ && $(EMAR) rcs $@ $^

GLES_LIBS := $(BUILD)/libgles_jni.a $(BUILD)/libgraphics_translation.a
DALVIK_LIBS += $(GLES_LIBS)
$(BUILD)/dalvik/dalvikvm.js $(DWEB)/dalvikvm.js: $(GLES_LIBS)
# WebGL 2 for graphics_translation in every build (unused in Node).
DVM_LDFLAGS += -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2

.PHONY: gles-libs
gles-libs: $(GLES_LIBS)

# --- sound ---------------------------------------------------------------------------
# AOSP's AudioTrack JNI over shim/audio's lean libmedia (include/media), the
# in-process mixer and its sinks; AudioSystem natives, MediaPlayer
# (libmedia_jni, through System.loadLibrary) and arcwasm.audio.AudioStats.
AUDIO_SRCS := $(FWB)/core/jni/android_media_AudioTrack.cpp shim/audio/audio_flinger.cc \
              shim/audio/audio_track.cc shim/audio/register_audio.cc shim/audio/media_player.cc
AUDIO_XFLAGS = -Ishim/audio/include $(FW_INCLUDES) $(BINDER_INC)
$(call dobj,$(AUDIO_SRCS)): XFLAGS = $(AUDIO_XFLAGS)
$(BUILD)/libaudio_jni.a: $(call dobj,$(AUDIO_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

AUDIO_LIBS := $(BUILD)/libaudio_jni.a
DALVIK_LIBS += $(AUDIO_LIBS)
$(BUILD)/dalvik/dalvikvm.js $(DWEB)/dalvikvm.js: $(AUDIO_LIBS)

# The page side (Web Audio), in every browser build; GWEB_LDFLAGS comes from
# DVM_LDFLAGS in mk/topic-graphics.mk, included later.
AUDIO_PRE_JS := shim/audio/web_audio.js
$(DWEB)/dalvikvm.js $(BUILD)/graphics-web/dalvikvm.js: $(AUDIO_PRE_JS) mk/topic-gles-audio.mk
$(DWEB)/dalvikvm.js: DVM_LDFLAGS += --pre-js $(AUDIO_PRE_JS)
$(BUILD)/graphics-web/dalvikvm.js: GWEB_LDFLAGS += --pre-js $(AUDIO_PRE_JS)

# System.loadLibrary("media_jni") looks for the file in /system/lib before it
# reaches the statically linked library (shim/dalvik/static_jni_libs.cc).
AUDIO_LIB_STUB := $(BUILD)/dalvik/empty-library.so
$(AUDIO_LIB_STUB):
	@mkdir -p $(dir $@) && : > $@
DSYSTEM_ADD += --add $(AUDIO_LIB_STUB):system/lib/libmedia_jni.so
$(DSYSTEM_IMAGE): $(AUDIO_LIB_STUB)

.PHONY: audio-libs gles-test
audio-libs: $(AUDIO_LIBS)
gles-test: dalvik
	cd $(BUILD)/dalvik && $(NODE) dalvikvm.js -cp /system/framework/arc-tests.jar arcwasm.GlesTest

# --- gles-web: the graphics-web build with the GL and sound demo ---------------------
#   make gles-web   # build/graphics-web/gles.html: GlesDemo (rotating triangle in a
#                   # GLSurfaceView, "Play tone"); ?selftest=1 taps the button
$(BUILD)/graphics-web/gles.html: demo/gles/index.html
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: gles-web
gles-web: graphics-web $(BUILD)/graphics-web/gles.html
