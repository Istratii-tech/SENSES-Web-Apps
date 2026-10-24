# Topic "art10": ART (AOSP 10, android-10.0.0_r47, API 29) for WebAssembly,
# interpreter only, with libcore 10 (OpenJDK-based core-oj.jar and
# core-libart.jar), ICU 63 and the natives libjavacore, libopenjdk,
# libopenjdkjvm. Sources come from third_party/aosp10 (tools/fetch_aosp10.sh);
# see docs/topics/art10.md. The decisions are topic art8's (mk/topic-art8.mk).
#
#   make art10              # build/art10/artvm10.js (Node)
#   make art10-web          # build/art10-web/: serve with tools/serve.py, open
#                           # index.html?arg=-cp&arg=/system/framework/arc-tests.jar&arg=arcwasm.VmTest
#   make art10-test         # VmTest on ART 10 in Node
#   make art10-bigheap-test # objects above 2 GB (a 2.5 GB heap)
#
# Everything is kept apart from the 6.0.1 and 8.1 builds: objects in
# build/art10/obj, libraries in build/art10/lib, the /system image in
# build/art10/system.img.
#
# The system/core 10 libraries are separate archives (A10_SYS_LIBS below) for
# the framework 10 (topic fw10-core) to link: libbase10, liblog10,
# libcutils10, libziparchive10, libnativebridge10, libnativeloader10,
# libnativehelper10, libbacktrace10 (a stub), libicuuc63/libicui18n63,
# libfdlibm10.

A10    := third_party/aosp10
A10BLD := $(BUILD)/art10
A10OBJ := $(A10BLD)/obj
A10LIB := $(A10BLD)/lib
a10obj  = $(patsubst %,$(A10OBJ)/%.o,$(1))

A10_COMMON := -pthread -O2 -g1 -D_GNU_SOURCE -DARC_WASM=1 -DANDROID_SMP=1 \
              -include shim/art10/art10_wasm_compat.h \
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
              -Wno-logical-not-parentheses -Wno-deprecated-this-capture \
              -Wno-inconsistent-missing-override -Wno-ambiguous-reversed-operator \
              -Wno-deprecated-literal-operator -Wno-nontrivial-memcall \
              -Wno-invalid-partial-specialization \
              -MMD -MP
A10_CFLAGS   := $(A10_COMMON) -std=gnu11
A10_CXXFLAGS := $(A10_COMMON) -std=gnu++17 -Wno-register

$(A10OBJ)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(EMCC) $(A10_CFLAGS) $(A10FLAGS) -c $< -o $@

$(A10OBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(A10_CXXFLAGS) $(A10FLAGS) -c $< -o $@

$(A10OBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(A10_CXXFLAGS) $(A10FLAGS) -c $< -o $@

-include $(shell find $(A10OBJ) -name '*.d' 2>/dev/null)

bp10 = $(shell tools/bp_srcs.py $(1) $(2) --prefix $(dir $(1)) $(3) 2>/dev/null)

# --- include paths of system/core 10, libnativehelper 10 -------------------------
# Linux UAPI headers (linux/rtnetlink.h, linux/if_ether.h ...) for constants;
# musl in Emscripten has none. Searched after the system headers.
A10_UAPI := -idirafter $(A10)/bionic/libc/kernel/uapi -idirafter $(A10)/bionic/libc/kernel/uapi/asm-x86 \
            -idirafter $(A10)/bionic/libc/kernel/android/uapi
A10_SYS_INCLUDES := -Ishim/art10/include -Ishim/art8/include -Ishim/art/include \
    -I$(A10)/system/core/base/include -I$(A10)/system/core/liblog/include \
    -I$(A10)/system/core/libcutils/include -I$(A10)/system/core/libutils/include \
    -I$(A10)/system/core/libsystem/include -I$(A10)/system/core/libziparchive/include \
    -I$(A10)/system/core/libnativebridge/include -I$(A10)/system/core/libnativeloader/include \
    -I$(A10)/system/core/libbacktrace/include -I$(A10)/system/core/libprocinfo/include \
    -I$(A10)/libnativehelper/include_jni -I$(A10)/libnativehelper/include \
    -I$(A10)/libnativehelper/header_only_include -I$(A10)/libnativehelper/platform_include \
    -sUSE_ZLIB=1
A10_SYS_FLAGS := $(A10_SYS_INCLUDES) $(A10_UAPI)

# --- libbase (system/core/base) -----------------------------------------------------
A10_BASE_SRCS := $(call bp10,$(A10)/system/core/base/Android.bp,libbase,--target linux)
$(A10OBJ)/$(A10)/system/core/base/%: A10FLAGS := $(A10_SYS_FLAGS)
$(A10LIB)/libbase10.a: $(call a10obj,$(A10_BASE_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- liblog -------------------------------------------------------------------------
# Text goes to shim/common/alog_shim.cc (stderr with write(2), filtered by
# ARC_LOG; in libposix_translation); shim/art10/liblog10.cc adds the rest of
# liblog 10's API on top of it (the event log is dropped).
$(A10OBJ)/shim/art10/liblog10.cc.o: A10FLAGS := $(A10_SYS_FLAGS)
$(A10LIB)/liblog10.a: $(call a10obj,shim/art10/liblog10.cc)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libcutils (the host list, without the Windows/fs_config pieces) --------------
# ashmem-dev.cpp rather than the host's ashmem-host.cpp, as for 8.1: regions on
# posix_translation's /dev/ashmem.
A10_CUTILS := $(A10)/system/core/libcutils
A10_CUTILS_SRCS := $(filter-out %/ashmem-host.cpp %/trace-host.cpp, \
                     $(call bp10,$(A10_CUTILS)/Android.bp,libcutils,--target host --target not_windows)) \
                   $(A10_CUTILS)/ashmem-dev.cpp $(A10_CUTILS)/trace-host.cpp
$(A10OBJ)/$(A10_CUTILS)/%: A10FLAGS := $(A10_SYS_FLAGS)
$(A10LIB)/libcutils10.a: $(call a10obj,$(A10_CUTILS_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libziparchive (+ libutils' FileMap, which it maps entries with) --------------
A10_ZIP_SRCS := $(call bp10,$(A10)/system/core/libziparchive/Android.bp,libziparchive)
$(A10OBJ)/$(A10)/system/core/libziparchive/%: A10FLAGS := $(A10_SYS_FLAGS) -DZLIB_CONST -D_FILE_OFFSET_BITS=64
$(A10LIB)/libziparchive10.a: $(call a10obj,$(A10_ZIP_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
# FileMap as in a release build (NDEBUG): a stored, empty APK entry at a
# page-aligned offset (Freedoom's assets) is an asset that does not open,
# not an abort on assert(length > 0).
$(A10OBJ)/$(A10)/system/core/libutils/%: A10FLAGS := $(A10_SYS_FLAGS) -DNDEBUG
$(A10LIB)/libfilemap10.a: $(call a10obj,$(A10)/system/core/libutils/FileMap.cpp)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libnativebridge, libnativeloader ---------------------------------------------
# (ABI_STRING: the ABI of the runtime, a global flag of the Android build;
# NeedsNativeBridge compares an app's ISA with it.)
$(A10OBJ)/$(A10)/system/core/libnativebridge/% $(A10OBJ)/$(A10)/system/core/libnativeloader/%: \
    A10FLAGS := $(A10_SYS_FLAGS) -DABI_STRING='"wasm32"'
$(A10LIB)/libnativebridge10.a: $(call a10obj,$(A10)/system/core/libnativebridge/native_bridge.cc)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
$(A10LIB)/libnativeloader10.a: $(call a10obj,$(A10)/system/core/libnativeloader/native_loader.cpp)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libbacktrace: a stub (no unwinding of wasm stacks from C++) ------------------
$(A10OBJ)/shim/art10/backtrace10.cc.o: A10FLAGS := $(A10_SYS_FLAGS)
$(A10LIB)/libbacktrace10.a: $(call a10obj,shim/art10/backtrace10.cc)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libnativehelper ------------------------------------------------------------------
# JniInvocation (dlopen of libart.so) is not used: artvm10 links ART in.
# libnativehelper and libjavacore (libcore 10) each have a class JniConstants
# of their own, separate libraries on a device; linked together here, the
# helper's is renamed.
A10_NH_SRCS := $(filter-out %/JniInvocation.cpp, \
                 $(call bp10,$(A10)/libnativehelper/Android.bp,libnativehelper))
$(A10OBJ)/$(A10)/libnativehelper/%: A10FLAGS := $(A10_SYS_FLAGS) -DJniConstants=NativeHelperJniConstants
$(A10LIB)/libnativehelper10.a: $(call a10obj,$(A10_NH_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- ICU 63 (external/icu/icu4c) ------------------------------------------------------
# stubdata.cpp: the empty icudt63_dat; libcore's ICU natives map the data file
# (/system/usr/icu/icudt63l.dat) themselves (udata_setCommonData).
ICU63 := $(A10)/external/icu/icu4c/source
ICU63_INCLUDES := -I$(ICU63)/common -I$(ICU63)/i18n
$(A10OBJ)/$(ICU63)/common/% $(A10OBJ)/$(ICU63)/stubdata/%: A10FLAGS := $(ICU63_INCLUDES) -D_REENTRANT \
    -DU_COMMON_IMPLEMENTATION -DU_HAVE_NL_LANGINFO_CODESET=0 -DU_TIMEZONE=timezone
$(A10OBJ)/$(ICU63)/i18n/%: A10FLAGS := $(ICU63_INCLUDES) -D_REENTRANT -DU_I18N_IMPLEMENTATION
$(A10LIB)/libicuuc63.a: $(call a10obj,$(wildcard $(ICU63)/common/*.cpp) \
                          $(ICU63)/stubdata/stubdata.cpp)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
$(A10LIB)/libicui18n63.a: $(call a10obj,$(wildcard $(ICU63)/i18n/*.cpp))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- fdlibm (StrictMath) ----------------------------------------------------------
A10_FDLIBM_SRCS := $(call bp10,$(A10)/external/fdlibm/Android.bp,libfdlibm)
$(A10OBJ)/$(A10)/external/fdlibm/%: A10FLAGS := -D_IEEE_LIBM -D__LITTLE_ENDIAN -fno-strict-aliasing
$(A10LIB)/libfdlibm10.a: $(call a10obj,$(A10_FDLIBM_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# The system/core 10 libraries for other topics (fw10-core) to link.
A10_SYS_LIBS := $(A10LIB)/libnativeloader10.a $(A10LIB)/libnativebridge10.a $(A10LIB)/libnativehelper10.a \
                $(A10LIB)/libziparchive10.a $(A10LIB)/libfilemap10.a $(A10LIB)/libbacktrace10.a \
                $(A10LIB)/libcutils10.a $(A10LIB)/libbase10.a $(A10LIB)/liblog10.a
.PHONY: art10-sys-libs
art10-sys-libs: $(A10_SYS_LIBS) $(A10LIB)/libicuuc63.a $(A10LIB)/libicui18n63.a $(A10LIB)/libfdlibm10.a

# --- libart (art/runtime 10, with libartbase, libdexfile, libprofile) -------------------
# In 10 parts of the runtime moved to libraries of their own: libartbase
# (base/, arch/instruction_set), libdexfile (dex/), libprofile; libartpalette
# is the host's fake. All go into libart10.a.
A10ART := $(A10)/art
ART10_RT := $(A10ART)/runtime
ART10_BASE_SRCS := $(addprefix $(A10ART)/libartbase/,$(shell tools/bp_srcs.py \
                     $(A10ART)/libartbase/Android.bp libartbase_defaults 2>/dev/null)) \
                   $(A10ART)/libartbase/base/mem_map_unix.cc
ART10_DEX_SRCS := $(call bp10,$(A10ART)/libdexfile/Android.bp,libdexfile_defaults)
ART10_PROF_SRCS := $(call bp10,$(A10ART)/libprofile/Android.bp,libprofile_defaults)
# Interpreter only: the C++ switch interpreter (mterp is assembly), no JIT
# code, no compilers, no assembly. arch/<isa>/ code is replaced by
# shim/art10/arch_wasm10.cc (entry points, context), the host's
# runtime_linux.cc and thread_linux.cc by shim/art10/*_wasm10.cc.
ART10_SRCS := $(filter-out %/runtime_linux.cc %/thread_linux.cc, \
                $(call bp10,$(ART10_RT)/Android.bp,libart_defaults,--target host --target linux)) \
              $(ART10_RT)/interpreter/mterp/mterp_stub.cc \
              $(A10ART)/openjdkjvm/OpenjdkJvm.cc $(A10)/external/lz4/lib/lz4.c \
              $(ART10_BASE_SRCS) $(ART10_DEX_SRCS) $(ART10_PROF_SRCS) \
              $(A10ART)/libartpalette/system/palette_fake.cc \
              $(addprefix shim/art10/,arch_wasm10.cc thread_wasm10.cc runtime_wasm10.cc sigchain_wasm10.cc \
                jni_call_wasm10.cc static_jni_libs_art10.cc intrinsics_wasm10.cc quickening_wasm10.cc \
                interpreter_frames_wasm10.cc dex_jit_art10.cc dormant_wasm10.cc) shim/dexjit/dex_jit_compiler.cc \
              shim/art/interpreter_frames_wasm.cc
# The generated operator<< of enums (gensrcs of each library).
ART10_OPOUT := $(foreach lib,runtime:art_operator_srcs libartbase:art_libartbase_operator_srcs \
                  libdexfile:dexfile_operator_srcs, \
                 $(addprefix $(word 1,$(subst :, ,$(lib)))/,$(shell tools/bp_srcs.py \
                   $(A10ART)/$(word 1,$(subst :, ,$(lib)))/Android.bp $(word 2,$(subst :, ,$(lib))) 2>/dev/null)))
ART10_OPOUT_SRCS := $(patsubst %.h,$(A10BLD)/gen/%_operator_out.cc,$(ART10_OPOUT))
$(A10BLD)/gen/%_operator_out.cc: $(A10ART)/%.h $(A10ART)/tools/generate_operator_out.py
	@mkdir -p $(dir $@)
	@python3 $(A10ART)/tools/generate_operator_out.py $(A10ART)/$(word 1,$(subst /, ,$*)) $< > $@
# ART10_NDEBUG= (empty) builds ART with its DCHECKs (kIsDebugBuild); after a
# change, remove build/art10/obj/third_party/aosp10/art (make does not track
# flags).
ART10_NDEBUG ?= -DNDEBUG
# As art/build/art.go for a host build (CMS is the default collector there
# too; artvm10 picks the non-concurrent mark-sweep). No read barrier: the
# concurrent copying collector, which needs one, is not used. No compact
# dex (dex files are opened as they are). ART_USE_CXX_INTERPRETER: the switch
# interpreter, not mterp (assembly).
ART10_DEFINES := -DBUILDING_LIBART=1 -DART_DEFAULT_GC_TYPE_IS_CMS -DIMT_SIZE=43 \
    -DART_USE_CXX_INTERPRETER=1 \
    -DART_DEFAULT_COMPACT_DEX_LEVEL=none -DUSE_D8_DESUGAR=1 \
    -DART_STACK_OVERFLOW_GAP_arm=8192 -DART_STACK_OVERFLOW_GAP_arm64=8192 \
    -DART_STACK_OVERFLOW_GAP_mips=16384 -DART_STACK_OVERFLOW_GAP_mips64=16384 \
    -DART_STACK_OVERFLOW_GAP_x86=8192 -DART_STACK_OVERFLOW_GAP_x86_64=8192 \
    -DART_FRAME_SIZE_LIMIT=1736 -DART_BASE_ADDRESS=0x70000000 \
    -DART_BASE_ADDRESS_MIN_DELTA=-0x1000000 -DART_BASE_ADDRESS_MAX_DELTA=0x1000000 \
    -D_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS $(ART10_NDEBUG)
ART10_FLAGS := -I$(ART10_RT) -I$(A10ART) -I$(A10ART)/libartbase -I$(A10ART)/libdexfile \
    -I$(A10ART)/libprofile -I$(A10ART)/libelffile -I$(A10ART)/libartpalette/include -I$(A10ART)/cmdline \
    -I$(A10ART)/sigchainlib -I$(A10)/external/lz4/lib -Ishim/art10 \
    $(A10_SYS_INCLUDES) $(ICU63_INCLUDES) $(ART10_DEFINES) $(A10_UAPI) \
    -include shim/art10/art_runtime_wasm10.h -fno-rtti
$(A10OBJ)/$(A10ART)/% $(A10OBJ)/shim/art10/% $(A10OBJ)/$(A10BLD)/gen/%: A10FLAGS := $(ART10_FLAGS)
# The frame stack of flat calls is topic art's (the same for ART 6.0.1, 8.1, 10).
$(A10OBJ)/shim/art/interpreter_frames_wasm.cc.o: A10FLAGS := $(ART10_FLAGS)
# (lz4: image_space.cc decompresses boot images with it; none is used here)
$(A10OBJ)/$(A10)/external/lz4/%: A10FLAGS :=
$(A10LIB)/libart10.a: $(call a10obj,$(ART10_SRCS) $(ART10_OPOUT_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- BoringSSL (libcrypto only), expat ----------------------------------------------
# libcrypto: what libcore's NativeBN needs (BIGNUM for java.math.BigInteger,
# which java.time uses); C only (OPENSSL_NO_ASM). The archive is for other
# topics too (conscrypt 10: add libssl).
BSSL10 := $(A10)/external/boringssl
BSSL10_INCLUDES := -I$(BSSL10)/src/include
BSSL10_CRYPTO_SRCS := $(call bp10,$(BSSL10)/sources.bp,libcrypto_sources)
$(A10OBJ)/$(BSSL10)/%: A10FLAGS := $(BSSL10_INCLUDES) -I$(BSSL10)/src/crypto -DOPENSSL_NO_ASM \
    -DBORINGSSL_IMPLEMENTATION -DOPENSSL_SMALL -DBORINGSSL_ANDROID_SYSTEM \
    -Wno-implicit-function-declaration
$(A10LIB)/libcrypto10.a: $(call a10obj,$(BSSL10_CRYPTO_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
A10_EXPAT := $(A10)/external/expat
# (Hash salt from getrandom(), which Emscripten's libc has.)
$(A10OBJ)/$(A10_EXPAT)/%: A10FLAGS := -I$(A10_EXPAT) -I$(A10_EXPAT)/lib -DHAVE_EXPAT_CONFIG_H -DHAVE_GETRANDOM
$(A10LIB)/libexpat10.a: $(call a10obj,$(call bp10,$(A10_EXPAT)/Android.bp,libexpat))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libjavacore 10 (libcore/luni natives, with libandroidio) ----------------------------
# NativeBN on libcrypto10, ExpatParser on libexpat10; shim/art10/javacore_register10.cc
# replaces Register.cpp.
A10JC := $(A10)/libcore/luni/src/main/native
A10JC_SRCS := $(filter-out %/Register.cpp, \
                $(call bp10,$(A10JC)/Android.bp,luni_native_srcs) \
                $(call bp10,$(A10JC)/Android.bp,libandroidio_srcs)) \
              $(A10)/libcore/dalvik/src/main/native/org_apache_harmony_dalvik_NativeTestTarget.cpp \
              shim/art10/javacore_register10.cc
A10JC_FLAGS := $(A10_SYS_FLAGS) $(ICU63_INCLUDES) -I$(A10)/libcore/include -I$(A10JC) \
               -DU_USING_ICU_NAMESPACE=0 $(BSSL10_INCLUDES) -I$(A10_EXPAT)/lib
$(A10OBJ)/$(A10)/libcore/luni/% $(A10OBJ)/$(A10)/libcore/dalvik/%: A10FLAGS := $(A10JC_FLAGS)
$(A10OBJ)/shim/art10/javacore_register10.cc.o: A10FLAGS := $(A10JC_FLAGS)
$(A10LIB)/libjavacore10.a: $(call a10obj,$(A10JC_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libopenjdk 10 (libcore/ojluni natives) -------------------------------------------
# Its JNI_OnLoad (ojluni's OnLoad.cpp) is renamed for the table of static
# JNI libraries; JVM_* (libopenjdkjvm) is art/openjdkjvm, in libart10.
A10OJ := $(A10)/libcore/ojluni/src/main/native
# LinuxWatchService needs inotify (there is none). The C files are written
# for Linux with glibc: the host build of libopenjdk defines __GLIBC__ too.
A10OJ_SRCS := $(filter-out %/LinuxWatchService.c, \
                $(call bp10,$(A10OJ)/Android.bp,libopenjdk_native_srcs))
# (libopenjdk has a class JniConstants of its own too, as libjavacore and
# libnativehelper: renamed.)
A10OJ_FLAGS := $(A10_SYS_FLAGS) $(ICU63_INCLUDES) -I$(A10)/libcore/include -I$(A10OJ) -I$(A10JC) \
               -DJniConstants=OpenjdkJniConstants \
               -I$(A10)/external/fdlibm -D_LARGEFILE64_SOURCE -DLINUX -Wno-parentheses-equality \
               -Wno-constant-logical-operand -Wno-sometimes-uninitialized
$(A10OBJ)/$(A10OJ)/%: A10FLAGS := $(A10OJ_FLAGS)
$(A10OBJ)/$(A10OJ)/%.c.o: A10FLAGS := $(A10OJ_FLAGS) -D__linux__ -D__GLIBC__ -Dsendfile64=sendfile
$(A10OBJ)/$(A10OJ)/OnLoad.cpp.o: A10FLAGS := $(A10OJ_FLAGS) -DJNI_OnLoad=arc_wasm_openjdk10_JNI_OnLoad
# The C files with Java_* functions (natives ART finds by name), through
# wrappers that also make a table of them (tools/gen_jni_syms.py), as for 8.1.
A10OJ_SYM_SRCS := $(shell tools/gen_jni_syms.py filter $(filter %.c,$(A10OJ_SRCS)) 2>/dev/null)
A10JNISYMS := $(A10BLD)/jnisyms
A10OJ_WRAPPERS := $(patsubst $(A10OJ)/%.c,$(A10JNISYMS)/openjdk/%.c,$(A10OJ_SYM_SRCS))
$(A10JNISYMS)/openjdk/%.c: $(A10OJ)/%.c tools/gen_jni_syms.py
	@mkdir -p $(dir $@)
	@python3 tools/gen_jni_syms.py wrapper $< $@ arc_wasm_jni_syms_openjdk_$*
$(A10JNISYMS)/openjdk_index.c: tools/gen_jni_syms.py mk/topic-art10.mk $(A10OJ_WRAPPERS)
	@mkdir -p $(dir $@)
	@python3 tools/gen_jni_syms.py index openjdk10 $@ \
	    $(patsubst $(A10OJ)/%.c,arc_wasm_jni_syms_openjdk_%,$(A10OJ_SYM_SRCS))
$(A10OBJ)/$(A10JNISYMS)/%: A10FLAGS := $(A10OJ_FLAGS) -D__linux__ -D__GLIBC__ -Dsendfile64=sendfile -I.
A10OJ_OBJS := $(call a10obj,$(filter-out $(A10OJ_SYM_SRCS),$(A10OJ_SRCS)) $(A10OJ_WRAPPERS) \
                $(A10JNISYMS)/openjdk_index.c)
$(A10LIB)/libopenjdk10.a: $(A10OJ_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

.PHONY: art10-libs
art10-libs: $(A10LIB)/libart10.a $(A10LIB)/libjavacore10.a $(A10LIB)/libopenjdk10.a art10-sys-libs

# --- Java: core-oj.jar, core-libart.jar and the test programs ------------------------
A10FW := $(A10BLD)/framework
A10_CORE_JARS := $(A10FW)/core-oj.jar $(A10FW)/core-libart.jar
# One run builds both (and core-all-classes.jar, to compile against).
$(A10FW)/core-libart.jar: $(A10FW)/core-oj.jar
$(A10FW)/core-oj.jar: tools/build_core10_jars.py
	python3 -I tools/build_core10_jars.py $(A10) $(A10FW)
A10_CORE_CLASSES := $(A10FW)/core-all-classes.jar
# The tests of topic art (VmTest, Bench, BigHeapTest, ArtSpeedTest), the Java 8
# ones (tests/java8) and the Java 9 / dex 039 ones (tests/java9): d8
# --min-api 29 makes dex 039 (const-method-handle, const-method-type). Topics
# add tests (ART10_JAVA_TESTS +=, and the class jars to compile them against:
# ART10_TESTS_CLASSPATH +=, both with $(A10FW)/arc-tests.jar as a
# prerequisite).
ART10_JAVA_TESTS := $(addprefix tests/java/arcwasm/,VmTest.java Hello.java Bench.java \
                      BigHeapTest.java ArtTestNatives.java ArtSpeedTest.java JitTest.java JitCallBench.java JitCatchBench.java) \
                    tests/java8/arcwasm/Java8Test.java tests/java8/arcwasm/DormantTest.java
ART10_TESTS_CLASSPATH :=
# Java 9 sources (private interface methods, try on a variable) need javac
# -source 9, which takes no -bootclasspath: they compile against JDK's
# java.base patched with libcore 10's classes. arcwasm.ConstMh (ldc of method
# handles and types, which javac never emits) is written by
# tools/gen_const_mh_test.py before them.
ART10_JAVA9_TESTS := $(wildcard tests/java9/arcwasm/*.java)
A10TJ := $(A10BLD)/tests-java
$(A10FW)/arc-tests.jar: $(ART10_JAVA_TESTS) $(ART10_JAVA9_TESTS) $(ART_DX_TESTS_DEX) $(A10FW)/core-oj.jar \
                        tools/gen_const_mh_test.py
	rm -rf $(A10TJ) && mkdir -p $(A10TJ)/classes $(A10TJ)/dex
	javac -source 8 -target 8 -Xlint:-options -encoding UTF-8 \
	    -bootclasspath $(A10_CORE_CLASSES) \
	    $(if $(strip $(ART10_TESTS_CLASSPATH)),-classpath $(subst $() ,:,$(strip $(ART10_TESTS_CLASSPATH)))) \
	    -d $(A10TJ)/classes $(ART10_JAVA_TESTS)
	python3 -I tools/gen_const_mh_test.py $(A10TJ)/classes
	javac -source 9 -target 9 -Xlint:-options -XDstringConcat=inline -encoding UTF-8 \
	    --patch-module java.base=$(A10_CORE_CLASSES) -classpath $(A10TJ)/classes \
	    -d $(A10TJ)/classes $(ART10_JAVA9_TESTS)
	cd $(A10TJ)/classes && jar cf ../classes.jar .
	"$(D8)" --release --min-api 29 --lib $(A10_CORE_CLASSES) \
	    $(addprefix --lib ,$(ART10_TESTS_CLASSPATH)) \
	    --output $(A10TJ)/dex $(A10TJ)/classes.jar
	cp $(ART_DX_TESTS_DEX) $(A10TJ)/dex/classes2.dex
	rm -f $@ && cd $(A10TJ)/dex && zip -q "$(abspath $@)" classes.dex classes2.dex

# --- /system image -----------------------------------------------------------
# build.prop of a 10 device (SDK 29); ICU 63's data; the time zone data of
# java.util.TimeZone (libcore's ZoneInfoDB; java.time uses ICU's) and the
# country table of TimeZoneFinder (system/timezone). On a device these are in
# the runtime APEX (/apex/com.android.runtime/etc/{icu,tz}); here the runtime
# module is /system (ANDROID_RUNTIME_ROOT=/system, artvm10.cc), so they are
# in /system/etc/{icu,tz}.
# Topics add files with ASYSTEM10_ADD += --add HOST:system/...
A10_TZ := $(A10)/system/timezone/output_data
A10SYSTEM_IMAGE := $(A10BLD)/system.img
# Topic boot-dex: the jars' classes*.dex stored and 64 KiB-aligned, so that
# ART maps them from the image instead of extracting copies
# (tools/make_system_image.py). A10_IMAGE_OPTS= builds them deflated.
A10_IMAGE_OPTS ?= --store-dex
$(A10BLD)/build.prop: mk/topic-art10.mk
	@mkdir -p $(dir $@)
	printf '%s\n' ro.build.version.sdk=29 ro.build.version.release=10 \
	    ro.build.version.codename=REL ro.build.version.preview_sdk=0 \
	    ro.build.id=QQ3A.200805.001 ro.build.type=user ro.product.cpu.abi=arm64-v8a \
	    ro.product.cpu.abilist=armeabi-v7a,armeabi,arm64-v8a ro.product.cpu.abilist32=armeabi-v7a,armeabi \
	    ro.product.cpu.abilist64=arm64-v8a \
	    ro.product.model=arc-wasm ro.product.manufacturer=arc-wasm ro.product.brand=arc-wasm \
	    ro.product.name=arcwasm10 ro.product.device=arcwasm10 dalvik.vm.heapsize=512m \
	    dalvik.vm.heapgrowthlimit=256m > $@
# ICU's data as files of their own, item by item (tools/icu_dat_split.py; topic
# image-on-demand): an app maps the items it uses, not the whole 23 MB .dat, of which a
# mapping is filled into wasm memory whole (patches/aosp10/zz-image-on-demand.patch: ICU
# and ICU4J read the folder). A10_ICU_FILES= (empty): the .dat, as before.
A10_ICU_FILES ?= 1
A10_ICU_DIR := $(A10BLD)/icu63
$(A10_ICU_DIR)/.split: $(ICU63)/stubdata/icudt63l.dat tools/icu_dat_split.py
	rm -rf $(A10_ICU_DIR) && python3 tools/icu_dat_split.py $< $(A10_ICU_DIR) && touch $@
A10_ICU_ADD := $(if $(A10_ICU_FILES),--add $(A10_ICU_DIR)/icudt63l:system/etc/icu/icudt63l,\
                 --add $(ICU63)/stubdata/icudt63l.dat:system/etc/icu/icudt63l.dat)
A10_ICU_DEP := $(if $(A10_ICU_FILES),$(A10_ICU_DIR)/.split,$(ICU63)/stubdata/icudt63l.dat)
$(A10SYSTEM_IMAGE): $(A10_CORE_JARS) $(A10FW)/arc-tests.jar $(A10BLD)/build.prop tools/make_system_image.py \
                    $(A10_TZ)/iana/tzdata $(A10_TZ)/android/tzlookup.xml $(A10_ICU_DEP)
	python3 tools/make_system_image.py $@ $(A10_IMAGE_OPTS) \
	    --add $(A10BLD)/build.prop:system/build.prop \
	    --add $(A10FW)/core-oj.jar:system/framework/core-oj.jar \
	    --add $(A10FW)/core-libart.jar:system/framework/core-libart.jar \
	    --add $(A10FW)/arc-tests.jar:system/framework/arc-tests.jar \
	    $(A10_ICU_ADD) \
	    --add $(A10_TZ)/iana/tzdata:system/etc/tz/tzdata \
	    --add $(A10_TZ)/android/tzlookup.xml:system/etc/tz/tzlookup.xml \
	    --add $(A10_TZ)/version/tz_version:system/etc/tz/tz_version \
	    $(ASYSTEM10_ADD)

# --- artvm10 -------------------------------------------------------------------
.SECONDEXPANSION:
# The VFS setup (/system, /data on OPFS, /dev/binder) is topic art's
# art/vfs_setup.cc; the ArtTestNatives natives are art/test_natives.cc.
ART10_MAIN_OBJ := $(call a10obj,art10/artvm10.cc art/test_natives.cc)
$(ART10_MAIN_OBJ): A10FLAGS := $(A10_SYS_INCLUDES)
ART10_LIBS := $(A10LIB)/libart10.a $(A10LIB)/libjavacore10.a $(A10LIB)/libopenjdk10.a \
              $(A10_SYS_LIBS) $(A10LIB)/libicui18n63.a $(A10LIB)/libicuuc63.a $(A10LIB)/libfdlibm10.a \
              $(A10LIB)/libcrypto10.a $(A10LIB)/libexpat10.a \
              $(A10LIB)/libart10_shim.a $(BUILD)/libdev_binder.a
# libc functions the stack expects from bionic (shared with topics art, art8).
A10SHIM_SRCS := shim/dalvik/libc_extras.cc shim/art/libc_extras_art.cc shim/art8/libc_extras_art8.cc \
                shim/art10/libc_extras_art10.cc
$(A10OBJ)/shim/dalvik/% $(A10OBJ)/shim/art/libc_extras_art.cc.o $(A10OBJ)/shim/art8/libc_extras_art8.cc.o \
    $(A10OBJ)/shim/art10/libc_extras_art10.cc.o: A10FLAGS := $(A10_SYS_INCLUDES) $(A10_UAPI)
$(A10LIB)/libart10_shim.a: $(call a10obj,$(A10SHIM_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
# Memory grows on demand up to 4 GB (addresses above 2 GB must work).
ART10_STACK ?= 1MB
# The table grows: compiled methods of the dex JIT (topic dex-jit).
ART10_LDFLAGS := -pthread -sPROXY_TO_PTHREAD -sUSE_ZLIB=1 -sINITIAL_MEMORY=256MB -sALLOW_TABLE_GROWTH \
                 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB \
                 -sSTACK_SIZE=$(ART10_STACK) -sDEFAULT_PTHREAD_STACK_SIZE=1MB -sPTHREAD_POOL_SIZE=12 \
                 -sEXIT_RUNTIME=1 -sASSERTIONS=1 --profiling-funcs --js-library $(OPFS_JS) --js-library $(CLOCK_JS) \
                 --preload-file $(A10SYSTEM_IMAGE)@/arc/system.img \
                 --extern-pre-js $(DVM_PRE_JS) --pre-js art/artvm_pre.js \
                 -sMALLOC=none $(BUILD)/pwaobj/dlmalloc_report.o
# malloc: Emscripten's dlmalloc with what the PWA's has (mk/topic-pwa.mk): the
# break's lock shared with the mmap allocator, a report of damage, and no
# zeros written where memory was never handed out (topic wasm-heap-trim).
# Every artvm10.wasm then goes through binaryen's -Os (wasm_opt, Makefile;
# topic a10-size): 29.5 -> 22.8 MB, 61800 -> 40500 functions.
# ($$(ART_VFS_OBJ): topics included later add to it, e.g. tiktok's /proc/self.)
ART10_LINK_DEPS := $(ART10_MAIN_OBJ) $$(ART_VFS_OBJ) $(KERNEL_OBJ) $(ART10_LIBS) \
                   $(BUILD)/libposix_translation.a $(A10SYSTEM_IMAGE) $(OPFS_JS) $(CLOCK_JS) $(DVM_PRE_JS) \
                   art/artvm_pre.js mk/topic-art10.mk $(BUILD)/pwaobj/dlmalloc_report.o
# Node has no OPFS: the in-memory one of topic activity serves /data.
$(A10BLD)/artvm10.js: $(ART10_LINK_DEPS) shim/opfs/memory_opfs.js
	@mkdir -p $(dir $@)
	$(EMXX) $(ART10_LDFLAGS) --pre-js shim/opfs/memory_opfs.js -sENVIRONMENT=node,worker \
	    $(ART10_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) \
	    -Wl,--start-group $(ART10_LIBS) $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
	$(call wasm_opt,$(@:.js=.wasm))

.PHONY: art10 art10-test art10-bigheap-test
art10: $(A10BLD)/artvm10.js
art10-test: art10
	cd $(A10BLD) && $(NODE) artvm10.js -cp /system/framework/arc-tests.jar arcwasm.VmTest
# Objects above 2 GB of wasm memory (a 2.5 GB heap; options as topic art's).
art10-bigheap-test: art10
	cd $(A10BLD) && $(NODE) artvm10.js $(ART_BIGHEAP_OPTS) -cp /system/framework/arc-tests.jar arcwasm.BigHeapTest

# --- artvm10 for the browser ---------------------------------------------------------
A10WEB := $(BUILD)/art10-web
$(A10WEB)/artvm10.js: $(ART10_LINK_DEPS)
	@mkdir -p $(dir $@)
	$(EMXX) $(ART10_LDFLAGS) -sENVIRONMENT=web,worker \
	    $(ART10_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) \
	    -Wl,--start-group $(ART10_LIBS) $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
	$(call wasm_opt,$(@:.js=.wasm))
$(A10WEB)/index.html: art10/web/index.html
	@mkdir -p $(dir $@)
	cp $< $@
$(A10WEB)/report.js: shim/report.js
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: art10-web
art10-web: $(A10WEB)/artvm10.js $(A10WEB)/index.html $(A10WEB)/report.js

# --- the NDK bridge's ART side, for topics that link the bridge ---------------------------
# The bridge (shim/ndk: the ARM and AArch64 linkers, guest libc, JNI tables)
# is reached from ART through weak symbols (static_jni_libs_art10.cc:
# ndk_dalvik_*; jni_call_wasm10.cc: arc_ndk_art_call_native), as for 6.0.1
# and 8.1; its ART side (shim/fw6-system/ndk_art.cc: dlopen, guest
# JNI_OnLoad, jmethodID = ArtMethod*) compiles against ART 10 as it is. The
# framework 10 (topic fw10-system) links it as fw8-system does for 8.1:
#   ART10_LIBS += $(ART10_NDK_ART_OBJ) <bridge archives built with $(A10_SYS_INCLUDES)>
#   ART10_LDFLAGS += -Wl,-u,ndk_dalvik_dlopen -Wl,-u,arc_ndk_art_call_native ...
# (and for arm64-v8a, topic ndk-arm64's: -Wl,-u,ndk64_call_guest_args ...).
ART10_NDK_ART_OBJ := $(call a10obj,shim/fw6-system/ndk_art.cc)
$(ART10_NDK_ART_OBJ): A10FLAGS := $(ART10_FLAGS) -Ishim/ndk
.PHONY: art10-ndk-art
art10-ndk-art: $(ART10_NDK_ART_OBJ)
