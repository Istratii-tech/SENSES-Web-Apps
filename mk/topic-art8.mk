# Topic "art8": ART (AOSP 8.1, android-8.1.0_r81) for WebAssembly,
# interpreter only, with libcore 8.1 (OpenJDK-based core-oj.jar and
# core-libart.jar), ICU 58 and the natives libjavacore, libopenjdk,
# libopenjdkjvm. Sources come from third_party/aosp81 (tools/fetch_aosp81.sh);
# see docs/topics/art8.md.
#
#   make art8              # build/art8/artvm8.js (Node)
#   make art8-web          # build/art8-web/: serve with tools/serve.py, open
#                          # index.html?arg=-cp&arg=/system/framework/arc-tests.jar&arg=arcwasm.VmTest
#   make art8-test         # VmTest on ART 8.1 in Node
#   make art8-bigheap-test # objects above 2 GB (a 2.5 GB heap)
#
# Everything is kept apart from the 6.0.1 build (mk/topic-art.mk): objects
# in build/art8/obj, libraries in build/art8/lib, the /system image in
# build/art8/system.img.
#
# The system/core 8.1 libraries are separate archives (A8_SYS_LIBS below) for
# the framework 8.1 (topic fw8-core) to link: libbase8, liblog8, libcutils8,
# libziparchive8, libnativebridge8, libnativeloader8, libnativehelper8,
# libbacktrace8 (a stub), libicuuc58/libicui18n58, libfdlibm8.

A81    := third_party/aosp81
A8BLD  := $(BUILD)/art8
A8OBJ  := $(A8BLD)/obj
A8LIB  := $(A8BLD)/lib
a8obj   = $(patsubst %,$(A8OBJ)/%.o,$(1))

A8_COMMON := -pthread -O2 -g1 -D_GNU_SOURCE -DARC_WASM=1 -DANDROID_SMP=1 \
             -include shim/art8/art8_wasm_compat.h \
             -Wno-unused-parameter -Wno-deprecated-declarations \
             -Wno-unused-variable -Wno-unused-function -Wno-sign-compare \
             -Wno-unused-but-set-variable -Wno-deprecated-register \
             -Wno-unknown-warning-option -Wno-c++11-narrowing \
             -Wno-missing-field-initializers -Wno-format -Wno-invalid-offsetof \
             -Wno-unused-private-field -Wno-unused-local-typedef \
             -Wno-tautological-constant-compare -Wno-null-pointer-arithmetic \
             -Wno-vla-cxx-extension -Wno-deprecated-copy -Wno-unused-const-variable \
             -Wno-nullability-completeness -Wno-implicit-const-int-float-conversion \
             -Wno-enum-constexpr-conversion -Wno-missing-template-arg-list-after-template-kw \
             -Wno-unused-lambda-capture -Wno-defaulted-function-deleted \
             -Wno-thread-safety -Wno-gnu-designator -Wno-c99-designator \
             -Wno-logical-not-parentheses \
             -MMD -MP
A8_CFLAGS   := $(A8_COMMON) -std=gnu11
A8_CXXFLAGS := $(A8_COMMON) -std=gnu++14 -Wno-register

$(A8OBJ)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(EMCC) $(A8_CFLAGS) $(A8FLAGS) -c $< -o $@

$(A8OBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(A8_CXXFLAGS) $(A8FLAGS) -c $< -o $@

$(A8OBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(A8_CXXFLAGS) $(A8FLAGS) -c $< -o $@

-include $(shell find $(A8OBJ) -name '*.d' 2>/dev/null)

bp = $(shell tools/bp_srcs.py $(1) $(2) --prefix $(dir $(1)) $(3) 2>/dev/null)

# --- include paths of system/core 8.1, libnativehelper 8.1 ------------------
# Linux UAPI headers (linux/rtnetlink.h, linux/if_ether.h ...) for constants;
# musl in Emscripten has none. Searched after the system headers.
A8_UAPI := -idirafter $(A81)/bionic/libc/kernel/uapi -idirafter $(A81)/bionic/libc/kernel/uapi/asm-x86
A8_SYS_INCLUDES := -Ishim/art8/include -Ishim/art/include \
    -I$(A81)/system/core/base/include -I$(A81)/system/core/liblog/include \
    -I$(A81)/system/core/libcutils/include -I$(A81)/system/core/libutils/include \
    -I$(A81)/system/core/libsystem/include -I$(A81)/system/core/libziparchive/include \
    -I$(A81)/system/core/libnativebridge/include -I$(A81)/system/core/libnativeloader/include \
    -I$(A81)/system/core/libbacktrace/include \
    -I$(A81)/libnativehelper/include_jni -I$(A81)/libnativehelper/include \
    -I$(A81)/libnativehelper/platform_include -I$(A81)/libnativehelper/include/nativehelper \
    -sUSE_ZLIB=1
A8_SYS_FLAGS := $(A8_SYS_INCLUDES) $(A8_UAPI)

# --- libbase (system/core/base) ----------------------------------------------
A8_BASE_SRCS := $(call bp,$(A81)/system/core/base/Android.bp,libbase,--target linux)
$(A8OBJ)/$(A81)/system/core/base/%: A8FLAGS := $(A8_SYS_FLAGS)
$(A8LIB)/libbase8.a: $(call a8obj,$(A8_BASE_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- liblog ----------------------------------------------------------------------
# Text goes to shim/common/alog_shim.cc (stderr with write(2), filtered by
# ARC_LOG; in libposix_translation); shim/art8/liblog8.cc adds the rest of
# liblog 8.1's API on top of it (the event log is dropped).
A8_LOG_SRCS := shim/art8/liblog8.cc
$(A8OBJ)/shim/art8/liblog8.cc.o: A8FLAGS := $(A8_SYS_FLAGS)
$(A8LIB)/liblog8.a: $(call a8obj,$(A8_LOG_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libcutils (the host list, without the Windows/fs_config pieces) ----------
# ashmem-dev.c rather than the host's ashmem-host.c (temporary files in /tmp,
# which the VFS has not got): regions on posix_translation's /dev/ashmem,
# what CursorWindow (SQLite queries), MemoryFile and Parcel's ashmem_valid()
# of the 8.1 framework need (topics fw8-core, fw8-system).
A8_CUTILS := $(A81)/system/core/libcutils
A8_CUTILS_SRCS := $(filter-out %/dlmalloc_stubs.c %/ashmem-host.c, \
                    $(call bp,$(A8_CUTILS)/Android.bp,libcutils,--target host --target not_windows)) \
                  $(A8_CUTILS)/ashmem-dev.c
$(A8OBJ)/$(A8_CUTILS)/%: A8FLAGS := $(A8_SYS_FLAGS)
$(A8LIB)/libcutils8.a: $(call a8obj,$(A8_CUTILS_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libziparchive (+ libutils' FileMap, which it maps entries with) ----------
# FileMap.cpp is libutils' (topic fw8-core builds libutils); it is in its own
# archive so that a link with libutils 8.1 can leave this one out.
A8_ZIP_SRCS := $(call bp,$(A81)/system/core/libziparchive/Android.bp,libziparchive)
$(A8OBJ)/$(A81)/system/core/libziparchive/%: A8FLAGS := $(A8_SYS_FLAGS) -DZLIB_CONST -D_FILE_OFFSET_BITS=64
$(A8LIB)/libziparchive8.a: $(call a8obj,$(A8_ZIP_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
# FileMap as in a release build (NDEBUG): a stored, empty APK entry at a
# page-aligned offset (Freedoom's assets) is an asset that does not open,
# not an abort on assert(length > 0).
$(A8OBJ)/$(A81)/system/core/libutils/%: A8FLAGS := $(A8_SYS_FLAGS) -DNDEBUG
$(A8LIB)/libfilemap8.a: $(call a8obj,$(A81)/system/core/libutils/FileMap.cpp)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libnativebridge, libnativeloader -----------------------------------------
A8_NB_SRCS := $(A81)/system/core/libnativebridge/native_bridge.cc \
              $(A81)/system/core/libnativeloader/native_loader.cpp
$(A8OBJ)/$(A81)/system/core/libnativebridge/% $(A8OBJ)/$(A81)/system/core/libnativeloader/%: \
    A8FLAGS := $(A8_SYS_FLAGS)
$(A8LIB)/libnativebridge8.a: $(call a8obj,$(A81)/system/core/libnativebridge/native_bridge.cc)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
$(A8LIB)/libnativeloader8.a: $(call a8obj,$(A81)/system/core/libnativeloader/native_loader.cpp)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libbacktrace: a stub (no unwinding of wasm stacks from C++) --------------
$(A8OBJ)/shim/art8/backtrace8.cc.o: A8FLAGS := $(A8_SYS_FLAGS)
$(A8LIB)/libbacktrace8.a: $(call a8obj,shim/art8/backtrace8.cc)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libnativehelper ----------------------------------------------------------------
# JniInvocation (dlopen of libart.so) is not used: artvm8 links ART in.
A8_NH_SRCS := $(filter-out %/JniInvocation.cpp, \
                $(call bp,$(A81)/libnativehelper/Android.bp,libnativehelper,--target linux))
$(A8OBJ)/$(A81)/libnativehelper/%: A8FLAGS := $(A8_SYS_FLAGS)
$(A8LIB)/libnativehelper8.a: $(call a8obj,$(A8_NH_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- ICU 58 (external/icu/icu4c) -------------------------------------------------
# stubdata.c: the empty icudt58_dat; libcore's ICU natives map the data file
# (/system/usr/icu/icudt58l.dat) themselves (udata_setCommonData).
ICU58 := $(A81)/external/icu/icu4c/source
ICU58_INCLUDES := -I$(ICU58)/common -I$(ICU58)/i18n
$(A8OBJ)/$(ICU58)/common/% $(A8OBJ)/$(ICU58)/stubdata/%: A8FLAGS := $(ICU58_INCLUDES) -D_REENTRANT \
    -DU_COMMON_IMPLEMENTATION -DU_HAVE_NL_LANGINFO_CODESET=0 -DU_TIMEZONE=timezone
$(A8OBJ)/$(ICU58)/i18n/%: A8FLAGS := $(ICU58_INCLUDES) -D_REENTRANT -DU_I18N_IMPLEMENTATION
$(A8LIB)/libicuuc58.a: $(call a8obj,$(call bp,$(ICU58)/common/Android.bp,libicuuc) \
                         $(ICU58)/stubdata/stubdata.c)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
$(A8LIB)/libicui18n58.a: $(call a8obj,$(call bp,$(ICU58)/i18n/Android.bp,libicui18n))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- fdlibm (StrictMath) -----------------------------------------------------
A8_FDLIBM_SRCS := $(shell tools/mk_srcs.py $(A81)/external/fdlibm/Android.mk src_files $(A81)/external/fdlibm/ 2>/dev/null)
$(A8OBJ)/$(A81)/external/fdlibm/%: A8FLAGS := -D_IEEE_LIBM -D__LITTLE_ENDIAN -fno-strict-aliasing
$(A8LIB)/libfdlibm8.a: $(call a8obj,$(A8_FDLIBM_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# The system/core 8.1 libraries for other topics (fw8-core) to link.
A8_SYS_LIBS := $(A8LIB)/libnativeloader8.a $(A8LIB)/libnativebridge8.a $(A8LIB)/libnativehelper8.a \
               $(A8LIB)/libziparchive8.a $(A8LIB)/libfilemap8.a $(A8LIB)/libbacktrace8.a \
               $(A8LIB)/libcutils8.a $(A8LIB)/libbase8.a $(A8LIB)/liblog8.a
.PHONY: art8-sys-libs
art8-sys-libs: $(A8_SYS_LIBS) $(A8LIB)/libicuuc58.a $(A8LIB)/libicui18n58.a $(A8LIB)/libfdlibm8.a

# --- libart (art/runtime 8.1) ---------------------------------------------------------
ART8_RT := $(A81)/art/runtime
# Interpreter only: the C++ switch interpreter (mterp is assembly), no JIT
# code, no compilers, no assembly. arch/<isa>/ code is replaced by
# shim/art8/arch_wasm8.cc (entry points, context), the host's runtime_linux.cc
# and thread_linux.cc by shim/art8/*_wasm8.cc.
ART8_SRCS := $(filter-out %/runtime_linux.cc %/thread_linux.cc, \
               $(call bp,$(ART8_RT)/Android.bp,libart,--target host)) \
             $(ART8_RT)/interpreter/mterp/mterp_stub.cc \
             $(ART8_RT)/openjdkjvm/OpenjdkJvm.cc $(A81)/external/lz4/lib/lz4.c \
             $(addprefix shim/art8/,arch_wasm8.cc thread_wasm8.cc runtime_wasm8.cc sigchain_wasm8.cc \
               jni_call_wasm8.cc static_jni_libs_art8.cc intrinsics_wasm8.cc dex_jit_art8.cc) \
             shim/art/interpreter_frames_wasm.cc shim/dexjit/dex_jit_compiler.cc
ART8_OPOUT_HDRS := $(shell tools/bp_srcs.py $(ART8_RT)/Android.bp art_operator_srcs 2>/dev/null)
ART8_OPOUT_SRCS := $(patsubst %.h,$(A8BLD)/gen/%_operator_out.cc,$(ART8_OPOUT_HDRS))
$(A8BLD)/gen/%_operator_out.cc: $(ART8_RT)/%.h $(ART8_RT)/generate-operator-out.py
	@mkdir -p $(dir $@)
	@python3 $(ART8_RT)/generate-operator-out.py $(ART8_RT) $< > $@
# ART8_NDEBUG= (empty) builds ART with its DCHECKs (kIsDebugBuild); after a
# change, remove build/art8/obj/third_party/aosp81/art (make does not track
# flags).
ART8_NDEBUG ?= -DNDEBUG
# As art/build/art.go for a host build (CMS is the default collector there
# too; artvm8 picks the non-concurrent mark-sweep). No read barrier: the
# concurrent copying collector, which needs one, is not used.
ART8_DEFINES := -DBUILDING_LIBART=1 -DART_DEFAULT_GC_TYPE_IS_CMS -DIMT_SIZE=43 \
    -DART_ENABLE_VDEX -DART_STACK_OVERFLOW_GAP_arm=8192 -DART_STACK_OVERFLOW_GAP_arm64=8192 \
    -DART_STACK_OVERFLOW_GAP_mips=16384 -DART_STACK_OVERFLOW_GAP_mips64=16384 \
    -DART_STACK_OVERFLOW_GAP_x86=8192 -DART_STACK_OVERFLOW_GAP_x86_64=8192 \
    -DART_FRAME_SIZE_LIMIT=1736 -DART_BASE_ADDRESS=0x70000000 \
    -DART_BASE_ADDRESS_MIN_DELTA=-0x1000000 -DART_BASE_ADDRESS_MAX_DELTA=0x1000000 \
    -D_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS $(ART8_NDEBUG)
ART8_FLAGS := -I$(ART8_RT) -I$(ART8_RT)/generated -I$(A81)/art -I$(A81)/art/cmdline \
    -I$(A81)/art/sigchainlib -I$(A81)/external/lz4/lib -Ishim/art8 \
    $(A8_SYS_INCLUDES) $(ICU58_INCLUDES) $(ART8_DEFINES) $(A8_UAPI) \
    -include shim/art8/art_runtime_wasm8.h -fno-rtti
$(A8OBJ)/$(ART8_RT)/% $(A8OBJ)/shim/art8/% $(A8OBJ)/$(A8BLD)/gen/%: A8FLAGS := $(ART8_FLAGS)
# The frame stack of flat calls is topic art's (the same for ART 6.0.1 and 8.1).
$(A8OBJ)/shim/art/interpreter_frames_wasm.cc.o: A8FLAGS := $(ART8_FLAGS)
# (lz4: image_space.cc decompresses boot images with it; none is used here)
$(A8OBJ)/$(A81)/external/lz4/%: A8FLAGS :=
$(A8LIB)/libart8.a: $(call a8obj,$(ART8_SRCS) $(ART8_OPOUT_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- BoringSSL (libcrypto only), expat ------------------------------------------------
# libcrypto: what libcore's NativeBN needs (BIGNUM for java.math.BigInteger,
# which java.time uses); C only (OPENSSL_NO_ASM). The archive is for other
# topics too (conscrypt 8.1: add libssl).
BSSL8 := $(A81)/external/boringssl
# <openssl/base.h> of this BoringSSL knows no wasm32; __myriad2__ is its one
# 32-bit target with no CPU-specific code (OPENSSL_32_BIT only), which is
# what wasm32 is here (no patch to a project other topics share).
BSSL8_INCLUDES := -I$(BSSL8)/src/include -D__myriad2__
BSSL8_CRYPTO_SRCS := $(call bp,$(BSSL8)/sources.bp,libcrypto_sources)
$(A8OBJ)/$(BSSL8)/%: A8FLAGS := $(BSSL8_INCLUDES) -I$(BSSL8)/src/crypto -DOPENSSL_NO_ASM \
    -DBORINGSSL_IMPLEMENTATION -DOPENSSL_SMALL -DBORINGSSL_ANDROID_SYSTEM \
    -Wno-implicit-function-declaration
$(A8LIB)/libcrypto8.a: $(call a8obj,$(BSSL8_CRYPTO_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
A8_EXPAT := $(A81)/external/expat
$(A8OBJ)/$(A8_EXPAT)/%: A8FLAGS := -I$(A8_EXPAT) -I$(A8_EXPAT)/lib -DHAVE_EXPAT_CONFIG_H
$(A8LIB)/libexpat8.a: $(call a8obj,$(call bp,$(A8_EXPAT)/Android.bp,libexpat))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libjavacore 8.1 (libcore/luni natives) -------------------------------------------
# All of libcore's sub.mk (NativeBN on libcrypto8, ExpatParser on libexpat8);
# shim/art8/javacore_register8.cc replaces Register.cpp.
A8JC := $(A81)/libcore/luni/src/main/native
A8JC_SRCS := $(filter-out %/Register.cpp, \
               $(shell tools/mk_srcs.py $(A8JC)/sub.mk LOCAL_SRC_FILES $(A8JC)/ 2>/dev/null)) \
             $(A81)/libcore/dalvik/src/main/native/org_apache_harmony_dalvik_NativeTestTarget.cpp \
             shim/art8/javacore_register8.cc
A8JC_FLAGS := $(A8_SYS_FLAGS) $(ICU58_INCLUDES) -I$(A81)/libcore/include -I$(A8JC) \
              -DU_USING_ICU_NAMESPACE=0 $(BSSL8_INCLUDES) -I$(A8_EXPAT)/lib
$(A8OBJ)/$(A81)/libcore/luni/% $(A8OBJ)/$(A81)/libcore/dalvik/%: A8FLAGS := $(A8JC_FLAGS)
$(A8OBJ)/shim/art8/javacore_register8.cc.o: A8FLAGS := $(A8JC_FLAGS)
$(A8LIB)/libjavacore8.a: $(call a8obj,$(A8JC_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libopenjdk 8.1 (libcore/ojluni natives) ------------------------------------------
# Its JNI_OnLoad (ojluni's Register.cpp) is renamed for the table of static
# JNI libraries; JVM_* (libopenjdkjvm) is art/runtime/openjdkjvm, in libart8.
A8OJ := $(A81)/libcore/ojluni/src/main/native
# LinuxWatchService needs inotify (there is none). The C files are written
# for Linux with glibc: the host build of libopenjdk defines __GLIBC__ too.
A8OJ_SRCS := $(filter-out %/LinuxWatchService.c, \
               $(shell tools/mk_srcs.py $(A8OJ)/openjdksub.mk LOCAL_SRC_FILES $(A8OJ)/ 2>/dev/null))
A8OJ_FLAGS := $(A8_SYS_FLAGS) $(ICU58_INCLUDES) -I$(A81)/libcore/include -I$(A8OJ) \
              -I$(A81)/external/fdlibm -D_LARGEFILE64_SOURCE -DLINUX -Wno-parentheses-equality \
              -Wno-constant-logical-operand -Wno-sometimes-uninitialized
$(A8OBJ)/$(A8OJ)/%: A8FLAGS := $(A8OJ_FLAGS)
$(A8OBJ)/$(A8OJ)/%.c.o: A8FLAGS := $(A8OJ_FLAGS) -D__linux__ -D__GLIBC__ -Dsendfile64=sendfile
$(A8OBJ)/$(A8OJ)/Register.cpp.o: A8FLAGS := $(A8OJ_FLAGS) -DJNI_OnLoad=arc_wasm_openjdk8_JNI_OnLoad
# The C files with Java_* functions (natives ART finds by name, e.g.
# sun.nio.fs.UnixNativeDispatcher's) are compiled through generated
# wrappers that also make a table of them (tools/gen_jni_syms.py), which
# shim/art8/static_jni_libs_art8.cc searches for dlsym on libopenjdk.so.
A8OJ_SYM_SRCS := $(shell tools/gen_jni_syms.py filter $(filter %.c,$(A8OJ_SRCS)) 2>/dev/null)
A8JNISYMS := $(A8BLD)/jnisyms
A8OJ_WRAPPERS := $(patsubst $(A8OJ)/%.c,$(A8JNISYMS)/openjdk/%.c,$(A8OJ_SYM_SRCS))
$(A8JNISYMS)/openjdk/%.c: $(A8OJ)/%.c tools/gen_jni_syms.py
	@mkdir -p $(dir $@)
	@python3 tools/gen_jni_syms.py wrapper $< $@ arc_wasm_jni_syms_openjdk_$*
$(A8JNISYMS)/openjdk_index.c: tools/gen_jni_syms.py mk/topic-art8.mk $(A8OJ_WRAPPERS)
	@mkdir -p $(dir $@)
	@python3 tools/gen_jni_syms.py index openjdk8 $@ \
	    $(patsubst $(A8OJ)/%.c,arc_wasm_jni_syms_openjdk_%,$(A8OJ_SYM_SRCS))
$(A8OBJ)/$(A8JNISYMS)/%: A8FLAGS := $(A8OJ_FLAGS) -D__linux__ -D__GLIBC__ -Dsendfile64=sendfile -I.
A8OJ_OBJS := $(call a8obj,$(filter-out $(A8OJ_SYM_SRCS),$(A8OJ_SRCS)) $(A8OJ_WRAPPERS) \
               $(A8JNISYMS)/openjdk_index.c)
$(A8LIB)/libopenjdk8.a: $(A8OJ_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

.PHONY: art8-libs
art8-libs: $(A8LIB)/libart8.a $(A8LIB)/libjavacore8.a $(A8LIB)/libopenjdk8.a art8-sys-libs

# --- Java: core-oj.jar, core-libart.jar and the test programs ------------------------
A8FW := $(A8BLD)/framework
A8_CORE_JARS := $(A8FW)/core-oj.jar $(A8FW)/core-libart.jar
# One run builds both (and core-all-classes.jar, to compile against).
$(A8FW)/core-libart.jar: $(A8FW)/core-oj.jar
$(A8FW)/core-oj.jar: tools/build_core8_jars.py
	python3 -I tools/build_core8_jars.py $(A81) $(A8FW)
A8_CORE_CLASSES := $(A8FW)/core-all-classes.jar
# The tests of topic art (VmTest, Bench, BigHeapTest, ArtSpeedTest) and the
# Java 8 ones (tests/java8): d8 --min-api 26 makes dex 038 (default and
# static interface methods, invoke-polymorphic). Topics add tests that need
# more (ART8_JAVA_TESTS +=, and the class jars to compile them against:
# ART8_TESTS_CLASSPATH +=, both with $(A8FW)/arc-tests.jar as a prerequisite).
ART8_JAVA_TESTS := $(addprefix tests/java/arcwasm/,VmTest.java Hello.java Bench.java \
                     BigHeapTest.java ArtTestNatives.java ArtSpeedTest.java JitTest.java JitCallBench.java JitCatchBench.java) \
                   $(wildcard tests/java8/arcwasm/*.java)
ART8_TESTS_CLASSPATH :=
$(A8FW)/arc-tests.jar: $(ART8_JAVA_TESTS) $(ART_DX_TESTS_DEX) $(A8FW)/core-oj.jar
	rm -rf $(A8BLD)/tests-java && mkdir -p $(A8BLD)/tests-java/classes $(A8BLD)/tests-java/dex
	javac -source 8 -target 8 -Xlint:-options -encoding UTF-8 \
	    -bootclasspath $(A8_CORE_CLASSES) \
	    $(if $(strip $(ART8_TESTS_CLASSPATH)),-classpath $(subst $() ,:,$(strip $(ART8_TESTS_CLASSPATH)))) \
	    -d $(A8BLD)/tests-java/classes $(ART8_JAVA_TESTS)
	cd $(A8BLD)/tests-java/classes && jar cf ../classes.jar .
	"$(D8)" --release --min-api 26 --lib $(A8_CORE_CLASSES) \
	    $(addprefix --lib ,$(ART8_TESTS_CLASSPATH)) \
	    --output $(A8BLD)/tests-java/dex $(A8BLD)/tests-java/classes.jar
	cp $(ART_DX_TESTS_DEX) $(A8BLD)/tests-java/dex/classes2.dex
	rm -f $@ && cd $(A8BLD)/tests-java/dex && zip -q "$(abspath $@)" classes.dex classes2.dex

# --- /system image -----------------------------------------------------------
# build.prop of an 8.1 device (SDK 27); ICU 58's data; the time zone data of
# java.util.TimeZone (libcore's ZoneInfoDB; java.time uses ICU's) and the
# country table of TimeZoneFinder (system/timezone). Topics add files with
# ASYSTEM8_ADD += --add HOST:system/...
A8_TZ := $(A81)/system/timezone/output_data
A8SYSTEM_IMAGE := $(A8BLD)/system.img
# Topic boot-dex8: the jars' classes*.dex stored and 64 KiB-aligned, so that
# ART maps them from the image instead of extracting copies
# (tools/make_system_image.py). A8_IMAGE_OPTS= builds them deflated.
A8_IMAGE_OPTS ?= --store-dex
$(A8BLD)/build.prop: mk/topic-art8.mk
	@mkdir -p $(dir $@)
	printf '%s\n' ro.build.version.sdk=27 ro.build.version.release=8.1.0 \
	    ro.build.version.codename=REL ro.build.version.preview_sdk=0 \
	    ro.build.id=OPM8.181105.002 ro.build.type=user ro.product.cpu.abi=armeabi-v7a \
	    ro.product.cpu.abilist=armeabi-v7a,armeabi ro.product.cpu.abilist32=armeabi-v7a,armeabi \
	    ro.product.model=arc-wasm ro.product.manufacturer=arc-wasm ro.product.brand=arc-wasm \
	    ro.product.name=arcwasm8 ro.product.device=arcwasm8 dalvik.vm.heapsize=512m \
	    dalvik.vm.heapgrowthlimit=256m > $@
# ICU's data as files of their own (as A10_ICU_FILES, topic image-on-demand;
# patches/aosp81/zz-image-on-demand.patch). A8_ICU_FILES= (empty): the .dat, as before.
A8_ICU_FILES ?= 1
A8_ICU_DIR := $(A8BLD)/icu58
$(A8_ICU_DIR)/.split: $(ICU58)/stubdata/icudt58l.dat tools/icu_dat_split.py
	rm -rf $(A8_ICU_DIR) && python3 tools/icu_dat_split.py $< $(A8_ICU_DIR) && touch $@
A8_ICU_ADD := $(if $(A8_ICU_FILES),--add $(A8_ICU_DIR)/icudt58l:system/usr/icu/icudt58l,\
                --add $(ICU58)/stubdata/icudt58l.dat:system/usr/icu/icudt58l.dat)
A8_ICU_DEP := $(if $(A8_ICU_FILES),$(A8_ICU_DIR)/.split,$(ICU58)/stubdata/icudt58l.dat)
$(A8SYSTEM_IMAGE): $(A8_CORE_JARS) $(A8FW)/arc-tests.jar $(A8BLD)/build.prop tools/make_system_image.py \
                   $(A8_TZ)/iana/tzdata $(A8_TZ)/android/tzlookup.xml $(A8_ICU_DEP)
	python3 tools/make_system_image.py $@ $(A8_IMAGE_OPTS) \
	    --add $(A8BLD)/build.prop:system/build.prop \
	    --add $(A8FW)/core-oj.jar:system/framework/core-oj.jar \
	    --add $(A8FW)/core-libart.jar:system/framework/core-libart.jar \
	    --add $(A8FW)/arc-tests.jar:system/framework/arc-tests.jar \
	    $(A8_ICU_ADD) \
	    --add $(A8_TZ)/iana/tzdata:system/usr/share/zoneinfo/tzdata \
	    --add $(A8_TZ)/android/tzlookup.xml:system/usr/share/zoneinfo/tzlookup.xml \
	    $(ASYSTEM8_ADD)

# --- artvm8 -------------------------------------------------------------------
.SECONDEXPANSION:
# The VFS setup (/system, /data on OPFS, /dev/binder) is topic art's
# art/vfs_setup.cc; the ArtTestNatives natives are art/test_natives.cc.
ART8_MAIN_OBJ := $(call a8obj,art8/artvm8.cc art/test_natives.cc)
$(ART8_MAIN_OBJ): A8FLAGS := $(A8_SYS_INCLUDES)
ART8_LIBS := $(A8LIB)/libart8.a $(A8LIB)/libjavacore8.a $(A8LIB)/libopenjdk8.a \
             $(A8_SYS_LIBS) $(A8LIB)/libicui18n58.a $(A8LIB)/libicuuc58.a $(A8LIB)/libfdlibm8.a \
             $(A8LIB)/libcrypto8.a $(A8LIB)/libexpat8.a \
             $(A8LIB)/libart8_shim.a $(BUILD)/libdev_binder.a
# libc functions the stack expects from bionic (shared with topic art).
A8SHIM_SRCS := shim/dalvik/libc_extras.cc shim/art/libc_extras_art.cc shim/art8/libc_extras_art8.cc
$(A8OBJ)/shim/dalvik/% $(A8OBJ)/shim/art/libc_extras_art.cc.o $(A8OBJ)/shim/art8/libc_extras_art8.cc.o: \
    A8FLAGS := $(A8_SYS_INCLUDES) $(A8_UAPI)
$(A8LIB)/libart8_shim.a: $(call a8obj,$(A8SHIM_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
# Memory grows on demand up to 4 GB (addresses above 2 GB must work).
ART8_STACK ?= 1MB
# The table grows: compiled methods of the dex JIT (topic dex-jit).
ART8_LDFLAGS := -pthread -sPROXY_TO_PTHREAD -sUSE_ZLIB=1 -sINITIAL_MEMORY=256MB -sALLOW_TABLE_GROWTH \
                -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB \
                -sSTACK_SIZE=$(ART8_STACK) -sDEFAULT_PTHREAD_STACK_SIZE=1MB -sPTHREAD_POOL_SIZE=12 \
                -sEXIT_RUNTIME=1 -sASSERTIONS=1 --profiling-funcs --js-library $(OPFS_JS) --js-library $(CLOCK_JS) \
                --preload-file $(A8SYSTEM_IMAGE)@/arc/system.img \
                --extern-pre-js $(DVM_PRE_JS) --pre-js art/artvm_pre.js
# ($$(ART_VFS_OBJ): topics included later add to it, e.g. tiktok's /proc/self.)
ART8_LINK_DEPS := $(ART8_MAIN_OBJ) $$(ART_VFS_OBJ) $(KERNEL_OBJ) $(ART8_LIBS) \
                  $(BUILD)/libposix_translation.a $(A8SYSTEM_IMAGE) $(OPFS_JS) $(CLOCK_JS) $(DVM_PRE_JS) \
                  art/artvm_pre.js mk/topic-art8.mk
# Node has no OPFS: the in-memory one of topic activity serves /data.
$(A8BLD)/artvm8.js: $(ART8_LINK_DEPS) shim/opfs/memory_opfs.js
	@mkdir -p $(dir $@)
	$(EMXX) $(ART8_LDFLAGS) --pre-js shim/opfs/memory_opfs.js -sENVIRONMENT=node,worker \
	    $(ART8_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) \
	    -Wl,--start-group $(ART8_LIBS) $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
	$(call wasm_opt,$(@:.js=.wasm))

.PHONY: art8 art8-test art8-bigheap-test
art8: $(A8BLD)/artvm8.js
art8-test: art8
	cd $(A8BLD) && $(NODE) artvm8.js -cp /system/framework/arc-tests.jar arcwasm.VmTest
# Objects above 2 GB of wasm memory (a 2.5 GB heap; options as topic art's).
art8-bigheap-test: art8
	cd $(A8BLD) && $(NODE) artvm8.js $(ART_BIGHEAP_OPTS) -cp /system/framework/arc-tests.jar arcwasm.BigHeapTest

# --- artvm8 for the browser ---------------------------------------------------------
#   make art8-web   # build/art8-web/: serve with tools/serve.py, open
#                   # index.html?arg=-cp&arg=/system/framework/arc-tests.jar&arg=arcwasm.VmTest
A8WEB := $(BUILD)/art8-web
$(A8WEB)/artvm8.js: $(ART8_LINK_DEPS)
	@mkdir -p $(dir $@)
	$(EMXX) $(ART8_LDFLAGS) -sENVIRONMENT=web,worker \
	    $(ART8_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) \
	    -Wl,--start-group $(ART8_LIBS) $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
	$(call wasm_opt,$(@:.js=.wasm))
$(A8WEB)/index.html: art8/web/index.html
	@mkdir -p $(dir $@)
	cp $< $@
$(A8WEB)/report.js: shim/report.js
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: art8-web
art8-web: $(A8WEB)/artvm8.js $(A8WEB)/index.html $(A8WEB)/report.js
