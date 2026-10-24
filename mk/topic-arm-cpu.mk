# Topic "arm-cpu" (docs/topics/arm-cpu.md): the ARM CPU that runs Android
# NDK code (armeabi: ARMv5TE ARM/Thumb, soft-float; armeabi-v7a: ARMv7-A
# with Thumb-2, VFPv3 and NEON) inside the wasm process. Implements
# shim/ndk/ndk_cpu.h.
#
#   make ndk-cpu            # build/libndk_cpu.a
#   make ndk-cpu-test       # guest test programs + runner, run under Node
#                           # (interpreter only, then everything translated to wasm),
#                           # then random ARM code run both ways (--fuzz N [SEED])
#   make ndk-cpu-bench      # the MIPS benchmark, interpreter and translator
#   make ndk-cpu-web        # build/ndk-cpu/index.html (serve build/ with tools/serve.py)
#
# Guest test programs are compiled with the host's clang (Apple clang has
# the ARM backend; emsdk's has not) and linked with emsdk's lld as
# position-independent executables that the runner loads anywhere in wasm
# memory. They use compiler-rt's generic builtins from emsdk and bionic's
# libm.a from the NDK's android-9 platform (extracted from ndk.tar.gz).
#
# Included by the Makefile after mk/dalvik.mk and the earlier topics.

NDKCPU := $(BUILD)/ndk-cpu
NDKCPU_SRCS := $(addprefix shim/ndk/cpu/,decode_arm.cc decode_thumb.cc decode_vfp.cc exec.cc jit.cc neon.cc runtime.cc signal.cc)
NDKCPU_OBJS := $(patsubst shim/ndk/cpu/%.cc,$(NDKCPU)/obj/%.o,$(NDKCPU_SRCS)) $(NDKCPU)/obj/jit_pack.o
NDKCPU_HDRS := shim/ndk/ndk_cpu.h shim/ndk/cpu/ndk_cpu_ext.h shim/ndk/cpu/cpu_internal.h shim/ndk/jit_pack.h
# -O2: the interpreter is the hot path of every NDK app.
NDKCPU_CXXFLAGS := -std=gnu++17 -pthread -O2 -g1 -Ishim -Wall -Wno-unused-function

$(NDKCPU)/obj/%.o: shim/ndk/cpu/%.cc $(NDKCPU_HDRS) mk/topic-arm-cpu.mk
	@mkdir -p $(dir $@)
	$(EMXX) $(NDKCPU_CXXFLAGS) -c $< -o $@

# The translators' packs (topic shared-translations), also in libndk_cpu64.a.
$(NDKCPU)/obj/jit_pack.o: shim/ndk/jit_pack.cc shim/ndk/jit_pack.h shim/membudget/mem_budget.h mk/topic-arm-cpu.mk
	@mkdir -p $(dir $@)
	$(EMXX) $(NDKCPU_CXXFLAGS) -c $< -o $@

$(BUILD)/libndk_cpu.a: $(NDKCPU_OBJS)
	@rm -f $@ && $(EMAR) rcs $@ $^

# For mk/topic-ndk-bridge.mk, which links it into dalvikvm (this file does
# not add it to DALVIK_LIBS). The final link needs -sALLOW_TABLE_GROWTH for
# the translation to wasm (see docs/topics/arm-cpu.md).
NDK_CPU_LIB := $(BUILD)/libndk_cpu.a

.PHONY: ndk-cpu ndk-cpu-test ndk-cpu-bench ndk-cpu-web
ndk-cpu: $(BUILD)/libndk_cpu.a

# --- guest toolchain -----------------------------------------------------------------
ARM_CC ?= /usr/bin/clang $(PREFIX_MAP)
ARM_LD ?= $(EMSDK)/upstream/bin/lld -flavor gnu
AOSP44_ARCHIVES ?= $(ARC_SIBLINGS)/arm44-web/out/downloads/aosp
NDK_PLATFORM := $(BUILD)/ndk/9/platforms/android-9/arch-arm
NDK_LIBM := $(NDK_PLATFORM)/usr/lib/libm.a
$(NDK_LIBM):
	@mkdir -p $(BUILD)/ndk
	tar xzf "$(AOSP44_ARCHIVES)/prebuilts/ndk.tar.gz" -C $(BUILD)/ndk 9/platforms/android-9/arch-arm
	@touch $@

CRT_DIR := $(EMSDK)/upstream/emscripten/system/lib/compiler-rt/lib/builtins
CRT_FILES := adddf3 addsf3 subdf3 subsf3 muldf3 mulsf3 divdf3 divsf3 comparedf2 comparesf2 \
             extendsfdf2 truncdfsf2 fixdfsi fixsfsi fixunsdfsi fixunssfsi fixdfdi fixsfdi \
             fixunsdfdi fixunssfdi floatsidf floatsisf floatunsidf floatunsisf floatdidf floatdisf \
             floatundidf floatundisf negdf2 negsf2 divsi3 udivsi3 modsi3 umodsi3 divdi3 udivdi3 \
             moddi3 umoddi3 udivmoddi4 divmoddi4 ashldi3 ashrdi3 lshrdi3 muldi3 cmpdi2 ucmpdi2 \
             clzsi2 ctzsi2 popcountsi2 clzdi2 ctzdi2 popcountdi2 bswapsi2 bswapdi2 fp_mode

GUEST_DIR := tests/ndk/cpu
GUEST_COMMON := t_main t_arith t_memory t_control t_setjmp t_float t_atomic t_callback t_signal t_jit bench rt/guest_rt
GUEST_CFLAGS := -O2 -fPIC -ffreestanding -fno-stack-protector -fno-unwind-tables \
                -fno-asynchronous-unwind-tables -ffp-contract=off -fno-exceptions \
                -Wall -Wno-unused-function -I$(GUEST_DIR)/rt
# Variants: the ABI and the default instruction set of the compiled C code.
NDKCPU_VARIANTS := v5-arm v5-thumb v7-arm v7-thumb v7-neon
ARCH_v5-arm := --target=armv5te-linux-androideabi -mfloat-abi=soft -marm
ARCH_v5-thumb := --target=armv5te-linux-androideabi -mfloat-abi=soft -mthumb
ARCH_v7-arm := --target=armv7a-linux-androideabi -mfloat-abi=softfp -mfpu=vfpv3-d16 -marm
ARCH_v7-thumb := --target=armv7a-linux-androideabi -mfloat-abi=softfp -mfpu=vfpv3-d16 -mthumb
ARCH_v7-neon := --target=armv7a-linux-androideabi -mfloat-abi=softfp -mfpu=neon -mthumb
# Extra objects per architecture (always ARM / always Thumb ones compiled
# with an override of -marm/-mthumb, which comes last).
EXTRA_v5 := t_asm_arm:-marm t_thumb:-mthumb
EXTRA_v7 := t_asm_arm:-marm t_thumb:-mthumb t_thumb2:-mthumb t_vfp:
EXTRA_neon := t_neon:

define NDKCPU_VARIANT
NDKCPU_GOBJ_$(1) := $$(patsubst %,$(NDKCPU)/$(1)/%.o,$$(GUEST_COMMON)) $(NDKCPU)/$(1)/rt/guest_asm.o \
                    $$(NDKCPU_XOBJ_$(1))
$(NDKCPU)/$(1)/%.o: $(GUEST_DIR)/%.c $(GUEST_DIR)/rt/guest.h mk/topic-arm-cpu.mk
	@mkdir -p $$(dir $$@)
	$(ARM_CC) $$(ARCH_$(1)) $(GUEST_CFLAGS) -c $$< -o $$@
$(NDKCPU)/$(1)/rt/guest_asm.o: $(GUEST_DIR)/rt/guest_asm.S
	@mkdir -p $$(dir $$@)
	$(ARM_CC) $$(ARCH_$(1)) -c $$< -o $$@
$(NDKCPU)/$(1)/crt/%.o: $(CRT_DIR)/%.c
	@mkdir -p $$(dir $$@)
	$(ARM_CC) $$(ARCH_$(1)) -O2 -fPIC -ffreestanding -fno-unwind-tables -fno-asynchronous-unwind-tables -w -c $$< -o $$@
$(NDKCPU)/$(1)/libcrt.a: $$(patsubst %,$(NDKCPU)/$(1)/crt/%.o,$(CRT_FILES))
	@rm -f $$@ && $(EMAR) rcs $$@ $$^
$(NDKCPU)/guest/$(1).elf: $$(NDKCPU_GOBJ_$(1)) $(NDKCPU)/$(1)/libcrt.a $(NDK_LIBM)
	@mkdir -p $$(dir $$@)
	$(ARM_LD) -pie --no-dynamic-linker -z notext -z max-page-size=4096 -e guest_main -o $$@ \
	    $$(NDKCPU_GOBJ_$(1)) --start-group $(NDKCPU)/$(1)/libcrt.a $(NDK_LIBM) --end-group
endef

# An extra object: NAME:MODE-FLAG (empty flag: the variant's default).
define NDKCPU_EXTRA
NDKCPU_XOBJ_$(1) += $(NDKCPU)/$(1)/x/$(2).o
$(NDKCPU)/$(1)/x/$(2).o: $(GUEST_DIR)/$(2).c $(GUEST_DIR)/rt/guest.h mk/topic-arm-cpu.mk
	@mkdir -p $$(dir $$@)
	$(ARM_CC) $$(ARCH_$(1)) $(GUEST_CFLAGS) $(3) -c $$< -o $$@
endef

$(foreach v,v5-arm v5-thumb,$(foreach x,$(EXTRA_v5),\
  $(eval $(call NDKCPU_EXTRA,$(v),$(word 1,$(subst :, ,$(x))),$(word 2,$(subst :, ,$(x)))))))
$(foreach v,v7-arm v7-thumb v7-neon,$(foreach x,$(EXTRA_v7),\
  $(eval $(call NDKCPU_EXTRA,$(v),$(word 1,$(subst :, ,$(x))),$(word 2,$(subst :, ,$(x)))))))
$(foreach x,$(EXTRA_neon),\
  $(eval $(call NDKCPU_EXTRA,v7-neon,$(word 1,$(subst :, ,$(x))),$(word 2,$(subst :, ,$(x))))))

$(foreach v,$(NDKCPU_VARIANTS),$(eval $(call NDKCPU_VARIANT,$(v))))
NDKCPU_GUESTS := $(patsubst %,$(NDKCPU)/guest/%.elf,$(NDKCPU_VARIANTS))

# --- runner (Node and browsers) ----------------------------------------------------------
NDKCPU_RUNNER_OBJ := $(NDKCPU)/obj/ndk_cpu_test.o
$(NDKCPU_RUNNER_OBJ): $(GUEST_DIR)/runner/ndk_cpu_test.cc $(NDKCPU_HDRS)
	@mkdir -p $(dir $@)
	$(EMXX) $(NDKCPU_CXXFLAGS) -c $< -o $@
NDKCPU_TEST_LDFLAGS := -pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=8 -sINITIAL_MEMORY=256MB -sALLOW_TABLE_GROWTH \
                       -sSTACK_SIZE=1MB -sEXIT_RUNTIME=1 -sENVIRONMENT=web,worker,node -O2
$(NDKCPU)/ndk_cpu_test.js: $(NDKCPU_RUNNER_OBJ) $(BUILD)/libndk_cpu.a $(NDKCPU_GUESTS)
	$(EMXX) $(NDKCPU_TEST_LDFLAGS) $(NDKCPU_RUNNER_OBJ) $(BUILD)/libndk_cpu.a \
	    --embed-file $(NDKCPU)/guest@/guest -o $@
$(NDKCPU)/index.html: $(GUEST_DIR)/index.html
	cp $< $@
$(NDKCPU)/report.js: shim/report.js
	cp $< $@

# Twice: interpreter only, and every block translated to wasm on first use.
ndk-cpu-test: $(NDKCPU)/ndk_cpu_test.js
	cd $(NDKCPU) && $(NODE) ndk_cpu_test.js --jit 0 $(NDKCPU_ARGS)
	cd $(NDKCPU) && $(NODE) ndk_cpu_test.js --jit 1 $(NDKCPU_ARGS)
	cd $(NDKCPU) && $(NODE) ndk_cpu_test.js --fuzz 3000 --fuzz-neon --fuzz-thumb 2000   # random ARM / Thumb-2 code: interpreted = translated (topic arm-chains)
ndk-cpu-bench: $(NDKCPU)/ndk_cpu_test.js
	cd $(NDKCPU) && $(NODE) ndk_cpu_test.js --no-threads --bench 1000 --jit 0 $(NDKCPU_ARGS)
	cd $(NDKCPU) && $(NODE) ndk_cpu_test.js --no-threads --bench 1000 $(NDKCPU_ARGS)
ndk-cpu-web: $(NDKCPU)/ndk_cpu_test.js $(NDKCPU)/index.html $(NDKCPU)/report.js

# --- static coverage on a real library ----------------------------------------------------
#   make ndk-cpu-scan NDK_SO=path/to/lib.so   (e.g. lib/armeabi/libAngryBirdsClassic.so from an APK)
$(NDKCPU)/obj/ndk_cpu_scan.o: $(GUEST_DIR)/scan/ndk_cpu_scan.cc $(NDKCPU_HDRS)
	@mkdir -p $(dir $@)
	$(EMXX) $(NDKCPU_CXXFLAGS) -c $< -o $@
$(NDKCPU)/ndk_cpu_scan.js: $(NDKCPU)/obj/ndk_cpu_scan.o $(BUILD)/libndk_cpu.a
	$(EMXX) -pthread -O2 -sNODERAWFS=1 -sENVIRONMENT=node -sINITIAL_MEMORY=64MB -sALLOW_MEMORY_GROWTH=1 \
	    $^ -o $@
.PHONY: ndk-cpu-scan
ndk-cpu-scan: $(NDKCPU)/ndk_cpu_scan.js
	$(NODE) $(NDKCPU)/ndk_cpu_scan.js "$(NDK_SO)"
