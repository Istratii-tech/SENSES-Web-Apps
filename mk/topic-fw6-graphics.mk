# Topic "fw6-graphics": the graphics of the Android 6.0.1 framework
# (android-6.0.1_r81) for ART in WebAssembly (docs/topics/fw6-graphics.md).
#
#   Skia (external/skia, branch m) drawing in software, with freetype,
#   harfbuzz_ng, libpng, libjpeg, giflib, libwebp and expat;
#   minikin (text layout); hwui (frameworks/base/libs/hwui: SkiaCanvas, the
#   software Canvas, and RenderNode/DisplayList; the GL renderer is
#   compiled but not used yet);
#   libui, libgui (BufferQueue, Surface, SurfaceComposerClient) with
#   SurfaceFlinger in the process (shim/fw6-graphics), a software gralloc;
#   libinput (InputChannel, MotionEvent/KeyEvent);
#   the android.graphics / android.view JNI of libandroid_runtime
#   (libandroid_runtime_graphics.a), registered by topic fw6-core's gRegJNI
#   (shim/fw6/register_fw6.cc).
#
#   make fw6-graphics-libs     # the libraries
#   make fw6-graphics-test     # native test (gtest) in Node
#   make art                   # artvm with all of it (FW6_JNI_LIBS, FW6_LIBS)
#
# Built on topic fw6-core (mk/topic-fw6-core.mk: FW6_INCLUDES, system/core,
# libbinder, libandroidfw, the libandroid_runtime frame). Extra AOSP
# projects: patches/aosp60/fw6-graphics.projects. Objects go to
# build/fw6g/obj, libraries to build/fw6g/lib (variables: FG*).

FG     := $(BUILD)/fw6g
FGOBJ  := $(FG)/obj
FGLIB  := $(FG)/lib
fgobj   = $(patsubst %,$(FGOBJ)/%.o,$(1))
FGEXT  := $(A60)/external

FG_COMMON := $(filter-out -MMD -MP,$(A_COMMON)) -MMD -MP -Wno-unused-value \
             -Wno-macro-redefined -Wno-shift-negative-value -Wno-unused-label \
             -Wno-implicit-fallthrough -Wno-unused-lambda-capture -Wno-pessimizing-move \
             -Wno-inconsistent-missing-override -Wno-overloaded-virtual \
             -Wno-dangling-else -Wno-parentheses -Wno-tautological-compare \
             -Wno-self-assign -Wno-char-subscripts -Wno-string-plus-int \
             -Wno-unused-command-line-argument -Wno-nontrivial-memcall
FG_CFLAGS   := $(FG_COMMON) -std=gnu11
FG_CXXFLAGS := $(FG_COMMON) -std=gnu++14 -Wno-register

$(FGOBJ)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(EMCC) $(FG_CFLAGS) $(FGFLAGS) -c $< -o $@

$(FGOBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(FG_CXXFLAGS) $(FGFLAGS) -c $< -o $@

$(FGOBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(FG_CXXFLAGS) $(FGFLAGS) -c $< -o $@

-include $(shell find $(FGOBJ) -name '*.d' 2>/dev/null)

# (make does not notice changed flags: after changing any of the flags
# below, remove the objects concerned, e.g. rm -rf build/fw6g/obj.)
define fg_archive
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
endef

# --- freetype 2.6 --------------------------------------------------------------
FG_FT := $(FGEXT)/freetype
FG_FT_SRCS := $(shell tools/mk_srcs.py $(FG_FT)/Android.mk LOCAL_SRC_FILES $(FG_FT)/)
$(FGOBJ)/$(FG_FT)/%: FGFLAGS := -I$(FG_FT)/include -DFT2_BUILD_LIBRARY -DDARWIN_NO_CARBON \
    -I$(FGEXT)/libpng -sUSE_ZLIB=1
$(FGLIB)/libft2.a: $(call fgobj,$(FG_FT_SRCS))
	$(fg_archive)

# --- libpng 1.6 ----------------------------------------------------------------
FG_PNG := $(FGEXT)/libpng
FG_PNG_SRCS := $(shell tools/mk_srcs.py $(FG_PNG)/Android.mk common_SRC_FILES $(FG_PNG)/)
$(FGOBJ)/$(FG_PNG)/%: FGFLAGS := -sUSE_ZLIB=1 -DPNG_ARM_NEON_OPT=0
$(FGLIB)/libpng.a: $(call fgobj,$(FG_PNG_SRCS))
	$(fg_archive)

# --- libjpeg 6b ----------------------------------------------------------------
FG_JPEG := $(FGEXT)/jpeg
FG_JPEG_SRCS := $(addprefix $(FG_JPEG)/,$(filter %.c,$(shell sed -n '/^LOCAL_SRC_FILES := /,/jmemnobs/p' \
                  $(FG_JPEG)/Android.mk | tr -d '\\')))
$(FGOBJ)/$(FG_JPEG)/%: FGFLAGS := -DAVOID_TABLES -DANDROID_TILE_BASED_DECODE -fstrict-aliasing
$(FGLIB)/libjpeg.a: $(call fgobj,$(FG_JPEG_SRCS))
	$(fg_archive)

# --- giflib 5 ------------------------------------------------------------------
FG_GIF := $(FGEXT)/giflib
FG_GIF_SRCS := $(shell tools/mk_srcs.py $(FG_GIF)/Android.mk LOCAL_SRC_FILES $(FG_GIF)/)
$(FGOBJ)/$(FG_GIF)/%: FGFLAGS := -DHAVE_CONFIG_H -I$(FG_GIF)
$(FGLIB)/libgif.a: $(call fgobj,$(FG_GIF_SRCS))
	$(fg_archive)

# --- expat: Skia's fonts.xml parser --------------------------------------------
FG_EXPAT := $(FGEXT)/expat
FG_EXPAT_SRCS := $(shell tools/mk_srcs.py $(FG_EXPAT)/Android.mk common_SRC_FILES $(FG_EXPAT)/)
$(FGOBJ)/$(FG_EXPAT)/%: FGFLAGS := -DHAVE_EXPAT_CONFIG_H -I$(FG_EXPAT) -I$(FG_EXPAT)/lib
$(FGLIB)/libexpat.a: $(call fgobj,$(FG_EXPAT_SRCS))
	$(fg_archive)

# --- libwebp: decode and encode in one (portable code, no cpu-features) ----------
FG_WEBP := $(FGEXT)/webp
FG_WEBP_SRCS := $(addprefix $(FG_WEBP)/src/,$(filter-out %/cpu-features.c %_neon.c %_sse2.c %_avx2.c \
    %_mips32.c %_mips_dsp_r2.c, $(shell grep -o '[a-z_0-9]*/[a-z_0-9-]*\.c' $(FG_WEBP)/src/Android.mk | sort -u)))
$(FGOBJ)/$(FG_WEBP)/%: FGFLAGS := -DWEBP_SWAP_16BIT_CSP -I$(FG_WEBP)/include -I$(FG_WEBP)/src
$(FGLIB)/libwebp.a: $(call fgobj,$(FG_WEBP_SRCS))
	$(fg_archive)

# --- harfbuzz_ng (with ICU 55 from the ART build) --------------------------------
# harfbuzz checks `!this` in member functions; clang drops such checks
# (see docs/topics/graphics.md).
FG_HB := $(FGEXT)/harfbuzz_ng
FG_HB_SRCS := $(addprefix $(FG_HB)/,$(filter %.cc,$(shell tools/mk_srcs.py $(FG_HB)/Android.mk \
                HARFBUZZ_SRC_FILES)) src/hb-icu.cc)
$(FGOBJ)/$(FG_HB)/%: FGFLAGS := -I$(FG_HB)/src $(ICU55_INCLUDES) -DHB_NO_MT -DHAVE_OT -DHAVE_ICU \
    -DHAVE_ICU_BUILTIN -fno-delete-null-pointer-checks
$(FGLIB)/libharfbuzz_ng.a: $(call fgobj,$(FG_HB_SRCS))
	$(fg_archive)

# --- Skia (branch m), software only ----------------------------------------------
# Android.mk's list minus the GPU backend (src/gpu, *_Gpu), PDF (needs
# sfntly), and the ARM/x86 optimizations (the portable *_opts_none.cpp of
# the MIPS list instead). SkUserConfig.h: SK_SUPPORT_GPU from the command
# line, no SK_SUPPORT_PDF under ARC_WASM (patches/aosp60/fw6-graphics.patch).
FG_SKIA := $(FGEXT)/skia
FG_SKIA_SRCS := $(filter-out src/gpu/% src/pdf/% src/doc/SkDocument_PDF.cpp \
                  src/image/SkImage_Gpu.cpp src/image/SkSurface_Gpu.cpp \
                  src/core/SkUtilsArm.cpp src/opts/%, \
                  $(shell tools/mk_srcs.py $(FG_SKIA)/Android.mk LOCAL_SRC_FILES)) \
                $(shell tools/mk_srcs.py $(FG_SKIA)/Android.mk LOCAL_SRC_FILES_mips)
FG_SKIA_SRCS := $(addprefix $(FG_SKIA)/,$(sort $(FG_SKIA_SRCS)))
FG_SKIA_INCLUDES := $(addprefix -I$(FG_SKIA)/include/,codec c config core pathops pipe effects \
                      images ports utils) \
                    -I$(FG_SKIA)/src/utils
# The defines every user of the Skia headers needs to agree on.
FG_SKIA_DEFINES := -DSK_RELEASE -DSK_SUPPORT_GPU=0 -DSK_BUILD_FOR_ANDROID -DSK_BUILD_FOR_ANDROID_FRAMEWORK
$(FGOBJ)/$(FG_SKIA)/%: FGFLAGS := $(FG_SKIA_INCLUDES) $(FG_SKIA_DEFINES) -DSKIA_IMPLEMENTATION=1 \
    $(addprefix -I$(FG_SKIA)/src/,codec core sfnt image opts utils effects lazy ports pdf) \
    -I$(FG_SKIA)/include/gpu -I$(FG_SKIA)/src/gpu -I$(FG_SKIA)/third_party/etc1 \
    -I$(FG_SKIA)/third_party/ktx \
    -I$(FG_JPEG) -I$(FG_PNG) -I$(FG_GIF) -I$(FG_WEBP)/include -I$(FG_EXPAT)/lib -I$(FG_FT)/include \
    -sUSE_ZLIB=1 -DLOG_TAG=\"skia\" -I$(A60)/system/core/include
$(FGLIB)/libskia.a: $(call fgobj,$(FG_SKIA_SRCS))
	$(fg_archive)

FG_SKIA_LIBS := $(FGLIB)/libskia.a $(FGLIB)/libharfbuzz_ng.a $(FGLIB)/libft2.a $(FGLIB)/libpng.a \
                $(FGLIB)/libjpeg.a $(FGLIB)/libgif.a $(FGLIB)/libwebp.a $(FGLIB)/libexpat.a

.PHONY: fw6-skia
fw6-skia: $(FG_SKIA_LIBS)

# --- include paths of the framework's native graphics code ----------------------------
# FW6_INCLUDES (topic fw6-core) plus: Android's EGL native types
# (shim/fw6-graphics/include), the GL and HAL headers, Skia, minikin;
# fw6_graphics_compat.h (ScopedTrace: the stack is built without
# HAVE_ANDROID_OS).
FG_FWB := $(A60)/frameworks/base
FG_FWN := $(A60)/frameworks/native
FG_MINIKIN := $(A60)/frameworks/minikin
FG_HWUI := $(FG_FWB)/libs/hwui
FG_JNI := $(FG_FWB)/core/jni
FG_FW_INCLUDES_BASE := -Ishim/fw6-graphics/include $(FW6_INCLUDES) \
    -I$(FG_FWN)/opengl/include -I$(A60)/hardware/libhardware/include \
    -I$(A60)/system/core/libsync/include \
    $(FG_SKIA_INCLUDES) $(FG_SKIA_DEFINES) -I$(FG_SKIA)/src/core -I$(FG_SKIA)/src/effects \
    -I$(FG_SKIA)/src/images -I$(FG_HB)/src -I$(FG_FT)/include -I$(FG_MINIKIN)/include \
    -DEGL_EGLEXT_PROTOTYPES -DGL_GLEXT_PROTOTYPES -DLOG_NDEBUG=1
# ScopedTrace and cutils/trace.h for hwui, libgui and the rest; not for
# shim/input (Emscripten's atomics, see there).
FG_FW_INCLUDES := $(FG_FW_INCLUDES_BASE) -include shim/fw6-graphics/include/fw6_graphics_compat.h

# --- minikin (text layout: fonts, itemization, line breaking) ----------------------
FG_MINIKIN_SRCS := $(addprefix $(FG_MINIKIN)/libs/minikin/,$(shell tools/mk_srcs.py \
                     $(FG_MINIKIN)/libs/minikin/Android.mk minikin_src_files))
$(FGOBJ)/$(FG_MINIKIN)/%: FGFLAGS := $(FG_FW_INCLUDES) -DLOG_TAG=\"Minikin\"
$(FGLIB)/libminikin.a: $(call fgobj,$(FG_MINIKIN_SRCS))
	$(fg_archive)

# --- hwui ------------------------------------------------------------------------
# All of libhwui (Android.common.mk) without RenderScript: SkiaCanvas (the
# software Canvas), RenderNode/DisplayList (recorded by every View), and the
# OpenGL renderer and RenderThread for the hardware path.
FG_HWUI_SRCS := $(addprefix $(FG_HWUI)/,$(filter-out tests/%,$(shell tools/mk_srcs.py \
                  $(FG_HWUI)/Android.common.mk LOCAL_SRC_FILES)))
# hwui's GL path (topic fw6-hwui, docs/topics/fw6-hwui.md): its GL calls go
# to Emscripten's WebGL2 and its EGL calls to shim/fw6-hwui/hwui_egl.cc
# (renames: shim/fw6-hwui/hwui_gl_names.h, tools/gen_hwui_gl_names.py), as
# do those of the code built on its headers (the JNI).
FG_HWUI_GL := -Ishim/fw6-hwui -include shim/fw6-hwui/hwui_gl_wasm.h
# Emscripten's GL as hwui needs it: WebGL2 (client-side arrays: hwui_gl.cc).
FG_GL_LDFLAGS := -sMAX_WEBGL_VERSION=2
$(FGOBJ)/$(FG_HWUI)/%: FGFLAGS := $(FG_FW_INCLUDES) $(FG_HWUI_GL) -I$(FG_HWUI) \
    -DATRACE_TAG=ATRACE_TAG_VIEW -DLOG_TAG=\"OpenGLRenderer\" -ffast-math -Wno-gnu-static-float-init
$(FGLIB)/libhwui.a: $(call fgobj,$(FG_HWUI_SRCS))
	$(fg_archive)

# --- libui, libgui, libinput (frameworks/native) -----------------------------------------
# libui without FramebufferNativeWindow (the fb HAL); libgui without the
# sensor service client; GraphicBuffers come from shim/fw6-graphics/gralloc_sw.cc.
FG_NL := $(FG_FWN)/libs
FG_UI_SRCS := $(addprefix $(FG_NL)/ui/,$(filter-out FramebufferNativeWindow.cpp, \
                $(shell tools/mk_srcs.py $(FG_NL)/ui/Android.mk LOCAL_SRC_FILES)))
FG_GUI_SRCS := $(addprefix $(FG_NL)/gui/,$(filter-out ISensorEventConnection.cpp ISensorServer.cpp \
                 Sensor.cpp SensorEventQueue.cpp SensorManager.cpp, \
                 $(shell tools/mk_srcs.py $(FG_NL)/gui/Android.mk LOCAL_SRC_FILES)))
FG_INPUT_SRCS := $(addprefix $(FG_NL)/input/,$(filter %.cpp,$(shell tools/mk_srcs.py \
                   $(FG_NL)/input/Android.mk commonSources) \
                   $(shell tools/mk_srcs.py $(FG_NL)/input/Android.mk deviceSources)))
$(FGOBJ)/$(FG_NL)/%: FGFLAGS := $(FG_FW_INCLUDES) -DDONT_USE_FENCE_SYNC -DLOG_TAG=\"libgui\"
$(FGOBJ)/$(FG_NL)/ui/%: FGFLAGS := $(FG_FW_INCLUDES) -DLOG_TAG=\"libui\"
$(FGOBJ)/$(FG_NL)/input/%: FGFLAGS := $(FG_FW_INCLUDES) -DLOG_TAG=\"libinput\"
$(FGLIB)/libui.a: $(call fgobj,$(FG_UI_SRCS))
	$(fg_archive)
$(FGLIB)/libgui.a: $(call fgobj,$(FG_GUI_SRCS))
	$(fg_archive)
$(FGLIB)/libinput.a: $(call fgobj,$(FG_INPUT_SRCS))
	$(fg_archive)

# --- SurfaceFlinger in the process, gralloc, GL extension stubs --------------------------
FG_SHIM_SRCS := shim/fw6-graphics/gralloc_sw.cc shim/fw6-graphics/surface_flinger6.cc \
                shim/fw6-graphics/gl_extras.cc
$(FGOBJ)/shim/fw6-graphics/%: FGFLAGS := $(FG_FW_INCLUDES) -I$(FG_HWUI) -I$(FG_JNI) \
    -I$(FG_JNI)/android/graphics
$(FGLIB)/libfw6_surfaceflinger.a: $(call fgobj,$(FG_SHIM_SRCS))
	$(fg_archive)

# --- the graphics part of libandroid_runtime ------------------------------------------
FG_GJNI_SRCS := $(addprefix $(FG_JNI)/android/graphics/,$(addsuffix .cpp, \
    AutoDecodeCancel AvoidXfermode Bitmap BitmapFactory Camera CanvasProperty ColorFilter \
    DrawFilter FontFamily CreateJavaOutputStreamAdaptor Graphics HarfBuzzNGFaceSkia Interpolator \
    MaskFilter Matrix MinikinSkia MinikinUtils Movie NinePatch NinePatchImpl NinePatchPeeker \
    Paint PaintImpl Path PathMeasure PathEffect Picture PorterDuff BitmapRegionDecoder \
    Rasterizer Region Shader Typeface TypefaceImpl Utils Xfermode YuvToJpegEncoder)) \
  $(addprefix $(FG_JNI)/,$(addsuffix .cpp, \
    android_graphics_Canvas android_graphics_Picture \
    android_view_DisplayListCanvas android_view_RenderNode android_view_RenderNodeAnimator \
    android_view_HardwareLayer android_view_ThreadedRenderer android_view_TextureView \
    android_text_StaticLayout \
    com_android_internal_view_animation_NativeInterpolatorFactoryHelper \
    \
    android_view_Surface android_view_SurfaceControl android_view_SurfaceSession \
    android_view_GraphicBuffer android_view_DisplayEventReceiver \
    android_view_InputChannel android_view_InputDevice android_view_InputEventReceiver \
    android_view_InputEventSender android_view_InputQueue android_view_KeyCharacterMap \
    android_view_KeyEvent android_view_MotionEvent android_view_PointerIcon \
    android_view_VelocityTracker android_emoji_EmojiFactory)) \
  $(FG_JNI)/android/graphics/SurfaceTexture.cpp shim/fw6-graphics/register_fw6_graphics.cc \
  shim/fw6-graphics/arcwasm_graphics6_jni.cc shim/input/browser_input.cc shim/input/register_input.cc
$(FGOBJ)/$(FG_JNI)/%: FGFLAGS := $(FG_FW_INCLUDES) $(FG_HWUI_GL) -I$(FG_JNI)/android/graphics -I$(FG_HWUI) \
    -I$(FG_JNI) -I$(FG_FWN)/opengl/libs -I$(FGEXT)/jpeg -I$(FGEXT)/expat/lib -DPACKED= \
    -I$(A60)/frameworks/opt/emoji \
    -DLOG_TAG=\"GraphicsJNI\"
# The browser's input (topic input's dispatcher and the arcwasm.input
# natives), built against libinput 6.0.
$(FGOBJ)/shim/input/%: FGFLAGS := $(FG_FW_INCLUDES_BASE) -DARC_WASM_FW6=1 -I$(FG_JNI) \
    -Ishim/input -DLOG_TAG=\"BrowserInput\"
$(FGOBJ)/shim/fw6-graphics/arcwasm_graphics6_jni.cc.o: FGFLAGS := $(FG_FW_INCLUDES_BASE) -Ishim/input
$(FGLIB)/libandroid_runtime_graphics.a: $(call fgobj,$(FG_GJNI_SRCS))
	$(fg_archive)

# hwui's EGL (topic fw6-hwui).
$(FGOBJ)/shim/fw6-hwui/%: FGFLAGS := $(FG_FW_INCLUDES) $(FG_HWUI_GL)
$(FGLIB)/libfw6_hwui_egl.a: $(call fgobj,shim/fw6-hwui/hwui_egl.cc shim/fw6-hwui/hwui_gl.cc)
	$(fg_archive)

FG_LIBS := $(FGLIB)/libhwui.a $(FGLIB)/libfw6_hwui_egl.a $(FGLIB)/libminikin.a $(FGLIB)/libfw6_surfaceflinger.a \
           $(FGLIB)/libgui.a $(FGLIB)/libui.a $(FGLIB)/libinput.a $(FG_SKIA_LIBS)

.PHONY: fw6-graphics-libs
fw6-graphics-libs: $(FGLIB)/libandroid_runtime_graphics.a $(FG_LIBS)

# --- the display backend (shim/graphics/framebuffer.h) ------------------------------------
$(FGOBJ)/shim/graphics/%: FGFLAGS :=
FG_FB_HEADLESS := $(call fgobj,shim/graphics/framebuffer_headless.cc)

# --- into artvm (topic fw6-core's FW6_JNI_LIBS / FW6_LIBS) ---------------------------------
# The JNI library is linked whole (gRegJNI refers to its register_*
# functions weakly). The display: memory (framebuffer_headless.cc) in both
# artvm builds for now; the browser's canvas (gralloc/WebGL) needs its own
# page build (docs/topics/fw6-graphics.md).
FW6_JNI_LIBS += $(FGLIB)/libandroid_runtime_graphics.a
FW6_LIBS += $(FG_LIBS) $(FG_FB_HEADLESS)
$(ABLD)/artvm.js $(AWEB)/artvm.js: $(FGLIB)/libandroid_runtime_graphics.a $(FG_LIBS) $(FG_FB_HEADLESS)
$(ABLD)/artvm.js $(AWEB)/artvm.js: ART_LDFLAGS += $(FG_GL_LDFLAGS)

# --- fonts for /system: what framework/fonts6/fonts.xml (/system/etc/fonts.xml) names --------
FG_FONTS := $(wildcard $(FGEXT)/roboto-fonts/Roboto*.ttf) $(FG_FWB)/data/fonts/DroidSansMono.ttf
FG_FONTS_ADD := $(foreach f,$(FG_FONTS),--add $(f):system/fonts/$(notdir $(f))) \
                --add framework/fonts6/fonts.xml:system/etc/fonts.xml
FG_KEYBOARDS := $(FG_FWB)/data/keyboards
FG_KEYS_ADD := --add $(FG_KEYBOARDS)/Generic.kl:system/usr/keylayout/Generic.kl \
               --add $(FG_KEYBOARDS)/Generic.kcm:system/usr/keychars/Generic.kcm \
               --add $(FG_KEYBOARDS)/Virtual.kcm:system/usr/keychars/Virtual.kcm
ASYSTEM_ADD += $(FG_FONTS_ADD) $(FG_KEYS_ADD)
$(ASYSTEM_IMAGE): $(FG_FONTS) framework/fonts6/fonts.xml

# --- native test: build/fw6g/fw6_graphics_test.js (Node) ---------------------------------------
# libutils' Static.cpp is linked as an object: it only has String8/16's
# static initializer, and wasm-ld runs the static initializers of an
# archive member only when something else keeps the member alive.
FG_TEST_IMAGE := $(FG)/test-system.img
$(FG_TEST_IMAGE): $(FG_FONTS) framework/fonts6/fonts.xml tools/make_system_image.py
	@mkdir -p $(dir $@)
	python3 tools/make_system_image.py $@ $(FG_FONTS_ADD)
# android::Paint, TypefaceImpl and the minikin glue without the JNI around them.
FG_TEST_OBJ := $(call fgobj,tests/fw6-graphics/fw6_graphics_test.cc \
                 $(addprefix $(FG_JNI)/android/graphics/,PaintImpl.cpp TypefaceImpl.cpp \
                   MinikinSkia.cpp MinikinUtils.cpp HarfBuzzNGFaceSkia.cpp))
$(call fgobj,tests/fw6-graphics/fw6_graphics_test.cc): FGFLAGS := $(FG_FW_INCLUDES) $(FG_HWUI_GL) -I$(FG_HWUI) \
    -I$(FG_JNI)/android/graphics \
    -Ishim/fw6-graphics -I$(GT)/googletest/include
FG_UTILS_STATIC := $(call f6obj,$(A60)/system/core/libutils/Static.cpp)
FG_TEST_LDFLAGS := -pthread -sPROXY_TO_PTHREAD -sUSE_ZLIB=1 -sINITIAL_MEMORY=256MB \
    -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB \
    -sPTHREAD_POOL_SIZE=12 -sEXIT_RUNTIME=1 -sASSERTIONS=1 --profiling-funcs \
    -sENVIRONMENT=node,worker --js-library $(OPFS_JS) --js-library $(CLOCK_JS) $(FG_GL_LDFLAGS)
# (FW6_LIBS: fw6-core's libraries, here already with this topic's, above.)
FG_TEST_LIBS := $(FG_LIBS) $(filter-out $(FG_LIBS) $(FG_FB_HEADLESS),$(FW6_LIBS)) \
                $(ALIB)/libartsys.a $(ALIB)/libart_shim.a \
                $(ALIB)/libicui18n.a $(ALIB)/libicuuc.a $(BUILD)/libposix_translation.a
$(FG)/fw6_graphics_test.js: $(FG_TEST_OBJ) $(FG_FB_HEADLESS) $(FG_UTILS_STATIC) $(GTEST_OBJS) \
                            $(ART_VFS_OBJ) $(KERNEL_OBJ) $(FG_TEST_LIBS) $(FG_TEST_IMAGE) $(OPFS_JS) $(CLOCK_JS)
	$(EMXX) $(FG_TEST_LDFLAGS) --preload-file $(FG_TEST_IMAGE)@/arc/system.img \
	    $(FG_TEST_OBJ) $(FG_FB_HEADLESS) $(FG_UTILS_STATIC) $(GTEST_OBJS) $(ART_VFS_OBJ) $(KERNEL_OBJ) \
	    -Wl,--start-group $(FG_TEST_LIBS) -Wl,--end-group $(FG_TEST_LDEXTRA) -o $@

.PHONY: fw6-graphics-test
fw6-graphics-test: $(FG)/fw6_graphics_test.js
	cd $(FG) && $(NODE) fw6_graphics_test.js

# --- Java tests on ART -------------------------------------------------------------------
# The Android 4.4 tests, unchanged, compiled against the 6.0.1 framework,
# with the 6.0 implementations of the arcwasm classes they use
# (framework/java-arcwasm6; natives in libarcwasm_graphics6.so above).
FG_JAVA6 := $(addprefix framework/java-arcwasm6/arcwasm/,graphics/FramebufferSurface.java \
              input/BrowserInput.java input/InputManagerService.java)
FG_TESTS := GraphicsTest ViewTest InputTest
# tests/java6: tests of 6.0-only API (not in the Dalvik build's tests/java).
FG_TESTS6 := SurfaceTest HwuiTest
FG_TEST_SRCS := $(addprefix tests/java/arcwasm/,$(addsuffix .java,$(FG_TESTS))) \
                $(addprefix tests/java6/arcwasm/,$(addsuffix .java,$(FG_TESTS6)))
ART_JAVA_TESTS += $(FG_JAVA6) $(FG_TEST_SRCS)
$(AFW)/arc-tests.jar: $(FG_JAVA6) $(FG_TEST_SRCS)
# GraphicsTest's PNG (the rule is in mk/topic-graphics.mk, included later).
ASYSTEM_ADD += --add $(BUILD)/dalvik/graphics-test.png:system/media/arcwasm/graphics-test.png
$(ASYSTEM_IMAGE): $(BUILD)/dalvik/graphics-test.png
# Demo programs of the Android 4.4 build that run on 6.0 unchanged:
# GraphicsDemo (animation), InputDemo (DOM input, ?selftest=1).
FG_DEMOS := GraphicsDemo InputDemo
ART_JAVA_TESTS += $(addprefix tests/java/arcwasm/,$(addsuffix .java,$(FG_DEMOS)))
$(AFW)/arc-tests.jar: $(addprefix tests/java/arcwasm/,$(addsuffix .java,$(FG_DEMOS)))

# --- fw6-graphics-web: artvm with the display on the page's canvas ----------------------
#   make fw6-graphics-web   # build/fw6-graphics-web/: serve with tools/serve.py, open
#                           # index.html (GraphicsDemo), input.html (InputDemo),
#                           # index.html?arg=-cp&arg=/system/framework/arc-tests.jar&arg=arcwasm.SurfaceTest
# art-web with the gralloc display backend instead of the one in memory:
# SurfaceFlinger composes into a gralloc buffer of ARC's graphics_translation,
# whose compositor thread shows it on the page's <canvas> (OffscreenCanvas,
# WebGL2), as graphics-web does for Android 4.4 (docs/topics/graphics.md);
# and the page's DOM input (shim/input/browser_input.js).
FGWEB := $(BUILD)/fw6-graphics-web
FG_GRALLOC_FB := $(call gobj,shim/graphics/framebuffer_gralloc.cc)
FGWEB_LDFLAGS = $(filter-out -sENVIRONMENT=%,$(ART_LDFLAGS)) -sENVIRONMENT=web,worker \
    -sOFFSCREENCANVAS_SUPPORT -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
    --pre-js shim/input/browser_input.js
$(FGWEB)/artvm.js: $(ART_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) $(FG_GRALLOC_FB) \
                   $(BUILD)/libgraphics_translation.a $(ALIB)/libart.a $(FW6_JNI_LIBS) $(FW6_LIBS) \
                   $(BUILD)/libposix_translation.a $(ASYSTEM_IMAGE) $(OPFS_JS) $(CLOCK_JS) \
                   shim/input/browser_input.js art/artvm_pre.js
	@mkdir -p $(dir $@)
	$(EMXX) $(FGWEB_LDFLAGS) $(ART_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) $(FG_GRALLOC_FB) \
	    -Wl,--start-group $(filter-out $(FG_FB_HEADLESS),$(ART_LIBS)) \
	    $(BUILD)/libgraphics_translation.a $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
# The 4.4 pages, loading artvm.js.
$(FGWEB)/index.html: demo/graphics/index.html
	@mkdir -p $(dir $@)
	sed -e 's/dalvikvm/artvm/g' -e 's/Dalvik/ART (Android 6.0.1)/g' $< > $@
$(FGWEB)/input.html: demo/input/index.html
	@mkdir -p $(dir $@)
	sed -e 's/dalvikvm/artvm/g' -e 's/Dalvik/ART (Android 6.0.1)/g' $< > $@
$(FGWEB)/report.js: shim/report.js
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: fw6-graphics-web
fw6-graphics-web: $(FGWEB)/artvm.js $(FGWEB)/index.html $(FGWEB)/input.html $(FGWEB)/report.js
