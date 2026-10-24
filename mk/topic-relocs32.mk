# Topic "relocs32": armeabi-v7a libraries built the way new NDK builds are,
# for the 32-bit loader (shim/ndk/ndk_linker.cc): Android's packed APS2
# relocations (DT_ANDROID_REL) and RELR (DT_RELR, DT_ANDROID_RELR), GNU hash
# only, symbol versions, ELF TLS (docs/topics/relocs32.md).
#
#   make relocs32-test-apk    # build/relocs32/ArcWasmNdkTestRelocs.apk
#   make ndk-unit-test        # the loader on these libraries too (Node, stub CPU)
#   make relocs32-test        # NdkRelocTest on Dalvik 4.4, ART 6.0.1 and ART 8.1 (Node)
#
# No NDK: the host's clang against AOSP 8.1's bionic headers (as
# mk/topic-ndk-arm64.mk does for arm64), linked by emsdk's lld with
# --no-undefined against stand-ins of the system libraries with the NDK's
# 32-bit ARM symbols (tools/ndk64/gen_stub_libs.py --arm). libgcc's helpers
# come from tests/ndk's libndkrt.a (mk/topic-ndk-bridge.mk). The APK
# (org.arcwasm.ndktest.relocs, only lib/armeabi-v7a/) carries tests/ndk's
# libndkhelper.so and libndktest.so (NdkTest's checks) and libndkreloc.so
# with its dependency libndkrelocdep.so (tests/ndk/reloc.c, relocdep.c:
# RelocLib's checks). The variants between them: standard DT_RELR tags and
# Android's (--use-android-relr-tags), -z now (DT_FLAGS_1), a version script,
# libndkhelper.so's exports of version "libndkhelper.so", the name of the
# VER_FLG_BASE entry too (GNU ld's --default-symver, as in Firefox's
# libraries; tests/ndk/helper_symver.map).
RL := $(BUILD)/relocs32
RL_STUBS := libc.so libm.so libdl.so liblog.so libstdc++.so
RL_CC := /usr/bin/clang $(PREFIX_MAP) --target=armv7a-linux-androideabi21 -march=armv7-a -mfloat-abi=softfp \
         -mfpu=vfpv3-d16 -mthumb
RL_BIONIC := third_party/aosp81/bionic
RL_CFLAGS := -fpic -nostdinc -isystem $(shell /usr/bin/clang -print-resource-dir)/include \
             -isystem $(RL_BIONIC)/libc/include -isystem $(RL_BIONIC)/libc/arch-arm/include \
             -isystem $(RL_BIONIC)/libc/kernel/uapi -isystem $(RL_BIONIC)/libc/kernel/uapi/asm-arm \
             -isystem $(RL_BIONIC)/libc/kernel/android/uapi \
             -isystem third_party/aosp81/libnativehelper/include_jni \
             -isystem third_party/aosp81/system/core/liblog/include \
             -fno-exceptions -ffunction-sections -fdata-sections -fstack-protector-strong -O2 -g
# The reloc libraries use ELF TLS, which bionic has from API 29 (below,
# clang emulates TLS with __emutls_get_address); the thread pointer from
# TPIDRURO (clang would call __aeabi_read_tp, which bionic does not export).
RL_TLS_CFLAGS := --target=armv7a-linux-androideabi29 -fno-emulated-tls -mtp=cp15
RL_LD := $(EMSDK)/upstream/bin/lld -flavor gnu
RL_LDFLAGS := -shared --no-undefined --hash-style=gnu --pack-dyn-relocs=android+relr --build-id \
              -z max-page-size=16384 -L$(RL)/stubs -L$(RL)/libs/armeabi-v7a
RL_RT := $(BUILD)/ndk/armeabi-v7a/libndkrt.a

# (One run writes every stub; make 3.81 has no grouped targets.)
$(RL)/stubs/libc.so.S: tools/ndk64/gen_stub_libs.py tests/ndk/android81-ndk-symbols.txt
	@mkdir -p $(RL)/stubs
	python3 tools/ndk64/gen_stub_libs.py --arm --add libc.so:__tls_get_addr \
	    tests/ndk/android81-ndk-symbols.txt $(RL)/stubs $(RL_STUBS)
$(addprefix $(RL)/stubs/,$(addsuffix .S,$(filter-out libc.so,$(RL_STUBS)))): $(RL)/stubs/libc.so.S
	@test -f $@
$(RL)/stubs/%.so: $(RL)/stubs/%.so.S
	$(RL_CC) -c $< -o $<.o
	$(RL_LD) -shared -soname $*.so -o $@ $<.o
$(RL)/obj/%.c.o: tests/ndk/%.c
	@mkdir -p $(dir $@)
	$(RL_CC) $(RL_CFLAGS) -c $< -o $@
$(RL)/obj/%.cc.o: tests/ndk/%.cc
	@mkdir -p $(dir $@)
	$(RL_CC) $(RL_CFLAGS) -std=c++11 -fno-rtti -nostdinc++ -c $< -o $@
$(RL)/obj/reloc.c.o $(RL)/obj/relocdep.c.o: RL_CFLAGS += $(RL_TLS_CFLAGS)
$(RL)/obj/crt_so.o: tests/ndk64/crt_so.c
	@mkdir -p $(dir $@)
	$(RL_CC) $(RL_CFLAGS) -c $< -o $@

RL_LIBDIR := $(RL)/libs/armeabi-v7a
RL_LIBS := $(addprefix $(RL_LIBDIR)/,libndkhelper.so libndktest.so libndkrelocdep.so libndkreloc.so)
RL_DEPS := $(RL)/obj/crt_so.o $(RL_RT) $(addprefix $(RL)/stubs/,$(RL_STUBS))
$(RL_LIBDIR)/libndkhelper.so: $(RL)/obj/helper.c.o tests/ndk/helper_symver.map $(RL_DEPS)
	@mkdir -p $(dir $@)
	$(RL_LD) $(RL_LDFLAGS) --version-script tests/ndk/helper_symver.map -soname libndkhelper.so -o $@ $(filter %.o,$^) $(RL_RT) \
	    -lc -lm -ldl -llog
$(RL_LIBDIR)/libndktest.so: $(RL)/obj/ndktest.c.o $(RL)/obj/ndktest_cxx.cc.o $(RL_LIBDIR)/libndkhelper.so $(RL_DEPS)
	$(RL_LD) $(RL_LDFLAGS) -soname libndktest.so -o $@ $(filter %.o,$^) $(RL_RT) -lndkhelper -lstdc++ \
	    -lc -lm -ldl -llog
$(RL_LIBDIR)/libndkrelocdep.so: $(RL)/obj/relocdep.c.o tests/ndk/relocdep.map $(RL_DEPS)
	@mkdir -p $(dir $@)
	$(RL_LD) $(RL_LDFLAGS) --version-script tests/ndk/relocdep.map -soname libndkrelocdep.so -o $@ \
	    $(filter %.o,$^) $(RL_RT) -lc
$(RL_LIBDIR)/libndkreloc.so: $(RL)/obj/reloc.c.o $(RL_LIBDIR)/libndkrelocdep.so $(RL_DEPS)
	$(RL_LD) $(RL_LDFLAGS) --use-android-relr-tags -z now -soname libndkreloc.so -o $@ $(filter %.o,$^) \
	    $(RL_RT) -lndkrelocdep -lc -ldl

RL_APK := $(RL)/ArcWasmNdkTestRelocs.apk
$(RL_APK): $(shell find tests/ndk/apk -type f) $(RL_LIBS) tools/build_test_apk.sh
	rm -rf $(RL)/src && mkdir -p $(RL) && cp -R tests/ndk/apk $(RL)/src
	sed -i.orig 's/package="org.arcwasm.ndktest"/package="org.arcwasm.ndktest.relocs"/' \
	    $(RL)/src/AndroidManifest.xml
	AAPT="$(AAPT)" D8="$(D8)" ANDROID_JAR="$(ANDROID_JAR)" \
	    EXTRA_ASSETS="$(foreach l,$(RL_LIBS),$(l):lib/armeabi-v7a/$(notdir $(l)))" \
	    tools/build_test_apk.sh $(RL)/src $(RL)/work $@

# The loader's unit test (stub CPU) reads the relocated words back too.
NDK_UNIT_DIRS += $(RL_LIBDIR)
ndk-unit-test: $(RL_LIBS)

# In /system/app of the three 32-bit stacks; NdkRelocTest in their test jars
# (Dalvik compiles all of tests/java).
DSYSTEM_ADD += --add $(RL_APK):system/app/ArcWasmNdkTestRelocs.apk
$(DSYSTEM_IMAGE): $(RL_APK)
ASYSTEM_ADD += --add $(RL_APK):system/app/ArcWasmNdkTestRelocs.apk
$(ASYSTEM_IMAGE): $(RL_APK)
ART_JAVA_TESTS += tests/java/arcwasm/NdkRelocTest.java
$(AFW)/arc-tests.jar: tests/java/arcwasm/NdkRelocTest.java
ifneq ($(A8LIB),)
ASYSTEM8_ADD += --add $(RL_APK):system/app/ArcWasmNdkTestRelocs.apk
$(A8SYSTEM_IMAGE): $(RL_APK)
ART8_JAVA_TESTS += tests/java/arcwasm/NdkRelocTest.java
$(A8FW)/arc-tests.jar: tests/java/arcwasm/NdkRelocTest.java
endif

.PHONY: relocs32-test-apk relocs32-test
relocs32-test-apk: $(RL_APK)
relocs32-test: dalvik art art8
	tools/run_java_tests.sh node NdkRelocTest
	VM=art tools/run_java_tests.sh node NdkRelocTest
	VM=art8 tools/run_java_tests.sh node NdkRelocTest
