# Topic "fw8-graphics": the graphics, surfaces and input of the Android 8.1
# framework (android-8.1.0_r81) on ART 8.1 in WebAssembly
# (docs/topics/fw8-graphics.md), the way topics fw6-graphics and fw6-hwui
# did it for 6.0.1 (mk/topic-fw6-graphics.mk):
#
#   Skia (external/skia, oc-mr1) drawing in software, its GPU (GL) code
#   compiled for hwui's Skia pipeline, with freetype, harfbuzz_ng, libpng,
#   libjpeg-turbo, giflib, libwebp and expat (topic art8's);
#   minikin 8.1 (text layout) with Roboto and Noto (external/roboto-fonts,
#   external/noto-fonts);
#   hwui 8.1 (frameworks/base/libs/hwui: SkiaCanvas, RenderNode, the OpenGL
#   pipeline on WebGL2 as topic fw6-hwui did);
#   libui, libgui 8.1 (BufferQueue, Surface, SurfaceComposerClient), libnativewindow
#   (ANativeWindow_*, AHardwareBuffer) with SurfaceFlinger in the process
#   (shim/fw8-graphics) and a software gralloc (IAllocator/IMapper 2.0 in
#   process); libinput 8.1;
#   the android.graphics / android.view / android.text JNI of
#   libandroid_runtime 8.1 (libandroid_runtime_graphics8.a), registered by
#   topic fw8-core's gRegJNI (shim/fw8/register_fw8.cc).
#
#   make fw8-graphics-libs     # the libraries
#   make fw8-graphics-test     # native test (gtest) in Node
#   make art8                  # artvm8 with all of it
#   make fw8-graphics-web      # artvm8 with the display on the page's canvas
#
# Built on topic fw8-core (mk/topic-fw8-core.mk: FW8_INCLUDES, F8_LIBS, the
# libandroid_runtime 8.1 frame) and topic art8 (ICU 58, expat). Extra AOSP
# projects: patches/aosp81/fw8-graphics.projects. Objects go to
# build/fw8g/obj, libraries to build/fw8g/lib (variables: G8*).

G8     := $(BUILD)/fw8g
G8OBJ  := $(G8)/obj
G8LIB  := $(G8)/lib
g8obj   = $(patsubst %,$(G8OBJ)/%.o,$(1))
G8EXT  := $(A81)/external

G8_COMMON := $(filter-out -MMD -MP,$(F8_COMMON)) -MMD -MP -Wno-unused-value \
             -Wno-macro-redefined -Wno-shift-negative-value -Wno-unused-label \
             -Wno-implicit-fallthrough -Wno-pessimizing-move \
             -Wno-inconsistent-missing-override -Wno-overloaded-virtual \
             -Wno-dangling-else -Wno-parentheses -Wno-tautological-compare \
             -Wno-self-assign -Wno-char-subscripts -Wno-string-plus-int \
             -Wno-unused-command-line-argument -Wno-nontrivial-memcall \
             -Wno-deprecated-builtins -Wno-unknown-attributes -Wno-unqualified-std-cast-call \
             -Wno-bitwise-instead-of-logical -Wno-unused-result -Wno-deprecated-this-capture \
             -Wno-missing-braces -Wno-int-in-bool-context -Wno-return-type-c-linkage \
             -Wno-invalid-specialization
G8_CFLAGS   := $(G8_COMMON) -std=gnu11
G8_CXXFLAGS := $(G8_COMMON) -std=gnu++14 -Wno-register

$(G8OBJ)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(EMCC) $(G8_CFLAGS) $(G8FLAGS) -c $< -o $@
# C++ objects wait for the HIDL headers hidl-gen makes (below and topic
# fw8-core): nothing else orders them, and a clean "make -j8" compiled libgui
# and SurfaceFlinger before them (fatal error: android/hardware/.../types.h).
$(G8OBJ)/%.cpp.o: %.cpp | $(G8)/gen/hidl-cpp/hidl.stamp $(F8_HIDL_GENCPP)/hidl.stamp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(G8_CXXFLAGS) $(G8FLAGS) -c $< -o $@
$(G8OBJ)/%.cc.o: %.cc | $(G8)/gen/hidl-cpp/hidl.stamp $(F8_HIDL_GENCPP)/hidl.stamp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(G8_CXXFLAGS) $(G8FLAGS) -c $< -o $@
-include $(shell find $(G8OBJ) -name '*.d' 2>/dev/null)

# (make does not notice changed flags: after changing any of the flags
# below, remove the objects concerned, e.g. rm -rf build/fw8g/obj/...)
define g8_archive
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
endef
# The graphics compat header (ATRACE_NAME, ScopedTrace: utils/Trace.h 8.1 is
# empty without __ANDROID__), for everything built on libutils/libcutils.
G8_COMPAT := -Ishim/fw8-graphics/include -include shim/fw8-graphics/include/fw8_graphics_compat.h
g8bp = $(shell tools/bp_srcs.py $(1) $(2) --prefix $(dir $(1)) $(3) 2>/dev/null)

# --- freetype ---------------------------------------------------------------------
G8_FT := $(G8EXT)/freetype
$(G8OBJ)/$(G8_FT)/%: G8FLAGS := -I$(G8_FT)/include -DFT2_BUILD_LIBRARY -DDARWIN_NO_CARBON \
    -I$(G8EXT)/libpng -sUSE_ZLIB=1
# (Android.bp has two modules named libft2: the sources are the first one's.)
G8_FT_SRCS := $(addprefix $(G8_FT)/,$(shell sed -n '1,/^}/p' $(G8_FT)/Android.bp | grep -o '"src/[^"]*\.c"' | tr -d '"'))
$(G8LIB)/libft2.a: $(call g8obj,$(G8_FT_SRCS))
	$(g8_archive)

# --- libpng 1.6 (portable code, no NEON/SSE filters) ---------------------------------
G8_PNG := $(G8EXT)/libpng
$(G8OBJ)/$(G8_PNG)/%: G8FLAGS := -sUSE_ZLIB=1 -DPNG_ARM_NEON_OPT=0 -DPNG_INTEL_SSE_OPT=0 -std=gnu89
$(G8LIB)/libpng.a: $(call g8obj,$(call g8bp,$(G8_PNG)/Android.bp,libpng))
	$(g8_archive)

# --- libjpeg-turbo (portable: jsimd_none.c of the MIPS list) -------------------------
G8_JPEG := $(G8EXT)/libjpeg-turbo
$(G8OBJ)/$(G8_JPEG)/%: G8FLAGS := -fstrict-aliasing -I$(G8_JPEG)
$(G8LIB)/libjpeg.a: $(call g8obj,$(call g8bp,$(G8_JPEG)/Android.bp,libjpeg,--arch mips))
	$(g8_archive)

# --- giflib -------------------------------------------------------------------------
G8_GIF := $(G8EXT)/giflib
$(G8OBJ)/$(G8_GIF)/%: G8FLAGS := -DHAVE_CONFIG_H -I$(G8_GIF)
$(G8LIB)/libgif.a: $(call g8obj,$(call g8bp,$(G8_GIF)/Android.bp,libgif))
	$(g8_archive)

# --- libwebp: decode and encode in one (portable code, no cpu-features) --------------
G8_WEBP := $(G8EXT)/webp
G8_WEBP_SRCS := $(filter-out %/cpu-features.c %_neon.c %_sse2.c %_sse41.c %_avx2.c %_mips32.c \
                  %_mips_dsp_r2.c %_msa.c, \
                  $(call g8bp,$(G8_WEBP)/src/Android.bp,libwebp-decode) \
                  $(call g8bp,$(G8_WEBP)/src/Android.bp,libwebp-encode))
$(G8OBJ)/$(G8_WEBP)/%: G8FLAGS := -DWEBP_SWAP_16BIT_CSP -I$(G8_WEBP)/include -I$(G8_WEBP)/src
$(G8LIB)/libwebp.a: $(call g8obj,$(sort $(G8_WEBP_SRCS)))
	$(g8_archive)

# --- harfbuzz_ng (with topic art8's ICU 58) -------------------------------------------
# harfbuzz checks `!this` in member functions; clang drops such checks
# (see docs/topics/graphics.md).
G8_HB := $(G8EXT)/harfbuzz_ng
$(G8OBJ)/$(G8_HB)/%: G8FLAGS := -I$(G8_HB)/src $(ICU58_INCLUDES) -DHB_NO_MT -DHAVE_OT -DHAVE_ICU \
    -DHAVE_ICU_BUILTIN -fno-delete-null-pointer-checks -DU_USING_ICU_NAMESPACE=0
$(G8LIB)/libharfbuzz_ng.a: $(call g8obj,$(call g8bp,$(G8_HB)/Android.bp,libharfbuzz_ng))
	$(g8_archive)

# --- Skia (oc-mr1) ------------------------------------------------------------------
# Android.bp's list for the device with the portable opts of the MIPS list,
# without: Vulkan (src/gpu/vk), PDF (needs sfntly), XPS and the Mac/Windows
# utilities, RAW (dng_sdk) and HEIF (libheif) codecs, the assembly of
# SkJumper (its portable C++ stages are used). The GL backend of src/gpu is
# compiled: hwui 8.1's headers use GrContext, and its Skia pipeline draws
# with it. SkUserConfig.h: no RAW, HEIF, PDF and Vulkan under ARC_WASM
# (patches/aosp81/fw8-graphics.patch).
G8_SKIA := $(G8EXT)/skia
G8_SKIA_SRCS := $(filter-out src/gpu/vk/% src/pdf/% src/xps/% src/utils/win/% src/utils/mac/% \
                  src/codec/SkRaw% src/codec/SkHeif% src/jumper/%.S src/opts/%, \
                  $(shell tools/bp_srcs.py $(G8_SKIA)/Android.bp libskia 2>/dev/null)) \
                $(shell tools/bp_srcs.py $(G8_SKIA)/Android.bp libskia --arch mips 2>/dev/null | grep opts/)
G8_SKIA_SRCS := $(addprefix $(G8_SKIA)/,$(sort $(G8_SKIA_SRCS)))
G8_SKIA_INCDIRS := $(addprefix $(G8_SKIA)/include/,android c codec config core effects encode gpu \
                     gpu/gl pathops ports svg utils private)
G8_SKIA_INCLUDES := $(addprefix -I,$(G8_SKIA_INCDIRS))
# The defines every user of the Skia headers needs to agree on (on a device
# SkPreConfig.h derives SK_BUILD_FOR_ANDROID from __ANDROID__).
G8_SKIA_DEFINES := -DSK_RELEASE -DSK_BUILD_FOR_ANDROID -DSK_BUILD_FOR_ANDROID_FRAMEWORK
$(G8OBJ)/$(G8_SKIA)/%: G8FLAGS := $(G8_SKIA_INCLUDES) $(G8_SKIA_DEFINES) -DSKIA_IMPLEMENTATION=1 \
    $(addprefix -I$(G8_SKIA)/,src/c src/codec src/core src/effects src/fonts src/gpu src/image \
      src/images src/lazy src/opts src/pathops src/ports src/sfnt src/shaders src/shaders/gradients \
      src/sksl src/utils src/xml third_party/gif) \
    -I$(G8_JPEG) -I$(G8_PNG) -I$(G8_GIF) -I$(G8_WEBP)/include -I$(A8_EXPAT)/lib -I$(G8_FT)/include \
    -I$(A81)/frameworks/native/libs/nativewindow/include -I$(A81)/frameworks/native/libs/arect/include \
    -I$(A81)/frameworks/native/libs/nativebase/include -I$(A81)/frameworks/native/opengl/include \
    $(FW8_INCLUDES) $(G8_COMPAT) -DATRACE_TAG=ATRACE_TAG_VIEW -Wno-c++17-extensions
$(G8LIB)/libskia.a: $(call g8obj,$(G8_SKIA_SRCS))
	$(g8_archive)

G8_SKIA_LIBS := $(G8LIB)/libskia.a $(G8LIB)/libharfbuzz_ng.a $(G8LIB)/libft2.a $(G8LIB)/libpng.a \
                $(G8LIB)/libjpeg.a $(G8LIB)/libgif.a $(G8LIB)/libwebp.a

.PHONY: fw8-skia
fw8-skia: $(G8_SKIA_LIBS)

# --- the graphics HIDL interfaces (C++ from hidl-gen) -----------------------------------
# libui 8.1 allocates and maps buffers through IAllocator/IMapper 2.0
# (Gralloc2.cpp), libgui converts producers to and from HIDL
# (bufferqueue@1.0, hidl.token@1.0) and asks configstore@1.0 about the
# display. Generated with topic fw8-core's hidl-gen into build/fw8g/gen.
G8_HIDL_GEN := $(G8)/gen/hidl-cpp
G8_HIDL_PKGS := android.hardware.graphics.common@1.0 android.hardware.graphics.allocator@2.0 \
                android.hardware.graphics.mapper@2.0 android.hardware.configstore@1.0 \
                android.hardware.graphics.bufferqueue@1.0 android.hardware.media@1.0 \
                android.hidl.token@1.0
G8_HIDL_ROOTS := -randroid.hardware:$(A81)/hardware/interfaces \
                 -randroid.hidl:$(A81)/system/libhidl/transport
G8_HIDL_SRCS := $(addprefix $(G8_HIDL_GEN)/android/,hardware/graphics/common/1.0/types.cpp \
                  hardware/graphics/allocator/2.0/AllocatorAll.cpp \
                  hardware/graphics/mapper/2.0/MapperAll.cpp hardware/graphics/mapper/2.0/types.cpp \
                  hardware/configstore/1.0/SurfaceFlingerConfigsAll.cpp \
                  hardware/configstore/1.0/types.cpp \
                  hardware/graphics/bufferqueue/1.0/GraphicBufferProducerAll.cpp \
                  hardware/graphics/bufferqueue/1.0/ProducerListenerAll.cpp \
                  hardware/media/1.0/types.cpp hidl/token/1.0/TokenManagerAll.cpp)
$(G8_HIDL_GEN)/hidl.stamp: $(F8H)/hidl-gen mk/topic-fw8-graphics.mk
	rm -rf $(G8_HIDL_GEN) && mkdir -p $(G8_HIDL_GEN)
	for p in $(G8_HIDL_PKGS); do \
	  for l in c++-headers c++-sources; do \
	    $(F8H)/hidl-gen -o $(G8_HIDL_GEN) -L$$l $(G8_HIDL_ROOTS) $$p || exit 1; \
	  done; \
	done
	@touch $@
$(G8_HIDL_SRCS): $(G8_HIDL_GEN)/hidl.stamp ;

# --- include paths of the framework's native graphics code ----------------------------
# FW8_INCLUDES (topic fw8-core) plus: Android's EGL native types
# (shim/fw8-graphics/include), the GL, HAL and HIDL headers, Skia, minikin;
# fw8_graphics_compat.h (ATRACE_NAME/ScopedTrace without __ANDROID__).
G8_FWB := $(A81)/frameworks/base
G8_FWN := $(A81)/frameworks/native
G8_NL := $(G8_FWN)/libs
G8_MINIKIN := $(A81)/frameworks/minikin
G8_HWUI := $(G8_FWB)/libs/hwui
G8_JNI := $(G8_FWB)/core/jni
G8_FW_INCLUDES_BASE := -Ishim/fw8-graphics/include $(FW8_INCLUDES) \
    $(addprefix -I$(G8_NL)/,ui/include gui/include input/include nativewindow/include arect/include \
      nativebase/include math/include) \
    -I$(G8_FWN)/opengl/include -I$(A81)/hardware/libhardware/include \
    -I$(A81)/system/core/libsync/include -I$(A81)/system/core/libgrallocusage/include \
    -I$(A81)/hardware/interfaces/configstore/utils/include \
    -I$(A81)/system/libhidl/transport/token/1.0/utils/include \
    -I$(G8_HIDL_GEN) -I$(F8_HIDL_GENCPP) \
    $(G8_SKIA_INCLUDES) $(G8_SKIA_DEFINES) $(addprefix -I$(G8_SKIA)/src/,core effects image utils gpu) \
    -I$(G8_HB)/src -I$(G8_FT)/include -I$(G8_MINIKIN)/include $(ICU58_INCLUDES) -DU_USING_ICU_NAMESPACE=0 \
    -DEGL_EGLEXT_PROTOTYPES -DGL_GLEXT_PROTOTYPES -DLOG_NDEBUG=1
G8_FW_INCLUDES := $(G8_FW_INCLUDES_BASE) $(G8_COMPAT)

# --- minikin 8.1 -------------------------------------------------------------------------
G8_MINIKIN_SRCS := $(call g8bp,$(G8_MINIKIN)/libs/minikin/Android.bp,libminikin,--target android)
# (Hyphenator.h uses std::vector without including it; older libc++ brought it in.)
$(G8OBJ)/$(G8_MINIKIN)/%: G8FLAGS := $(G8_FW_INCLUDES) -I$(G8_MINIKIN)/libs/minikin -I$(GT)/googletest/include \
    -include vector -DLOG_TAG=\"Minikin\"
$(G8LIB)/libminikin.a: $(call g8obj,$(G8_MINIKIN_SRCS))
	$(g8_archive)

# --- libui, libgui, libnativewindow, libinput (frameworks/native) ----------------------------
# libui's buffers come from the IAllocator/IMapper 2.0 of AOSP's passthrough
# HAL (hardware/interfaces/graphics/{allocator,mapper}/2.0/default) over the
# software gralloc HAL of topic fw6-graphics (shim/fw6-graphics/gralloc_sw.cc):
# Gralloc2.cpp takes them directly under ARC_WASM (fw8-graphics.patch).
G8_UI_SRCS := $(call g8bp,$(G8_NL)/ui/Android.bp,libui,--target android)
G8_GUI_SRCS := $(call g8bp,$(G8_NL)/gui/Android.bp,libgui,--target android)
# (three modules of nativewindow/Android.bp are named libnativewindow)
G8_NW_SRCS := $(addprefix $(G8_NL)/nativewindow/,AHardwareBuffer.cpp ANativeWindow.cpp) \
              $(A81)/system/core/libgrallocusage/GrallocUsageConversion.cpp
G8_INPUT_SRCS := $(call g8bp,$(G8_NL)/input/Android.bp,libinput,--target android)
G8_HAL_DIR := $(A81)/hardware/interfaces/graphics
G8_GRALLOC_SRCS := $(G8_HAL_DIR)/mapper/2.0/default/GrallocMapper.cpp \
                   $(G8_HAL_DIR)/mapper/2.0/default/Gralloc0Mapper.cpp \
                   $(G8_HAL_DIR)/mapper/2.0/default/Gralloc1Mapper.cpp \
                   $(G8_HAL_DIR)/allocator/2.0/default/Gralloc.cpp \
                   $(G8_HAL_DIR)/allocator/2.0/default/Gralloc0Allocator.cpp \
                   $(G8_HAL_DIR)/allocator/2.0/default/Gralloc1Allocator.cpp \
                   shim/fw6-graphics/gralloc_sw.cc
$(G8OBJ)/$(G8_NL)/%: G8FLAGS := $(G8_FW_INCLUDES) -DLOG_TAG=\"libgui\"
$(G8OBJ)/$(G8_NL)/ui/%: G8FLAGS := $(G8_FW_INCLUDES) -DLOG_TAG=\"libui\"
$(G8OBJ)/$(G8_NL)/input/%: G8FLAGS := $(G8_FW_INCLUDES) -DLOG_TAG=\"libinput\"
$(G8OBJ)/$(G8_NL)/nativewindow/%: G8FLAGS := $(G8_FW_INCLUDES) -std=gnu++17 -DLOG_TAG=\"libnativewindow\"
$(G8OBJ)/$(A81)/system/core/libgrallocusage/%: G8FLAGS := $(G8_FW_INCLUDES)
$(G8OBJ)/$(G8_HAL_DIR)/%: G8FLAGS := $(G8_FW_INCLUDES) -I$(G8_HAL_DIR)/allocator/2.0/default \
    -I$(G8_HAL_DIR)/mapper/2.0/default \
    -DLOG_TAG=\"gralloc-hal\"
$(G8OBJ)/shim/fw6-graphics/gralloc_sw.cc.o: G8FLAGS := $(G8_FW_INCLUDES)
$(G8OBJ)/$(G8_HIDL_GEN)/%: G8FLAGS := $(G8_FW_INCLUDES)
$(call g8obj,$(G8_UI_SRCS) $(G8_GUI_SRCS) $(G8_NW_SRCS) $(G8_GRALLOC_SRCS) $(G8_HIDL_SRCS)): \
    $(G8_HIDL_GEN)/hidl.stamp
$(G8LIB)/libui8.a: $(call g8obj,$(G8_UI_SRCS) $(G8_GRALLOC_SRCS))
	$(g8_archive)
# GLConsumer's GL/EGL: hwui's on the RenderThread (TextureView), the
# program's otherwise (shim/fw8-graphics/glconsumer_gl.cc).
$(G8OBJ)/$(G8_NL)/gui/GLConsumer.cpp.o: G8FLAGS := $(G8_FW_INCLUDES) -DLOG_TAG=\"GLConsumer\" \
    -include shim/fw8-graphics/include/glconsumer_gl.h
$(G8OBJ)/$(G8_NL)/gui/GLConsumer.cpp.o: shim/fw8-graphics/include/glconsumer_gl.h
$(G8LIB)/libgui8.a: $(call g8obj,$(G8_GUI_SRCS) $(G8_NW_SRCS) shim/fw8-graphics/glconsumer_gl.cc \
                      $(A81)/hardware/interfaces/configstore/utils/ConfigStoreUtils.cpp \
                      $(A81)/system/libhidl/transport/token/1.0/utils/HybridInterface.cpp)
	$(g8_archive)
$(G8OBJ)/$(A81)/hardware/interfaces/configstore/% $(G8OBJ)/$(A81)/system/libhidl/transport/token/%: \
    G8FLAGS := $(G8_FW_INCLUDES)
$(G8LIB)/libinput8.a: $(call g8obj,$(G8_INPUT_SRCS))
	$(g8_archive)
$(G8LIB)/libgraphics_hidl8.a: $(call g8obj,$(G8_HIDL_SRCS))
	$(g8_archive)

.PHONY: fw8-gui
fw8-gui: $(G8LIB)/libminikin.a $(G8LIB)/libui8.a $(G8LIB)/libgui8.a $(G8LIB)/libinput8.a \
         $(G8LIB)/libgraphics_hidl8.a

# --- protobuf-lite (C++), the protos of hwui ---------------------------------------------
# hwui 8.1 dumps RenderNodes (protos/hwui.proto) and keeps frame statistics
# (core/proto/android/service/graphicsstats.proto) in protobuf-lite messages;
# the C++ comes from topic fw8-core's host aprotoc.
G8_PB := $(A81)/external/protobuf
G8_PB_WASM := -include shim/fw8-graphics/include/protobuf_wasm.h
$(G8OBJ)/$(G8_PB)/%: G8FLAGS := -I$(G8_PB)/android -I$(G8_PB)/src -DGOOGLE_PROTOBUF_NO_RTTI -DHAVE_PTHREAD \
    -Wno-sign-promo $(G8_PB_WASM)
$(G8LIB)/libprotobuf-cpp-lite.a: $(call g8obj,$(call g8bp,$(G8_PB)/Android.bp,libprotobuf-cpp-lite))
	$(g8_archive)
G8_PROTO_GEN := $(G8)/gen/proto
G8_PROTO_SRCS := $(G8_PROTO_GEN)/protos/hwui.pb.cc \
                 $(G8_PROTO_GEN)/frameworks/base/core/proto/android/service/graphicsstats.pb.cc
$(G8_PROTO_GEN)/protos/hwui.pb.cc: $(G8_HWUI)/protos/hwui.proto $(F8H)/aprotoc
	@mkdir -p $(dir $@)
	$(F8H)/aprotoc --cpp_out=$(G8_PROTO_GEN) -I$(G8_HWUI) $<
$(G8_PROTO_GEN)/frameworks/base/core/proto/android/service/graphicsstats.pb.cc: \
    $(G8_FWB)/core/proto/android/service/graphicsstats.proto $(F8H)/aprotoc
	@mkdir -p $(dir $@)
	$(F8H)/aprotoc --cpp_out=$(G8_PROTO_GEN) -I$(A81) $<
G8_PROTO_FLAGS := -I$(G8_PROTO_GEN) -I$(G8_PB)/src -DGOOGLE_PROTOBUF_NO_RTTI $(G8_PB_WASM)
$(G8OBJ)/$(G8_PROTO_GEN)/%: G8FLAGS := $(G8_PROTO_FLAGS)

# --- hwui 8.1 ----------------------------------------------------------------------------------
# libhwui (Android.bp) without Vulkan: Skia is built without SK_VULKAN, but
# hwui's headers use the Vulkan types of Skia's vk/ headers, which then come
# from a force-included <vulkan/vulkan.h>; the Vulkan calls of VulkanManager
# go to shim/fw8-graphics/hwui_vulkan_stub.cc ("no Vulkan"),
# RenderScript (not used by 8.1's hwui sources) and the GLES validation
# wrapper. SkiaCanvas is the software Canvas; RenderNode/DisplayList are
# recorded by every View; the OpenGL pipeline draws on WebGL2 (topic
# fw8-graphics' hwui EGL, as topic fw6-hwui for 6.0.1).
G8_HWUI_SRCS := $(filter-out %.proto $(G8_HWUI)/debug/% %/VulkanManager.cpp, \
                  $(call g8bp,$(G8_HWUI)/Android.bp,libhwui))
# hwui's GL path, as topic fw6-hwui did for 6.0.1 (docs/topics/fw6-hwui.md):
# its GL calls go to Emscripten's WebGL2 and its EGL calls to
# shim/fw6-hwui/hwui_egl.cc (the same files, built against 8.1's headers;
# renames: shim/fw6-hwui/hwui_gl_names.h, tools/gen_hwui_gl_names.py), as do
# those of the JNI built on its headers. Skia's GL code is not renamed: it
# finds its functions with eglGetProcAddress (the Skia pipeline, not used).
G8_HWUI_GL := -Ishim/fw6-hwui -include shim/fw6-hwui/hwui_gl_wasm.h
G8_HWUI_FLAGS := $(G8_FW_INCLUDES) $(G8_HWUI_GL) -I$(G8_HWUI) $(G8_PROTO_FLAGS) -I$(G8_SKIA)/include/private \
    -I$(G8_FWN)/vulkan/include -DVK_USE_PLATFORM_ANDROID_KHR -include vulkan/vulkan.h \
    -DATRACE_TAG=ATRACE_TAG_VIEW -DLOG_TAG=\"OpenGLRenderer\" -Wno-gnu-static-float-init -fno-rtti
$(G8OBJ)/$(G8_HWUI)/%: G8FLAGS := $(G8_HWUI_FLAGS)
$(G8OBJ)/shim/fw8-graphics/hwui_vulkan_stub.cc.o: G8FLAGS := $(G8_HWUI_FLAGS)
$(call g8obj,$(G8_HWUI_SRCS) shim/fw8-graphics/hwui_vulkan_stub.cc): $(G8_PROTO_SRCS) \
    $(G8_HIDL_GEN)/hidl.stamp
$(G8LIB)/libhwui8.a: $(call g8obj,$(G8_HWUI_SRCS) shim/fw8-graphics/hwui_vulkan_stub.cc $(G8_PROTO_SRCS))
	$(g8_archive)

.PHONY: fw8-hwui
fw8-hwui: $(G8LIB)/libhwui8.a $(G8LIB)/libprotobuf-cpp-lite.a

# --- SurfaceFlinger in the process, GL extension stubs, the display -----------------------
G8_SF_SRCS := shim/fw8-graphics/surface_flinger8.cc shim/fw8-graphics/gl_extras8.cc
$(G8OBJ)/shim/fw8-graphics/%: G8FLAGS := $(G8_FW_INCLUDES)
$(G8LIB)/libfw8_surfaceflinger.a: $(call g8obj,$(G8_SF_SRCS))
	$(g8_archive)
# The display backend (shim/graphics/framebuffer.h): memory, for artvm8 in
# Node and art8-web; the page build (fw8-graphics-web) links the gralloc one.
$(G8OBJ)/shim/graphics/%: G8FLAGS :=
G8_FB_HEADLESS := $(call g8obj,shim/graphics/framebuffer_headless.cc)

# --- the graphics part of libandroid_runtime 8.1 ------------------------------------------
# core/jni of AOSP 8.1 (the register_* functions gRegJNI of topic fw8-core
# refers to weakly): android.graphics (without pdf: no PDF backend or pdfium),
# drawables, android.view surfaces/rendering/input, android.text,
# PathParser, HardwareBuffer.
G8_GJNI_SRCS := $(addprefix $(G8_JNI)/android/graphics/,$(addsuffix .cpp, \
    Bitmap BitmapFactory Camera CanvasProperty ColorFilter DrawFilter FontFamily FontUtils \
    CreateJavaOutputStreamAdaptor GIFMovie GraphicBuffer Graphics HarfBuzzNGFaceSkia Interpolator \
    MaskFilter Matrix Movie MovieImpl NinePatch NinePatchPeeker Paint Path PathMeasure PathEffect \
    Picture BitmapRegionDecoder Region Shader SurfaceTexture Typeface Utils YuvToJpegEncoder)) \
  $(addprefix $(G8_JNI)/,$(addsuffix .cpp, \
    android_graphics_Canvas android_graphics_Picture \
    android_graphics_drawable_AnimatedVectorDrawable android_graphics_drawable_VectorDrawable \
    android_view_DisplayEventReceiver android_view_DisplayListCanvas android_view_HardwareLayer \
    android_view_InputChannel android_view_InputDevice android_view_InputEventReceiver \
    android_view_InputEventSender android_view_InputQueue android_view_KeyCharacterMap \
    android_view_KeyEvent android_view_MotionEvent android_view_PointerIcon android_view_RenderNode \
    android_view_RenderNodeAnimator android_view_Surface android_view_SurfaceControl \
    android_view_SurfaceSession android_view_TextureView android_view_ThreadedRenderer \
    android_view_VelocityTracker android_text_AndroidBidi android_text_StaticLayout \
    android_util_PathParser android_hardware_HardwareBuffer android_hardware_display_DisplayViewport \
    com_android_internal_view_animation_NativeInterpolatorFactoryHelper)) \
  $(G8_FWB)/libs/androidfw/DisplayEventDispatcher.cpp \
  shim/fw8-graphics/register_fw8_graphics.cc
# (no RTTI, as the platform builds it: FrameMetricsObserver has a virtual
# function declared and never defined, so no type_info anywhere)
G8_GJNI_BASE := $(G8_FW_INCLUDES) -fno-rtti -I$(G8_JNI)/android/graphics -I$(G8_HWUI) \
    $(G8_PROTO_FLAGS) -I$(G8_SKIA)/include/private -I$(G8_FWN)/vulkan/include \
    -DVK_USE_PLATFORM_ANDROID_KHR -include vulkan/vulkan.h \
    -I$(G8_JNI) -I$(G8_JNI)/include -I$(G8_JPEG) -I$(A8_EXPAT)/lib \
    -I$(A81)/frameworks/native/opengl/libs -I$(A81)/frameworks/native/opengl/libs/EGL/include \
    -I$(A81)/libcore/include -I$(G8_SKIA)/src/images -I$(G8_SKIA)/src/codec -I$(G8_GIF) \
    -I$(GT)/googletest/include -DLOG_TAG=\"GraphicsJNI\"
G8_GJNI_FLAGS := $(G8_HWUI_GL) $(G8_GJNI_BASE)
$(G8OBJ)/$(G8_JNI)/% $(G8OBJ)/shim/fw8-graphics/register_fw8_graphics.cc.o \
$(G8OBJ)/$(G8_FWB)/libs/androidfw/DisplayEventDispatcher.cpp.o: G8FLAGS := $(G8_GJNI_FLAGS)
# SurfaceTexture serves the apps' GL (EGL14 on graphics_translation in the
# page, GLConsumer of libgui): not on hwui's renamed GL/EGL.
$(G8OBJ)/$(G8_JNI)/android/graphics/SurfaceTexture.cpp.o: \
    G8FLAGS := $(G8_GJNI_BASE)
$(call g8obj,$(G8_GJNI_SRCS)): $(G8_PROTO_SRCS) $(G8_HIDL_GEN)/hidl.stamp
$(G8LIB)/libandroid_runtime_graphics8.a: $(call g8obj,$(G8_GJNI_SRCS))
	$(g8_archive)

# hwui's EGL on WebGL2 (shim/fw6-hwui, built for 8.1).
$(G8OBJ)/shim/fw6-hwui/%: G8FLAGS := $(G8_FW_INCLUDES) $(G8_HWUI_GL)
$(G8LIB)/libfw8_hwui_egl.a: $(call g8obj,shim/fw6-hwui/hwui_egl.cc shim/fw6-hwui/hwui_gl.cc)
	$(g8_archive)

G8_LIBS := $(G8LIB)/libhwui8.a $(G8LIB)/libfw8_hwui_egl.a $(G8LIB)/libminikin.a $(G8LIB)/libfw8_surfaceflinger.a \
           $(G8LIB)/libgui8.a $(G8LIB)/libui8.a $(G8LIB)/libinput8.a $(G8LIB)/libgraphics_hidl8.a \
           $(G8LIB)/libprotobuf-cpp-lite.a $(G8_SKIA_LIBS)

.PHONY: fw8-graphics-libs
fw8-graphics-libs: $(G8LIB)/libandroid_runtime_graphics8.a $(G8_LIBS) $(G8_FB_HEADLESS)

# --- the test programs' natives: libarcwasm_graphics8.so --------------------------------
# arcwasm.graphics.FramebufferSurface (reads the display back) and topic
# input's browser input dispatcher (shim/input, built against libinput 8.1:
# ARC_WASM_FW6 for what 6.0 added, ARC_WASM_FW8 for 8.x's displayId), for
# the Java of framework/java-arcwasm8; it adds itself to artvm8's table of
# static JNI libraries. Linked whole (only its constructor refers to it).
G8_TESTJNI_SRCS := shim/fw8-graphics/arcwasm_graphics8_jni.cc shim/input/browser_input.cc \
                   shim/input/register_input.cc
$(G8OBJ)/shim/input/%: G8FLAGS := $(G8_FW_INCLUDES_BASE) -DARC_WASM_FW6=1 -DARC_WASM_FW8=1 \
    -I$(G8_JNI) -I$(G8_JNI)/include -Ishim/input -DLOG_TAG=\"BrowserInput\"
$(G8OBJ)/shim/fw8-graphics/arcwasm_graphics8_jni.cc.o: G8FLAGS := $(G8_FW_INCLUDES_BASE) -Ishim/input
$(G8LIB)/libarcwasm_graphics8.a: $(call g8obj,$(G8_TESTJNI_SRCS))
	$(g8_archive)

# --- for the NDK bridge on ART 8.1 (topic fw8-system builds the bridge) ------------------
# G8_NDK_LIBS: what the bridge needs of the 8.1 graphics, built here against
# its headers:
#   shim/fw6-system/layer_target6.cc  the frame destination of guest
#       ANativeWindow_lock/unlockAndPost and EGL window surfaces on a Surface
#       (arcwasm_layer_target_for_window; the ANativeWindow API is the same in 8.1)
#   shim/ndk/ndk_android_fw.cc        the framework half of libandroid (assets,
#       ALooper, ANativeWindow_fromSurface)
#   shim/ndk/ndk_sync.cc              libsync.so, native fence fds
#   shim/ndk/ndk_hardware_buffer.cc   AHardwareBuffer_* (libnativewindow 8.1)
# Link with $(G8_NDK_LDFLAGS) (they register themselves; -u keeps them).
G8_NDK_SRCS := shim/fw6-system/layer_target6.cc shim/ndk/ndk_android_fw.cc shim/ndk/ndk_sync.cc \
               shim/ndk/ndk_hardware_buffer.cc
$(G8OBJ)/shim/fw6-system/layer_target6.cc.o: G8FLAGS := $(G8_FW_INCLUDES_BASE) -Ishim/gles
$(G8OBJ)/shim/ndk/ndk_android_fw.cc.o: G8FLAGS := $(G8_FW_INCLUDES_BASE) -std=gnu++17 -I$(G8_JNI) \
    -I$(G8_JNI)/include -Ishim/ndk -include android/looper.h
$(G8OBJ)/shim/ndk/ndk_sync.cc.o: G8FLAGS := -std=gnu++17 $(A8_SYS_INCLUDES) -Ishim/ndk
$(G8OBJ)/shim/ndk/ndk_hardware_buffer.cc.o: G8FLAGS := -std=gnu++17 $(A8_SYS_INCLUDES) -Ishim/ndk \
    $(addprefix -I$(G8_NL)/,nativewindow/include arect/include nativebase/include)
$(G8LIB)/libfw8_ndk_graphics.a: $(call g8obj,$(G8_NDK_SRCS))
	$(g8_archive)
G8_NDK_LIBS := $(G8LIB)/libfw8_ndk_graphics.a
G8_NDK_LDFLAGS := -Wl,-u,ndk_android_fw_linked -Wl,-u,ndk_sync_linked \
                  -Wl,-u,ndk_hardware_buffer_linked
.PHONY: fw8-graphics-ndk
fw8-graphics-ndk: $(G8_NDK_LIBS)

# --- fonts and key maps for /system ---------------------------------------------------------
# What framework/fonts8/fonts.xml (/system/etc/fonts.xml) names: Roboto,
# Roboto Condensed, Noto Serif, Droid Sans Mono; the fallbacks Noto Sans
# Symbols, Noto Color Emoji (7 MB) and Droid Sans Fallback (CJK, 3.4 MB).
G8_NOTO := $(G8EXT)/noto-fonts
G8_FONTS := $(wildcard $(G8EXT)/roboto-fonts/Roboto*.ttf) \
            $(addprefix $(G8_NOTO)/other/,NotoSerif-Regular.ttf NotoSerif-Bold.ttf NotoSerif-Italic.ttf \
              NotoSerif-BoldItalic.ttf NotoSansSymbols-Regular-Subsetted.ttf \
              NotoSansSymbols-Regular-Subsetted2.ttf) \
            $(G8_NOTO)/emoji/NotoColorEmoji.ttf \
            $(G8_FWB)/data/fonts/DroidSansMono.ttf $(G8_FWB)/data/fonts/DroidSansFallback.ttf
G8_FONTS_ADD := $(foreach f,$(G8_FONTS),--add $(f):system/fonts/$(notdir $(f))) \
                --add framework/fonts8/fonts.xml:system/etc/fonts.xml
G8_KEYBOARDS := $(G8_FWB)/data/keyboards
G8_KEYS_ADD := --add $(G8_KEYBOARDS)/Generic.kl:system/usr/keylayout/Generic.kl \
               --add $(G8_KEYBOARDS)/Generic.kcm:system/usr/keychars/Generic.kcm \
               --add $(G8_KEYBOARDS)/Virtual.kcm:system/usr/keychars/Virtual.kcm

# --- into artvm8 (topics art8 and fw8-core) --------------------------------------------------
# The JNI libraries are linked whole (gRegJNI refers to their register_*
# functions weakly; libarcwasm_graphics8 registers itself). The display:
# memory (framebuffer_headless.cc) in artvm8 and art8-web; the browser's
# canvas (gralloc/WebGL) is the page build's (fw8-graphics-web, below).
# Emscripten's GL as hwui needs it: WebGL2.
G8_GL_LDFLAGS := -sMAX_WEBGL_VERSION=2
ifneq ($(A8LIB),)
ART8_LIBS += -Wl,--whole-archive $(G8LIB)/libandroid_runtime_graphics8.a $(G8LIB)/libarcwasm_graphics8.a \
    -Wl,--no-whole-archive $(G8_LIBS) $(G8_FB_HEADLESS)
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js: $(G8LIB)/libandroid_runtime_graphics8.a \
    $(G8LIB)/libarcwasm_graphics8.a $(G8_LIBS) $(G8_FB_HEADLESS)
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js: ART8_LDFLAGS += $(G8_GL_LDFLAGS)
ASYSTEM8_ADD += $(G8_FONTS_ADD) $(G8_KEYS_ADD) \
    --add $(BUILD)/dalvik/graphics-test.png:system/media/arcwasm/graphics-test.png
$(A8SYSTEM_IMAGE): $(G8_FONTS) framework/fonts8/fonts.xml $(BUILD)/dalvik/graphics-test.png

# --- Java tests on ART 8.1 -------------------------------------------------------------------
# The Android 4.4 tests (GraphicsTest, ViewTest, InputTest) and the 6.0
# ones (SurfaceTest, HwuiTest), compiled against the 8.1 framework, with
# the 8.1 implementations of the arcwasm classes they use
# (framework/java-arcwasm8; natives in libarcwasm_graphics8.so above).
G8_JAVA8 := $(addprefix framework/java-arcwasm8/arcwasm/,graphics/FramebufferSurface.java \
              input/BrowserInput.java input/InputManagerService.java)
FW8G_TESTS := GraphicsTest ViewTest InputTest
FW8G_TESTS6 := SurfaceTest HwuiTest
G8_TEST_SRCS := $(addprefix tests/java/arcwasm/,$(addsuffix .java,$(FW8G_TESTS) GraphicsDemo InputDemo)) \
                $(addprefix tests/java6/arcwasm/,$(addsuffix .java,$(FW8G_TESTS6)))
ART8_JAVA_TESTS += $(G8_JAVA8) $(G8_TEST_SRCS)
$(A8FW)/arc-tests.jar: $(G8_JAVA8) $(G8_TEST_SRCS)
.PHONY: fw8-graphics-java-test
fw8-graphics-java-test: art8
	VM=art8 tools/run_java_tests.sh node $(FW8G_TESTS) $(FW8G_TESTS6)
endif

# --- native test: build/fw8g/fw8_graphics_test.js (Node) ---------------------------------------
# tests/fw8-graphics/fw8_graphics_test.cc on the libraries above, the kernel
# booted as artvm8 boots it (art/vfs_setup.cc) with a /system image of the
# fonts. libutils' Static.cpp is an object of its own (topic fw8-core).
ifneq ($(A8LIB),)
G8_TEST_IMAGE := $(G8)/test-system.img
$(G8_TEST_IMAGE): $(G8_FONTS) framework/fonts8/fonts.xml tools/make_system_image.py
	@mkdir -p $(dir $@)
	python3 tools/make_system_image.py $@ $(G8_FONTS_ADD)
# (sys8_fallbacks: fw8-core's weak ashmem_valid and log readers, in its JNI
# library. libhidl's transport/Static.cpp first: its static maps must be
# constructed before the static constructors of the HIDL interfaces register
# into them, and wasm-ld runs constructors in the order of the input files.)
G8_TEST_OBJ := $(call f8obj,$(A81)/system/libhidl/transport/Static.cpp) \
               $(call g8obj,tests/fw8-graphics/fw8_graphics_test.cc) $(call f8obj,shim/fw8/sys8_fallbacks.cc)
$(call g8obj,tests/fw8-graphics/fw8_graphics_test.cc): G8FLAGS := $(G8_HWUI_FLAGS) -Ishim/fw8-graphics \
    -I$(GT)/googletest/include
$(call g8obj,tests/fw8-graphics/fw8_graphics_test.cc): $(G8_PROTO_SRCS) $(G8_HIDL_GEN)/hidl.stamp
G8_TEST_LDFLAGS := -pthread -sPROXY_TO_PTHREAD -sUSE_ZLIB=1 -sINITIAL_MEMORY=256MB \
    -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB \
    -sPTHREAD_POOL_SIZE=12 -sEXIT_RUNTIME=1 -sASSERTIONS=1 --profiling-funcs \
    -sENVIRONMENT=node,worker --js-library $(OPFS_JS) --js-library $(CLOCK_JS) $(G8_GL_LDFLAGS)
G8_TEST_LIBS := $(G8_LIBS) $(F8_LIBS) $(F8_SYS_LIBS) $(A8LIB)/libicui18n58.a $(A8LIB)/libicuuc58.a \
                $(A8LIB)/libexpat8.a $(A8LIB)/libart8_shim.a $(BUILD)/libdev_binder.a \
                $(BUILD)/libposix_translation.a
# ($$(ART_VFS_OBJ): topics included later add to it, e.g. tiktok's /proc/self;
# .SECONDEXPANSION is on since mk/topic-art8.mk.)
$(G8)/fw8_graphics_test.js: $(G8_TEST_OBJ) $(G8_FB_HEADLESS) $(F8_UTILS_STATIC) $(GTEST_OBJS) \
                            $$(ART_VFS_OBJ) $(KERNEL_OBJ) $(G8_TEST_LIBS) $(G8_TEST_IMAGE) $(OPFS_JS) $(CLOCK_JS)
	$(EMXX) $(G8_TEST_LDFLAGS) --preload-file $(G8_TEST_IMAGE)@/arc/system.img \
	    $(G8_TEST_OBJ) $(G8_FB_HEADLESS) $(F8_UTILS_STATIC) $(GTEST_OBJS) $(ART_VFS_OBJ) $(KERNEL_OBJ) \
	    -Wl,--start-group $(G8_TEST_LIBS) -Wl,--end-group -o $@

.PHONY: fw8-graphics-test
fw8-graphics-test: $(G8)/fw8_graphics_test.js
	cd $(G8) && $(NODE) fw8_graphics_test.js $(GTEST_ARGS)
endif

# --- fw8-graphics-web: artvm8 with the display on the page's canvas ----------------------
#   make fw8-graphics-web   # build/fw8-graphics-web/: serve with tools/serve.py, open
#                           # index.html (GraphicsDemo), input.html (InputDemo),
#                           # index.html?arg=-cp&arg=/system/framework/arc-tests.jar&arg=arcwasm.ViewTest
# art8-web with the gralloc display backend instead of the one in memory:
# SurfaceFlinger 8.1 composes into a gralloc buffer of ARC's
# graphics_translation, whose compositor thread shows it on the page's
# <canvas> (OffscreenCanvas, WebGL2), as fw6-graphics-web does for 6.0.1;
# and the page's DOM input (shim/input/browser_input.js).
ifneq ($(A8LIB),)
G8WEB := $(BUILD)/fw8-graphics-web
G8_GRALLOC_FB := $(call gobj,shim/graphics/framebuffer_gralloc.cc)
G8WEB_LDFLAGS = $(ART8_LDFLAGS) -sENVIRONMENT=web,worker -sOFFSCREENCANVAS_SUPPORT \
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 --pre-js shim/input/browser_input.js
$(G8WEB)/artvm8.js: $(ART8_LINK_DEPS) $(G8_GRALLOC_FB) $(BUILD)/libgraphics_translation.a \
                    $(G8LIB)/libandroid_runtime_graphics8.a $(G8LIB)/libarcwasm_graphics8.a $(G8_LIBS) \
                    shim/input/browser_input.js
	@mkdir -p $(dir $@)
	$(EMXX) $(G8WEB_LDFLAGS) $(ART8_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) $(G8_GRALLOC_FB) \
	    -Wl,--start-group $(filter-out $(G8_FB_HEADLESS),$(ART8_LIBS)) \
	    $(BUILD)/libgraphics_translation.a $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
	$(call wasm_opt,$(@:.js=.wasm))
# The 4.4 pages, loading artvm8.js.
$(G8WEB)/index.html: demo/graphics/index.html
	@mkdir -p $(dir $@)
	sed -e 's/dalvikvm/artvm8/g' -e 's/Dalvik/ART (Android 8.1)/g' $< > $@
$(G8WEB)/input.html: demo/input/index.html
	@mkdir -p $(dir $@)
	sed -e 's/dalvikvm/artvm8/g' -e 's/Dalvik/ART (Android 8.1)/g' $< > $@
$(G8WEB)/report.js: shim/report.js
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: fw8-graphics-web
fw8-graphics-web: $(G8WEB)/artvm8.js $(G8WEB)/index.html $(G8WEB)/input.html $(G8WEB)/report.js
endif
