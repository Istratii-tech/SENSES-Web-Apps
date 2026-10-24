# Topic "fw10-core": the base of the Android 10 framework (AOSP
# android-10.0.0_r47, API 29; docs/topics/fw10-core.md) on topic art10's ART
# 10, the way topic fw8-core did it for 8.1 (mk/topic-fw8-core.mk):
#
#   host     aprotoc + protoc-gen-javastream (external/protobuf,
#            frameworks/base/tools/streaming_proto), hidl-gen
#            (system/tools/hidl), stats-log-api-gen (StatsLogInternal.java)
#            and sysprop_java (system/tools/sysprop): framework.jar 10 has
#            Java from all of them
#   Java     the boot class path jars of AOSP 10 but core-oj/core-libart
#            (topic art10; tools/build_framework10_jars.py), framework-res.apk
#            10 (aapt2) and its R classes, /system/build.prop (SDK 29). No
#            APEX: the jars of the runtime, conscrypt and media APEXes are in
#            /system/framework, as art10's BOOTCLASSPATH has them
#   native   libutils 10, libbinder 10 and servicemanager 10 on the
#            in-process /dev/binder of shim/binder, libhidl/libhwbinder in
#            process, libandroidfw 10, the libandroid_runtime 10 frame
#            (gRegJNI with weak entries) and its core JNI
#
#   make fw10-host        # the host tools
#   make fw10-jars        # the jars and framework-res.apk
#   make fw10-libs        # the native libraries
#   make art10            # artvm10 (topic art10) with all of it
#   make fw10-test        # the framework tests on ART 10 in Node
#
# Everything goes to build/fw10 (objects build/fw10/obj, libraries
# build/fw10/lib, jars build/fw10/framework, host tools build/fw10/host).
# Topics fw10-graphics, fw10-system and fw10-crypto-media build on it: see
# "Для тем фазы 2" in docs/topics/fw10-core.md.

F10BLD := $(BUILD)/fw10
F10FWB := $(A10)/frameworks/base
SDK ?= $(HOME)/Library/Android/sdk

# --- host tools ------------------------------------------------------------------
# Built with the Mac's clang, objects in build/fw10/host/obj.
F10H := $(F10BLD)/host
F10HOBJ := $(F10H)/obj
f10hobj = $(patsubst %,$(F10HOBJ)/%.o,$(1))
F10H_CXX := clang++
F10H_FLAGS := -O1 -g0 -w -MMD -MP
$(F10HOBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  HOSTCXX $<"
	@$(F10H_CXX) -std=gnu++14 $(F10H_FLAGS) $(F10HXFLAGS) -c $< -o $@
$(F10HOBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  HOSTCXX $<"
	@$(F10H_CXX) -std=gnu++17 $(F10H_FLAGS) $(F10HXFLAGS) -c $< -o $@
-include $(shell find $(F10HOBJ) -name '*.d' 2>/dev/null)

# aprotoc: protoc of AOSP 10 (protobuf 3.0, as in 8.1, with Android's
# javanano and javamicro generators); its library is also the C++ protobuf
# ("full") the two generators below link. protoc-gen-javastream: the plugin
# of framework-javastream-protos (field constants for
# android.util.proto.ProtoOutputStream).
F10_PB := $(A10)/external/protobuf
F10_PB_SRCS := $(addprefix $(F10_PB)/,$(shell python3 tools/bp_srcs.py $(F10_PB)/Android.bp aprotoc \
                 --target not_windows --target host))
F10_PB_LIB_SRCS := $(filter-out %/compiler/main.cc,$(F10_PB_SRCS))
$(call f10hobj,$(F10_PB_SRCS)): F10HXFLAGS := -I$(F10_PB)/android -I$(F10_PB)/src -DHAVE_ZLIB=1
$(F10H)/libaprotoc.a: $(call f10hobj,$(F10_PB_LIB_SRCS))
	@rm -f $@ && ar rcs $@ $^
$(F10H)/aprotoc: $(call f10hobj,$(F10_PB)/src/google/protobuf/compiler/main.cc) $(F10H)/libaprotoc.a
	$(F10H_CXX) -o $@ $^ -lz -lpthread
F10_JSTREAM := $(F10FWB)/tools/streaming_proto
F10_JSTREAM_SRCS := $(addprefix $(F10_JSTREAM)/,Errors.cpp stream_proto_utils.cpp string_utils.cpp java/main.cpp)
$(call f10hobj,$(F10_JSTREAM_SRCS)): F10HXFLAGS := -I$(F10_PB)/src -I$(F10_JSTREAM)
$(F10H)/protoc-gen-javastream: $(call f10hobj,$(F10_JSTREAM_SRCS)) $(F10H)/libaprotoc.a
	$(F10H_CXX) -o $@ $^ -lz -lpthread

# libbase 10 for the host tools (host copies, used by these tools only) and a
# liblog writing to stderr (topic fw8-core's).
F10_HBASE_SRCS := $(addprefix $(A10)/system/core/base/,file.cpp logging.cpp stringprintf.cpp strings.cpp \
                    errors_unix.cpp threads.cpp parsenetaddress.cpp) \
                  shim/fw8/host/liblog_host.cc
$(call f10hobj,$(F10_HBASE_SRCS)): F10HXFLAGS := -I$(A10)/system/core/base/include \
    -I$(A10)/system/core/liblog/include
$(F10H)/libbase_host.a: $(call f10hobj,$(F10_HBASE_SRCS))
	@rm -f $@ && ar rcs $@ $^

# hidl-gen 10: libhidl-gen, libhidl-gen-ast, libhidl-gen-hash (SHA-256 from
# CommonCrypto: shim/fw8/host/openssl/sha.h), libhidl-gen-host-utils,
# libhidl-gen-utils. The grammar is a GLR C++ parser, built with bison 2.7
# (topic fw8-core's $(F8_BISON) recipe, repeated: the Mac's bison 2.3 gets
# glr.cc wrong) and flex.
F10_HIDL := $(A10)/system/tools/hidl
F10_HIDL_GEN := $(F10H)/hidl-gen-src
# (the utils first: static FQName objects of the other libraries use
# FQName.cpp's static std::regex objects; in AOSP it is a shared library.)
F10_HIDL_SRCS := $(addprefix $(F10_HIDL)/,utils/FQName.cpp utils/FqInstance.cpp host_utils/Formatter.cpp \
                   host_utils/StringHelper.cpp \
                   $(shell python3 tools/bp_srcs.py $(F10_HIDL)/Android.bp libhidl-gen) \
                   $(filter %.cpp,$(shell python3 tools/bp_srcs.py $(F10_HIDL)/Android.bp libhidl-gen-ast)) \
                   hashing/Hash.cpp main.cpp) \
                 $(F10_HIDL_GEN)/hidl-gen_y.cpp $(F10_HIDL_GEN)/hidl-gen_l.cpp
BISON27_TGZ ?= $(ARC_SIBLINGS)/arm44-web/out/downloads/aosp/external/bison.tar.gz
F10_BISON ?= $(F10H)/bison/bin/bison
$(F10H)/bison/bin/bison:
	rm -rf $(F10H)/bison-src $(F10H)/bison-build $(F10H)/bison
	mkdir -p $(F10H)/bison-src $(F10H)/bison-build
	tar xzf "$(BISON27_TGZ)" -C $(F10H)/bison-src
	sed -i '' 's/^# if !(((__GLIBC__ > 2/# if !defined __APPLE__ \&\& !(((__GLIBC__ > 2/' $(F10H)/bison-src/lib/vasnprintf.c
	cd $(F10H)/bison-build && ../bison-src/configure --prefix="$(abspath $(F10H))/bison" --disable-nls \
	    CFLAGS="-O1 -w" > configure.log 2>&1 && $(MAKE) > make.log 2>&1 && $(MAKE) install > install.log 2>&1
	$@ --version | head -1
$(F10_HIDL_GEN)/hidl-gen_y.cpp: $(F10_HIDL)/hidl-gen_y.yy $(F10_BISON)
	@mkdir -p $(dir $@)
	$(F10_BISON) -d -o $@ --defines=$(F10_HIDL_GEN)/hidl-gen_y.h $<
$(F10_HIDL_GEN)/hidl-gen_y.h: $(F10_HIDL_GEN)/hidl-gen_y.cpp ;
$(F10_HIDL_GEN)/hidl-gen_l.cpp: $(F10_HIDL)/hidl-gen_l.ll $(F10_HIDL_GEN)/hidl-gen_y.h
	@mkdir -p $(dir $@)
	flex -o $@ $<
$(call f10hobj,$(F10_HIDL_SRCS)): F10HXFLAGS := -Ishim/fw8/host -I$(F10_HIDL) -I$(F10_HIDL_GEN) \
    -I$(F10_HIDL)/utils/include/hidl-util -I$(F10_HIDL)/utils/include \
    -I$(F10_HIDL)/host_utils/include/hidl-util -I$(F10_HIDL)/host_utils/include \
    -I$(F10_HIDL)/hashing/include/hidl-hash -I$(F10_HIDL)/hashing/include \
    -I$(A10)/system/core/base/include -I$(A10)/system/core/liblog/include \
    -I$(A10)/system/core/libutils/include -I$(A10)/system/core/libsystem/include
$(call f10hobj,$(F10_HIDL_GEN)/hidl-gen_l.cpp): $(F10_HIDL_GEN)/hidl-gen_y.h
$(F10H)/hidl-gen: $(call f10hobj,$(F10_HIDL_SRCS)) $(F10H)/libbase_host.a
	$(F10H_CXX) -o $@ $^

# stats-log-api-gen (frameworks/base/tools/stats_log_api_gen): writes
# StatsLogInternal.java (framework-statslog-gen) from the atoms of
# cmds/statsd/src/atoms.proto, which it links as C++ (libstats_proto_host and
# what it imports: libplatformprotos, core/proto and libs/incident/proto).
F10_STATS := $(F10FWB)/tools/stats_log_api_gen
F10_PROTOCPP := $(F10H)/proto-cpp
F10_PLATFORM_PROTOS := $(shell cd $(A10) 2>/dev/null && find frameworks/base/core/proto \
                         -name '*.proto' | sort) \
                       frameworks/base/cmds/statsd/src/atoms.proto \
                       frameworks/base/cmds/statsd/src/atom_field_options.proto
F10_PROTOCPP_SRCS := $(addprefix $(F10_PROTOCPP)/,$(F10_PLATFORM_PROTOS:.proto=.pb.cc))
$(F10_PROTOCPP)/protos.stamp: $(F10H)/aprotoc $(addprefix $(A10)/,$(F10_PLATFORM_PROTOS))
	rm -rf $(F10_PROTOCPP) && mkdir -p $(F10_PROTOCPP)
	cd $(A10) && "$(abspath $(F10H))/aprotoc" -I. -Iexternal/protobuf/src \
	    --cpp_out="$(abspath $(F10_PROTOCPP))" $(F10_PLATFORM_PROTOS)
	@touch $@
$(F10_PROTOCPP_SRCS): $(F10_PROTOCPP)/protos.stamp ;
$(call f10hobj,$(F10_PROTOCPP_SRCS) $(F10_STATS)/Collation.cpp $(F10_STATS)/main.cpp): \
    F10HXFLAGS := -I$(F10_PROTOCPP) -I$(F10_PB)/src -I$(A10)/system/core/base/include -DGOOGLE_PROTOBUF_NO_RTTI
$(call f10hobj,$(F10_STATS)/Collation.cpp $(F10_STATS)/main.cpp): $(F10_PROTOCPP)/protos.stamp
$(F10H)/stats-log-api-gen: $(call f10hobj,$(F10_STATS)/Collation.cpp $(F10_STATS)/main.cpp \
                             $(F10_PROTOCPP_SRCS)) $(F10H)/libaprotoc.a $(F10H)/libbase_host.a
	$(F10H_CXX) -o $@ $^ -lz -lpthread

# sysprop_java (system/tools/sysprop): Java of the .sysprop files of
# framework.jar's srcs (:platform-properties of system/libsysprop,
# :apex-properties of system/apex): android.sysprop.*Properties.
F10_SYSPROP := $(A10)/system/tools/sysprop
F10_SYSPROP_SRCS := $(addprefix $(F10_SYSPROP)/,Common.cpp CodeWriter.cpp JavaGen.cpp JavaMain.cpp) \
                    $(F10H)/sysprop-cpp/sysprop.pb.cc
$(F10H)/sysprop-cpp/sysprop.pb.cc: $(F10_SYSPROP)/sysprop.proto $(F10H)/aprotoc
	@mkdir -p $(dir $@)
	$(F10H)/aprotoc -I$(F10_SYSPROP) -I$(F10_PB)/src --cpp_out=$(dir $@) $<
$(call f10hobj,$(F10_SYSPROP_SRCS)): F10HXFLAGS := -I$(F10H)/sysprop-cpp -I$(F10_SYSPROP)/include \
    -I$(F10_PB)/src -I$(A10)/system/core/base/include
$(call f10hobj,$(F10_SYSPROP_SRCS)): $(F10H)/sysprop-cpp/sysprop.pb.cc
$(F10H)/sysprop_java: $(call f10hobj,$(F10_SYSPROP_SRCS)) $(F10H)/libaprotoc.a $(F10H)/libbase_host.a
	$(F10H_CXX) -o $@ $^ -lz -lpthread

F10_HOST_TOOLS := $(F10H)/aprotoc $(F10H)/protoc-gen-javastream $(F10H)/hidl-gen \
                  $(F10H)/stats-log-api-gen $(F10H)/sysprop_java
.PHONY: fw10-host
fw10-host: $(F10_HOST_TOOLS)

# --- framework-res.apk 10 and android.R / com.android.internal.R ---------------
# aapt2 (build-tools 35), as core/res/Android.bp (android_app framework-res;
# Soong links it with package id 0x01, -x): compile --legacy, link with
# --private-symbols com.android.internal, --no-auto-version,
# --auto-add-overlay. English and mdpi only by default, as for 4.4, 6.0.1 and
# 8.1 (FRAMEWORK10_RES_LOCALES=, FRAMEWORK10_RES_DENSITY= keep everything).
AAPT2 ?= $(lastword $(sort $(wildcard $(SDK)/build-tools/*/aapt2)))
F10RES := $(F10FWB)/core/res
F10RES_OUT := $(F10BLD)/framework-res
F10RES_APK := $(F10RES_OUT)/framework-res.apk
FRAMEWORK10_RES_LOCALES ?= en
FRAMEWORK10_RES_DENSITY ?= mdpi
F10RES_FLAGS := -x --min-sdk-version 29 --target-sdk-version 29 \
    --version-code 29 --version-name 10 --private-symbols com.android.internal --no-auto-version \
    --auto-add-overlay \
    $(if $(FRAMEWORK10_RES_LOCALES),-c $(FRAMEWORK10_RES_LOCALES)) \
    $(if $(FRAMEWORK10_RES_DENSITY),--preferred-density $(FRAMEWORK10_RES_DENSITY))
F10RES_GEN := $(F10RES_OUT)/gen
F10RES_JAVA := $(F10RES_GEN)/android/R.java $(F10RES_GEN)/android/Manifest.java \
    $(F10RES_GEN)/com/android/internal/R.java
$(F10RES_OUT)/aapt2-flags: FORCE
	@mkdir -p $(dir $@)
	@echo '$(F10RES_FLAGS)' | cmp -s - $@ || echo '$(F10RES_FLAGS)' > $@
F10RES_FILES := $(F10RES)/AndroidManifest.xml $(shell find $(F10RES)/res $(F10RES)/assets -type f 2>/dev/null)
$(F10RES_OUT)/res.zip: $(F10RES_FILES)
	@mkdir -p $(dir $@)
	"$(AAPT2)" compile --legacy --dir $(F10RES)/res -o $@ 2> $(F10RES_OUT)/compile.log \
	    || { tail -20 $(F10RES_OUT)/compile.log; rm -f $@; exit 1; }
$(F10RES_OUT)/aapt2.stamp: $(F10RES_OUT)/res.zip $(F10RES_OUT)/aapt2-flags
	@rm -rf $(F10RES_GEN) && mkdir -p $(F10RES_GEN)
	"$(AAPT2)" link -o $(F10RES_APK) $(F10RES_FLAGS) --manifest $(F10RES)/AndroidManifest.xml \
	    -A $(F10RES)/assets --java $(F10RES_GEN) $< 2> $(F10RES_OUT)/link.log \
	    || { grep -v '^warn: removing' $(F10RES_OUT)/link.log | tail -20; exit 1; }
	@echo "built $(F10RES_APK) ($$(wc -c < $(F10RES_APK) | tr -d ' ') bytes)"
	@touch $@
$(F10RES_APK) $(F10RES_JAVA): $(F10RES_OUT)/aapt2.stamp ;
.PHONY: fw10-res FORCE
fw10-res: $(F10RES_APK)

# --- native: shared flags and object rules ------------------------------------------
# Objects in build/fw10/obj. The flags are topic art10's (A10_CFLAGS /
# A10_CXXFLAGS of mk/topic-art10.mk, with its art10_wasm_compat.h) plus
# shim/fw10/fw10_compat.h. No __ANDROID__ (as art10 builds system/core 10):
# the few device-only paths the framework needs are opened for ARC_WASM by
# patches/aosp10/fw10-core.patch, as fw8-core did for 8.1.
F10OBJ := $(F10BLD)/obj
F10LIB := $(F10BLD)/lib
f10obj = $(patsubst %,$(F10OBJ)/%.o,$(1))
F10_CFLAGS := $(A10_CFLAGS) -include shim/fw10/fw10_compat.h
F10_CXXFLAGS := $(A10_CXXFLAGS) -include shim/fw10/fw10_compat.h
# FW10_INCLUDES: what code built against the 10 framework's native headers
# needs (topics fw10-graphics, fw10-system... compile with these).
#   shim/fw10/include  bionic_malloc.h (bionic's private header)
#   shim/fw8/include   linux/binder.h (the UAPI's, through -idirafter), selinux/,
#                      sys/system_properties.h... (the same for 8.1 and 10)
#   shim/binder/include  arc_binder.h (the in-process driver's extensions)
# The binder protocol is the 32-bit one (BINDER_IPC_32BIT), as for 6.0.1 and
# 8.1: the in-process driver speaks protocol 7.
FW10_INCLUDES := -Ishim/fw10/include -Ishim/fw8/include -Ishim/binder/include \
    $(addprefix -I$(A10)/system/core/,base/include liblog/include libcutils/include \
      libutils/include libsystem/include libziparchive/include libbacktrace/include \
      libprocessgroup/include libprocinfo/include) \
    -I$(A10)/libnativehelper/include_jni -I$(A10)/libnativehelper/include \
    -I$(A10)/libnativehelper/include/nativehelper -I$(A10)/libnativehelper/header_only_include \
    -I$(A10)/libnativehelper/platform_include \
    $(addprefix -I$(A10)/frameworks/native/,include libs/binder/include libs/binderthreadstate/include) \
    -I$(F10FWB)/libs/androidfw/include -I$(A10)/system/core/libmeminfo/include \
    -I$(A10)/system/libhidl/base/include -I$(A10)/system/libhidl/transport/include \
    -I$(A10)/system/libhidl/transport/token/1.0/utils/include \
    -I$(A10)/system/libhwbinder/include \
    $(A10_UAPI) -DBINDER_IPC_32BIT=1 -sUSE_ZLIB=1
# -DLOG_NDEBUG=1 as in a release build: ALOGV and LOG_FATAL_IF compile out
# (libbinder would log every command, see docs/topics/binder.md).
F10_FLAGS := $(FW10_INCLUDES) -DLOG_NDEBUG=1
$(F10OBJ)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(EMCC) $(F10_CFLAGS) $(F10_FLAGS) $(F10XFLAGS) -c $< -o $@
$(F10OBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(F10_CXXFLAGS) $(F10_FLAGS) $(F10XFLAGS) -c $< -o $@
$(F10OBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(F10_CXXFLAGS) $(F10_FLAGS) $(F10XFLAGS) -c $< -o $@
-include $(shell find $(F10OBJ) -name '*.d' 2>/dev/null)
f10bp = $(addprefix $(dir $(1)),$(shell tools/bp_srcs.py $(1) $(2) $(3)))

# --- libutils 10, the properties ------------------------------------------------------
# system/core 10 is topic art10's (A10_SYS_LIBS: libbase10, liblog10,
# libcutils10, libziparchive10, libnativehelper10, libfilemap10...); this
# topic adds libutils. The host list (Trace.cpp is the device's:
# utils/Trace.h is empty without __ANDROID__) without FileMap.cpp (art10's
# libfilemap10.a has it), with Looper on a pipe instead of eventfd
# (fw10-core.patch); CallStack/ProcessCallStack need libbacktrace: an empty
# CallStack (shim/fw6/callstack_stub.cc, the API is unchanged); the thread
# priorities of android.os.Process (shim/fw6/thread_priority.cc).
F10_UTILS := $(A10)/system/core/libutils
F10_UTILS_SRCS := $(filter-out %/CallStack.cpp %/ProcessCallStack.cpp %/FileMap.cpp, \
                    $(call f10bp,$(F10_UTILS)/Android.bp,libutils,--target linux --target host)) \
                  shim/fw6/thread_priority.cc shim/fw6/callstack_stub.cc
# (its sched_getscheduler is topic art10's libc extras, or the weak one of
# shim/fw8/cutils_fw8.cc.)
$(call f10obj,shim/fw6/thread_priority.cc): F10XFLAGS := -Dsched_getscheduler=arc_fw6_sched_getscheduler
$(F10LIB)/libutils10.a: $(call f10obj,$(F10_UTILS_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
# The property store: bionic's __system_property_* in process (6.0.1's,
# shim/fw6/system_properties.cc, and what 8.0 added) and libcutils 10's
# properties.cpp, which art10's host list of libcutils leaves out;
# libprocessgroup without cgroups (scheduling policies, cpusets, process
# groups: shim/fw10/processgroup10.cc, shim/fw8/cutils_fw8.cc) and sysinfo().
F10_PROPS_SRCS := shim/fw6/system_properties.cc shim/fw8/system_properties8.cc \
                  $(A10)/system/core/libcutils/properties.cpp shim/fw8/cutils_fw8.cc \
                  shim/fw10/processgroup10.cc
$(F10LIB)/libprops10.a: $(call f10obj,$(F10_PROPS_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libbinder 10, servicemanager 10 --------------------------------------------------
# On /dev/binder of shim/binder/dev_binder.cc (libdev_binder.a, mk/topic-binder.mk).
# Without the aidl-cpp interface IPackageManagerNative (:libbinder_aidl;
# nothing here calls it).
F10_BINDER := $(A10)/frameworks/native/libs/binder
# (+ libbinderthreadstate, which IPCThreadState shares with libhwbinder's.)
F10_BINDER_SRCS := $(filter %.cpp,$(call f10bp,$(F10_BINDER)/Android.bp,libbinder)) \
                   $(A10)/frameworks/native/libs/binderthreadstate/IPCThreadStateBase.cpp
$(F10LIB)/libbinder10.a: $(call f10obj,$(F10_BINDER_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
F10_SVCMGR := $(A10)/frameworks/native/cmds/servicemanager
F10_SVCMGR_SRCS := $(F10_SVCMGR)/service_manager.c $(F10_SVCMGR)/binder.c \
                   shim/fw10/servicemanager10_main.c shim/fw8/selinux8_stub.c
# (bionic's headers bring stdbool.h in and define __unused.)
$(call f10obj,$(F10_SVCMGR)/service_manager.c): F10XFLAGS := -Dmain=arc_servicemanager10_main \
    -Dbinder_loop=arc_servicemanager10_loop -include stdbool.h '-D__unused=__attribute__((__unused__))'
$(call f10obj,$(F10_SVCMGR)/binder.c): F10XFLAGS := '-D__unused=__attribute__((__unused__))'
$(call f10obj,shim/fw10/servicemanager10_main.c): F10XFLAGS := -I$(F10_SVCMGR)
$(F10LIB)/libservicemanager10.a: $(call f10obj,$(F10_SVCMGR_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libmeminfo (system/core) -------------------------------------------------------
# /proc/PID/smaps and /proc/meminfo for android.os.Debug and Process
# (getPss, getFreeMemory...); reads fail on a kernel without them.
F10_MEMINFO_SRCS := $(call f10bp,$(A10)/system/core/libmeminfo/Android.bp,libmeminfo)
$(F10LIB)/libmeminfo10.a: $(call f10obj,$(F10_MEMINFO_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libandroidfw 10 ---------------------------------------------------------------
# The device list (AssetManager and AssetManager2/ApkAssets/LoadedArsc/Idmap,
# the backup helpers, CursorWindow) without DisplayEventDispatcher (libgui:
# topic fw10-graphics).
F10_ANDROIDFW := $(F10FWB)/libs/androidfw
F10_ANDROIDFW_SRCS := $(filter-out %/DisplayEventDispatcher.cpp, \
                        $(call f10bp,$(F10_ANDROIDFW)/Android.bp,libandroidfw,--target android))
$(F10LIB)/libandroidfw10.a: $(call f10obj,$(F10_ANDROIDFW_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- HIDL in process: libhidlbase, libhwbinder, libhidltransport ---------------------
# As fw8-core: the HIDL runtime the framework's android.os.Hw* natives use,
# without HALs and without hwbinder (no /dev/hwbinder: libhidltransport's
# defaultServiceManager() is null, as on a device without hwbinder, and every
# IFoo.getService() fails; passthrough HALs cannot be opened either,
# shim/fw8/vndksupport_stub.c). The C++ of android.hidl.base@1.0 and
# android.hidl.manager@1.0/1.1/1.2 comes from hidl-gen 10.
F10_HIDL_GENCPP := $(F10BLD)/gen/hidl-cpp
F10_HIDL_CPP_PKGS := android.hidl.base@1.0 android.hidl.manager@1.0 android.hidl.manager@1.1 \
                     android.hidl.manager@1.2
F10_HIDL_CPP_SRCS := $(addprefix $(F10_HIDL_GENCPP)/android/hidl/,base/1.0/BaseAll.cpp base/1.0/types.cpp \
                       manager/1.0/ServiceManagerAll.cpp manager/1.0/ServiceNotificationAll.cpp \
                       manager/1.1/ServiceManagerAll.cpp manager/1.2/ServiceManagerAll.cpp \
                       manager/1.2/ClientCallbackAll.cpp)
$(F10_HIDL_GENCPP)/hidl.stamp: $(F10H)/hidl-gen $(wildcard $(A10)/system/libhidl/transport/base/1.0/*.hal \
                                 $(A10)/system/libhidl/transport/manager/*/*.hal)
	rm -rf $(F10_HIDL_GENCPP) && mkdir -p $(F10_HIDL_GENCPP)
	for p in $(F10_HIDL_CPP_PKGS); do \
	  for l in c++-headers c++-sources; do \
	    $(F10H)/hidl-gen -o $(F10_HIDL_GENCPP) -L$$l -randroid.hidl:$(A10)/system/libhidl/transport $$p || exit 1; \
	  done; \
	done
	@touch $@
$(F10_HIDL_CPP_SRCS): $(F10_HIDL_GENCPP)/hidl.stamp ;
F10_HIDL_LIB_SRCS := $(addprefix $(A10)/system/libhidl/base/,HidlInternal.cpp HidlSupport.cpp Status.cpp \
                       TaskRunner.cpp) \
                     $(addprefix $(A10)/system/libhwbinder/,Binder.cpp BpHwBinder.cpp BufferedTextOutput.cpp \
                       Debug.cpp IInterface.cpp IPCThreadState.cpp Parcel.cpp ProcessState.cpp Static.cpp \
                       TextOutput.cpp) \
                     $(addprefix $(A10)/system/libhidl/transport/,HidlBinderSupport.cpp HidlLazyUtils.cpp \
                       HidlPassthroughSupport.cpp HidlTransportSupport.cpp HidlTransportUtils.cpp \
                       ServiceManagement.cpp Static.cpp) \
                     $(F10_HIDL_CPP_SRCS) shim/fw8/vndksupport_stub.c
$(call f10obj,$(F10_HIDL_LIB_SRCS)): F10XFLAGS := -I$(F10_HIDL_GENCPP) -I$(A10)/system/core/libvndksupport/include \
    -I$(A10)/system/libhidl/transport
$(call f10obj,$(F10_HIDL_LIB_SRCS)): $(F10_HIDL_GENCPP)/hidl.stamp
$(F10LIB)/libhidl10.a: $(call f10obj,$(F10_HIDL_LIB_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libandroid_runtime 10: the frame and the core JNI --------------------------------
# AndroidRuntime.cpp itself is replaced by shim/fw10/android_runtime10.cc (the
# frame, RuntimeInit/ZygoteInit) and shim/fw10/register_fw10.cc (gRegJNI of
# 10, weak entries). The core: Log, SystemClock, EventLog, Trace,
# MessageQueue (Looper), Binder, Parcel, AssetManager/ApkAssets (10's
# AssetManager is AssetManager2 only)/StringBlock/XmlBlock, Process,
# SystemProperties, Configuration, VirtualRefBasePtr and the HIDL natives
# (android.os.HwBinder/HwParcel/HwBlob/HwRemoteBinder/HidlSupport/
# NativeHandle on libhidl10.a). Beyond the core, the rest of core/jni that
# needs neither graphics, media, input, sqlite nor hardware (as fw8-core).
F10_JNI := $(F10FWB)/core/jni
F10_JNI_CORE := android_util_Log android_os_SystemClock android_util_EventLog android_os_Trace \
    android_os_MessageQueue android_util_Binder android_os_Parcel android_util_AssetManager \
    android_content_res_ApkAssets android_util_StringBlock android_util_XmlBlock android_util_Process \
    android_os_SystemProperties android_nio_utils android_content_res_Configuration \
    com_android_internal_util_VirtualRefBasePtr android_os_HwBinder android_os_HwBlob android_os_HwParcel \
    android_os_HwRemoteBinder android_os_HidlSupport android_os_NativeHandle
F10_JNI_EXTRA := android_os_Debug android_util_FileObserver android_net_LocalSocketImpl \
    android_os_MemoryFile android_os_SharedMemory android_util_MemoryIntArray \
    android_ddm_DdmHandleNativeHeap android_backup_BackupDataInput android_backup_BackupDataOutput \
    android_backup_BackupHelperDispatcher android_backup_FileBackupHelperBase \
    android_app_backup_FullBackup android_content_res_ObbScanner android_animation_PropertyValuesHolder \
    com_android_internal_content_NativeLibraryHelper com_android_internal_os_AtomicDirectory \
    android_util_jar_StrictJarFile android_app_admin_SecurityLog
# The JNI that needs ICU (topic art10's ICU 63 libraries): its own archive,
# linked where ICU is (artvm10).
F10_JNI_ICU := android_text_AndroidCharacter
F10_JNI_SRCS := $(addprefix $(F10_JNI)/,$(addsuffix .cpp,$(F10_JNI_CORE) $(F10_JNI_EXTRA)) \
                  hwbinder/EphemeralStorage.cpp) \
                shim/fw10/android_runtime10.cc shim/fw10/register_fw10.cc shim/binder/binder_runtime.cc \
                shim/fw6/memtrack_stub.c shim/framework/debug_support.cc shim/fw8/debuggerd_stub.cc \
                shim/fw8/memunreachable_stub.cc shim/fw10/sys10_fallbacks.cc
F10_JNI_FLAGS := -I$(F10_JNI) -I$(F10_JNI)/include -I$(F10_HIDL_GENCPP) \
    -I$(A10)/external/icu/icu4c/source/common -I$(A10)/external/icu/icu4c/source/i18n \
    -I$(A10)/system/core/libmemtrack/include -I$(A10)/system/core/libvndksupport/include \
    -I$(A10)/system/core/libmemunreachable/include \
    -DU_USING_ICU_NAMESPACE=0
$(call f10obj,$(F10_JNI_SRCS)): F10XFLAGS := $(F10_JNI_FLAGS)
$(call f10obj,$(F10_JNI_SRCS)): $(F10_HIDL_GENCPP)/hidl.stamp
$(call f10obj,shim/fw10/android_runtime10.cc shim/binder/binder_runtime.cc): \
    F10XFLAGS := $(F10_JNI_FLAGS) -DLOG_TAG=\"AndroidRuntime\"
# It includes the UAPI's <linux/capability.h> and libcutils' fs_config.h,
# which brings its own copy of it unless __BIONIC__: the copy is kept out.
$(call f10obj,$(F10_JNI)/android_util_AssetManager.cpp): \
    F10XFLAGS := $(F10_JNI_FLAGS) -D_SYSTEM_CORE_INCLUDE_PRIVATE_ANDROID_FILESYSTEM_CAPABILITY_H
# (bionic's <cutils/ashmem.h> includes the UAPI's <linux/ashmem.h>.)
$(call f10obj,$(F10_JNI)/android_os_MemoryFile.cpp): F10XFLAGS := $(F10_JNI_FLAGS) -include linux/ashmem.h
$(call f10obj,$(F10_JNI)/android_os_Debug.cpp): F10XFLAGS := $(F10_JNI_FLAGS) -include malloc_info.h
$(F10LIB)/libandroid_runtime10.a: $(call f10obj,$(F10_JNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
F10_JNI_ICU_SRCS := $(addprefix $(F10_JNI)/,$(addsuffix .cpp,$(F10_JNI_ICU)))
$(call f10obj,$(F10_JNI_ICU_SRCS)): F10XFLAGS := $(F10_JNI_FLAGS)
$(F10LIB)/libandroid_runtime10_icu.a: $(call f10obj,$(F10_JNI_ICU_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# FW10_JNI_LIBS: libraries of JNI objects (each defines register_* functions
# of gRegJNI). Linked whole: register_fw10.cc refers to register_* weakly,
# and a weak reference does not pull an archive member in. Topics append
# theirs (FW10_JNI_LIBS += ...) and the libraries those need to F10_LIBS.
FW10_JNI_LIBS := $(F10LIB)/libandroid_runtime10.a $(F10LIB)/libandroid_runtime10_icu.a
F10_LIBS := $(F10LIB)/libandroidfw10.a $(F10LIB)/libbinder10.a $(F10LIB)/libservicemanager10.a \
            $(F10LIB)/libhidl10.a $(F10LIB)/libmeminfo10.a $(F10LIB)/libutils10.a $(F10LIB)/libprops10.a
.PHONY: fw10-libs
fw10-libs: $(FW10_JNI_LIBS) $(F10_LIBS)

# --- the boot class path jars ---------------------------------------------------
# BOOTCLASSPATH of AOSP 10 (build/make: the runtime APEX's core-oj
# core-libart okhttp bouncycastle apache-xml, then PRODUCT_BOOT_JARS of
# base_system.mk: framework ext telephony-common voip-common ims-common
# android.test.base, then the updatable ones: conscrypt, updatable-media),
# all in /system/framework (no APEX), as topic art10's artvm10.cc lists them.
# core-oj and core-libart are topic art10's; javac compiles against its
# core-all-classes.jar (A10_CORE_CLASSES). org.apache.http.legacy is not on
# the boot class path in 10: it is the shared library of apps that ask for
# it (topic fw10-system installs it as one).
F10FW := $(F10BLD)/framework
F10_BOOT_JARS := okhttp bouncycastle apache-xml framework ext telephony-common voip-common ims-common \
                 android.test.base conscrypt updatable-media
F10_LIB_JARS := conscrypt okhttp bouncycastle apache-xml ext
F10_OPT_JARS := telephony-common voip-common ims-common android.test.base org.apache.http.legacy
F10_JARS := $(addprefix $(F10FW)/,$(addsuffix .jar,$(F10_BOOT_JARS) org.apache.http.legacy))
# javac output of the framework and what it is compiled against, for tests
# and the topics of phase 2.
F10_CLASSES := $(foreach j,$(F10_LIB_JARS) framework updatable-media,$(F10FW)/$(j)-java/$(j)-classes.jar)
F10_JARS_PY := tools/build_framework10_jars.py
F10_JARS_ENV := AOSP10=$(A10) CORE10_CLASSES=$(A10_CORE_CLASSES) HOST=$(F10H) D8="$(D8)"
F10_LIB_SRCS := $(shell find $(addprefix $(A10)/external/,conscrypt/repackaged/common conscrypt/repackaged/platform \
                  conscrypt/publicapi okhttp/repackaged bouncycastle/repackaged nist-sip/java tagsoup/src \
                  libphonenumber/repackaged apache-xml/src/main/java rappor/client libtextclassifier/java) \
                  -type f -name '*.java' 2>/dev/null)
$(F10FW)/fw10-libs.stamp: $(F10_JARS_PY) $(A10_CORE_CLASSES) $(F10_LIB_SRCS)
	$(F10_JARS_ENV) python3 -I $(F10_JARS_PY) $(F10FW) libs
	@touch $@
$(foreach j,$(F10_LIB_JARS),$(F10FW)/$(j).jar $(F10FW)/$(j)-java/$(j)-classes.jar): $(F10FW)/fw10-libs.stamp ;
$(A10_CORE_CLASSES): $(A10FW)/core-oj.jar ;

F10_SRC_DIRS := $(addprefix $(F10FWB)/,$(addsuffix /java,core graphics location lowpan media media/apex \
                  media/mca/effect media/mca/filterfw media/mca/filterpacks drm opengl sax telecomm telephony \
                  wifi keystore rs))
F10_SRCS := $(shell find $(F10_SRC_DIRS) $(F10FWB)/core/proto $(F10FWB)/proto/src $(F10FWB)/graphics/proto \
              $(F10FWB)/cmds/statsd/src/atoms.proto $(A10)/system/libsysprop/srcs $(A10)/system/apex/apexd/aidl \
              -name '*.java' -o -name '*.aidl' -o -name '*.logtags' -o -name '*.proto' -o -name '*.sysprop' \
              2>/dev/null) $(F10FWB)/Android.bp $(F10FWB)/media/Android.bp $(F10FWB)/config/preloaded-classes
$(F10FW)/framework.jar: $(F10_JARS_PY) $(F10FW)/fw10-libs.stamp $(F10_HOST_TOOLS) $(F10RES_JAVA) $(F10_SRCS)
	$(F10_JARS_ENV) python3 -I $(F10_JARS_PY) $(F10FW) framework $(F10RES_JAVA)
$(F10FW)/updatable-media.jar $(F10FW)/framework-java/framework-classes.jar \
    $(F10FW)/updatable-media-java/updatable-media-classes.jar: $(F10FW)/framework.jar ;

F10_OPT_SRCS := $(shell find $(A10)/frameworks/opt/telephony/src $(A10)/frameworks/opt/telephony/proto \
                  $(A10)/frameworks/opt/net/voip/src $(A10)/frameworks/opt/net/ims/src \
                  $(F10FWB)/test-base/src $(A10)/external/apache-http/src $(A10)/external/apache-http/android \
                  -type f 2>/dev/null)
$(F10FW)/fw10-opt.stamp: $(F10_JARS_PY) $(F10FW)/framework.jar $(F10_OPT_SRCS)
	$(F10_JARS_ENV) python3 -I $(F10_JARS_PY) $(F10FW) opt
	@touch $@
$(addprefix $(F10FW)/,$(addsuffix .jar,$(F10_OPT_JARS))): $(F10FW)/fw10-opt.stamp ;

.PHONY: fw10-jars
fw10-jars: $(F10_JARS) $(F10RES_APK)

# --- the Java tests of the framework, against 10 -------------------------------------
# The framework tests of 4.4/6.0.1/8.1 (unchanged sources: Looper/Handler/
# MessageQueue, Binder/Parcel/servicemanager, APK code and resources,
# framework-res) and Framework10Test (tests/java10: API 29 classes, a
# Context of package "android", ApkAssets, hidden API, the boot class path
# of 10), in topic art10's arc-tests.jar.
FW10_TESTS := LooperTest BinderTest ApkTest FrameworkResTest Framework10Test
FW10_TEST_SRCS := $(addprefix tests/java/arcwasm/,LooperTest.java BinderTest.java ApkTest.java \
                    FrameworkResTest.java) tests/java10/arcwasm/Framework10Test.java

# --- artvm10 with the framework 10 ----------------------------------------------------
# mk/topic-art10.mk (included before this file) defines A10LIB, ART10_LIBS,
# ASYSTEM10_ADD, ART10_JAVA_TESTS...: artvm10 links the framework's natives
# and its /system has the boot class path of 10 (artvm10.cc lists it and
# calls arc_wasm_fw10_start(env) when android.os.Binder is there).
# /system/build.prop is framework/build10.prop (a later --add of the same
# path wins over art10's).
ASYSTEM10_ADD += $(foreach j,$(F10_JARS),--add $(j):system/framework/$(notdir $(j))) \
                 --add $(F10RES_APK):system/framework/framework-res.apk \
                 --add framework/build10.prop:system/build.prop \
                 --add $(TEST_APK):system/app/ArcWasmTestApp.apk \
                 --add $(BUILD)/dalvik/apk/dex/classes.dex:system/app/ArcWasmTestApp.dex
# (the test APK's classes.dex comes out of its rule, mk/topic-apk.mk; the
# rule that says so is topic fw8-core's)
$(A10SYSTEM_IMAGE): $(F10_JARS) $(F10RES_APK) framework/build10.prop $(TEST_APK)
# BinderTest's natives (tests/native/binder_test_jni.cc for 10's binder.c,
# against libbinder 10), added to art10's table of static JNI libraries by
# tests/native/fw10/binder_test_lib10.cc.
F10_BINDER_TEST_SRCS := tests/native/binder_test_jni.cc tests/native/fw10/binder_test_lib10.cc
$(call f10obj,$(F10_BINDER_TEST_SRCS)): F10XFLAGS := -I$(F10_SVCMGR) -DARC_FW10=1 -DLOG_TAG=\"BinderTest\"
$(F10LIB)/libarcwasm_binder_test10.a: $(call f10obj,$(F10_BINDER_TEST_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
# The JNI linked whole.
ART10_LIBS += -Wl,--whole-archive $(FW10_JNI_LIBS) $(F10LIB)/libarcwasm_binder_test10.a \
              -Wl,--no-whole-archive $(F10_LIBS)
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js: $(FW10_JNI_LIBS) $(F10LIB)/libarcwasm_binder_test10.a $(F10_LIBS)
ART10_JAVA_TESTS += $(FW10_TEST_SRCS)
ART10_TESTS_CLASSPATH += $(F10_CLASSES)
$(A10FW)/arc-tests.jar: $(FW10_TEST_SRCS) $(F10_CLASSES)
.PHONY: fw10-test
fw10-test: art10
	VM=art10 tools/run_java_tests.sh node VmTest $(FW10_TESTS)
