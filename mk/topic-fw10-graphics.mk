# Topic "fw10-graphics": the graphics, surfaces and input of the Android 10
# framework (android-10.0.0_r47, API 29) on ART 10 in WebAssembly
# (docs/topics/fw10-graphics.md), the way topic fw8-graphics did it for 8.1
# (mk/topic-fw8-graphics.mk):
#
#   Skia (external/skia, Android 10) drawing in software and, for hwui's
#   Skia pipeline, with its GL backend on WebGL2 (IS_WEBGL); freetype,
#   harfbuzz_ng, libpng, libjpeg-turbo, giflib, libwebp and topic art10's
#   expat and ICU 63;
#   minikin 10 (text layout) with Roboto and Noto (external/roboto-fonts,
#   external/noto-fonts);
#   hwui 10 (frameworks/base/libs/hwui: SkiaCanvas, RenderNode, the
#   SkiaOpenGL pipeline: Skia's GL on WebGL2 through hwui's EGL of topic
#   fw6-hwui);
#   libui, libgui 10 (BufferQueue, Surface, SurfaceComposerClient's
#   transactions), libnativewindow (ANativeWindow_*, AHardwareBuffer) with
#   SurfaceFlinger in the process (shim/fw10-graphics) and a software gralloc
#   (IAllocator/IMapper 2.0 in process); libinput 10;
#   the android.graphics / android.view / android.text JNI of
#   libandroid_runtime 10 (libandroid_runtime_graphics10.a), registered by
#   topic fw10-core's gRegJNI (shim/fw10/register_fw10.cc).
#
#   make fw10-graphics-libs    # the libraries
#   make art10                 # artvm10 with all of it
#   make fw10-graphics-web     # artvm10 with the display on the page's canvas
#
# Built on topic fw10-core (mk/topic-fw10-core.mk: FW10_INCLUDES, F10_LIBS,
# the libandroid_runtime 10 frame, the host hidl-gen/aprotoc) and topic
# art10 (ICU 63, expat). Extra AOSP projects:
# patches/aosp10/fw10-graphics.projects. Objects go to build/fw10g/obj,
# libraries to build/fw10g/lib (variables: G10*).

G10     := $(BUILD)/fw10g
G10OBJ  := $(G10)/obj
G10LIB  := $(G10)/lib
g10obj   = $(patsubst %,$(G10OBJ)/%.o,$(1))
G10EXT  := $(A10)/external

G10_WARN := -Wno-unused-value -Wno-macro-redefined -Wno-shift-negative-value -Wno-unused-label \
            -Wno-implicit-fallthrough -Wno-pessimizing-move -Wno-overloaded-virtual \
            -Wno-dangling-else -Wno-parentheses -Wno-tautological-compare \
            -Wno-self-assign -Wno-char-subscripts -Wno-string-plus-int \
            -Wno-unused-command-line-argument -Wno-deprecated-builtins -Wno-unknown-attributes \
            -Wno-unqualified-std-cast-call -Wno-bitwise-instead-of-logical -Wno-unused-result \
            -Wno-missing-braces -Wno-int-in-bool-context -Wno-return-type-c-linkage \
            -Wno-invalid-specialization -Wno-deprecated-anon-enum-enum-conversion \
            -Wno-deprecated-enum-enum-conversion -Wno-gnu-static-float-init \
            -Wno-unknown-pragmas -Wno-reorder-ctor -Wno-range-loop-construct \
            -Wno-deprecated-volatile -Wno-unused-but-set-parameter
G10_CFLAGS   := $(F10_CFLAGS) $(G10_WARN)
G10_CXXFLAGS := $(F10_CXXFLAGS) $(G10_WARN)

$(G10OBJ)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(EMCC) $(G10_CFLAGS) $(G10FLAGS) -c $< -o $@
# C++ objects wait for the HIDL headers hidl-gen makes (below and topic
# fw10-core): nothing else orders them.
$(G10OBJ)/%.cpp.o: %.cpp | $(G10)/gen/hidl-cpp/hidl.stamp $(F10_HIDL_GENCPP)/hidl.stamp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(G10_CXXFLAGS) $(G10FLAGS) -c $< -o $@
$(G10OBJ)/%.cc.o: %.cc | $(G10)/gen/hidl-cpp/hidl.stamp $(F10_HIDL_GENCPP)/hidl.stamp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(G10_CXXFLAGS) $(G10FLAGS) -c $< -o $@
-include $(shell find $(G10OBJ) -name '*.d' 2>/dev/null)

# (make does not notice changed flags: after changing any of the flags
# below, remove the objects concerned, e.g. rm -rf build/fw10g/obj/...)
define g10_archive
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
endef
g10bp = $(shell tools/bp_srcs.py $(1) $(2) --prefix $(dir $(1)) $(3) 2>/dev/null)

# --- freetype ---------------------------------------------------------------------
G10_FT := $(G10EXT)/freetype
$(G10OBJ)/$(G10_FT)/%: G10FLAGS := -I$(G10_FT)/include -DFT2_BUILD_LIBRARY -DDARWIN_NO_CARBON \
    -I$(G10EXT)/libpng -sUSE_ZLIB=1
# (Android.bp has two modules named libft2: the sources are the first one's.)
G10_FT_SRCS := $(addprefix $(G10_FT)/,$(shell sed -n '1,/^}/p' $(G10_FT)/Android.bp | grep -o '"src/[^"]*\.c"' | tr -d '"'))
$(G10LIB)/libft2.a: $(call g10obj,$(G10_FT_SRCS))
	$(g10_archive)

# --- libpng 1.6 (portable code, no NEON/SSE filters) ---------------------------------
G10_PNG := $(G10EXT)/libpng
$(G10OBJ)/$(G10_PNG)/%: G10FLAGS := -sUSE_ZLIB=1 -DPNG_ARM_NEON_OPT=0 -DPNG_INTEL_SSE_OPT=0 -std=gnu89
$(G10LIB)/libpng.a: $(call g10obj,$(call g10bp,$(G10_PNG)/Android.bp,libpng))
	$(g10_archive)

# --- libjpeg-turbo (portable: jsimd_none.c of the MIPS list) -------------------------
G10_JPEG := $(G10EXT)/libjpeg-turbo
$(G10OBJ)/$(G10_JPEG)/%: G10FLAGS := -fstrict-aliasing -I$(G10_JPEG)
$(G10LIB)/libjpeg.a: $(call g10obj,$(call g10bp,$(G10_JPEG)/Android.bp,libjpeg,--arch mips))
	$(g10_archive)

# --- giflib -------------------------------------------------------------------------
G10_GIF := $(G10EXT)/giflib
$(G10OBJ)/$(G10_GIF)/%: G10FLAGS := -DHAVE_CONFIG_H -I$(G10_GIF)
$(G10LIB)/libgif.a: $(call g10obj,$(call g10bp,$(G10_GIF)/Android.bp,libgif))
	$(g10_archive)

# --- libwebp: decode and encode in one (portable code, no cpu-features) --------------
G10_WEBP := $(G10EXT)/webp
G10_WEBP_SRCS := $(filter-out %/cpu-features.c %_neon.c %_sse2.c %_sse41.c %_avx2.c %_mips32.c \
                   %_mips_dsp_r2.c %_msa.c, \
                   $(call g10bp,$(G10_WEBP)/Android.bp,libwebp-decode) \
                   $(call g10bp,$(G10_WEBP)/Android.bp,libwebp-encode))
$(G10OBJ)/$(G10_WEBP)/%: G10FLAGS := -DWEBP_SWAP_16BIT_CSP -I$(G10_WEBP)/src -I$(G10_WEBP)
$(G10LIB)/libwebp.a: $(call g10obj,$(sort $(G10_WEBP_SRCS)))
	$(g10_archive)

# --- harfbuzz_ng (with topic art10's ICU 63) -------------------------------------------
# harfbuzz checks `!this` in member functions; clang drops such checks
# (see docs/topics/graphics.md).
G10_HB := $(G10EXT)/harfbuzz_ng
$(G10OBJ)/$(G10_HB)/%: G10FLAGS := -I$(G10_HB)/src $(ICU63_INCLUDES) -DHB_NO_MT -DHAVE_OT -DHAVE_ICU \
    -DHAVE_ICU_BUILTIN -DHAVE_INTEL_ATOMIC_PRIMITIVES -fno-delete-null-pointer-checks \
    -DU_USING_ICU_NAMESPACE=0
$(G10LIB)/libharfbuzz_ng.a: $(call g10obj,$(call g10bp,$(G10_HB)/Android.bp,libharfbuzz_ng))
	$(g10_archive)

# --- Skia (Android 10) ------------------------------------------------------------------
# Android.bp's list for the device (its GPU code is the device's), the
# portable SkOpts (no arch list), without: Vulkan (src/gpu/vk and the
# Vulkan memory allocator), PDF (needs sfntly), XPS, RAW (dng_sdk) and HEIF
# (libheif) codecs. SkUserConfig.h: no RAW, HEIF, PDF and Vulkan under
# ARC_WASM (patches/aosp10/fw10-graphics.patch). Its GL backend draws hwui's
# Skia pipeline on WebGL2: IS_WEBGL (GrGLCaps: WebGL's limits), and its GL
# functions are hwui's (shim/fw10-graphics/hwui_skia_gl.cc).
G10_SKIA := $(G10EXT)/skia
G10_SKIA_SRCS := $(filter-out src/gpu/vk/% src/pdf/% src/xps/% src/codec/SkRaw% src/codec/SkHeif% \
                   third_party/vulkanmemoryallocator/% src/gpu/gl/android/% src/gpu/gl/egl/%, \
                   $(shell tools/bp_srcs.py $(G10_SKIA)/Android.bp libskia --target android 2>/dev/null))
# (SkJpegInfo of src/pdf: the JPEG codec uses it too)
G10_SKIA_SRCS := $(addprefix $(G10_SKIA)/,$(sort $(G10_SKIA_SRCS) src/pdf/SkJpegInfo.cpp))
G10_SKIA_INCDIRS := $(addprefix $(G10_SKIA)/include/,android atlastext c codec config config/android core \
                      docs effects encode gpu pathops ports svg utils private) \
                    $(G10_SKIA)/third_party/skcms
G10_SKIA_INCLUDES := $(addprefix -I,$(G10_SKIA_INCDIRS))
# The defines every user of the Skia headers needs to agree on (on a device
# SkPreConfig.h derives SK_BUILD_FOR_ANDROID from __ANDROID__).
G10_SKIA_DEFINES := -DSK_RELEASE -DSK_BUILD_FOR_ANDROID -DSK_BUILD_FOR_ANDROID_FRAMEWORK
# No -fexceptions (topic heavy-apps-mem): Skia catches nothing (only the
# RAW codec, left out) and artvm10 links without exception catching, so a
# throw aborts either way; with it every call that may unwind went through a
# JS invoke_* trampoline (~18000 call sites, 140 trampolines; Opera's main
# thread ~90000 calls in 20 s), as Skia 8.1 never did.
$(G10OBJ)/$(G10_SKIA)/%: G10FLAGS := $(G10_SKIA_INCLUDES) $(G10_SKIA_DEFINES) -DSKIA_IMPLEMENTATION=1 \
    -DIS_WEBGL=1 -DSK_PRINT_CODEC_MESSAGES \
    $(addprefix -I$(G10_SKIA)/,src/c src/codec src/core src/effects src/fonts src/gpu src/image \
      src/images src/lazy src/opts src/pathops src/ports src/sfnt src/shaders src/shaders/gradients \
      src/sksl src/utils src/xml src/pdf third_party/gif third_party/etc1) \
    -I$(G10_JPEG) -I$(G10_PNG) -I$(G10_GIF) -I$(G10_WEBP)/src -I$(A10_EXPAT)/lib -I$(G10_FT)/include \
    -I$(A10)/frameworks/native/libs/nativewindow/include -I$(A10)/frameworks/native/libs/arect/include \
    -I$(A10)/frameworks/native/libs/nativebase/include -I$(A10)/frameworks/native/opengl/include \
    $(FW10_INCLUDES) -DATRACE_TAG=ATRACE_TAG_VIEW
$(G10LIB)/libskia.a: $(call g10obj,$(G10_SKIA_SRCS))
	$(g10_archive)

G10_SKIA_LIBS := $(G10LIB)/libskia.a $(G10LIB)/libharfbuzz_ng.a $(G10LIB)/libft2.a $(G10LIB)/libpng.a \
                 $(G10LIB)/libjpeg.a $(G10LIB)/libgif.a $(G10LIB)/libwebp.a

.PHONY: fw10-skia
fw10-skia: $(G10_SKIA_LIBS)

# --- the graphics HIDL interfaces (C++ from hidl-gen 10) ---------------------------------
# libui 10 allocates and maps buffers through IAllocator/IMapper (Gralloc2.cpp,
# Gralloc3.cpp), libgui converts producers to and from HIDL
# (bufferqueue@1.0/2.0, hidl.token@1.0). Generated with topic fw10-core's
# hidl-gen into build/fw10g/gen.
G10_HIDL_GEN := $(G10)/gen/hidl-cpp
G10_HIDL_PKGS := android.hardware.graphics.common@1.0 android.hardware.graphics.common@1.1 \
                 android.hardware.graphics.common@1.2 \
                 android.hardware.graphics.allocator@2.0 android.hardware.graphics.allocator@3.0 \
                 android.hardware.graphics.mapper@2.0 android.hardware.graphics.mapper@2.1 \
                 android.hardware.graphics.mapper@3.0 \
                 android.hardware.graphics.bufferqueue@1.0 android.hardware.graphics.bufferqueue@2.0 \
                 android.hardware.media@1.0 android.hidl.token@1.0
G10_HIDL_ROOTS := -randroid.hardware:$(A10)/hardware/interfaces \
                  -randroid.hidl:$(A10)/system/libhidl/transport
G10_HIDL_SRCS := $(addprefix $(G10_HIDL_GEN)/android/,hardware/graphics/common/1.0/types.cpp \
                   hardware/graphics/common/1.1/types.cpp hardware/graphics/common/1.2/types.cpp \
                   hardware/graphics/allocator/2.0/AllocatorAll.cpp \
                   hardware/graphics/allocator/3.0/AllocatorAll.cpp \
                   hardware/graphics/mapper/2.0/MapperAll.cpp hardware/graphics/mapper/2.0/types.cpp \
                   hardware/graphics/mapper/2.1/MapperAll.cpp \
                   hardware/graphics/mapper/3.0/MapperAll.cpp hardware/graphics/mapper/3.0/types.cpp \
                   hardware/graphics/bufferqueue/1.0/GraphicBufferProducerAll.cpp \
                   hardware/graphics/bufferqueue/1.0/ProducerListenerAll.cpp \
                   hardware/graphics/bufferqueue/2.0/GraphicBufferProducerAll.cpp \
                   hardware/graphics/bufferqueue/2.0/ProducerListenerAll.cpp \
                   hardware/graphics/bufferqueue/2.0/types.cpp \
                   hardware/media/1.0/types.cpp hidl/token/1.0/TokenManagerAll.cpp)
$(G10_HIDL_GEN)/hidl.stamp: $(F10H)/hidl-gen mk/topic-fw10-graphics.mk
	rm -rf $(G10_HIDL_GEN) && mkdir -p $(G10_HIDL_GEN)
	for p in $(G10_HIDL_PKGS); do \
	  for l in c++-headers c++-sources; do \
	    $(F10H)/hidl-gen -o $(G10_HIDL_GEN) -L$$l $(G10_HIDL_ROOTS) $$p || exit 1; \
	  done; \
	done
	@touch $@
$(G10_HIDL_SRCS): $(G10_HIDL_GEN)/hidl.stamp ;

# --- include paths of the framework's native graphics code ----------------------------
# FW10_INCLUDES (topic fw10-core) plus: Android's EGL native types
# (shim/fw8-graphics/include/EGL, the same for 10), the GL, HAL and HIDL
# headers, Skia, minikin. libui without BufferHub (LIBUI_IN_VNDK, as the
# vendor libui: no android.frameworks.bufferhub, no pdx), for everything
# that includes GraphicBuffer.h.
G10_FWB := $(A10)/frameworks/base
G10_FWN := $(A10)/frameworks/native
G10_NL := $(G10_FWN)/libs
G10_MINIKIN := $(A10)/frameworks/minikin
G10_HWUI := $(G10_FWB)/libs/hwui
G10_JNI := $(G10_FWB)/core/jni
# (G10_FW_INCLUDES_BASE: without the force-included compat header, for code
# that cannot have <cutils/trace.h>'s C++ <stdatomic.h> first: shim/input.)
G10_FW_INCLUDES_BASE := -Ishim/fw10-graphics/include -Ishim/fw8-graphics/include $(FW10_INCLUDES) \
    $(addprefix -I$(G10_NL)/,ui/include gui/include input/include nativewindow/include \
      nativewindow/include-private arect/include nativebase/include math/include) \
    -I$(G10_FWN)/opengl/include -I$(A10)/hardware/libhardware/include \
    -I$(A10)/system/core/libsync/include -I$(A10)/system/core/libgrallocusage/include \
    -I$(A10)/system/libhidl/transport/token/1.0/utils/include \
    -I$(G10_HIDL_GEN) -I$(F10_HIDL_GENCPP) \
    $(G10_SKIA_INCLUDES) $(G10_SKIA_DEFINES) $(addprefix -I$(G10_SKIA)/src/,core effects image utils gpu shaders) \
    -I$(G10_HB)/src -I$(G10_FT)/include -I$(G10_MINIKIN)/include $(ICU63_INCLUDES) -DU_USING_ICU_NAMESPACE=0 \
    -DEGL_EGLEXT_PROTOTYPES -DGL_GLEXT_PROTOTYPES -DLIBUI_IN_VNDK \
    -D_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION \
    -I$(A10)/system/core/libvndksupport/include -I$(GT)/googletest/include
G10_FW_INCLUDES := $(G10_FW_INCLUDES_BASE) -include shim/fw10-graphics/include/fw10_graphics_compat.h

# --- minikin 10 -------------------------------------------------------------------------
G10_MINIKIN_SRCS := $(call g10bp,$(G10_MINIKIN)/libs/minikin/Android.bp,libminikin,--target android)
# (not -I libs/minikin: its Locale.h would be <locale.h> on the Mac's
# case-insensitive file system; its sources include their headers by "".)
$(G10OBJ)/$(G10_MINIKIN)/%: G10FLAGS := $(G10_FW_INCLUDES) -DLOG_TAG=\"Minikin\"
$(G10LIB)/libminikin.a: $(call g10obj,$(G10_MINIKIN_SRCS))
	$(g10_archive)

# --- libui, libgui, libnativewindow, libinput (frameworks/native) ----------------------------
# libui's buffers come from the IAllocator/IMapper 2.0 of AOSP's passthrough
# HAL (hardware/interfaces/graphics/{allocator,mapper}/2.0/default and its
# utils) over the software gralloc HAL of topic fw6-graphics
# (shim/fw6-graphics/gralloc_sw.cc): Gralloc2.cpp takes them directly under
# ARC_WASM (fw10-graphics.patch); the 3.0 HALs are not there (getService
# fails, as on a device with a 2.0 gralloc). No BufferHub (libui:
# LIBUI_IN_VNDK, libgui: NO_BUFFERHUB).
G10_UI_SRCS := $(filter-out %/BufferHubBuffer.cpp %/BufferHubEventFd.cpp %/BufferHubMetadata.cpp, \
                 $(call g10bp,$(G10_NL)/ui/Android.bp,libui,--target android))
G10_GUI_SRCS := $(filter-out %/BufferHubConsumer.cpp %/BufferHubProducer.cpp, \
                  $(call g10bp,$(G10_NL)/gui/Android.bp,libgui,--target android))
G10_NW_SRCS := $(addprefix $(G10_NL)/nativewindow/,AHardwareBuffer.cpp ANativeWindow.cpp) \
               $(A10)/system/core/libgrallocusage/GrallocUsageConversion.cpp
G10_INPUT_SRCS := $(call g10bp,$(G10_NL)/input/Android.bp,libinput,--target android)
G10_HAL_DIR := $(A10)/hardware/interfaces/graphics
G10_GRALLOC_SRCS := $(G10_HAL_DIR)/mapper/2.0/default/passthrough.cpp \
                    $(G10_HAL_DIR)/allocator/2.0/default/passthrough.cpp \
                    shim/fw6-graphics/gralloc_sw.cc
$(G10OBJ)/$(G10_NL)/%: G10FLAGS := $(G10_FW_INCLUDES) -DLOG_TAG=\"libgui\" -DNO_BUFFERHUB \
    -DDEBUG_ONLY_CODE=0
$(G10OBJ)/$(G10_NL)/ui/%: G10FLAGS := $(G10_FW_INCLUDES) -DLOG_TAG=\"libui\"
$(G10OBJ)/$(G10_NL)/input/%: G10FLAGS := $(G10_FW_INCLUDES) -DLOG_TAG=\"libinput\"
$(G10OBJ)/$(G10_NL)/nativewindow/%: G10FLAGS := $(G10_FW_INCLUDES) -DLOG_TAG=\"libnativewindow\"
$(G10OBJ)/$(A10)/system/core/libgrallocusage/%: G10FLAGS := $(G10_FW_INCLUDES)
$(G10OBJ)/$(G10_HAL_DIR)/%: G10FLAGS := $(G10_FW_INCLUDES) \
    $(addprefix -I$(G10_HAL_DIR)/,mapper/2.0/utils/hal/include mapper/2.0/utils/passthrough/include \
      allocator/2.0/utils/hal/include allocator/2.0/utils/passthrough/include) \
    -DLOG_TAG=\"gralloc-hal\"
$(G10OBJ)/shim/fw6-graphics/gralloc_sw.cc.o: G10FLAGS := $(G10_FW_INCLUDES)
$(G10OBJ)/$(G10_HIDL_GEN)/%: G10FLAGS := $(G10_FW_INCLUDES)
$(call g10obj,$(G10_UI_SRCS) $(G10_GUI_SRCS) $(G10_NW_SRCS) $(G10_GRALLOC_SRCS) $(G10_HIDL_SRCS)): \
    $(G10_HIDL_GEN)/hidl.stamp
$(G10LIB)/libui10.a: $(call g10obj,$(G10_UI_SRCS) $(G10_GRALLOC_SRCS))
	$(g10_archive)
$(G10LIB)/libgui10.a: $(call g10obj,$(G10_GUI_SRCS) $(G10_NW_SRCS) \
                        $(A10)/system/libhidl/transport/token/1.0/utils/HybridInterface.cpp)
	$(g10_archive)
$(G10OBJ)/$(A10)/system/libhidl/transport/token/%: G10FLAGS := $(G10_FW_INCLUDES)
$(G10LIB)/libinput10.a: $(call g10obj,$(G10_INPUT_SRCS))
	$(g10_archive)
$(G10LIB)/libgraphics_hidl10.a: $(call g10obj,$(G10_HIDL_SRCS))
	$(g10_archive)

.PHONY: fw10-gui
fw10-gui: $(G10LIB)/libminikin.a $(G10LIB)/libui10.a $(G10LIB)/libgui10.a $(G10LIB)/libinput10.a \
          $(G10LIB)/libgraphics_hidl10.a

# --- protobuf-lite (C++), the protos of hwui ---------------------------------------------
# hwui 10 keeps frame statistics (libs/hwui/protos/graphicsstats.proto) in
# protobuf-lite messages; the C++ comes from topic fw10-core's host aprotoc.
G10_PB := $(A10)/external/protobuf
G10_PB_WASM := -include shim/fw8-graphics/include/protobuf_wasm.h
$(G10OBJ)/$(G10_PB)/%: G10FLAGS := -I$(G10_PB)/android -I$(G10_PB)/src -DGOOGLE_PROTOBUF_NO_RTTI \
    -DHAVE_PTHREAD -Wno-sign-promo $(G10_PB_WASM)
$(G10LIB)/libprotobuf-cpp-lite.a: $(call g10obj,$(call g10bp,$(G10_PB)/Android.bp,libprotobuf-cpp-lite))
	$(g10_archive)
G10_PROTO_GEN := $(G10)/gen/proto
G10_PROTO_SRCS := $(G10_PROTO_GEN)/protos/graphicsstats.pb.cc
$(G10_PROTO_GEN)/protos/graphicsstats.pb.cc: $(G10_HWUI)/protos/graphicsstats.proto $(F10H)/aprotoc
	@mkdir -p $(dir $@)
	$(F10H)/aprotoc --cpp_out=$(G10_PROTO_GEN) -I$(G10_HWUI) $<
G10_PROTO_FLAGS := -I$(G10_PROTO_GEN) -I$(G10_PB)/src -DGOOGLE_PROTOBUF_NO_RTTI $(G10_PB_WASM)
$(G10OBJ)/$(G10_PROTO_GEN)/%: G10FLAGS := $(G10_PROTO_FLAGS)

# --- hwui 10 ----------------------------------------------------------------------------------
# libhwui (Android.bp). SkiaCanvas is the software Canvas; RenderNode/
# SkiaDisplayList are recorded by every View; the SkiaOpenGL pipeline draws
# with Skia's GL on WebGL2: hwui's GL calls go to Emscripten's WebGL2
# functions and its EGL calls to shim/fw6-hwui/hwui_egl.cc (the renames of
# topic fw6-hwui), and Skia gets the same functions
# (shim/fw10-graphics/hwui_skia_gl.cc). The Vulkan pipeline compiles (Skia
# without SK_VULKAN still has its vk headers) and is never chosen
# (ro.hwui.use_vulkan is not set); the Vulkan loader it links against is
# shim/fw10-graphics/hwui_vulkan10_stub.cc ("no Vulkan"). Hardware bitmaps,
# readback and TextureView's images read the buffers' memory (the software
# gralloc) instead of EGLImages (fw10-graphics.patch).
# HWUIProperties.sysprop: shim/fw10-graphics/hwui_sysprop.cc; statslog.h:
# shim/fw10-graphics/include (nothing logged); the shader cache's BlobCache
# from libEGL (frameworks/native/opengl/libs/EGL), SHA-256 from topic
# art10's BoringSSL.
G10_HWUI_SRCS := $(filter-out %.proto %.sysprop,$(call g10bp,$(G10_HWUI)/Android.bp,libhwui)) \
                 $(G10_FWN)/opengl/libs/EGL/BlobCache.cpp $(G10_FWN)/opengl/libs/EGL/FileBlobCache.cpp \
                 shim/fw10-graphics/hwui_sysprop.cc shim/fw10-graphics/hwui_vulkan10_stub.cc
G10_HWUI_GL := -Ishim/fw6-hwui -include shim/fw6-hwui/hwui_gl_wasm.h
G10_HWUI_FLAGS := $(G10_FW_INCLUDES) $(G10_HWUI_GL) -I$(G10_HWUI) $(G10_PROTO_FLAGS) \
    -I$(G10_SKIA)/include/private -I$(G10_FWN)/vulkan/include -I$(G10_SKIA)/include/third_party/vulkan \
    -DVK_USE_PLATFORM_ANDROID_KHR -I$(G10_FWN)/opengl/libs/EGL $(BSSL10_INCLUDES) \
    -DATRACE_TAG=ATRACE_TAG_VIEW -DLOG_TAG=\"OpenGLRenderer\" -fno-rtti
$(G10OBJ)/$(G10_HWUI)/% $(G10OBJ)/$(G10_FWN)/opengl/% $(G10OBJ)/shim/fw10-graphics/hwui_%: \
    G10FLAGS := $(G10_HWUI_FLAGS)
$(call g10obj,$(G10_HWUI_SRCS)): $(G10_PROTO_SRCS) $(G10_HIDL_GEN)/hidl.stamp
$(G10LIB)/libhwui10.a: $(call g10obj,$(G10_HWUI_SRCS) $(G10_PROTO_SRCS))
	$(g10_archive)

# hwui's EGL on WebGL2 (shim/fw6-hwui, built for 10: Skia sees WebGL2's GL
# version, ARC_HWUI_SKIA_GL) and Skia's GL interface on it.
$(G10)/gen/hwui_skia_gl_procs.h: shim/fw6-hwui/hwui_gl_names.h
	@mkdir -p $(dir $@)
	sed -n 's/^#define \(gl[A-Za-z0-9_]*\) .*/M(\1)/p' $< | \
	    grep -v -e glGetBufferPointerv -e glFlushMappedBufferRange > $@
$(G10OBJ)/shim/fw6-hwui/%: G10FLAGS := $(G10_FW_INCLUDES) $(G10_HWUI_GL) -DARC_HWUI_SKIA_GL=1
$(G10OBJ)/shim/fw10-graphics/hwui_skia_gl.cc.o: G10FLAGS := $(G10_FW_INCLUDES) $(G10_HWUI_GL) \
    -I$(G10)/gen -I$(G10_SKIA)/include/private
$(G10OBJ)/shim/fw10-graphics/hwui_skia_gl.cc.o: $(G10)/gen/hwui_skia_gl_procs.h
$(G10LIB)/libfw10_hwui_egl.a: $(call g10obj,shim/fw6-hwui/hwui_egl.cc shim/fw6-hwui/hwui_gl.cc \
                                shim/fw10-graphics/hwui_skia_gl.cc)
	$(g10_archive)

.PHONY: fw10-hwui
fw10-hwui: $(G10LIB)/libhwui10.a $(G10LIB)/libprotobuf-cpp-lite.a $(G10LIB)/libfw10_hwui_egl.a

# --- SurfaceFlinger in the process, GL extension stubs, the display -----------------------
G10_SF_SRCS := shim/fw10-graphics/surface_flinger10.cc shim/fw10-graphics/gl_extras10.cc
$(G10OBJ)/shim/fw10-graphics/surface_flinger10.cc.o $(G10OBJ)/shim/fw10-graphics/gl_extras10.cc.o: \
    G10FLAGS := $(G10_FW_INCLUDES)
$(G10LIB)/libfw10_surfaceflinger.a: $(call g10obj,$(G10_SF_SRCS))
	$(g10_archive)
# The display backend (shim/graphics/framebuffer.h): memory, for artvm10 in
# Node and art10-web; the page build (fw10-graphics-web) links the gralloc one.
$(G10OBJ)/shim/graphics/%: G10FLAGS :=
G10_FB_HEADLESS := $(call g10obj,shim/graphics/framebuffer_headless.cc)

# --- the graphics part of libandroid_runtime 10 ------------------------------------------
# core/jni of AOSP 10 (the register_* functions gRegJNI of topic fw10-core
# refers to weakly): android.graphics (without pdf: no PDF backend or
# pdfium), fonts and text (MeasuredText, LineBreaker), drawables,
# ImageDecoder, android.view surfaces/rendering/input, android.text
# Hyphenator, PathParser, HardwareBuffer, ColorSpace.
G10_GJNI_SRCS := $(addprefix $(G10_JNI)/android/graphics/,$(addsuffix .cpp, \
    AnimatedImageDrawable Bitmap BitmapFactory BitmapRegionDecoder ByteBufferStreamAdaptor Camera \
    CanvasProperty ColorFilter CreateJavaOutputStreamAdaptor FontFamily FontUtils GIFMovie \
    GraphicBuffer Graphics ImageDecoder Interpolator MaskFilter Matrix Movie MovieImpl NinePatch \
    NinePatchPeeker Paint PaintFilter Path PathEffect PathMeasure Picture Region Shader \
    SurfaceTexture Typeface Utils YuvToJpegEncoder fonts/Font fonts/FontFamily text/LineBreaker \
    text/MeasuredText)) \
  $(addprefix $(G10_JNI)/,$(addsuffix .cpp, \
    android_graphics_Canvas android_graphics_ColorSpace android_graphics_Picture \
    android_graphics_drawable_AnimatedVectorDrawable android_graphics_drawable_VectorDrawable \
    android_view_CompositionSamplingListener android_view_DisplayEventReceiver \
    android_view_DisplayListCanvas android_view_TextureLayer \
    android_view_InputChannel android_view_InputDevice android_view_InputEventReceiver \
    android_view_InputEventSender android_view_InputQueue android_view_KeyCharacterMap \
    android_view_KeyEvent android_view_MotionEvent android_view_PointerIcon android_view_RenderNode \
    android_view_RenderNodeAnimator android_view_Surface android_view_SurfaceControl \
    android_view_SurfaceSession android_view_TextureView android_view_ThreadedRenderer \
    android_view_VelocityTracker android_text_Hyphenator android_util_PathParser \
    android_hardware_HardwareBuffer android_hardware_display_DisplayViewport \
    android_hardware_input_InputApplicationHandle android_hardware_input_InputWindowHandle \
    com_android_internal_view_animation_NativeInterpolatorFactoryHelper)) \
  $(G10_FWB)/libs/androidfw/DisplayEventDispatcher.cpp \
  shim/fw10-graphics/register_fw10_graphics.cc
# (no RTTI, as the platform builds it: FrameMetricsObserver has a virtual
# function declared and never defined, so no type_info anywhere)
G10_GJNI_BASE := $(G10_FW_INCLUDES) -fno-rtti -I$(G10_JNI)/android/graphics -I$(G10_HWUI) \
    $(G10_PROTO_FLAGS) -I$(G10_SKIA)/include/private -I$(G10_FWN)/vulkan/include \
    -I$(G10_SKIA)/include/third_party/vulkan -DVK_USE_PLATFORM_ANDROID_KHR \
    -I$(G10_JNI) -I$(G10_JNI)/include -I$(G10_JPEG) -I$(A10_EXPAT)/lib \
    -I$(G10_FWN)/opengl/libs -I$(G10_FWN)/opengl/libs/EGL/include \
    -I$(A10)/libcore/include -I$(G10_SKIA)/src/images -I$(G10_SKIA)/src/codec -I$(G10_GIF) \
    -I$(G10_SKIA)/include/utils -DLOG_TAG=\"GraphicsJNI\"
G10_GJNI_FLAGS := $(G10_HWUI_GL) $(G10_GJNI_BASE)
$(G10OBJ)/$(G10_JNI)/% $(G10OBJ)/shim/fw10-graphics/register_fw10_graphics.cc.o \
$(G10OBJ)/$(G10_FWB)/libs/androidfw/DisplayEventDispatcher.cpp.o: G10FLAGS := $(G10_GJNI_FLAGS)
# SurfaceTexture serves the apps' GL (EGL14 on graphics_translation in the
# page, GLConsumer of libgui): not on hwui's renamed GL/EGL.
$(G10OBJ)/$(G10_JNI)/android/graphics/SurfaceTexture.cpp.o: G10FLAGS := $(G10_GJNI_BASE)
$(call g10obj,$(G10_GJNI_SRCS)): $(G10_PROTO_SRCS) $(G10_HIDL_GEN)/hidl.stamp
$(G10LIB)/libandroid_runtime_graphics10.a: $(call g10obj,$(G10_GJNI_SRCS))
	$(g10_archive)

G10_LIBS := $(G10LIB)/libhwui10.a $(G10LIB)/libfw10_hwui_egl.a $(G10LIB)/libminikin.a \
            $(G10LIB)/libfw10_surfaceflinger.a $(G10LIB)/libgui10.a $(G10LIB)/libui10.a \
            $(G10LIB)/libinput10.a $(G10LIB)/libgraphics_hidl10.a $(G10LIB)/libprotobuf-cpp-lite.a \
            $(G10_SKIA_LIBS)

.PHONY: fw10-graphics-libs
fw10-graphics-libs: $(G10LIB)/libandroid_runtime_graphics10.a $(G10_LIBS) $(G10_FB_HEADLESS)

# --- the test programs' natives: libarcwasm_graphics10.so --------------------------------
# arcwasm.graphics.FramebufferSurface (reads the display back) and topic
# input's browser input dispatcher (shim/input, built against libinput 10:
# ARC_WASM_FW6 for what 6.0 added, ARC_WASM_FW8 for 8.x's displayId,
# ARC_WASM_FW10 for 9/10's), for the Java of framework/java-arcwasm10; it
# adds itself to artvm10's table of static JNI libraries. Linked whole
# (only its constructor refers to it).
G10_TESTJNI_SRCS := shim/fw10-graphics/arcwasm_graphics10_jni.cc shim/input/browser_input.cc \
                    shim/input/register_input.cc
$(G10OBJ)/shim/input/%: G10FLAGS := $(G10_FW_INCLUDES_BASE) -DARC_WASM_FW6=1 -DARC_WASM_FW8=1 \
    -DARC_WASM_FW10=1 -I$(G10_JNI) -I$(G10_JNI)/include -Ishim/input -DLOG_TAG=\"BrowserInput\"
$(G10OBJ)/shim/fw10-graphics/arcwasm_graphics10_jni.cc.o: G10FLAGS := $(G10_FW_INCLUDES_BASE) -Ishim/input
$(G10LIB)/libarcwasm_graphics10.a: $(call g10obj,$(G10_TESTJNI_SRCS))
	$(g10_archive)

# --- fonts and key maps for /system ---------------------------------------------------------
# What framework/fonts10/fonts.xml (/system/etc/fonts.xml) names: Roboto,
# Roboto Condensed, Noto Serif, Droid Sans Mono; the fallbacks Noto Sans
# Symbols, Noto Color Emoji (9 MB) and Droid Sans Fallback (CJK, 3.4 MB).
G10_NOTO := $(G10EXT)/noto-fonts
G10_FONTS := $(wildcard $(G10EXT)/roboto-fonts/Roboto*.ttf) \
             $(addprefix $(G10_NOTO)/other/,NotoSerif-Regular.ttf NotoSerif-Bold.ttf NotoSerif-Italic.ttf \
               NotoSerif-BoldItalic.ttf NotoSansSymbols-Regular-Subsetted.ttf \
               NotoSansSymbols-Regular-Subsetted2.ttf) \
             $(G10_NOTO)/emoji/NotoColorEmoji.ttf \
             $(G10_FWB)/data/fonts/DroidSansMono.ttf $(G10_FWB)/data/fonts/DroidSansFallback.ttf
G10_FONTS_ADD := $(foreach f,$(G10_FONTS),--add $(f):system/fonts/$(notdir $(f))) \
                 --add framework/fonts10/fonts.xml:system/etc/fonts.xml
G10_KEYBOARDS := $(G10_FWB)/data/keyboards
G10_KEYS_ADD := --add $(G10_KEYBOARDS)/Generic.kl:system/usr/keylayout/Generic.kl \
                --add $(G10_KEYBOARDS)/Generic.kcm:system/usr/keychars/Generic.kcm \
                --add $(G10_KEYBOARDS)/Virtual.kcm:system/usr/keychars/Virtual.kcm

# --- into artvm10 (topics art10 and fw10-core) --------------------------------------------------
# The JNI libraries are linked whole (gRegJNI refers to their register_*
# functions weakly; libarcwasm_graphics10 registers itself). The display:
# memory (framebuffer_headless.cc) in artvm10 and art10-web; the browser's
# canvas (gralloc/WebGL) is the page build's (fw10-graphics-web, below).
# Emscripten's GL as hwui needs it: WebGL2.
G10_GL_LDFLAGS := -sMAX_WEBGL_VERSION=2
ART10_LIBS += -Wl,--whole-archive $(G10LIB)/libandroid_runtime_graphics10.a \
    $(G10LIB)/libarcwasm_graphics10.a -Wl,--no-whole-archive $(G10_LIBS) $(G10_FB_HEADLESS)
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js: $(G10LIB)/libandroid_runtime_graphics10.a \
    $(G10LIB)/libarcwasm_graphics10.a $(G10_LIBS) $(G10_FB_HEADLESS)
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js: ART10_LDFLAGS += $(G10_GL_LDFLAGS)
ASYSTEM10_ADD += $(G10_FONTS_ADD) $(G10_KEYS_ADD) \
    --add $(BUILD)/dalvik/graphics-test.png:system/media/arcwasm/graphics-test.png
$(A10SYSTEM_IMAGE): $(G10_FONTS) framework/fonts10/fonts.xml $(BUILD)/dalvik/graphics-test.png

# --- Java tests on ART 10 -------------------------------------------------------------------
# The Android 4.4 tests (GraphicsTest, ViewTest), compiled against the 10
# framework, and the 10 versions of the 6.0 ones (tests/java10: SurfaceTest
# on SurfaceControl.Builder/Transaction and the layer tree of 10, HwuiTest
# on the Skia pipeline and HardwareRenderer; ImeTest, topic
# ios-audio-keyboard: the page's keyboard as the input method), with the 10
# implementation of the arcwasm class they use (framework/java-arcwasm10;
# natives in libarcwasm_graphics10.so above).
G10_JAVA10 := $(addprefix framework/java-arcwasm10/arcwasm/,graphics/FramebufferSurface.java \
                input/BrowserInput.java input/InputManagerService.java)
FW10G_TESTS := GraphicsTest ViewTest InputTest
FW10G_TESTS6 := SurfaceTest HwuiTest ImeTest
G10_TEST_SRCS := $(addprefix tests/java/arcwasm/,$(addsuffix .java,$(FW10G_TESTS) GraphicsDemo InputDemo)) \
                 $(addprefix tests/java10/arcwasm/,$(addsuffix .java,$(FW10G_TESTS6)))
ART10_JAVA_TESTS += $(G10_JAVA10) $(G10_TEST_SRCS)
$(A10FW)/arc-tests.jar: $(G10_JAVA10) $(G10_TEST_SRCS)
.PHONY: fw10-graphics-java-test
fw10-graphics-java-test: art10
	VM=art10 tools/run_java_tests.sh node $(FW10G_TESTS) $(FW10G_TESTS6)

# --- fw10-graphics-web: artvm10 with the display on the page's canvas ----------------------
#   make fw10-graphics-web  # build/fw10-graphics-web/: serve with tools/serve.py, open
#                           # index.html (GraphicsDemo), input.html (InputDemo),
#                           # index.html?arg=-cp&arg=/system/framework/arc-tests.jar&arg=arcwasm.HwuiTest
# art10-web with the gralloc display backend instead of the one in memory:
# SurfaceFlinger 10 composes into a gralloc buffer of ARC's
# graphics_translation, whose compositor thread shows it on the page's
# <canvas> (OffscreenCanvas, WebGL2), as fw8-graphics-web does for 8.1;
# and the page's DOM input (shim/input/browser_input.js).
G10WEB := $(BUILD)/fw10-graphics-web
G10_GRALLOC_FB := $(call gobj,shim/graphics/framebuffer_gralloc.cc)
G10WEB_LDFLAGS = $(ART10_LDFLAGS) -sENVIRONMENT=web,worker -sOFFSCREENCANVAS_SUPPORT \
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 --pre-js shim/input/browser_input.js
$(G10WEB)/artvm10.js: $(ART10_LINK_DEPS) $(G10_GRALLOC_FB) $(BUILD)/libgraphics_translation.a \
                      $(G10LIB)/libandroid_runtime_graphics10.a $(G10LIB)/libarcwasm_graphics10.a \
                      $(G10_LIBS) shim/input/browser_input.js
	@mkdir -p $(dir $@)
	$(EMXX) $(G10WEB_LDFLAGS) $(ART10_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) $(G10_GRALLOC_FB) \
	    -Wl,--start-group $(filter-out $(G10_FB_HEADLESS),$(ART10_LIBS)) \
	    $(BUILD)/libgraphics_translation.a $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
	$(call wasm_opt,$(@:.js=.wasm))
# The 4.4 pages, loading artvm10.js.
$(G10WEB)/index.html: demo/graphics/index.html
	@mkdir -p $(dir $@)
	sed -e 's/dalvikvm/artvm10/g' -e 's/Dalvik/ART (Android 10)/g' $< > $@
$(G10WEB)/input.html: demo/input/index.html
	@mkdir -p $(dir $@)
	sed -e 's/dalvikvm/artvm10/g' -e 's/Dalvik/ART (Android 10)/g' $< > $@
$(G10WEB)/report.js: shim/report.js
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: fw10-graphics-web
fw10-graphics-web: $(G10WEB)/artvm10.js $(G10WEB)/index.html $(G10WEB)/input.html $(G10WEB)/report.js
