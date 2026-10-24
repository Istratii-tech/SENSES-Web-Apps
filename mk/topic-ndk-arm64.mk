# Topic "ndk-arm64" (docs/topics/ndk-arm64.md): native code of Android apps
# built for arm64-v8a (AArch64). The CPU (shim/ndk/cpu64/, contract
# shim/ndk/ndk_cpu64.h) and its tests.
#
#   make ndk64-cpu          # build/libndk_cpu64.a
#   make ndk64-cpu-test     # guest test programs + runner under Node: interpreter, then everything translated
#   make ndk64-cpu-bench    # the MIPS benchmark
#   make ndk64-cpu-web      # build/ndk64-cpu/index.html for browsers
#
# Guest programs are compiled with the host's clang for aarch64-linux-android
# and linked with emsdk's lld as position-independent executables. The
# differential cases (tools/ndk64/gen_a64_cases.py, tests/ndk64/cpu/diff) run
# natively on the Mac first: their results there are the reference.
#
# Included by the Makefile after mk/dalvik.mk; the NDK bridge part (ELF64
# loader, LP64 thunks) is in the bridge's files and mk/topic-*-system.mk.

NDK64 := $(BUILD)/ndk64-cpu
NDK64_SRCS := $(addprefix shim/ndk/cpu64/,decode64.cc simd64_decode.cc exec64.cc fp64.cc simd64.cc runtime64.cc jit64.cc \
                                          signal64.cc)
NDK64_OBJS := $(patsubst shim/ndk/cpu64/%.cc,$(NDK64)/obj/%.o,$(NDK64_SRCS)) $(NDK64)/obj/jit_pack.o
NDK64_HDRS := shim/ndk/ndk_cpu64.h $(wildcard shim/ndk/cpu64/*.h) shim/ndk/cpu64/exec64_ops.inc shim/ndk/jit_pack.h
NDK64_CXXFLAGS := -std=gnu++17 -pthread -O2 -g1 -Ishim -Wall -Wno-unused-function

$(NDK64)/obj/%.o: shim/ndk/cpu64/%.cc $(NDK64_HDRS) mk/topic-ndk-arm64.mk
	@mkdir -p $(dir $@)
	$(EMXX) $(NDK64_CXXFLAGS) -c $< -o $@

# The translators' packs (shim/ndk/jit_pack.cc, also in libndk_cpu.a: with
# both libraries the linker takes one).
$(NDK64)/obj/jit_pack.o: shim/ndk/jit_pack.cc shim/ndk/jit_pack.h shim/membudget/mem_budget.h mk/topic-ndk-arm64.mk
	@mkdir -p $(dir $@)
	$(EMXX) $(NDK64_CXXFLAGS) -c $< -o $@

$(BUILD)/libndk_cpu64.a: $(NDK64_OBJS)
	@rm -f $@ && $(EMAR) rcs $@ $^

NDK_CPU64_LIB := $(BUILD)/libndk_cpu64.a

.PHONY: ndk64-cpu ndk64-cpu-test ndk64-cpu-bench ndk64-cpu-web
ndk64-cpu: $(NDK_CPU64_LIB)

# --- guest toolchain -------------------------------------------------------------------------
A64_CC ?= /usr/bin/clang $(PREFIX_MAP) --target=aarch64-linux-android21
A64_LD ?= $(EMSDK)/upstream/bin/lld -flavor gnu
A81_BIONIC := third_party/aosp81/bionic
A64_SYSINC := -I$(A81_BIONIC)/libc/include -I$(A81_BIONIC)/libc/kernel/uapi \
              -I$(A81_BIONIC)/libc/kernel/uapi/asm-arm64 -I$(A81_BIONIC)/libc/kernel/android/uapi

# libm: FreeBSD msun as bionic builds it (bionic/libm/Android.bp), the
# functions the tests use.
A64_LIBM_SRC := $(A81_BIONIC)/libm/upstream-freebsd/lib/msun/src
A64_LIBM_FILES := e_atan2 s_atan e_exp e_log e_log10 e_pow s_sin s_cos k_sin k_cos e_rem_pio2 k_rem_pio2 s_tan \
                  k_tan e_fmod s_sinf s_cosf k_sinf k_cosf e_rem_pio2f e_expf e_logf e_powf s_scalbn e_sqrtf \
                  s_floor s_ceil e_sqrt
A64_LIBM_FLAGS := -O2 -fPIC -ffreestanding -fno-stack-protector -D__BIONIC_NO_MATH_INLINES -D_BSD_SOURCE \
                  -DFLT_EVAL_METHOD=0 -include $(A81_BIONIC)/libm/freebsd-compat.h \
                  -I$(A81_BIONIC)/libm/upstream-freebsd/android/include -I$(A64_LIBM_SRC) -I$(A81_BIONIC)/libm \
                  $(A64_SYSINC) -w
$(NDK64)/libm/%.o: $(A64_LIBM_SRC)/%.c
	@mkdir -p $(dir $@)
	$(A64_CC) $(A64_LIBM_FLAGS) -c $< -o $@
$(NDK64)/libm64.a: $(patsubst %,$(NDK64)/libm/%.o,$(A64_LIBM_FILES))
	@rm -f $@ && $(EMAR) rcs $@ $^
# --- differential cases: generated, then run natively for the reference ----------------------
NDK64_DIFF := $(NDK64)/diff
A64_GUEST_DIR := tests/ndk64/cpu
# (One run writes both files; make 3.81 has no grouped targets.)
$(NDK64_DIFF)/cases.S: tools/ndk64/gen_a64_cases.py
	@mkdir -p $(NDK64_DIFF)
	python3 tools/ndk64/gen_a64_cases.py $(NDK64_DIFF)
$(NDK64_DIFF)/cases.c: $(NDK64_DIFF)/cases.S
	@test -f $@
NDK64_NATIVE_SRCS := $(A64_GUEST_DIR)/diff/diff_driver.c $(A64_GUEST_DIR)/diff/neon_diff.c \
                     $(A64_GUEST_DIR)/diff/native_main.c $(NDK64_DIFF)/cases.c
$(NDK64_DIFF)/diff_native: $(NDK64_NATIVE_SRCS) $(NDK64_DIFF)/cases.S $(A64_GUEST_DIR)/rt/guest_asm.S \
                           $(A64_GUEST_DIR)/diff/diff.h
	/usr/bin/clang -O1 -DDIFF_NATIVE -I$(A64_GUEST_DIR)/diff -o $@ $(NDK64_NATIVE_SRCS) $(NDK64_DIFF)/cases.S \
	    $(A64_GUEST_DIR)/rt/guest_asm.S
$(NDK64)/guest/golden.bin: $(NDK64_DIFF)/diff_native
	@mkdir -p $(dir $@)
	$< $@

# --- guest test programs ------------------------------------------------------------------------
# Portable groups of the ARMv7 tests, AArch64 groups, the runtime, the cases.
A64_GUEST_COMMON := $(addprefix tests/ndk/cpu/,t_arith.c t_memory.c t_float.c t_setjmp.c t_callback.c bench.c) \
                    $(addprefix $(A64_GUEST_DIR)/,t_main.c t_control.c t_atomic.c t_abi.c t_system.c t_vector.c t_jit.c \
                      t_diff.c rt/guest_rt.c diff/diff_driver.c diff/neon_diff.c) \
                    $(NDK64_DIFF)/cases.c
A64_GUEST_CFLAGS := -O2 -fPIC -ffreestanding -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
                    -ffp-contract=off -fno-exceptions -mno-outline-atomics -Wall -Wno-unused-function \
                    -I$(A64_GUEST_DIR)/rt -I$(A64_GUEST_DIR)/diff
# Assembling the cases needs the instructions of v8.1-8.3 (LSE, LDAPR) whatever the variant compiles C for.
A64_ASFLAGS := -march=armv8.3-a+lse+rcpc+crc
NDK64_VARIANTS := a64 a64-lse
ARCH_a64 := -march=armv8-a+crc
ARCH_a64-lse := -march=armv8.2-a+lse+crc

define NDK64_VARIANT
NDK64_GOBJ_$(1) := $$(patsubst %.c,$(NDK64)/$(1)/%.o,$$(subst $(NDK64_DIFF)/,gen/,$(A64_GUEST_COMMON))) \
                   $(NDK64)/$(1)/guest_asm.o $(NDK64)/$(1)/cases_s.o
$(NDK64)/$(1)/tests/%.o: tests/%.c $(A64_GUEST_DIR)/rt/guest.h $(A64_GUEST_DIR)/diff/diff.h mk/topic-ndk-arm64.mk
	@mkdir -p $$(dir $$@)
	$(A64_CC) $$(ARCH_$(1)) $(A64_GUEST_CFLAGS) -c $$< -o $$@
$(NDK64)/$(1)/gen/%.o: $(NDK64_DIFF)/%.c $(A64_GUEST_DIR)/diff/diff.h
	@mkdir -p $$(dir $$@)
	$(A64_CC) $$(ARCH_$(1)) $(A64_GUEST_CFLAGS) -c $$< -o $$@
$(NDK64)/$(1)/guest_asm.o: $(A64_GUEST_DIR)/rt/guest_asm.S
	@mkdir -p $$(dir $$@)
	$(A64_CC) $(A64_ASFLAGS) -c $$< -o $$@
$(NDK64)/$(1)/cases_s.o: $(NDK64_DIFF)/cases.S
	@mkdir -p $$(dir $$@)
	$(A64_CC) $(A64_ASFLAGS) -c $$< -o $$@
$(NDK64)/guest/$(1).elf: $$(NDK64_GOBJ_$(1)) $(NDK64)/libm64.a
	@mkdir -p $$(dir $$@)
	$(A64_LD) -pie --no-dynamic-linker -z notext -z max-page-size=4096 -e guest_main -o $$@ \
	    $$(NDK64_GOBJ_$(1)) $(NDK64)/libm64.a
endef
$(foreach v,$(NDK64_VARIANTS),$(eval $(call NDK64_VARIANT,$(v))))
NDK64_GUESTS := $(patsubst %,$(NDK64)/guest/%.elf,$(NDK64_VARIANTS)) $(NDK64)/guest/golden.bin

# --- runner (Node and browsers) -----------------------------------------------------------------
NDK64_RUNNER_OBJ := $(NDK64)/obj/ndk_cpu64_test.o
$(NDK64_RUNNER_OBJ): $(A64_GUEST_DIR)/runner/ndk_cpu64_test.cc $(NDK64_HDRS)
	@mkdir -p $(dir $@)
	$(EMXX) $(NDK64_CXXFLAGS) -c $< -o $@
NDK64_TEST_LDFLAGS := -pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=8 -sINITIAL_MEMORY=256MB -sALLOW_TABLE_GROWTH \
                      -sSTACK_SIZE=1MB -sEXIT_RUNTIME=1 -sENVIRONMENT=web,worker,node -O2
$(NDK64)/ndk_cpu64_test.js: $(NDK64_RUNNER_OBJ) $(NDK_CPU64_LIB) $(NDK64_GUESTS)
	$(EMXX) $(NDK64_TEST_LDFLAGS) $(NDK64_RUNNER_OBJ) $(NDK_CPU64_LIB) --embed-file $(NDK64)/guest@/guest -o $@
$(NDK64)/index.html: $(A64_GUEST_DIR)/index.html
	cp $< $@
$(NDK64)/report.js: shim/report.js
	cp $< $@

ndk64-cpu-test: $(NDK64)/ndk_cpu64_test.js
	cd $(NDK64) && $(NODE) ndk_cpu64_test.js --jit 0 $(NDK64_ARGS)
	cd $(NDK64) && $(NODE) ndk_cpu64_test.js --jit 1 $(NDK64_ARGS)
	cd $(NDK64) && $(NODE) ndk_cpu64_test.js --jit 1 --windows $(NDK64_ARGS)
ndk64-cpu-bench: $(NDK64)/ndk_cpu64_test.js
	cd $(NDK64) && $(NODE) ndk_cpu64_test.js --no-threads --bench 1000 --jit 0 $(NDK64_ARGS)
	cd $(NDK64) && $(NODE) ndk_cpu64_test.js --no-threads --bench 1000 $(NDK64_ARGS)
ndk64-cpu-web: $(NDK64)/ndk_cpu64_test.js $(NDK64)/index.html $(NDK64)/report.js

# --- the arm64-v8a bridge on ART 8.1 ------------------------------------------------------------
# The 64-bit linker and the thunks whose 32-bit versions do not fit an LP64
# guest join the bridge library of topic fw8-system (mk/topic-fw8-system.mk,
# included before this file: F8SLIB, f8sobj and its pattern rules), with the
# AArch64 CPU. The shared thunk files reach them through weak references
# (ndk_bridge.h), so the Dalvik and ART 6.0.1 builds, which have neither,
# link as before.
NDK64_BRIDGE_SRCS := $(addprefix shim/ndk/,ndk_linker64.cc ndk64_libc.cc ndk64_stdio.cc \
                       ndk64_jni.cc)
ifneq ($(A8LIB),)
$(call f8sobj,$(NDK64_BRIDGE_SRCS)): F8SXFLAGS := -std=gnu++17 -Ishim/ndk -Ishim/gles -Wno-c++17-extensions \
    -UARC_WASM_FW6
$(F8SLIB)/libndk_bridge_art8.a: $(call f8sobj,$(NDK64_BRIDGE_SRCS))
ART8_LIBS += $(NDK_CPU64_LIB)
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js: $(NDK_CPU64_LIB)
# The bridge refers to the CPU and to the ndk64_* files weakly: -u pulls them in (and the translator's
# fallback, which the generated wasm modules import by name).
ART8_LDFLAGS += -Wl,-u,ndk64_call_guest_args -Wl,-u,ndk64_host_stub -Wl,-u,ndk64_jit_exec_one \
                -Wl,-u,ndk64_cpu_report_trap -Wl,-u,ndk64_linker_linked -Wl,-u,ndk64_libc_linked \
                -Wl,-u,ndk64_stdio_linked -Wl,-u,ndk64_jni_linked
endif

# --- NdkArm64Test: tests/ndk's libraries built for arm64-v8a ----------------------------------
# No NDK: the host's clang against AOSP 8.1's bionic headers, linked with
# --no-undefined against stand-ins of the system libraries with the NDK's
# arm64 symbols (tools/ndk64/gen_stub_libs.py). The APK is tests/ndk/apk as
# package org.arcwasm.ndktest.arm64 with only lib/arm64-v8a/, so the package
# service picks the 64-bit ABI; arcwasm.NdkArm64Test runs NdkTest's checks.
NDK64T := $(BUILD)/ndk64
NDK64T_STUBS := libc.so libm.so libdl.so liblog.so libstdc++.so libOpenSLES.so libmediandk.so libaaudio.so
NDK64T_CFLAGS := -fpic -nostdinc -isystem $(shell /usr/bin/clang -print-resource-dir)/include \
                 -isystem $(A81_BIONIC)/libc/include -isystem $(A81_BIONIC)/libc/arch-arm64/include \
                 -isystem $(A81_BIONIC)/libc/kernel/uapi -isystem $(A81_BIONIC)/libc/kernel/uapi/asm-arm64 \
                 -isystem $(A81_BIONIC)/libc/kernel/android/uapi \
                 -isystem third_party/aosp81/libnativehelper/include_jni \
                 -isystem third_party/aosp81/system/core/liblog/include \
                 -fno-exceptions -ffunction-sections -fstack-protector -O2 -g
NDK64T_LDFLAGS := -shared --no-undefined --hash-style=both -z max-page-size=4096 -L$(NDK64T)/stubs \
                  -L$(NDK64T)/libs/arm64-v8a
# (One run writes every stub; make 3.81 has no grouped targets.)
$(NDK64T)/stubs/libc.so.S: tools/ndk64/gen_stub_libs.py tests/ndk/android81-ndk-symbols.txt
	@mkdir -p $(NDK64T)/stubs
	python3 tools/ndk64/gen_stub_libs.py tests/ndk/android81-ndk-symbols.txt $(NDK64T)/stubs $(NDK64T_STUBS)
$(addprefix $(NDK64T)/stubs/,$(addsuffix .S,$(filter-out libc.so,$(NDK64T_STUBS)))): $(NDK64T)/stubs/libc.so.S
	@test -f $@
$(NDK64T)/stubs/%.so: $(NDK64T)/stubs/%.so.S
	$(A64_CC) -c $< -o $<.o
	$(A64_LD) -shared -soname $*.so -o $@ $<.o
$(NDK64T)/obj/%.c.o: tests/ndk/%.c
	@mkdir -p $(dir $@)
	$(A64_CC) $(NDK64T_CFLAGS) -c $< -o $@
$(NDK64T)/obj/%.cc.o: tests/ndk/%.cc
	@mkdir -p $(dir $@)
	$(A64_CC) $(NDK64T_CFLAGS) -std=c++11 -fno-rtti -nostdinc++ -c $< -o $@
$(NDK64T)/obj/crt_so.o: tests/ndk64/crt_so.c
	@mkdir -p $(dir $@)
	$(A64_CC) $(NDK64T_CFLAGS) -c $< -o $@
NDK64T_LIBS := $(NDK64T)/libs/arm64-v8a/libndkhelper.so $(NDK64T)/libs/arm64-v8a/libndktest.so
$(NDK64T)/libs/arm64-v8a/libndkhelper.so: $(NDK64T)/obj/helper.c.o $(NDK64T)/obj/crt_so.o \
        $(addprefix $(NDK64T)/stubs/,$(NDK64T_STUBS))
	@mkdir -p $(dir $@)
	$(A64_LD) $(NDK64T_LDFLAGS) -soname libndkhelper.so -o $@ $(filter %.o,$^) -lc -lm -ldl -llog
$(NDK64T)/libs/arm64-v8a/libndktest.so: $(NDK64T)/obj/ndktest.c.o $(NDK64T)/obj/ndktest_cxx.cc.o \
        $(NDK64T)/obj/crt_so.o $(NDK64T)/libs/arm64-v8a/libndkhelper.so $(addprefix $(NDK64T)/stubs/,$(NDK64T_STUBS))
	$(A64_LD) $(NDK64T_LDFLAGS) -soname libndktest.so -o $@ $(filter %.o,$^) -lndkhelper -lstdc++ \
	    -lc -lm -ldl -llog
NDK64T_APK := $(NDK64T)/ArcWasmNdkTestArm64.apk
$(NDK64T_APK): $(shell find tests/ndk/apk -type f) $(NDK64T_LIBS) tools/build_test_apk.sh
	rm -rf $(NDK64T)/src && mkdir -p $(NDK64T) && cp -R tests/ndk/apk $(NDK64T)/src
	sed -i.orig 's/package="org.arcwasm.ndktest"/package="org.arcwasm.ndktest.arm64"/' \
	    $(NDK64T)/src/AndroidManifest.xml
	AAPT="$(AAPT)" D8="$(D8)" ANDROID_JAR="$(ANDROID_JAR)" \
	    EXTRA_ASSETS="$(foreach l,$(NDK64T_LIBS),$(l):lib/arm64-v8a/$(notdir $(l)))" \
	    tools/build_test_apk.sh $(NDK64T)/src $(NDK64T)/work $@
.PHONY: ndk64-test-apk ndk64-test
ndk64-test-apk: $(NDK64T_APK)

# --- NdkMediaArm64Test: OpenSL ES, libmediandk and libaaudio from arm64 code -------------------
# tests/ndk64/sles64test.c and NdkMediaArmTest's tests/ndk8cm/ndk8cmtest.c
# (its natives renamed for arcwasm.NdkMediaArm64Test) with AOSP 8.1's
# OpenSL ES and NDK media headers, linked like libndktest.so; in
# /system/lib64 (System.load: the ELF64 loader).
NDK64T_MEDIA_FLAGS := -isystem third_party/aosp81/frameworks/wilhelm/include \
    -isystem third_party/aosp81/frameworks/av/media/ndk/include \
    -isystem third_party/aosp81/frameworks/av/media/libaaudio/include \
    $(foreach f,nativeExtract nativeDecode nativeAAudio,-DJava_arcwasm_NdkMediaArmTest_$(f)=Java_arcwasm_NdkMediaArm64Test_$(f))
$(NDK64T)/obj/media/%.c.o: tests/ndk8cm/%.c
	@mkdir -p $(dir $@)
	$(A64_CC) $(NDK64T_CFLAGS) $(NDK64T_MEDIA_FLAGS) -c $< -o $@
$(NDK64T)/obj/media/%.c.o: tests/ndk64/%.c
	@mkdir -p $(dir $@)
	$(A64_CC) $(NDK64T_CFLAGS) $(NDK64T_MEDIA_FLAGS) -c $< -o $@
NDK64T_MEDIA_LIB := $(NDK64T)/lib64/libndkmedia64test.so
$(NDK64T_MEDIA_LIB): $(NDK64T)/obj/media/sles64test.c.o $(NDK64T)/obj/media/ndk8cmtest.c.o \
        $(NDK64T)/obj/crt_so.o $(addprefix $(NDK64T)/stubs/,$(NDK64T_STUBS))
	@mkdir -p $(dir $@)
	$(A64_LD) $(NDK64T_LDFLAGS) -soname libndkmedia64test.so -o $@ $(filter %.o,$^) -lOpenSLES -lmediandk \
	    -laaudio -lc -lm -llog
.PHONY: ndk64-media-lib
ndk64-media-lib: $(NDK64T_MEDIA_LIB)
ifneq ($(A8LIB),)
ASYSTEM8_ADD += --add $(NDK64T_APK):system/app/ArcWasmNdkTestArm64.apk \
                --add $(NDK64T_MEDIA_LIB):system/lib64/libndkmedia64test.so
$(A8SYSTEM_IMAGE): $(NDK64T_APK) $(NDK64T_MEDIA_LIB)
NDK64_JAVA_TESTS := tests/java/arcwasm/NdkArm64Test.java tests/java/arcwasm/Arm64AppsTest.java \
                    tests/java/arcwasm/NdkMediaArm64Test.java
ART8_JAVA_TESTS += $(NDK64_JAVA_TESTS)
$(A8FW)/arc-tests.jar: $(NDK64_JAVA_TESTS)
endif
# Real apps' arm64 libraries (not in git; tools/ndk64/prepare_apps.py puts them
# in build/ndk64-apps/NAME): in artvm8's /system/ndk64-apps when there.
NDK64_APPS := $(wildcard $(BUILD)/ndk64-apps/*/lib/*.so $(BUILD)/ndk64-apps/*/base.apk)
ifneq ($(A8LIB)$(NDK64_APPS),)
ASYSTEM8_ADD += $(if $(NDK64_APPS),--add $(BUILD)/ndk64-apps:system/ndk64-apps)
$(A8SYSTEM_IMAGE): $(NDK64_APPS)
endif
ndk64-test: art8
	VM=art8 tools/run_java_tests.sh node NdkArm64Test NdkMediaArm64Test Arm64AppsTest
