# Topic "ndk-bridge": running Android NDK libraries (ARM ELF .so from an
# APK) inside dalvikvm (docs/topics/ndk-bridge.md). The bridge (shim/ndk/:
# ELF loader, bionic/GLES/JNI thunks, Dalvik hooks) is linked into
# dalvikvm together with the ARM CPU of topic "arm-cpu" (NDK_CPU_LIB from
# mk/topic-arm-cpu.mk); without it, a stub CPU that runs no ARM code.
#
#   make ndk-unit-test        # loader/thunk unit tests (Node, stub CPU)
#   make ndk-check-imports SO=path/to/lib.so   # every import has a thunk?
#   make ndk-test-libs        # tests/ndk ARM libraries (clang + lld)
#   make ndk-test             # NdkTest in dalvikvm (Node)

NDK_BRIDGE_SRCS := $(addprefix shim/ndk/,ndk_core.cc ndk_linker.cc ndk_libc.cc ndk_stdio.cc \
                     ndk_pthread.cc ndk_green.cc ndk_libm.cc ndk_misc.cc ndk_gles.cc ndk_jni.cc)
NDK_FLAGS := -std=gnu++17 $(D_INCLUDES) -Ishim/ndk -Wno-c++17-extensions
$(call dobj,$(NDK_BRIDGE_SRCS)): XFLAGS := $(NDK_FLAGS)
$(DOBJ)/shim/ndk/ndk_dalvik.cc.o: XFLAGS := -std=gnu++17 $(D_DVM_FLAGS) -Ishim/ndk
$(BUILD)/libndk_bridge.a: $(call dobj,$(NDK_BRIDGE_SRCS) shim/ndk/ndk_dalvik.cc)
	@rm -f $@ && $(EMAR) rcs $@ $^

# The ARM CPU: topic arm-cpu's library (mk/topic-arm-cpu.mk, included
# before this file), or the stub (no ARM code runs).
NDK_CPU_LIB ?= $(if $(NDKCPU_OBJS),$(BUILD)/libndk_cpu.a)
$(DOBJ)/shim/ndk/testing/%: XFLAGS := -std=gnu++17
$(BUILD)/libndk_cpu_stub.a: $(call dobj,shim/ndk/testing/ndk_cpu_stub.cc)
	@rm -f $@ && $(EMAR) rcs $@ $^
NDK_CPU := $(if $(NDK_CPU_LIB),$(NDK_CPU_LIB),$(BUILD)/libndk_cpu_stub.a)

NDK_LIBS := $(BUILD)/libndk_bridge.a $(NDK_CPU)
DALVIK_LIBS += $(NDK_LIBS)
GWEB_LIBS += $(NDK_LIBS)
$(BUILD)/dalvik/dalvikvm.js $(DWEB)/dalvikvm.js $(GWEB)/dalvikvm.js: $(NDK_LIBS)
# Dalvik refers to the bridge only weakly (static_jni_libs.cc, wasm_call.cc),
# which does not pull archive members in: -u does (the link flags are
# expanded in the recipes, so appending here reaches them).
NDK_LDFLAGS := -Wl,-u,ndk_dalvik_dlopen -Wl,-u,arc_ndk_platform_invoke
# The arm-cpu interpreter translates hot guest blocks into wasm functions it
# adds to the function table (~10x faster); without table growth it stays
# an interpreter.
NDK_LDFLAGS += -sALLOW_TABLE_GROWTH
DVM_LDFLAGS += $(NDK_LDFLAGS)
GWEB_LDFLAGS += $(NDK_LDFLAGS)

# --- unit tests (plain Node program, files from the host: NODERAWFS) ------------
NDK_UNIT := $(BUILD)/ndk/ndk_unit_test.js
$(DOBJ)/tests/ndk/%: XFLAGS := $(NDK_FLAGS)
$(NDK_UNIT): $(call dobj,tests/ndk/ndk_unit_test.cc tests/ndk/unit_support.cc) \
             $(call dobj,$(NDK_BRIDGE_SRCS)) $(BUILD)/libndk_cpu_stub.a
	@mkdir -p $(dir $@)
	$(EMXX) -pthread -sNODERAWFS=1 -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sASSERTIONS=1 \
	    -sINITIAL_MEMORY=256MB -sPTHREAD_POOL_SIZE=4 $(NDK_UNIT_LDFLAGS) $^ -o $@

.PHONY: ndk-unit-test ndk-check-imports
# (Later topics add library directories: NDK_UNIT_DIRS += and prerequisites.)
NDK_UNIT_DIRS = $(foreach a,$(NDK_ABIS),$(BUILD)/ndk/libs/$(a))
ndk-unit-test: $(NDK_UNIT) ndk-test-libs
	$(foreach d,$(NDK_UNIT_DIRS),$(NODE) $(NDK_UNIT) $(d) &&) true
ndk-check-imports: $(NDK_UNIT)
	$(NODE) $(NDK_UNIT) --check-imports $(SO)

# --- ARM test libraries (tests/ndk) -----------------------------------------------
# Built like the NDK (android-9; armeabi: ARMv5TE soft-float, armeabi-v7a:
# Thumb-2, softfp, VFPv3-D16) with the host's clang and lld (the one in
# emsdk), against the NDK's headers and stub libraries from AOSP's
# prebuilts (extracted into build/ndk). libgcc's helpers come from
# compiler-rt's generic C builtins (emsdk) plus tests/ndk/aeabi_*.
NDK_ARCHIVE ?= $(ARC_SIBLINGS)/arm44-web/out/downloads/aosp/prebuilts/ndk.tar.gz
NDK_SYSROOT := $(BUILD)/ndk/9/platforms/android-9/arch-arm/usr
NDK_ABIS := armeabi armeabi-v7a
NDKT_ARCH_armeabi := --target=armv5te-linux-androideabi -march=armv5te -mfloat-abi=soft
NDKT_ARCH_armeabi-v7a := --target=armv7a-linux-androideabi -march=armv7-a -mfloat-abi=softfp \
                         -mfpu=vfpv3-d16 -mthumb
NDKT_CFLAGS := -fpic -nostdinc -isystem $(NDK_SYSROOT)/include \
               -isystem $(shell clang -print-resource-dir)/include \
               -fno-exceptions -ffunction-sections -fstack-protector -O2 -g
NDKT_CXXFLAGS := -std=c++11 -fno-rtti -nostdinc++
NDKT_LD := $(EMSDK)/upstream/bin/lld -flavor gnu -shared --hash-style=sysv -z max-page-size=4096
NDK_RT_DIR := $(EMSDK)/upstream/emscripten/system/lib/compiler-rt/lib/builtins
NDK_RT_SRCS := adddf3 addsf3 subdf3 subsf3 muldf3 mulsf3 divdf3 divsf3 comparedf2 comparesf2 \
               extendsfdf2 truncdfsf2 fixdfsi fixsfsi fixunsdfsi fixunssfsi floatsidf floatsisf \
               floatunsidf floatunsisf fixdfdi fixunsdfdi floatdidf floatundidf negdf2 negsf2 \
               udivsi3 divsi3 umodsi3 modsi3 udivmoddi4 divdi3 udivdi3 moddi3 umoddi3 \
               ashldi3 lshrdi3 ashrdi3 muldi3 clzsi2 ctzsi2 divmoddi4 fp_mode

$(NDK_SYSROOT)/include/jni.h:
	@mkdir -p $(BUILD)/ndk
	tar xzf "$(NDK_ARCHIVE)" -C $(BUILD)/ndk 9/platforms/android-9/arch-arm
	touch $@

# lib<name>.so for each ABI from tests/ndk/*.c/.cc; DT_NEEDED on the
# libraries named in the third argument.
define ndk_abi
NDKT_CC_$(1) := clang $(PREFIX_MAP) $$(NDKT_ARCH_$(1)) $$(NDKT_CFLAGS)
$(BUILD)/ndk/$(1)/rt/%.o: $(NDK_RT_DIR)/%.c | $(NDK_SYSROOT)/include/jni.h
	@mkdir -p $$(dir $$@)
	$$(NDKT_CC_$(1)) -fvisibility=hidden -w -c $$< -o $$@
$(BUILD)/ndk/$(1)/rt/aeabi_divmod.o: tests/ndk/aeabi_divmod.S
	@mkdir -p $$(dir $$@)
	$$(NDKT_CC_$(1)) -c $$< -o $$@
$(BUILD)/ndk/$(1)/rt/aeabi_extra.o: tests/ndk/aeabi_extra.c | $(NDK_SYSROOT)/include/jni.h
	@mkdir -p $$(dir $$@)
	$$(NDKT_CC_$(1)) -fvisibility=hidden -c $$< -o $$@
$(BUILD)/ndk/$(1)/libndkrt.a: $(patsubst %,$(BUILD)/ndk/$(1)/rt/%.o,$(NDK_RT_SRCS) aeabi_divmod aeabi_extra)
	@rm -f $$@ && $(EMSDK)/upstream/bin/llvm-ar rcs $$@ $$^
$(BUILD)/ndk/$(1)/obj/%.c.o: tests/ndk/%.c | $(NDK_SYSROOT)/include/jni.h
	@mkdir -p $$(dir $$@)
	$$(NDKT_CC_$(1)) -c $$< -o $$@
$(BUILD)/ndk/$(1)/obj/%.cc.o: tests/ndk/%.cc | $(NDK_SYSROOT)/include/jni.h
	@mkdir -p $$(dir $$@)
	$$(NDKT_CC_$(1)) $(NDKT_CXXFLAGS) -c $$< -o $$@
endef
define ndk_test_lib
$(BUILD)/ndk/libs/$(1)/lib$(2).so: $(patsubst %,$(BUILD)/ndk/$(1)/obj/%.o,$(3)) $(BUILD)/ndk/$(1)/libndkrt.a \
        $(foreach d,$(4),$(BUILD)/ndk/libs/$(1)/lib$(d).so)
	@mkdir -p $$(dir $$@)
	$(NDKT_LD) -soname lib$(2).so -o $$@ $(NDK_SYSROOT)/lib/crtbegin_so.o \
	    $(patsubst %,$(BUILD)/ndk/$(1)/obj/%.o,$(3)) $(BUILD)/ndk/$(1)/libndkrt.a \
	    -L$(BUILD)/ndk/libs/$(1) $(addprefix -l,$(4)) -L$(NDK_SYSROOT)/lib $(5) \
	    -lc -lm -ldl -llog $(NDK_SYSROOT)/lib/crtend_so.o
NDK_TEST_LIBS_$(1) += $(BUILD)/ndk/libs/$(1)/lib$(2).so
endef
$(foreach a,$(NDK_ABIS),$(eval $(call ndk_abi,$(a))) \
  $(eval $(call ndk_test_lib,$(a),ndkhelper,helper.c,,)) \
  $(eval $(call ndk_test_lib,$(a),ndktest,ndktest.c ndktest_cxx.cc,ndkhelper,-lstdc++)))
NDK_TEST_LIBS := $(foreach a,$(NDK_ABIS),$(NDK_TEST_LIBS_$(a)))

.PHONY: ndk-test-libs
ndk-test-libs: $(NDK_TEST_LIBS)

# --- the NDK test APKs (tests/ndk/apk + the libraries in lib/ABI/) -----------------
# ArcWasmNdkTest.apk carries both ABIs (the package service picks
# armeabi-v7a); ArcWasmNdkTestV5.apk, the same code as package
# org.arcwasm.ndktest.v5, only armeabi.
NDK_APK_SRCS := $(shell find tests/ndk/apk -type f)
NDK_TEST_APK := $(BUILD)/dalvik/apk-ndk/ArcWasmNdkTest.apk
NDK_TEST_APK_V5 := $(BUILD)/dalvik/apk-ndk/ArcWasmNdkTestV5.apk
ndk_apk_libs = $(foreach a,$(1),$(foreach l,$(NDK_TEST_LIBS_$(a)),$(l):lib/$(a)/$(notdir $(l))))
$(NDK_TEST_APK): $(NDK_APK_SRCS) $(NDK_TEST_LIBS) tools/build_test_apk.sh
	AAPT="$(AAPT)" D8="$(D8)" ANDROID_JAR="$(ANDROID_JAR)" \
	    EXTRA_ASSETS="$(call ndk_apk_libs,$(NDK_ABIS))" \
	    tools/build_test_apk.sh tests/ndk/apk $(BUILD)/dalvik/apk-ndk/work $@
$(NDK_TEST_APK_V5): $(NDK_APK_SRCS) $(NDK_TEST_LIBS_armeabi) tools/build_test_apk.sh
	rm -rf $(BUILD)/dalvik/apk-ndk/src-v5 && mkdir -p $(BUILD)/dalvik/apk-ndk && cp -R tests/ndk/apk $(BUILD)/dalvik/apk-ndk/src-v5
	sed -i.orig 's/package="org.arcwasm.ndktest"/package="org.arcwasm.ndktest.v5"/' \
	    $(BUILD)/dalvik/apk-ndk/src-v5/AndroidManifest.xml
	AAPT="$(AAPT)" D8="$(D8)" ANDROID_JAR="$(ANDROID_JAR)" \
	    EXTRA_ASSETS="$(call ndk_apk_libs,armeabi)" \
	    tools/build_test_apk.sh $(BUILD)/dalvik/apk-ndk/src-v5 $(BUILD)/dalvik/apk-ndk/work-v5 $@
DSYSTEM_ADD += --add $(NDK_TEST_APK):system/app/ArcWasmNdkTest.apk \
               --add $(NDK_TEST_APK_V5):system/app/ArcWasmNdkTestV5.apk
$(DSYSTEM_IMAGE): $(NDK_TEST_APK) $(NDK_TEST_APK_V5)

.PHONY: ndk-test-apk ndk-test
ndk-test-apk: $(NDK_TEST_APK) $(NDK_TEST_APK_V5)
ndk-test: dalvik
	cd $(BUILD)/dalvik && $(NODE) dalvikvm.js -cp /system/framework/arc-tests.jar arcwasm.NdkTest
