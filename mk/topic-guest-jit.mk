# Topic "guest-jit": code apps generate at run time, and the per-app JIT
# switch (docs/topics/guest-jit.md).
#
#   make guest-jit-libs     # NdkJitTest's libraries
#   make guest-jit-test     # NdkJitTest on 4.4, 6.0.1, 8.1 and 10 (Node), JIT on and off
#
# NdkJitTest's libraries: tests/guest-jit/ndkjit.c for armeabi-v7a
# (libndkjit.so, built like topic relocs32's libraries) and arm64-v8a
# (libndkjit64.so, like topic ndk-arm64's), loaded with System.load from
# /system/lib and /system/lib64 (the arm64 one on the stacks with the AArch64
# CPU). The stubs, crt_so.o and libndkrt.a are those topics' (their rules
# come later in mk/, so the paths are spelled out here).
GJ := $(BUILD)/guest-jit
GJ_LIB32 := $(GJ)/lib/libndkjit.so
GJ_LIB64 := $(GJ)/lib64/libndkjit64.so
GJ_RL := $(BUILD)/relocs32
GJ_N64 := $(BUILD)/ndk64
GJ_RT32 := $(BUILD)/ndk/armeabi-v7a/libndkrt.a

$(GJ)/obj32/ndkjit.o: tests/guest-jit/ndkjit.c mk/topic-guest-jit.mk
	@mkdir -p $(dir $@)
	$(RL_CC) $(RL_CFLAGS) -c $< -o $@
$(GJ_LIB32): $(GJ)/obj32/ndkjit.o $(GJ_RL)/obj/crt_so.o $(GJ_RT32) \
        $(addprefix $(GJ_RL)/stubs/,libc.so libdl.so liblog.so)
	@mkdir -p $(dir $@)
	$(RL_LD) -shared --no-undefined --hash-style=gnu --build-id -z max-page-size=16384 -L$(GJ_RL)/stubs \
	    -soname libndkjit.so -o $@ $(filter %.o,$^) $(GJ_RT32) -lc -ldl -llog

$(GJ)/obj64/ndkjit.o: tests/guest-jit/ndkjit.c mk/topic-guest-jit.mk
	@mkdir -p $(dir $@)
	$(A64_CC) $(NDK64T_CFLAGS) -DNDKJIT64 -c $< -o $@
$(GJ_LIB64): $(GJ)/obj64/ndkjit.o $(GJ_N64)/obj/crt_so.o $(addprefix $(GJ_N64)/stubs/,libc.so libdl.so liblog.so)
	@mkdir -p $(dir $@)
	$(A64_LD) -shared --no-undefined --hash-style=both -z max-page-size=4096 -L$(GJ_N64)/stubs \
	    -soname libndkjit64.so -o $@ $(filter %.o,$^) -lc -ldl -llog

GJ_TEST := tests/java/arcwasm/NdkJitTest.java
# 4.4 (Dalvik compiles all of tests/java) and 6.0.1: the ARM library.
DSYSTEM_ADD += --add $(GJ_LIB32):system/lib/libndkjit.so
$(DSYSTEM_IMAGE): $(GJ_LIB32)
ASYSTEM_ADD += --add $(GJ_LIB32):system/lib/libndkjit.so
$(ASYSTEM_IMAGE): $(GJ_LIB32)
ART_JAVA_TESTS += $(GJ_TEST)
$(AFW)/arc-tests.jar: $(GJ_TEST)
# 8.1 and 10: both.
ifneq ($(A8LIB),)
ASYSTEM8_ADD += --add $(GJ_LIB32):system/lib/libndkjit.so --add $(GJ_LIB64):system/lib64/libndkjit64.so
$(A8SYSTEM_IMAGE): $(GJ_LIB32) $(GJ_LIB64)
ART8_JAVA_TESTS += $(GJ_TEST)
$(A8FW)/arc-tests.jar: $(GJ_TEST)
endif
ASYSTEM10_ADD += --add $(GJ_LIB32):system/lib/libndkjit.so --add $(GJ_LIB64):system/lib64/libndkjit64.so
$(A10SYSTEM_IMAGE): $(GJ_LIB32) $(GJ_LIB64)
ART10_JAVA_TESTS += $(GJ_TEST)
$(A10FW)/arc-tests.jar: $(GJ_TEST)

.PHONY: guest-jit-libs guest-jit-test
guest-jit-libs: $(GJ_LIB32) $(GJ_LIB64)
guest-jit-test: dalvik art art8 art10
	for vm in dalvik art art8 art10; do \
	  VM=$$vm tools/run_java_tests.sh node NdkJitTest; \
	  JIT=0 VM=$$vm tools/run_java_tests.sh node NdkJitTest; \
	done
