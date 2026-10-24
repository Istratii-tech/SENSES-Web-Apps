# Topic "fw8-core": the base of the Android 8.1 framework (AOSP
# android-8.1.0_r81, API 27; docs/topics/fw8-core.md), the way topic
# fw6-core did it for 6.0.1 (mk/topic-fw6-core.mk):
#
#   host     aprotoc + protoc-gen-javastream (external/protobuf,
#            frameworks/base/tools/streaming_proto) and hidl-gen
#            (system/tools/hidl): framework.jar 8.1 has protobuf and HIDL Java
#   Java     the boot class path jars of AOSP 8.1 but core-oj/core-libart
#            (topic art8; tools/build_framework8_jars.sh), framework-res.apk
#            8.1 (aapt2) and its R classes, /system/build.prop (SDK 27)
#   native   libutils 8.1, libbinder 8.1 and servicemanager 8.1 on the
#            in-process /dev/binder of shim/binder, libhidl/libhwbinder in
#            process, libandroidfw 8.1, the libandroid_runtime 8.1 frame
#            (gRegJNI with weak entries) and its core JNI
#
#   make fw8-host        # the host tools
#   make fw8-jars        # the jars and framework-res.apk
#   make fw8-libs        # the native libraries
#   make fw8-native-test # gtest of binder/servicemanager/libandroidfw 8.1 in Node
#   make fw8-tests-jar   # the framework's Java tests compiled against 8.1
#   make art8            # with topic art8 (mk/topic-art8.mk): artvm8 with all of it
#   make fw8-test        # LooperTest BinderTest ApkTest FrameworkResTest on ART 8.1 in Node
#
# Everything goes to build/fw8 (objects build/fw8/obj, libraries
# build/fw8/lib, jars build/fw8/framework, host tools build/fw8/host).

A81 ?= third_party/aosp81
F8BLD := $(BUILD)/fw8
F8FWB := $(A81)/frameworks/base
SDK ?= $(HOME)/Library/Android/sdk

# --- host tools ------------------------------------------------------------------
# Built with the Mac's clang, objects in build/fw8/host/obj.
F8H := $(F8BLD)/host
F8HOBJ := $(F8H)/obj
f8hobj = $(patsubst %,$(F8HOBJ)/%.o,$(1))
F8H_CXX := clang++
F8H_FLAGS := -O1 -g0 -w -MMD -MP
$(F8HOBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  HOSTCXX $<"
	@$(F8H_CXX) -std=gnu++14 $(F8H_FLAGS) $(F8HXFLAGS) -c $< -o $@
$(F8HOBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  HOSTCXX $<"
	@$(F8H_CXX) -std=gnu++17 $(F8H_FLAGS) $(F8HXFLAGS) -c $< -o $@
-include $(shell find $(F8HOBJ) -name '*.d' 2>/dev/null)

# aprotoc: protoc of AOSP 8.1 (protobuf 3.0 with Android's javanano and
# javamicro generators); protoc-gen-javastream: the plugin for
# LOCAL_PROTOC_OPTIMIZE_TYPE := stream (field constants for
# android.util.proto.ProtoOutputStream), on the same libprotoc objects.
F8_PB := $(A81)/external/protobuf
F8_PB_SRCS := $(addprefix $(F8_PB)/,$(shell python3 tools/bp_srcs.py $(F8_PB)/Android.bp aprotoc \
                --target not_windows --target host))
F8_PB_LIB_SRCS := $(filter-out %/compiler/main.cc,$(F8_PB_SRCS))
$(call f8hobj,$(F8_PB_SRCS)): F8HXFLAGS := -I$(F8_PB)/android -I$(F8_PB)/src -DHAVE_ZLIB=1
$(F8H)/libaprotoc.a: $(call f8hobj,$(F8_PB_LIB_SRCS))
	@rm -f $@ && ar rcs $@ $^
$(F8H)/aprotoc: $(call f8hobj,$(F8_PB)/src/google/protobuf/compiler/main.cc) $(F8H)/libaprotoc.a
	$(F8H_CXX) -o $@ $^ -lz -lpthread
F8_JSTREAM := $(F8FWB)/tools/streaming_proto
F8_JSTREAM_SRCS := $(addprefix $(F8_JSTREAM)/,Errors.cpp string_utils.cpp main.cpp)
$(call f8hobj,$(F8_JSTREAM_SRCS)): F8HXFLAGS := -I$(F8_PB)/src
$(F8H)/protoc-gen-javastream: $(call f8hobj,$(F8_JSTREAM_SRCS)) $(F8H)/libaprotoc.a
	$(F8H_CXX) -o $@ $^ -lz -lpthread

# hidl-gen: libhidl-gen, libhidl-gen-ast, libhidl-gen-hash (SHA-256 from
# CommonCrypto: shim/fw8/host/openssl/sha.h), libhidl-gen-utils, with
# libbase 8.1 for the host and a liblog writing to stderr (host copies, used
# by this tool only). The grammar goes through the Mac's bison 2.3 and flex.
F8_HIDL := $(A81)/system/tools/hidl
F8_HIDL_GEN := $(F8H)/hidl-gen-src
# (libhidl-gen-utils first: static FQName objects of the other libraries
# use FQName.cpp's static std::regex objects, constructed first only when
# its object comes first; in AOSP it is a shared library loaded before.)
F8_HIDL_SRCS := $(addprefix $(F8_HIDL)/,$(addprefix utils/,FQName.cpp Formatter.cpp StringHelper.cpp) \
                  $(shell python3 tools/bp_srcs.py $(F8_HIDL)/Android.bp libhidl-gen) \
                  $(filter %.cpp,$(shell python3 tools/bp_srcs.py $(F8_HIDL)/Android.bp libhidl-gen-ast)) \
                  Hash.cpp main.cpp) \
                $(F8_HIDL_GEN)/hidl-gen_y.cpp $(F8_HIDL_GEN)/hidl-gen_l.cpp \
                $(addprefix $(A81)/system/core/base/,file.cpp logging.cpp stringprintf.cpp strings.cpp \
                  errors_unix.cpp) \
                shim/fw8/host/liblog_host.cc
# The grammar is a GLR C++ parser (glr.cc) that the Mac's bison 2.3 gets
# wrong; AOSP 8.1 builds it with prebuilts/misc's bison 2.7. That is
# external/bison of AOSP 4.4, built here from its archive (outside the repo,
# path with spaces: only in the recipe) with gnulib's vasnprintf kept off %n
# (macOS aborts on %n in a writable format). BISON27_TGZ= another archive,
# or F8_BISON=/path/to/bison (2.7 or 3.x).
BISON27_TGZ ?= $(ARC_SIBLINGS)/arm44-web/out/downloads/aosp/external/bison.tar.gz
F8_BISON ?= $(F8H)/bison/bin/bison
$(F8H)/bison/bin/bison:
	rm -rf $(F8H)/bison-src $(F8H)/bison-build $(F8H)/bison
	mkdir -p $(F8H)/bison-src $(F8H)/bison-build
	tar xzf "$(BISON27_TGZ)" -C $(F8H)/bison-src
	sed -i '' 's/^# if !(((__GLIBC__ > 2/# if !defined __APPLE__ \&\& !(((__GLIBC__ > 2/' $(F8H)/bison-src/lib/vasnprintf.c
	cd $(F8H)/bison-build && ../bison-src/configure --prefix="$(abspath $(F8H))/bison" --disable-nls \
	    CFLAGS="-O1 -w" > configure.log 2>&1 && $(MAKE) > make.log 2>&1 && $(MAKE) install > install.log 2>&1
	$@ --version | head -1
$(F8_HIDL_GEN)/hidl-gen_y.cpp: $(F8_HIDL)/hidl-gen_y.yy $(F8_BISON)
	@mkdir -p $(dir $@)
	$(F8_BISON) -d -o $@ --defines=$(F8_HIDL_GEN)/hidl-gen_y.h $<
$(F8_HIDL_GEN)/hidl-gen_y.h: $(F8_HIDL_GEN)/hidl-gen_y.cpp ;
$(F8_HIDL_GEN)/hidl-gen_l.cpp: $(F8_HIDL)/hidl-gen_l.ll $(F8_HIDL_GEN)/hidl-gen_y.h
	@mkdir -p $(dir $@)
	flex -o $@ $<
$(call f8hobj,$(F8_HIDL_SRCS)): F8HXFLAGS := -Ishim/fw8/host -I$(F8_HIDL) -I$(F8_HIDL_GEN) \
    -I$(F8_HIDL)/utils/include/hidl-util -I$(F8_HIDL)/utils/include -I$(F8_HIDL)/include_hash/hidl-hash -I$(F8_HIDL)/include_hash \
    -I$(A81)/system/core/base/include -I$(A81)/system/core/liblog/include \
    -I$(A81)/system/core/libutils/include -I$(A81)/system/core/libsystem/include
$(call f8hobj,$(F8_HIDL_GEN)/hidl-gen_l.cpp): $(F8_HIDL_GEN)/hidl-gen_y.h
$(F8H)/hidl-gen: $(call f8hobj,$(F8_HIDL_SRCS))
	$(F8H_CXX) -o $@ $^

.PHONY: fw8-host
fw8-host: $(F8H)/aprotoc $(F8H)/protoc-gen-javastream $(F8H)/hidl-gen

# --- framework-res.apk 8.1 and android.R / com.android.internal.R ---------------
# aapt2 (build-tools 35), as core/res/Android.mk (LOCAL_USE_AAPT2): compile
# --legacy, then link with package id 0x01 (-x), --private-symbols
# com.android.internal, --no-auto-version. English and mdpi only by default,
# as for 4.4 and 6.0.1 (FRAMEWORK8_RES_LOCALES=, FRAMEWORK8_RES_DENSITY= keep
# everything).
AAPT2 ?= $(lastword $(sort $(wildcard $(SDK)/build-tools/*/aapt2)))
F8RES := $(F8FWB)/core/res
F8RES_OUT := $(F8BLD)/framework-res
F8RES_APK := $(F8RES_OUT)/framework-res.apk
FRAMEWORK8_RES_LOCALES ?= en
FRAMEWORK8_RES_DENSITY ?= mdpi
F8RES_FLAGS := -x --min-sdk-version 27 --target-sdk-version 27 \
    --version-code 27 --version-name 8.1.0 --private-symbols com.android.internal --no-auto-version \
    $(if $(FRAMEWORK8_RES_LOCALES),-c $(FRAMEWORK8_RES_LOCALES)) \
    $(if $(FRAMEWORK8_RES_DENSITY),--preferred-density $(FRAMEWORK8_RES_DENSITY))
F8RES_GEN := $(F8RES_OUT)/gen
F8RES_JAVA := $(F8RES_GEN)/android/R.java $(F8RES_GEN)/android/Manifest.java \
    $(F8RES_GEN)/com/android/internal/R.java
$(F8RES_OUT)/aapt2-flags: FORCE
	@mkdir -p $(dir $@)
	@echo '$(F8RES_FLAGS)' | cmp -s - $@ || echo '$(F8RES_FLAGS)' > $@
F8RES_FILES := $(F8RES)/AndroidManifest.xml $(shell find $(F8RES)/res $(F8RES)/assets -type f 2>/dev/null)
$(F8RES_OUT)/res.zip: $(F8RES_FILES)
	@mkdir -p $(dir $@)
	"$(AAPT2)" compile --legacy --dir $(F8RES)/res -o $@ 2> $(F8RES_OUT)/compile.log \
	    || { tail -20 $(F8RES_OUT)/compile.log; rm -f $@; exit 1; }
$(F8RES_OUT)/aapt2.stamp: $(F8RES_OUT)/res.zip $(F8RES_OUT)/aapt2-flags
	@rm -rf $(F8RES_GEN) && mkdir -p $(F8RES_GEN)
	"$(AAPT2)" link -o $(F8RES_APK) $(F8RES_FLAGS) --manifest $(F8RES)/AndroidManifest.xml \
	    -A $(F8RES)/assets --java $(F8RES_GEN) $< 2> $(F8RES_OUT)/link.log \
	    || { grep -v '^warn: removing' $(F8RES_OUT)/link.log | tail -20; exit 1; }
	@echo "built $(F8RES_APK) ($$(wc -c < $(F8RES_APK) | tr -d ' ') bytes)"
	@touch $@
$(F8RES_APK) $(F8RES_JAVA): $(F8RES_OUT)/aapt2.stamp ;

# --- the boot class path jars ---------------------------------------------------
# BOOTCLASSPATH of AOSP 8.1 (build/make: TARGET_CORE_JARS of core/envsetup.mk,
# then PRODUCT_BOOT_JARS of target/product/core_minimal.mk), in order.
# core-oj and core-libart are topic art8's; until they are there, javac
# compiles against core-all-classes.jar, libcore 8.1 compiled by javac only
# (never dexed, never in /system). CORE8_CLASSES=a.jar:b.jar uses others.
F8FW := $(F8BLD)/framework
F8_BOOT_JARS := core-oj core-libart conscrypt okhttp bouncycastle apache-xml legacy-test ext \
                framework telephony-common voip-common ims-common org.apache.http.legacy.boot \
                android.hidl.base-V1.0-java android.hidl.manager-V1.0-java
F8_LIB_JARS := conscrypt okhttp bouncycastle ext apache-xml
F8_OPT_JARS := legacy-test telephony-common voip-common ims-common org.apache.http.legacy.boot \
               android.hidl.base-V1.0-java android.hidl.manager-V1.0-java
F8_JARS := $(addprefix $(F8FW)/,$(addsuffix .jar,$(filter-out core-oj core-libart,$(F8_BOOT_JARS))))
# javac output of the framework and what it is compiled against, for tests.
F8_CLASSES := $(foreach j,$(F8_LIB_JARS) framework,$(F8FW)/$(j)-java/$(j)-classes.jar)
F8_JARS_SH := tools/build_framework8_jars.sh
F8_HOST_TOOLS := $(F8H)/aprotoc $(F8H)/protoc-gen-javastream $(F8H)/hidl-gen
ifneq ($(A8_CORE_CLASSES),)
# Topic art8's (made with core-oj.jar and core-libart.jar).
F8_CORE_CLASSES := $(A8_CORE_CLASSES)
$(A8_CORE_CLASSES): $(A8FW)/core-oj.jar ;
else
F8_CORE_CLASSES := $(F8FW)/core-all-java/core-all-classes.jar
$(F8_CORE_CLASSES): $(F8_JARS_SH) $(A81)/libcore/openjdk_java_files.mk $(A81)/libcore/non_openjdk_java_files.mk
	$(F8_JARS_SH) $(F8FW) core
endif
CORE8_CLASSES ?= $(F8_CORE_CLASSES)
export CORE8_CLASSES
F8_LIB_SRCS := $(shell find $(addprefix $(A81)/external/,conscrypt/common/src/main/java \
                 conscrypt/platform/src/main/java okhttp bouncycastle/bcprov/src/main nist-sip/java \
                 tagsoup/src libphonenumber apache-xml/src/main/java) -type f -name '*.java' 2>/dev/null)
$(F8FW)/fw8-libs.stamp: $(F8_JARS_SH) $(F8_CORE_CLASSES) $(F8_LIB_SRCS)
	$(F8_JARS_SH) $(F8FW) libs
	@touch $@
$(foreach j,$(F8_LIB_JARS),$(F8FW)/$(j).jar $(F8FW)/$(j)-java/$(j)-classes.jar): $(F8FW)/fw8-libs.stamp ;

F8_SRC_DIRS := $(addprefix $(F8FWB)/,$(addsuffix /java,core graphics location media media/mca/effect \
                 media/mca/filterfw media/mca/filterpacks drm opengl sax telecomm telephony wifi lowpan \
                 keystore rs))
F8_SRCS := $(shell find $(F8_SRC_DIRS) $(F8FWB)/packages/services $(F8FWB)/core/proto $(F8FWB)/proto \
             -name '*.java' -o -name '*.aidl' -o -name '*.logtags' -o -name '*.proto' 2>/dev/null) \
           $(F8FWB)/Android.mk $(F8FWB)/config/preloaded-classes
$(F8FW)/framework.jar: $(F8_JARS_SH) $(F8FW)/fw8-libs.stamp $(F8_HOST_TOOLS) $(F8RES_JAVA) $(F8_SRCS)
	$(F8_JARS_SH) $(F8FW) framework $(F8RES_JAVA)
$(F8FW)/framework-java/framework-classes.jar: $(F8FW)/framework.jar ;

F8_OPT_SRCS := $(shell find $(A81)/frameworks/opt/telephony/src $(A81)/frameworks/opt/telephony/proto \
                 $(A81)/frameworks/opt/net/voip/src $(A81)/frameworks/opt/net/ims/src \
                 $(F8FWB)/legacy-test/src $(A81)/external/apache-http/src $(A81)/external/apache-http/android \
                 -type f 2>/dev/null)
$(F8FW)/fw8-opt.stamp: $(F8_JARS_SH) $(F8FW)/framework.jar $(F8_OPT_SRCS)
	$(F8_JARS_SH) $(F8FW) opt
	@touch $@
$(addprefix $(F8FW)/,$(addsuffix .jar,$(F8_OPT_JARS))): $(F8FW)/fw8-opt.stamp ;

.PHONY: fw8-jars FORCE
fw8-jars: $(F8_JARS) $(F8RES_APK)

# --- native: shared flags and object rules ------------------------------------------
# Objects in build/fw8/obj. The flags are topic art8's (A8_COMMON of
# mk/topic-art8.mk, repeated here so that this topic builds before art8 is
# merged) with shim/fw8/fw8_compat.h. No __ANDROID__ (as art8 builds
# system/core 8.1): the few device-only paths the framework needs are
# opened for ARC_WASM by patches/aosp81/fw8-core.patch, as fw6-core did.
F8OBJ := $(F8BLD)/obj
F8LIB := $(F8BLD)/lib
f8obj = $(patsubst %,$(F8OBJ)/%.o,$(1))
F8_COMMON := -pthread -O2 -g1 -D_GNU_SOURCE -DARC_WASM=1 -DANDROID_SMP=1 \
             -include shim/fw8/fw8_compat.h \
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
             -Wno-logical-not-parentheses -MMD -MP
F8_CFLAGS := $(F8_COMMON) -std=gnu11
F8_CXXFLAGS := $(F8_COMMON) -std=gnu++14 -Wno-register
# FW8_INCLUDES: what code built against the 8.1 framework's native headers
# needs (topics fw8-graphics, fw8-system... compile with these).
#   shim/fw8/include   linux/binder.h (8.1's UAPI), selinux/, sys/system_properties.h
#   shim/binder/include  arc_binder.h (the in-process driver's extensions)
# The binder protocol is the 32-bit one (BINDER_IPC_32BIT), as for 6.0.1.
F8_UAPI := -idirafter $(A81)/bionic/libc/kernel/uapi -idirafter $(A81)/bionic/libc/kernel/uapi/asm-x86 \
           -idirafter $(A81)/bionic/libc/kernel/android/uapi
FW8_INCLUDES := -Ishim/fw8/include -Ishim/binder/include \
    $(addprefix -I$(A81)/system/core/,base/include liblog/include libcutils/include \
      libutils/include libsystem/include libziparchive/include libbacktrace/include \
      libprocessgroup/include) \
    -I$(A81)/libnativehelper/include_jni -I$(A81)/libnativehelper/include \
    -I$(A81)/libnativehelper/include/nativehelper \
    -I$(A81)/libnativehelper/platform_include -I$(A81)/external/safe-iop/include \
    $(addprefix -I$(A81)/frameworks/native/,include libs/binder/include) \
    -I$(F8FWB)/libs/androidfw/include \
    -I$(A81)/system/libhidl/base/include -I$(A81)/system/libhidl/transport/include \
    -I$(A81)/system/libhwbinder/include \
    $(F8_UAPI) -DBINDER_IPC_32BIT=1 -sUSE_ZLIB=1
# -DLOG_NDEBUG=1 as in a release build: ALOGV and LOG_FATAL_IF compile out
# (libbinder would log every command, see docs/topics/binder.md).
F8_FLAGS := $(FW8_INCLUDES) -DLOG_NDEBUG=1
$(F8OBJ)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(EMCC) $(F8_CFLAGS) $(F8_FLAGS) $(F8XFLAGS) -c $< -o $@
$(F8OBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(F8_CXXFLAGS) $(F8_FLAGS) $(F8XFLAGS) -c $< -o $@
$(F8OBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(F8_CXXFLAGS) $(F8_FLAGS) $(F8XFLAGS) -c $< -o $@
-include $(shell find $(F8OBJ) -name '*.d' 2>/dev/null)
f8bp = $(addprefix $(dir $(1)),$(shell tools/bp_srcs.py $(1) $(2) $(3)))

# --- system/core 8.1 of topic art8 -------------------------------------------------
# libbase, liblog, libcutils, libziparchive, libnativehelper are topic
# art8's (build/art8/lib,
# mk/topic-art8.mk; libutils' FileMap.cpp is in libutils8.a here, art8's
# libfilemap8.a is left out). Until art8 is merged, the native tests link
# temporary copies built here from the same sources (build/fw8/tmp; never
# part of artvm): libbase's and libcutils' lists as art8 builds them, and
# liblog's API on shim/common/alog_shim.cc (tests/native/fw8/liblog8_tmp.cc).
ifneq ($(A8LIB),)
F8_SYS_LIBS := $(addprefix $(A8LIB)/,libnativehelper8.a libcutils8.a libziparchive8.a libbase8.a liblog8.a)
else
F8TMP := $(F8BLD)/tmp
# base/file.cpp and libcutils/threads.c with art8's two changes (its
# art8.patch: GetExecutablePath for Emscripten, bionic's gettid()), made on
# copies: the tree is art8's to patch. ashmem-dev.c rather than the host's
# ashmem-host.c (temporary files): posix_translation has /dev/ashmem, and
# libbinder 8.1's Parcel needs ashmem_valid(), which only the device file has.
F8_TMP_SRCS := $(filter-out %/test_utils.cpp %/file.cpp %/logging.cpp, \
                 $(call f8bp,$(A81)/system/core/base/Android.bp,libbase,--target linux)) \
               $(filter-out %/dlmalloc_stubs.c %/threads.c %/ashmem-host.c, \
                 $(call f8bp,$(A81)/system/core/libcutils/Android.bp,libcutils, \
                 --target host --target not_windows)) \
               $(A81)/system/core/libcutils/ashmem-dev.c \
               $(call f8bp,$(A81)/system/core/libziparchive/Android.bp,libziparchive) \
               $(addprefix $(A81)/libnativehelper/,JNIHelp.cpp JniConstants.cpp toStringArray.cpp) \
               $(F8TMP)/src/base/file.cpp $(F8TMP)/src/base/logging.cpp $(F8TMP)/src/libcutils/threads.c \
               tests/native/fw8/liblog8_tmp.cc
$(F8TMP)/src/base/file.cpp: $(A81)/system/core/base/file.cpp
	@mkdir -p $(dir $@)
	sed -e '/^#else$$/{N;s|^#else\n#error unknown OS|#elif defined(__EMSCRIPTEN__)\n  return "/system/bin/artvm8";\n#else\n#error unknown OS|;}' $< > $@
# (logging.cpp's GetThreadId has no case for Emscripten and returns nothing;
# not in art8.patch yet.)
$(F8TMP)/src/base/logging.cpp: $(A81)/system/core/base/logging.cpp
	@mkdir -p $(dir $@)
	sed '/return syscall(__NR_gettid);/{n;s|^#elif defined(_WIN32)$$|#elif defined(__EMSCRIPTEN__)\n  return gettid();\n#elif defined(_WIN32)|;}' $< > $@
$(call f8obj,$(F8TMP)/src/base/logging.cpp): F8XFLAGS := -I$(A81)/system/core/base
$(F8TMP)/src/libcutils/threads.c: $(A81)/system/core/libcutils/threads.c
	@mkdir -p $(dir $@)
	sed 's|^#ifndef __ANDROID__$$|#if !defined(__ANDROID__) \&\& !defined(__EMSCRIPTEN__)|' $< > $@
$(call f8obj,$(F8TMP)/src/libcutils/threads.c): F8XFLAGS := -I$(A81)/system/core/libcutils
$(call f8obj,$(filter $(A81)/system/core/libziparchive/%,$(F8_TMP_SRCS))): F8XFLAGS := -DZLIB_CONST -D_FILE_OFFSET_BITS=64
$(F8TMP)/libsys8_tmp.a: $(call f8obj,$(F8_TMP_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
F8_SYS_LIBS := $(F8TMP)/libsys8_tmp.a
endif

# --- libutils 8.1, the properties ------------------------------------------------------
# The host list (Trace.cpp is the device's: utils/Trace.h is empty without
# __ANDROID__), with Looper on a pipe instead of eventfd
# (fw8-core.patch); CallStack/ProcessCallStack need libbacktrace: an empty
# CallStack (shim/fw6/callstack_stub.cc, the API is unchanged); the thread
# priorities of android.os.Process (shim/fw6/thread_priority.cc).
F8_UTILS := $(A81)/system/core/libutils
# Static.cpp (the empty String8/String16 every string starts from, made by
# its static constructor) is not in the archive but an object of its own,
# F8_UTILS_STATIC, for every link with libutils8.a: from the archive its
# constructor did not reach the link even when the member was pulled in
# (wasm-ld; libbinder 8.1 has a Static.cpp.o member too), and every String8
# then started from a null buffer.
F8_UTILS_SRCS := $(filter-out %/CallStack.cpp %/ProcessCallStack.cpp %/Static.cpp, \
                   $(call f8bp,$(F8_UTILS)/Android.bp,libutils,--target linux --target host)) \
                 shim/fw6/thread_priority.cc shim/fw6/callstack_stub.cc
F8_UTILS_STATIC := $(call f8obj,$(F8_UTILS)/Static.cpp)
# (its sched_getscheduler is topic art8's here: libc_extras_art8.cc, or the
# weak one of shim/fw8/cutils_fw8.cc.)
$(call f8obj,shim/fw6/thread_priority.cc): F8XFLAGS := -Dsched_getscheduler=arc_fw6_sched_getscheduler
$(F8LIB)/libutils8.a: $(call f8obj,$(F8_UTILS_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
# The property store: bionic's __system_property_* in process (6.0.1's,
# shim/fw6/system_properties.cc, and what 8.0 added) and libcutils 8.1's
# properties.cpp, which art8's host list of libcutils leaves out.
F8_PROPS_SRCS := shim/fw6/system_properties.cc shim/fw8/system_properties8.cc \
                 $(A81)/system/core/libcutils/properties.cpp shim/fw8/cutils_fw8.cc
$(F8LIB)/libprops8.a: $(call f8obj,$(F8_PROPS_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libbinder 8.1, servicemanager 8.1 --------------------------------------------------
# On /dev/binder of shim/binder/dev_binder.cc (libdev_binder.a, mk/topic-binder.mk).
# Without the aidl-cpp interface IPackageManagerNative (nothing here calls it).
F8_BINDER := $(A81)/frameworks/native/libs/binder
F8_BINDER_SRCS := $(filter %.cpp,$(call f8bp,$(F8_BINDER)/Android.bp,libbinder))
$(F8LIB)/libbinder8.a: $(call f8obj,$(F8_BINDER_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
F8_SVCMGR := $(A81)/frameworks/native/cmds/servicemanager
F8_SVCMGR_SRCS := $(F8_SVCMGR)/service_manager.c $(F8_SVCMGR)/binder.c \
                  shim/fw8/servicemanager8_main.c shim/fw8/selinux8_stub.c
# (bionic's headers bring stdbool.h in and define __unused.)
$(call f8obj,$(F8_SVCMGR)/service_manager.c): F8XFLAGS := -Dmain=arc_servicemanager8_main \
    -Dbinder_loop=arc_servicemanager8_loop -include stdbool.h '-D__unused=__attribute__((__unused__))'
$(call f8obj,$(F8_SVCMGR)/binder.c): F8XFLAGS := '-D__unused=__attribute__((__unused__))'
$(call f8obj,shim/fw8/servicemanager8_main.c): F8XFLAGS := -I$(F8_SVCMGR)
$(F8LIB)/libservicemanager8.a: $(call f8obj,$(F8_SVCMGR_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libandroidfw 8.1 ---------------------------------------------------------------
# The device list (AssetManager and AssetManager2/ApkAssets/LoadedArsc, the
# backup helpers, CursorWindow) without DisplayEventDispatcher (libgui:
# topic fw8-graphics).
F8_ANDROIDFW := $(F8FWB)/libs/androidfw
F8_ANDROIDFW_SRCS := $(filter-out %/DisplayEventDispatcher.cpp, \
                       $(call f8bp,$(F8_ANDROIDFW)/Android.bp,libandroidfw,--target android))
$(F8LIB)/libandroidfw8.a: $(call f8obj,$(F8_ANDROIDFW_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- native tests (gtest in Node) -----------------------------------------------------
# tests/native/fw8: binder8_test.cc (servicemanager 8.1, libbinder 8.1
# across two binder processes, Parcel, Looper, clocks, properties) and
# androidfw8_test.cc (AssetManager/ResTable and AssetManager2 on
# framework-res.apk 8.1). The kernel boots as in artvm, with /dev/binder
# and a /system image of framework-res.apk and build.prop.
# FW8_JNI_LIBS: libraries of JNI objects (each defines register_* functions
# of gRegJNI). Linked whole: register_fw8.cc refers to register_* weakly,
# and a weak reference does not pull an archive member in. Topics append
# theirs (FW8_JNI_LIBS += ...) and the libraries those need to F8_LIBS.
FW8_JNI_LIBS := $(F8LIB)/libandroid_runtime8.a
F8_LIBS := $(F8LIB)/libandroidfw8.a $(F8LIB)/libbinder8.a $(F8LIB)/libservicemanager8.a \
           $(F8LIB)/libhidl8.a $(F8LIB)/libutils8.a $(F8LIB)/libprops8.a
F8T := $(F8BLD)/native-test
F8T_IMAGE := $(F8T)/system.img
$(F8T_IMAGE): $(F8RES_APK) framework/build8.prop tools/make_system_image.py
	@mkdir -p $(dir $@)
	python3 tools/make_system_image.py $@ --add $(F8RES_APK):system/framework/framework-res.apk \
	    --add framework/build8.prop:system/build.prop
F8T_SRCS := tests/native/fw8/binder8_test.cc tests/native/fw8/androidfw8_test.cc \
            tests/native/fw8/runtime8_test.cc
$(call f8obj,$(F8T_SRCS)): F8XFLAGS := -I$(GT)/googletest/include -I$(F8_SVCMGR)
F8T_MAIN_OBJ := $(call obj,tests/native/fw8/fw8_test_main.cc)
$(F8T_MAIN_OBJ): shim/binder/dev_binder.h
$(F8T)/fw8_native_test.js: $(F8T_MAIN_OBJ) $(call f8obj,$(F8T_SRCS)) $(F8_UTILS_STATIC) $(KERNEL_OBJ) \
                           $(FW8_JNI_LIBS) $(F8_LIBS) \
                           $(F8_SYS_LIBS) $(BUILD)/libdev_binder.a $(BUILD)/libposix_translation.a \
                           $(GTEST_OBJS) $(F8T_IMAGE)
	@mkdir -p $(dir $@)
	$(EMXX) -pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=12 -sUSE_ZLIB=1 \
	    -sINITIAL_MEMORY=256MB -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB \
	    -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB \
	    -sEXIT_RUNTIME=1 -sENVIRONMENT=node,worker -sASSERTIONS=1 --profiling-funcs \
	    --embed-file $(F8T_IMAGE)@/arc/system.img \
	    $(F8T_MAIN_OBJ) $(call f8obj,$(F8T_SRCS)) $(F8_UTILS_STATIC) $(KERNEL_OBJ) \
	    -Wl,--whole-archive $(FW8_JNI_LIBS) -Wl,--no-whole-archive \
	    -Wl,--start-group $(F8_LIBS) $(F8_SYS_LIBS) $(BUILD)/libdev_binder.a \
	    $(BUILD)/libposix_translation.a -Wl,--end-group $(GTEST_OBJS) -o $@

.PHONY: fw8-libs fw8-native-test
fw8-libs: $(FW8_JNI_LIBS) $(F8LIB)/libandroid_runtime8_icu.a $(F8_LIBS)
fw8-native-test: $(F8T)/fw8_native_test.js
	cd $(F8T) && $(NODE) fw8_native_test.js $(GTEST_ARGS)

# --- HIDL in process: libhidlbase, libhwbinder, libhidltransport ---------------------
# The HIDL runtime the framework's android.os.Hw* natives use, without HALs
# and without hwbinder: there is no /dev/hwbinder, so libhidltransport's
# defaultServiceManager() is null (as on a device without hwbinder) and
# every IFoo.getService() fails; passthrough HALs cannot be opened either
# (shim/fw8/vndksupport_stub.c). hidl_string, hidl_vec, HwParcel and HwBlob
# work in process. The C++ of android.hidl.base@1.0 and
# android.hidl.manager@1.0/1.1 comes from hidl-gen.
F8_HIDL_GENCPP := $(F8BLD)/gen/hidl-cpp
F8_HIDL_CPP_PKGS := android.hidl.base@1.0 android.hidl.manager@1.0 android.hidl.manager@1.1
F8_HIDL_CPP_SRCS := $(addprefix $(F8_HIDL_GENCPP)/android/hidl/,base/1.0/BaseAll.cpp base/1.0/types.cpp \
                      manager/1.0/ServiceManagerAll.cpp manager/1.0/ServiceNotificationAll.cpp \
                      manager/1.1/ServiceManagerAll.cpp)
$(F8_HIDL_GENCPP)/hidl.stamp: $(F8H)/hidl-gen $(wildcard $(A81)/system/libhidl/transport/base/1.0/*.hal \
                                $(A81)/system/libhidl/transport/manager/*/*.hal)
	rm -rf $(F8_HIDL_GENCPP) && mkdir -p $(F8_HIDL_GENCPP)
	for p in $(F8_HIDL_CPP_PKGS); do \
	  for l in c++-headers c++-sources; do \
	    $(F8H)/hidl-gen -o $(F8_HIDL_GENCPP) -L$$l -randroid.hidl:$(A81)/system/libhidl/transport $$p || exit 1; \
	  done; \
	done
	@touch $@
$(F8_HIDL_CPP_SRCS): $(F8_HIDL_GENCPP)/hidl.stamp ;
F8_HIDL_LIB_SRCS := $(addprefix $(A81)/system/libhidl/base/,HidlInternal.cpp HidlSupport.cpp Status.cpp \
                      TaskRunner.cpp) \
                    $(addprefix $(A81)/system/libhwbinder/,$(shell tools/bp_srcs.py \
                      $(A81)/system/libhwbinder/Android.bp libhwbinder)) \
                    $(addprefix $(A81)/system/libhidl/transport/,HidlBinderSupport.cpp \
                      HidlTransportSupport.cpp HidlTransportUtils.cpp ServiceManagement.cpp Static.cpp) \
                    $(F8_HIDL_CPP_SRCS) shim/fw8/vndksupport_stub.c
$(call f8obj,$(F8_HIDL_LIB_SRCS)): F8XFLAGS := -I$(F8_HIDL_GENCPP) -I$(A81)/system/core/libvndksupport/include
$(call f8obj,$(F8_HIDL_LIB_SRCS)): $(F8_HIDL_GENCPP)/hidl.stamp
$(F8LIB)/libhidl8.a: $(call f8obj,$(F8_HIDL_LIB_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libandroid_runtime 8.1: the frame and the core JNI --------------------------------
# AndroidRuntime.cpp itself is replaced by shim/fw8/android_runtime8.cc (the
# frame, RuntimeInit/ZygoteInit) and shim/fw8/register_fw8.cc (gRegJNI of
# 8.1, weak entries). The core: Log, SystemClock, EventLog, Trace,
# MessageQueue (Looper), Binder, Parcel, AssetManager/StringBlock/XmlBlock,
# Process, SystemProperties, nio, Configuration, VirtualRefBasePtr and the
# HIDL natives (android.os.HwBinder/HwParcel/HwBlob/HwRemoteBinder on
# libhidl8.a). Beyond the core, the rest of core/jni that needs neither
# graphics, media, input, sqlite nor hardware (as fw6-core did for 6.0.1).
F8_JNI := $(F8FWB)/core/jni
F8_JNI_CORE := android_util_Log android_os_SystemClock android_util_EventLog android_os_Trace \
    android_os_MessageQueue android_util_Binder android_os_Parcel android_util_AssetManager \
    android_util_StringBlock android_util_XmlBlock android_util_Process android_os_SystemProperties \
    android_nio_utils android_content_res_Configuration com_android_internal_util_VirtualRefBasePtr \
    android_os_HwBinder android_os_HwBlob android_os_HwParcel android_os_HwRemoteBinder
F8_JNI_EXTRA := android_os_Debug android_util_FileObserver android_net_LocalSocketImpl \
    android_net_TrafficStats android_os_MemoryFile android_os_SharedMemory android_util_MemoryIntArray \
    android_ddm_DdmHandleNativeHeap android_backup_BackupDataInput android_backup_BackupDataOutput \
    android_backup_BackupHelperDispatcher android_backup_FileBackupHelperBase \
    android_app_backup_FullBackup android_content_res_ObbScanner android_animation_PropertyValuesHolder \
    com_android_internal_content_NativeLibraryHelper com_android_internal_net_NetworkStatsFactory \
    android_util_jar_StrictJarFile
# The JNI that needs ICU (topic art8's ICU 58 libraries): its own archive,
# linked where ICU is (artvm8), not into the native tests.
F8_JNI_ICU := android_text_AndroidCharacter
F8_JNI_SRCS := $(addprefix $(F8_JNI)/,$(addsuffix .cpp,$(F8_JNI_CORE) $(F8_JNI_EXTRA)) \
                 hwbinder/EphemeralStorage.cpp) \
               shim/fw8/android_runtime8.cc shim/fw8/register_fw8.cc shim/binder/binder_runtime.cc \
               shim/fw6/memtrack_stub.c shim/framework/debug_support.cc shim/fw8/debuggerd_stub.cc \
               shim/fw8/memunreachable_stub.cc shim/fw8/sys8_fallbacks.cc
F8_JNI_FLAGS := -I$(F8_JNI) -I$(F8_JNI)/include -I$(F8_HIDL_GENCPP) \
    -I$(A81)/external/icu/icu4c/source/common -I$(A81)/external/icu/icu4c/source/i18n \
    -I$(A81)/system/core/libmemtrack/include -I$(A81)/system/core/libvndksupport/include \
    -I$(A81)/system/core/libmemunreachable/include \
    -DU_USING_ICU_NAMESPACE=0
$(call f8obj,$(F8_JNI_SRCS)): F8XFLAGS := $(F8_JNI_FLAGS)
$(call f8obj,$(F8_JNI_SRCS)): $(F8_HIDL_GENCPP)/hidl.stamp
$(call f8obj,shim/fw8/android_runtime8.cc shim/binder/binder_runtime.cc): \
    F8XFLAGS := $(F8_JNI_FLAGS) -DLOG_TAG=\"AndroidRuntime\"
# It includes the UAPI's <linux/capability.h> and libcutils' fs_config.h,
# which brings its own copy of it unless __BIONIC__: the copy is kept out.
$(call f8obj,$(F8_JNI)/android_util_AssetManager.cpp): \
    F8XFLAGS := $(F8_JNI_FLAGS) -D_SYSTEM_CORE_INCLUDE_PRIVATE_ANDROID_FILESYSTEM_CAPABILITY_H
# (bionic's <cutils/ashmem.h> includes the UAPI's <linux/ashmem.h>.)
$(call f8obj,$(F8_JNI)/android_os_MemoryFile.cpp): F8XFLAGS := $(F8_JNI_FLAGS) -include linux/ashmem.h
$(call f8obj,$(F8_JNI)/android_os_Debug.cpp): F8XFLAGS := $(F8_JNI_FLAGS) -include malloc_info.h
$(F8LIB)/libandroid_runtime8.a: $(call f8obj,$(F8_JNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
F8_JNI_ICU_SRCS := $(addprefix $(F8_JNI)/,$(addsuffix .cpp,$(F8_JNI_ICU)))
$(call f8obj,$(F8_JNI_ICU_SRCS)): F8XFLAGS := $(F8_JNI_FLAGS)
$(F8LIB)/libandroid_runtime8_icu.a: $(call f8obj,$(F8_JNI_ICU_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- the Java tests of the framework, against 8.1 -----------------------------------
# The Android 4.4 framework tests (unchanged sources, as for 6.0.1):
# Looper/Handler/MessageQueue, Binder/Parcel/servicemanager, APK code and
# resources, framework-res. make fw8-tests-jar compiles them against the
# 8.1 framework (javac + d8 --min-api 27) without a VM, to catch API
# changes; with topic art8 they go into its arc-tests.jar (below).
FW8_TESTS := LooperTest BinderTest ApkTest FrameworkResTest
FW8_TEST_SRCS := $(addprefix tests/java/arcwasm/,$(addsuffix .java,$(FW8_TESTS)))
$(F8BLD)/tests-java/fw8-tests.jar: $(FW8_TEST_SRCS) $(F8_CLASSES) $(F8_CORE_CLASSES)
	rm -rf $(F8BLD)/tests-java && mkdir -p $(F8BLD)/tests-java/classes $(F8BLD)/tests-java/dex
	javac -source 8 -target 8 -Xlint:-options -encoding UTF-8 -nowarn \
	    -bootclasspath $(CORE8_CLASSES) \
	    -classpath $(subst $() ,:,$(strip $(F8_CLASSES))) -d $(F8BLD)/tests-java/classes $(FW8_TEST_SRCS)
	cd $(F8BLD)/tests-java/classes && jar cf ../classes.jar .
	"$(D8)" --release --min-api 27 --lib $(CORE8_CLASSES) \
	    $(addprefix --lib ,$(F8_CLASSES)) --output $(F8BLD)/tests-java/dex $(F8BLD)/tests-java/classes.jar
	rm -f $@ && cd $(F8BLD)/tests-java/dex && zip -q "$(abspath $@)" classes.dex
.PHONY: fw8-tests-jar
fw8-tests-jar: $(F8BLD)/tests-java/fw8-tests.jar

# --- with topic art8: artvm8 with the framework 8.1 ----------------------------------
# mk/topic-art8.mk (included before this file) defines A8LIB, ART8_LIBS,
# ASYSTEM8_ADD, ART8_JAVA_TESTS... Once it is merged, artvm8 links the
# framework's natives and its /system has the boot class path of 8.1;
# javac compiles against art8's core-all-classes.jar (F8_CORE_CLASSES,
# above). What artvm8 itself
# must do (art8/artvm8.cc, topic art8's) is in docs/topics/fw8-core.md
# ("Как подключить к artvm8"): BOOTCLASSPATH of 8.1 by default, and a weak
# call of arc_wasm_fw8_start(env) after JNI_CreateJavaVM.
ifneq ($(A8LIB),)
ASYSTEM8_ADD += $(foreach j,$(F8_JARS),--add $(j):system/framework/$(notdir $(j))) \
                --add $(F8RES_APK):system/framework/framework-res.apk \
                --add framework/build8.prop:system/build.prop \
                --add $(TEST_APK):system/app/ArcWasmTestApp.apk \
                --add $(BUILD)/dalvik/apk/dex/classes.dex:system/app/ArcWasmTestApp.dex
$(A8SYSTEM_IMAGE): $(F8_JARS) $(F8RES_APK) framework/build8.prop $(TEST_APK)
# (the test APK's classes.dex comes out of its rule, mk/topic-apk.mk)
$(BUILD)/dalvik/apk/dex/classes.dex: $(TEST_APK) ;
# BinderTest's natives (tests/native/binder_test_jni.cc for 8.1's binder.c,
# against libbinder 8.1), added to art8's table of static JNI libraries by
# tests/native/fw8/binder_test_lib8.cc.
F8_BINDER_TEST_SRCS := tests/native/binder_test_jni.cc tests/native/fw8/binder_test_lib8.cc
$(call f8obj,$(F8_BINDER_TEST_SRCS)): F8XFLAGS := -I$(F8_SVCMGR) -DARC_FW8=1 -DLOG_TAG=\"BinderTest\"
$(F8LIB)/libarcwasm_binder_test8.a: $(call f8obj,$(F8_BINDER_TEST_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
# libfilemap8.a out (FileMap is in libutils8.a); the JNI linked whole.
ART8_LIBS := $(filter-out %/libfilemap8.a,$(ART8_LIBS)) $(F8_UTILS_STATIC) \
    -Wl,--whole-archive $(FW8_JNI_LIBS) $(F8LIB)/libandroid_runtime8_icu.a \
    $(F8LIB)/libarcwasm_binder_test8.a -Wl,--no-whole-archive $(F8_LIBS)
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js: $(FW8_JNI_LIBS) $(F8LIB)/libandroid_runtime8_icu.a \
    $(F8LIB)/libarcwasm_binder_test8.a $(F8_LIBS) $(F8_UTILS_STATIC)
ART8_JAVA_TESTS += $(FW8_TEST_SRCS)
ART8_TESTS_CLASSPATH += $(F8_CLASSES)
$(A8FW)/arc-tests.jar: $(FW8_TEST_SRCS) $(F8_CLASSES)
.PHONY: fw8-test
fw8-test: art8
	VM=art8 tools/run_java_tests.sh node VmTest $(FW8_TESTS)
endif
