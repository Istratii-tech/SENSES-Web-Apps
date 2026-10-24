# Topic "graphics": android.graphics on Dalvik, drawn by software Skia.
#
# external/skia (AOSP 4.4.4) without its GPU backend, with the libraries it
# decodes and draws text with (freetype, libpng, libjpeg, giflib, libwebp,
# expat for the font config, harfbuzz_ng for text shaping), the
# android.graphics JNI from frameworks/base/core/jni/android/graphics, and
# shim/graphics (registration, and FramebufferSurface: a display that shows
# Canvas-drawn buffers through ARC's gralloc and EGL on WebGL2).
# The extra AOSP projects are listed in patches/aosp44/graphics.projects.
# Notes: docs/topics/graphics.md.

GFX_EXT := $(A44)/external
SKIA    := $(GFX_EXT)/skia

# --- freetype 2.4.2 (external/freetype) --------------------------------------
FT := $(GFX_EXT)/freetype
FT_SRCS := $(shell tools/mk_srcs.py $(FT)/Android.mk LOCAL_SRC_FILES $(FT)/)
$(DOBJ)/$(FT)/%: XFLAGS := -I$(FT)/builds -I$(FT)/include -DFT2_BUILD_LIBRARY \
    -DDARWIN_NO_CARBON -I$(GFX_EXT)/libpng -sUSE_ZLIB=1
$(BUILD)/libft2.a: $(call dobj,$(FT_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libpng 1.2 (external/libpng) ----------------------------------------------
PNG := $(GFX_EXT)/libpng
PNG_SRCS := $(shell tools/mk_srcs.py $(PNG)/Android.mk common_SRC_FILES $(PNG)/)
$(DOBJ)/$(PNG)/%: XFLAGS := -std=gnu89 -sUSE_ZLIB=1
$(BUILD)/libpng.a: $(call dobj,$(PNG_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libjpeg 6b (external/jpeg) ------------------------------------------------
# jmemnobs.c (plain malloc) instead of the ashmem/tmpfile backing store, and
# no ARM IDCT assembly.
JPEG := $(GFX_EXT)/jpeg
# The library's list from Android.mk (mk_srcs.py would also pick up the
# command-line tools' lists, which reassign LOCAL_SRC_FILES).
JPEG_SRCS := $(addprefix $(JPEG)/,$(addsuffix .c, \
    jcapimin jcapistd jccoefct jccolor jcdctmgr jchuff jcinit jcmainct jcmarker \
    jcmaster jcomapi jcparam jcphuff jcprepct jcsample jctrans jdapimin jdapistd \
    jdatadst jdatasrc jdcoefct jdcolor jddctmgr jdhuff jdinput jdmainct jdmarker \
    jdmaster jdmerge jdphuff jdpostct jdsample jdtrans jerror jfdctflt jfdctfst \
    jfdctint jidctflt jidctfst jidctint jidctred jquant1 jquant2 jutils jmemmgr \
    jmemnobs))
$(DOBJ)/$(JPEG)/%: XFLAGS := -DAVOID_TABLES -DANDROID_TILE_BASED_DECODE -fstrict-aliasing
$(BUILD)/libjpeg.a: $(call dobj,$(JPEG_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- giflib (external/giflib) --------------------------------------------------
GIF := $(GFX_EXT)/giflib
GIF_SRCS := $(shell tools/mk_srcs.py $(GIF)/Android.mk LOCAL_SRC_FILES $(GIF)/)
$(DOBJ)/$(GIF)/%: XFLAGS := -DHAVE_CONFIG_H -I$(GIF)
$(BUILD)/libgif.a: $(call dobj,$(GIF_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- expat (external/expat): Skia's font config parser -------------------------
EXPAT := $(GFX_EXT)/expat
EXPAT_SRCS := $(shell tools/mk_srcs.py $(EXPAT)/Android.mk common_SRC_FILES $(EXPAT)/)
$(DOBJ)/$(EXPAT)/%: XFLAGS := -DHAVE_EXPAT_CONFIG_H -I$(EXPAT) -I$(EXPAT)/lib
$(BUILD)/libexpat.a: $(call dobj,$(EXPAT_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libwebp (external/webp): the decode and encode libraries in one --------------
# Without -DANDROID: the portable code paths, no cpu-features probing.
WEBP := $(GFX_EXT)/webp
# (Both lists name shared files as ../dsp/... and so on.)
WEBP_SRCS := $(sort $(filter-out %/cpu-features.c %_neon.c %_sse2.c, $(subst /dec/../,/,$(subst /enc/../,/, \
    $(shell tools/mk_srcs.py $(WEBP)/src/dec/Android.mk LOCAL_SRC_FILES $(WEBP)/src/dec/) \
    $(shell tools/mk_srcs.py $(WEBP)/src/enc/Android.mk LOCAL_SRC_FILES $(WEBP)/src/enc/)))))
$(DOBJ)/$(WEBP)/%: XFLAGS := -DWEBP_SWAP_16BIT_CSP -I$(WEBP)/include
$(BUILD)/libwebp.a: $(call dobj,$(WEBP_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- harfbuzz_ng (external/harfbuzz_ng): shaping for TextLayoutCache ------------
HB := $(GFX_EXT)/harfbuzz_ng
HB_SRCS := $(shell tools/mk_srcs.py $(HB)/Android.mk LOCAL_SRC_FILES $(HB)/)
# harfbuzz checks `!this` in member functions (hb_object_header_t::destroy
# on a NULL object); clang drops such checks as undefined behaviour and the
# NULL object's reference count at address 0 gets decremented.
$(DOBJ)/$(HB)/%: XFLAGS := -I$(HB)/src $(ICU_INCLUDES) -DHB_NO_MT -DHAVE_OT -DHAVE_ICU \
    -fno-delete-null-pointer-checks
$(BUILD)/libharfbuzz_ng.a: $(call dobj,$(HB_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- Skia (external/skia), software only ---------------------------------------
# Android.mk's list minus the GPU backend (src/gpu, SkImage/SkSurface_Gpu),
# PDF (needs sfntly), the debugger canvas and the ARM optimizations; the
# portable *_opts_none.cpp are used instead. SK_SUPPORT_GPU=0 needs the SkUserConfig.h change in
# patches/aosp44/graphics.patch.
SKIA_SRCS := $(filter-out src/gpu/% src/pdf/% src/doc/SkDocument_PDF.cpp \
               src/image/SkImage_Gpu.cpp src/image/SkSurface_Gpu.cpp \
               src/utils/debugger/% \
               src/opts/%_arm.cpp src/opts/%_neon.cpp src/opts/%.S src/opts/opts_check_arm.cpp \
               src/core/SkUtilsArm.cpp, \
               $(shell tools/mk_srcs.py $(SKIA)/Android.mk LOCAL_SRC_FILES))
SKIA_SRCS := $(addprefix $(SKIA)/,$(SKIA_SRCS))
SKIA_INCLUDES := $(addprefix -I$(SKIA)/include/,core config effects images lazy pathops \
                   pipe ports utils xml) \
                 $(addprefix -I$(SKIA)/src/,core image images lazy opts sfnt utils)
# The defines every user of the Skia headers needs to agree on.
SKIA_DEFINES := -DSK_BUILD_FOR_ANDROID -DSK_RELEASE -DSK_SUPPORT_GPU=0
SKIA_XFLAGS := $(SKIA_INCLUDES) $(SKIA_DEFINES) $(FW_INCLUDES) \
    -I$(FT)/include -I$(PNG) -I$(GIF) -I$(JPEG) -I$(EXPAT)/lib -I$(WEBP)/include -sUSE_ZLIB=1 \
    -DDCT_IFAST_SUPPORTED -DSK_USE_FREETYPE_EMBOLDEN \
    -DSK_FONTHOST_FREETYPE_RUNTIME_VERSION=0x020400 -DSK_CAN_USE_DLOPEN=0 \
    -DLOG_TAG=\"skia\" -Wno-unused-private-field -Wno-tautological-compare
# -I$(SKIA): for "../harfbuzz/src/harfbuzz-shaper.h" in SkTypeface_android.h
# (only the HB_Script enum of the old external/harfbuzz is used).
SKIA_XFLAGS += -I$(SKIA)
$(DOBJ)/$(SKIA)/%: XFLAGS := $(SKIA_XFLAGS)
$(DOBJ)/$(SKIA)/src/utils/android/ashmem.cpp.o: XFLAGS := $(SKIA_XFLAGS) -Ishim/linux/..
$(DOBJ)/$(SKIA)/src/ports/SkFontConfigParser_android.cpp.o: XFLAGS := $(SKIA_XFLAGS) \
    -include shim/graphics/system_property_get.h
$(BUILD)/libskia.a: $(call dobj,$(SKIA_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

GFX_EXT_LIBS := $(BUILD)/libskia.a $(BUILD)/libharfbuzz_ng.a $(BUILD)/libft2.a \
                $(BUILD)/libpng.a $(BUILD)/libjpeg.a $(BUILD)/libgif.a $(BUILD)/libwebp.a \
                $(BUILD)/libexpat.a

.PHONY: skia
skia: $(GFX_EXT_LIBS)

# --- android.graphics JNI (frameworks/base/core/jni/android/graphics) -----------
# The graphics part of libandroid_runtime's Android.mk, without
# SurfaceTexture (libgui), YuvToJpegEncoder and pdf.
# No USE_OPENGL_RENDERER: hwui is not built, shim/graphics/include/hwui
# stands in for the headers it is still included through. Bitmap/Region
# Parcel natives use libbinder (mk/topic-binder.mk), Typeface.createFromAsset
# and 9-patch decoding use libandroidfw (mk/topic-apk.mk).
GJNI := $(FWB)/core/jni/android/graphics
GJNI_SRCS := $(addprefix $(GJNI)/,$(addsuffix .cpp, \
    AutoDecodeCancel Bitmap BitmapFactory BitmapRegionDecoder Camera Canvas ColorFilter DrawFilter \
    CreateJavaOutputStreamAdaptor Graphics HarfBuzzNGFaceSkia Interpolator \
    LayerRasterizer MaskFilter Matrix Movie NinePatch NinePatchImpl NinePatchPeeker \
    Paint Path PathMeasure PathEffect Picture PorterDuff Rasterizer Region Shader \
    TextLayout TextLayoutCache Typeface Utils Xfermode)) \
    $(FWB)/core/jni/android_nio_utils.cpp
GJNI_XFLAGS := $(SKIA_INCLUDES) $(SKIA_DEFINES) -I$(SKIA) $(FW_INCLUDES) $(ICU_INCLUDES) \
    -I$(SKIA)/include -I$(GJNI) -Ishim/graphics/include/hwui -I$(HB)/src \
    -DLOG_TAG=\"GraphicsJNI\"
# What android.graphics needs from android.os/android.util: SystemProperties
# (DisplayMetrics) and FloatMath; Trace (BitmapFactory) has its natives in
# shim/graphics/android_os_Trace.cc.
GJNI_SRCS += $(addprefix $(FWB)/core/jni/,android_os_SystemProperties.cpp android_util_FloatMath.cpp)
$(DOBJ)/$(FWB)/core/jni/android_os_SystemProperties.cpp.o: XFLAGS := $(FW_INCLUDES)
$(DOBJ)/$(FWB)/core/jni/android_util_FloatMath.cpp.o: XFLAGS := $(GJNI_XFLAGS)
$(DOBJ)/$(GJNI)/%: XFLAGS := $(GJNI_XFLAGS)
$(DOBJ)/$(FWB)/core/jni/android_nio_utils.cpp.o: XFLAGS := $(GJNI_XFLAGS)
$(DOBJ)/shim/graphics/%: XFLAGS := $(GJNI_XFLAGS)
GSHIM_SRCS := shim/graphics/system_property_get.cc shim/graphics/register_graphics.cc \
              shim/graphics/android_os_Trace.cc \
              shim/graphics/arcwasm_graphics_FramebufferSurface.cc
$(BUILD)/libandroid_graphics.a: $(call dobj,$(GJNI_SRCS) $(GSHIM_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# The display backend (shim/graphics/framebuffer.h): headless in builds
# without GL; graphics-web below links framebuffer_gralloc.cc instead.
$(BUILD)/libframebuffer_headless.a: $(call dobj,shim/graphics/framebuffer_headless.cc)
	@rm -f $@ && $(EMAR) rcs $@ $^

GRAPHICS_LIBS := $(BUILD)/libandroid_graphics.a $(GFX_EXT_LIBS)

.PHONY: graphics-libs
graphics-libs: $(GRAPHICS_LIBS) $(BUILD)/libframebuffer_headless.a

# --- into dalvikvm (Node and dalvik-web) -------------------------------------------
DALVIK_LIBS += $(GRAPHICS_LIBS) $(BUILD)/libframebuffer_headless.a
$(BUILD)/dalvik/dalvikvm.js $(DWEB)/dalvikvm.js: $(GRAPHICS_LIBS) $(BUILD)/libframebuffer_headless.a

# framework.jar also gets framework/java-arcwasm (FramebufferSurface).
$(DFW)/framework.jar: $(shell find framework/java-arcwasm -name '*.java')

# /system: fonts (AOSP's Roboto and DroidSansMono with a config listing just
# them) and the PNG GraphicsTest decodes.
GFX_FONTS := Roboto-Regular Roboto-Bold Roboto-Italic Roboto-BoldItalic DroidSansMono
GFX_TEST_PNG := $(BUILD)/dalvik/graphics-test.png
$(GFX_TEST_PNG): tools/make_test_png.py
	@mkdir -p $(dir $@)
	python3 tools/make_test_png.py $@
DSYSTEM_EXTRA += $(foreach f,$(GFX_FONTS),$(FWB)/data/fonts/$(f).ttf:system/fonts/$(f).ttf) \
                 framework/fonts/system_fonts.xml:system/etc/system_fonts.xml \
                 framework/fonts/fallback_fonts.xml:system/etc/fallback_fonts.xml \
                 $(GFX_TEST_PNG):system/media/arcwasm/graphics-test.png
$(DSYSTEM_IMAGE): $(foreach f,$(GFX_FONTS),$(FWB)/data/fonts/$(f).ttf) \
                  framework/fonts/system_fonts.xml framework/fonts/fallback_fonts.xml $(GFX_TEST_PNG)

# --- graphics-web: dalvikvm with a display, for the browser ---------------------
#   make graphics-web   # build/graphics-web/: serve with tools/serve.py, open
#                       # index.html (GraphicsDemo) or
#                       # index.html?arg=-cp&arg=/system/framework/arc-tests.jar&arg=arcwasm.GraphicsTest
# dalvik-web plus graphics_translation (EGL/GLES on WebGL2, gralloc) and the
# gralloc display backend, whose compositor thread takes over the page's
# <canvas> as an OffscreenCanvas.
GWEB := $(BUILD)/graphics-web
GFB_GRALLOC_OBJ := $(call gobj,shim/graphics/framebuffer_gralloc.cc)
$(GFB_GRALLOC_OBJ): $(BUILD)/gobj/%.o: % shim/graphics/framebuffer.h
	@mkdir -p $(dir $@)
	$(EMXX) $(GCXXFLAGS) -c $< -o $@
GWEB_LIBS := $(filter-out $(BUILD)/libframebuffer_headless.a,$(DALVIK_LIBS)) \
             $(BUILD)/libgraphics_translation.a
# Safari 15.2 (iOS 15.2) is the oldest with SharedArrayBuffer. Before Safari
# 17 there is no WebGL on an OffscreenCanvas, so the display shows its frames
# on the page's 2D canvas there (framebuffer_gralloc.cc). So the canvas is
# not handed to main()'s thread at startup (OFFSCREENCANVASES_TO_PTHREAD):
# the display decides, and the compositor thread gets it when it wants it.
GWEB_LDFLAGS := $(DVM_LDFLAGS) -sENVIRONMENT=web,worker -sOFFSCREENCANVAS_SUPPORT \
                '-sOFFSCREENCANVASES_TO_PTHREAD=""' \
                -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sMIN_SAFARI_VERSION=150200
$(GWEB)/dalvikvm.js: $(DVM_MAIN_OBJ) $(DVM_VFS_OBJ) $(KERNEL_OBJ) $(GFB_GRALLOC_OBJ) $(GWEB_LIBS) \
                     $(BUILD)/libposix_translation.a $(DSYSTEM_IMAGE) $(OPFS_JS) $(CLOCK_JS) mk/topic-graphics.mk
	@mkdir -p $(dir $@)
	$(EMXX) $(GWEB_LDFLAGS) $(DVM_MAIN_OBJ) $(DVM_VFS_OBJ) $(KERNEL_OBJ) $(GFB_GRALLOC_OBJ) \
	    -Wl,--start-group $(GWEB_LIBS) $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
$(GWEB)/index.html: demo/graphics/index.html
	@mkdir -p $(dir $@)
	cp $< $@
$(GWEB)/report.js: shim/report.js
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: graphics-web graphics-test
graphics-web: $(GWEB)/dalvikvm.js $(GWEB)/index.html $(GWEB)/report.js
graphics-test: dalvik
	cd $(BUILD)/dalvik && $(NODE) dalvikvm.js -cp /system/framework/arc-tests.jar arcwasm.GraphicsTest
