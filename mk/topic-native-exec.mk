# Topic native-exec (docs/topics/native-exec.md): what Android apps need to
# unpack and run native programs of their own.
#
#   shim/opfs/opfs_file.cc                symlinks and permission bits on /data
#                                         (OPFS): the handler's own table
#   upstream/src/posix_translation/       chmod/fchmod to the handler, relative
#                                         symlink targets, mmap at 4 KB offsets,
#                                         a spawned process's own descriptors 0-2
#   shim/multiproc/light_process.cc       spawned processes: parent, exit status,
#                                         waitpid
#   shim/kernel/arc_kernel.cc             wait4, getppid, a process's own cwd
#   shim/ndk/ndk_exec.cc                  arc_spawn: ARM/arm64 executables run as
#                                         spawned processes (Android 10's bridge)
#   patches/aosp10/zz-spawn.patch         Os.sysconf(_SC_PAGESIZE) = 4096,
#                                         ProcessBuilder -> arc_spawn
#
#   make native-exec-progs  the test programs (build/native-exec)
#   make native-exec-test   NativeExecTest on art10 in Node

# --- the bridge part: in Android 10's bridge, kept in the link -----------------------
ifneq ($(F10SLIB),)
NE_NDK_OBJ := $(call f10sobj,shim/ndk/ndk_exec.cc)
$(NE_NDK_OBJ): F10SXFLAGS := -std=gnu++17 -Ishim/ndk -Ishim/gles -Wno-c++17-extensions -UARC_WASM_FW6
$(NE_NDK_OBJ): shim/multiproc/include/arc_lproc.h shim/ndk/ndk_bridge.h
$(F10SLIB)/libndk_bridge_art10.a: $(NE_NDK_OBJ)
ART10_LDFLAGS += -Wl,-u,ndk_exec_linked -Wl,-u,arc_spawn
endif

# --- test programs ----------------------------------------------------------------------
# exectest: an arm64-v8a PIE executable (crt_exe64.c as bionic's crtbegin,
# against topic ndk-arm64's stub libc.so) and an armeabi-v7a one (the NDK's
# crtbegin_dynamic.o and libc.so of android-9, as topic ndk-bridge's test
# libraries); static64: a static-pie arm64 program with its own syscalls.
NE := $(BUILD)/native-exec
# (topic ndk-arm64's and ndk-bridge's mk files come after this one: their
# directories spelled out where make needs them at once)
NE_STUBS64 := $(BUILD)/ndk64/stubs
NE_SYSROOT32 := $(BUILD)/ndk/9/platforms/android-9/arch-arm/usr
NE_PROGS := $(NE)/arm64/exectest $(NE)/arm64/static64 $(NE)/armeabi-v7a/exectest
$(NE)/arm64/%.o: tests/native-exec/%.c $(NE_STUBS64)/libc.so
	@mkdir -p $(dir $@)
	$(A64_CC) $(NDK64T_CFLAGS) -fPIE -c $< -o $@
$(NE)/arm64/exectest: $(NE)/arm64/crt_exe64.o $(NE)/arm64/exectest.o $(NE_STUBS64)/libc.so
	$(A64_LD) -pie --dynamic-linker /system/bin/linker64 --hash-style=both -z max-page-size=4096 \
	    -e _start -o $@ $(NE)/arm64/crt_exe64.o $(NE)/arm64/exectest.o -L$(NE_STUBS64) -lc
$(NE)/arm64/static64: tests/native-exec/static64.S
	@mkdir -p $(dir $@)
	$(A64_CC) -c $< -o $@.o
	$(A64_LD) -pie --no-dynamic-linker -z max-page-size=4096 -e _start -o $@ $@.o
$(NE)/armeabi-v7a/exectest.o: tests/native-exec/exectest.c | $(NE_SYSROOT32)/include/jni.h
	@mkdir -p $(dir $@)
	$(NDKT_CC_armeabi-v7a) -fPIE -c $< -o $@
$(NE)/armeabi-v7a/exectest: $(NE)/armeabi-v7a/exectest.o $(BUILD)/ndk/armeabi-v7a/libndkrt.a
	$(EMSDK)/upstream/bin/lld -flavor gnu -pie --dynamic-linker /system/bin/linker --hash-style=sysv \
	    -z max-page-size=4096 -z notext -o $@ $(NE_SYSROOT32)/lib/crtbegin_dynamic.o $< \
	    $(BUILD)/ndk/armeabi-v7a/libndkrt.a -L$(NE_SYSROOT32)/lib -lc $(NE_SYSROOT32)/lib/crtend_android.o
.PHONY: native-exec-progs
native-exec-progs: $(NE_PROGS)

ifneq ($(A10SYSTEM_IMAGE),)
ASYSTEM10_ADD += --add $(NE)/arm64/exectest:system/etc/arcwasm/native-exec/arm64/exectest \
                 --add $(NE)/arm64/static64:system/etc/arcwasm/native-exec/arm64/static64 \
                 --add $(NE)/armeabi-v7a/exectest:system/etc/arcwasm/native-exec/armeabi-v7a/exectest
$(A10SYSTEM_IMAGE): $(NE_PROGS)
ART10_JAVA_TESTS += tests/java10/arcwasm/NativeExecTest.java
$(A10FW)/arc-tests.jar: tests/java10/arcwasm/NativeExecTest.java
endif

.PHONY: native-exec-test
native-exec-test: art10
	VM=art10 tools/run_java_tests.sh node NativeExecTest
