# Topic "vulkan": Vulkan 1.1 for guest native code on the browser's WebGPU
# (docs/topics/vulkan.md).
#
#   shim/vulkan/*.cc          the driver: libvulkan.so of the NDK bridge for both
#                             guest ABIs (a host library, shim/ndk/ndk_host_libs.h),
#                             in artvm8 and artvm10
#   shim/vulkan/webgpu_host.js  the page side (the GPUDevice), a --pre-js of the page
#                             builds (fw8-graphics-web, fw10-graphics-web, SENSES Web
#                             Apps' a81 and a10)
#   tools/vulkan/gen_vk.py    entry points and arm64 struct conversions from vk.xml
#   tools/vulkan/build_tint.sh  Tint (SPIR-V -> WGSL) as build/vulkan/tint/arc_tint.{js,wasm},
#                             copied next to the page builds' VM when built
#   tests/vulkan/             VulkanTest's library (armeabi-v7a, arm64-v8a)
#
#   tools/fetch_vulkan_headers.sh                  # third_party/vulkan-headers
#   tools/vulkan/build_tint.sh WORK build/vulkan/tint   # once (needs cmake; WORK on a big disk)
#   VM=art8 tools/run_java_tests.sh node VulkanTest
#   VM=art8 PAGE=fw8-graphics-web tools/run_java_tests.sh chrome VulkanTest
#
# Included after the stacks' mk files (alphabetical order): A8LIB, F8*, G8WEB,
# A10*, G10WEB, SAVM, NDK64T, FCM_NDK8 are defined.

VKB := $(BUILD)/vulkan
VK_HEADERS := third_party/vulkan-headers
VK_GEN := $(VKB)/gen/vk_api_gen.h $(VKB)/gen/vk_thunks_gen.inc
VK_SRCS := $(addprefix shim/vulkan/,vk_command.cc vk_device.cc vk_format.cc vk_gpu.cc vk_instance.cc \
             vk_jni.cc vk_pipeline.cc vk_queue.cc vk_spirv.cc vk_thunks.cc)

$(VK_HEADERS)/.version: tools/fetch_vulkan_headers.sh
	tools/fetch_vulkan_headers.sh
# (One run writes both; make 3.81 has no grouped targets.)
$(VKB)/gen/vk_api_gen.h: tools/vulkan/gen_vk.py shim/vulkan/vk_extensions.txt $(VK_HEADERS)/.version
	@mkdir -p $(dir $@)
	python3 tools/vulkan/gen_vk.py $(VK_HEADERS)/registry/vk.xml shim/vulkan/vk_extensions.txt $(VKB)/gen
$(VKB)/gen/vk_thunks_gen.inc: $(VKB)/gen/vk_api_gen.h
	@test -f $@

# The driver uses no Android headers: built once per stack only for the
# stack's JNI header and flags (-pthread, -O2), as topic webview8 does.
VK_FLAGS := -std=gnu++17 -Ishim/vulkan -I$(VKB)/gen -I$(VK_HEADERS)/include -Ishim/ndk -Ishim/gles \
            -Ishim/gles/include -DVK_NO_PROTOTYPES -DVK_USE_PLATFORM_ANDROID_KHR -Wno-unused-function

ifneq ($(A8LIB),)
VK8 := $(VKB)/art8
$(VK8)/obj/%.cc.o: %.cc $(VK_GEN) mk/topic-vulkan.mk
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(F8_CXXFLAGS) $(F8S_FLAGS) $(VK_FLAGS) -c $< -o $@
$(VK8)/libarcvk.a: $(patsubst %,$(VK8)/obj/%.o,$(VK_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART8_LIBS += -Wl,--whole-archive $(VK8)/libarcvk.a -Wl,--no-whole-archive
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js $(G8WEB)/artvm8.js $(SAVM)/a81/artvm8.js: $(VK8)/libarcvk.a
# System.load wants a file in /system/lib (an empty one; the library is linked in).
ASYSTEM8_ADD += --add $(F8S)/empty.so:system/lib/libarcwasm_vulkan.so
# The page builds: the GPU's owner.
G8WEB_LDFLAGS += --pre-js shim/vulkan/webgpu_host.js
$(G8WEB)/artvm8.js $(SAVM)/a81/artvm8.js: shim/vulkan/webgpu_host.js
endif

ifneq ($(G10WEB),)
VK10 := $(VKB)/art10
$(VK10)/obj/%.cc.o: %.cc $(VK_GEN) mk/topic-vulkan.mk
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(F10_CXXFLAGS) $(F10S_FLAGS) $(VK_FLAGS) -c $< -o $@
$(VK10)/libarcvk.a: $(patsubst %,$(VK10)/obj/%.o,$(VK_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART10_LIBS += -Wl,--whole-archive $(VK10)/libarcvk.a -Wl,--no-whole-archive
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js $(G10WEB)/artvm10.js $(SAVM)/a10/artvm10.js: $(VK10)/libarcvk.a
ASYSTEM10_ADD += --add $(F10S)/empty.so:system/lib/libarcwasm_vulkan.so
G10WEB_LDFLAGS += --pre-js shim/vulkan/webgpu_host.js
$(G10WEB)/artvm10.js $(SAVM)/a10/artvm10.js: shim/vulkan/webgpu_host.js
endif

# --- Tint next to the page builds' VM (when tools/vulkan/build_tint.sh made it) ----------
VK_TINT := $(wildcard $(VKB)/tint/arc_tint.js $(VKB)/tint/arc_tint.wasm)
ifneq ($(VK_TINT),)
VK_TINT_DIRS := $(if $(G8WEB),$(G8WEB)) $(if $(G10WEB),$(G10WEB)) $(if $(SAVM),$(SAVM)/a81 $(SAVM)/a10)
define vk_copy_tint
$(1)/$(notdir $(2)): $(2)
	@mkdir -p $(1)
	cp $(2) $(1)/
endef
$(foreach d,$(VK_TINT_DIRS),$(foreach f,$(VK_TINT),$(eval $(call vk_copy_tint,$(d),$(f)))))
VK_TINT_COPIES := $(foreach d,$(VK_TINT_DIRS),$(addprefix $(d)/,$(notdir $(VK_TINT))))
.PHONY: vulkan-tint
vulkan-tint: $(VK_TINT_COPIES)
$(if $(G8WEB),$(G8WEB)/artvm8.js) $(if $(G10WEB),$(G10WEB)/artvm10.js): | vulkan-tint
endif

# --- VulkanTest's libraries ----------------------------------------------------------------
# tests/vulkan/vktest.c with the Khronos headers: for armeabi-v7a like
# NdkMediaArmTest's library (NDK r9's android-18 sysroot, clang + lld; the
# headers make Vulkan's calls aapcs-vfp there), for arm64-v8a like
# NdkMediaArm64Test's (AOSP 8.1's bionic headers, the NDK's stand-in libraries).
# libvulkan.so is dlopen'ed, as engines do.
VKT := $(VKB)/test
VKT_DEPS := tests/vulkan/vktest.c tests/vulkan/vktest_spirv.h $(VK_HEADERS)/.version
ifneq ($(A8LIB)$(G10WEB),)
$(VKT)/arm/vktest.c.o: $(VKT_DEPS) | $$(NDK18_SYSROOT)/include/jni.h
	@mkdir -p $(dir $@)
	$(NDK6_CC) -isystem $(VK_HEADERS)/include -Itests/vulkan -c $< -o $@
$(VKT)/libvktest.so: $(VKT)/arm/vktest.c.o $(BUILD)/ndk/armeabi-v7a/libndkrt.a
	$(NDKT_LD) -soname libvktest.so -o $@ $(NDK18_SYSROOT)/lib/crtbegin_so.o $^ \
	    -L$(NDK18_SYSROOT)/lib -llog -lc -lm -ldl $(NDK18_SYSROOT)/lib/crtend_so.o
$(VKT)/arm64/vktest.c.o: $(VKT_DEPS)
	@mkdir -p $(dir $@)
	$(A64_CC) $(NDK64T_CFLAGS) -isystem $(VK_HEADERS)/include -Itests/vulkan \
	    -DNATIVE_PROBE=Java_arcwasm_VulkanTest_nativeProbe64 -c $< -o $@
$(VKT)/libvktest64.so: $(VKT)/arm64/vktest.c.o $(NDK64T)/obj/crt_so.o $(addprefix $(NDK64T)/stubs/,$(NDK64T_STUBS))
	$(A64_LD) $(NDK64T_LDFLAGS) -soname libvktest64.so -o $@ $(filter %.o,$^) -lc -lm -ldl -llog
.PHONY: vulkan-test-libs
vulkan-test-libs: $(VKT)/libvktest.so $(VKT)/libvktest64.so
endif
ifneq ($(A8LIB),)
ASYSTEM8_ADD += --add $(VKT)/libvktest.so:system/lib/libvktest.so \
                --add $(VKT)/libvktest64.so:system/lib64/libvktest64.so
$(A8SYSTEM_IMAGE): $(VKT)/libvktest.so $(VKT)/libvktest64.so
ART8_JAVA_TESTS += tests/java8/arcwasm/VulkanTest.java
$(A8FW)/arc-tests.jar: tests/java8/arcwasm/VulkanTest.java
endif
ifneq ($(G10WEB),)
ASYSTEM10_ADD += --add $(VKT)/libvktest.so:system/lib/libvktest.so \
                 --add $(VKT)/libvktest64.so:system/lib64/libvktest64.so
$(A10SYSTEM_IMAGE): $(VKT)/libvktest.so $(VKT)/libvktest64.so
ART10_JAVA_TESTS += tests/java8/arcwasm/VulkanTest.java
$(A10FW)/arc-tests.jar: tests/java8/arcwasm/VulkanTest.java
endif

# The test shaders: tests/vulkan/*.spvasm -> tests/vulkan/vktest_spirv.h (in git),
# with SPIRV-Tools' spirv-as (SPIRV_AS=path/to/spirv-as make vulkan-test-shaders).
.PHONY: vulkan-test-shaders
vulkan-test-shaders:
	python3 tools/vulkan/spv_to_h.py $(SPIRV_AS) tests/vulkan/vktest_spirv.h \
	    kComputeSpirv=tests/vulkan/compute.spvasm kVertexSpirv=tests/vulkan/vertex.spvasm \
	    kFragmentSpirv=tests/vulkan/fragment.spvasm

.PHONY: vulkan
vulkan: $(if $(A8LIB),$(VK8)/libarcvk.a) $(if $(G10WEB),$(VK10)/libarcvk.a)
