# Topic "art": ART (AOSP 6.0.1) for WebAssembly, interpreter only, with
# libcore 6.0 (core-libart.jar), ICU 55 and libjavacore. Sources come from
# third_party/aosp60 (tools/fetch_aosp60.sh); see docs/topics/art.md.
#
#   make art          # build/art/artvm.js (Node)
#   make art-web      # build/art-web/: serve with tools/serve.py, open
#                     # index.html?arg=-cp&arg=/system/framework/arc-tests.jar&arg=arcwasm.VmTest
#   make art-test     # VmTest on ART in Node
#
# Everything is kept apart from the Dalvik build: objects in build/art/obj,
# libraries in build/art/lib, the /system image in build/art/system.img.

A60   := third_party/aosp60
ABLD  := $(BUILD)/art
AOBJ  := $(ABLD)/obj
ALIB  := $(ABLD)/lib
aobj   = $(patsubst %,$(AOBJ)/%.o,$(1))

A_COMMON := -pthread -O2 -g1 -D_GNU_SOURCE -DARC_WASM=1 -DANDROID_SMP=1 \
            -include shim/dalvik/AndroidConfig.h -include shim/art/art_wasm_compat.h \
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
            -MMD -MP
A_CFLAGS   := $(A_COMMON) -std=gnu11
A_CXXFLAGS := $(A_COMMON) -std=gnu++14 -Wno-register

$(AOBJ)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(EMCC) $(A_CFLAGS) $(AFLAGS) -c $< -o $@

$(AOBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(A_CXXFLAGS) $(AFLAGS) -c $< -o $@

$(AOBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(A_CXXFLAGS) $(AFLAGS) -c $< -o $@

-include $(shell find $(AOBJ) -name '*.d' 2>/dev/null)

# --- ICU 55 (external/icu/icu4c) --------------------------------------------
ICU55 := $(A60)/external/icu/icu4c/source
# ../stubdata/stubdata.c: the empty icudt55_dat; libcore's ICU natives map
# the data file (/system/usr/icu/icudt55l.dat) themselves.
ICU55_UC_SRCS   := $(shell tools/mk_srcs.py $(ICU55)/common/Android.mk src_files)
ICU55_I18N_SRCS := $(filter %.c %.cpp,$(shell tools/mk_srcs.py $(ICU55)/i18n/Android.mk src_files))
ICU55_INCLUDES  := -I$(ICU55)/common -I$(ICU55)/i18n
$(AOBJ)/$(ICU55)/common/% $(AOBJ)/$(ICU55)/stubdata/%: AFLAGS := $(ICU55_INCLUDES) -D_REENTRANT -DU_COMMON_IMPLEMENTATION \
    -DU_HAVE_NL_LANGINFO_CODESET=0 -DU_TIMEZONE=timezone
$(AOBJ)/$(ICU55)/i18n/%: AFLAGS := $(ICU55_INCLUDES) -D_REENTRANT -DU_I18N_IMPLEMENTATION
$(ALIB)/libicuuc.a: $(call aobj,$(subst /common/../stubdata/,/stubdata/,$(addprefix $(ICU55)/common/,$(ICU55_UC_SRCS))))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
$(ALIB)/libicui18n.a: $(call aobj,$(addprefix $(ICU55)/i18n/,$(ICU55_I18N_SRCS)))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- shared include paths ----------------------------------------------------
A_INCLUDES := -Ishim/art/include -I$(A60)/system/core/include \
              -I$(A60)/libnativehelper/include/nativehelper -I$(A60)/libnativehelper/include \
              -I$(A60)/libcore/include -I$(A60)/external/safe-iop/include -sUSE_ZLIB=1

# --- libnativehelper, libbase, libziparchive, libutils pieces ----------------
A_NH_SRCS := $(addprefix $(A60)/libnativehelper/,JNIHelp.cpp JniConstants.cpp toStringArray.cpp)
A_BASE_SRCS := $(addprefix $(A60)/system/core/base/,file.cpp stringprintf.cpp strings.cpp logging.cpp)
A_SYS_SRCS := $(A_NH_SRCS) $(A_BASE_SRCS) \
              $(A60)/system/core/libziparchive/zip_archive.cc \
              $(A60)/system/core/libutils/FileMap.cpp \
              $(A60)/system/core/libnativebridge/native_bridge.cc \
              $(addprefix $(A60)/system/core/libcutils/,trace-host.c ashmem-dev.c process_name.c)
$(AOBJ)/$(A60)/libnativehelper/% $(AOBJ)/$(A60)/system/%: AFLAGS := $(A_INCLUDES) -I$(A60)/system/core/base/include \
    -I$(A60)/system/core/include/nativebridge $(A_UAPI)
$(ALIB)/libartsys.a: $(call aobj,$(A_SYS_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- fdlibm (StrictMath) -----------------------------------------------------
A_FDLIBM_SRCS := $(shell tools/mk_srcs.py $(A60)/external/fdlibm/Android.mk src_files $(A60)/external/fdlibm/)
$(AOBJ)/$(A60)/external/fdlibm/%: AFLAGS := -D_IEEE_LIBM -D__LITTLE_ENDIAN -fno-strict-aliasing
$(ALIB)/libfdlibm.a: $(call aobj,$(A_FDLIBM_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libart ------------------------------------------------------------------
ART_RT := $(A60)/art/runtime
# Interpreter only: no compilers and no assembly. arch/<isa>/ code is
# replaced by shim/art/arch_wasm.cc (entry points, trampolines, context);
# the host's *_linux.cc by shim/art/*_wasm.cc.
ART_SRCS := $(filter-out arch/%.S %.S, \
              $(patsubst %.arm,%,$(shell sed -n '/^LIBART_COMMON_SRC_FILES :=/,/^LIBART_TARGET_LDFLAGS/p' \
                $(ART_RT)/Android.mk | grep -v '[:+]=' | tr -d '\\'))) \
            monitor_linux.cc
ART_SRCS := $(addprefix $(ART_RT)/,$(ART_SRCS)) \
            shim/art/arch_wasm.cc shim/art/thread_wasm.cc shim/art/runtime_wasm.cc \
            shim/art/jni_call_wasm.cc shim/art/sigchain_wasm.cc shim/art/static_jni_libs_art.cc \
            shim/art/interpreter_frames_wasm.cc shim/art/intrinsics_wasm.cc
ART_OPOUT_HDRS := $(shell sed -n '/^LIBART_ENUM_OPERATOR_OUT_HEADER_FILES :=/,/^$$/p' \
                    $(ART_RT)/Android.mk | grep -v ':=' | tr -d '\\')
ART_OPOUT_SRCS := $(patsubst %.h,$(ABLD)/gen/%_operator_out.cc,$(ART_OPOUT_HDRS))
$(ABLD)/gen/%_operator_out.cc: $(ART_RT)/%.h $(A60)/art/tools/generate-operator-out.py
	@mkdir -p $(dir $@)
	@python3 $(A60)/art/tools/generate-operator-out.py $(ART_RT) $< > $@
# ART_NDEBUG= (empty) builds ART with its DCHECKs (kIsDebugBuild).
ART_NDEBUG ?= -DNDEBUG
ART_DEFINES := -DBUILDING_LIBART=1 -DUSE_DLMALLOC -DIMT_SIZE=64 \
    -DART_DEFAULT_GC_TYPE_IS_CMS -DART_BASE_ADDRESS=0x70000000 \
    -DART_BASE_ADDRESS_MIN_DELTA=-0x1000000 -DART_BASE_ADDRESS_MAX_DELTA=0x1000000 \
    -DART_DEFAULT_INSTRUCTION_SET_FEATURES=default $(ART_NDEBUG) -DLOG_TAG=\"art\"
ART_FLAGS := -I$(ART_RT) -I$(A60)/art -I$(A60)/art/cmdline -I$(A60)/art/sigchainlib -Ishim/art \
    $(A_INCLUDES) $(ICU55_INCLUDES) $(ART_DEFINES) -include shim/art/art_runtime_wasm.h \
    -Wno-thread-safety -Wno-unused-lambda-capture -Wno-defaulted-function-deleted
$(AOBJ)/$(ART_RT)/% $(AOBJ)/shim/art/% $(AOBJ)/$(ABLD)/gen/%: AFLAGS := $(ART_FLAGS)
$(ALIB)/libart.a: $(call aobj,$(ART_SRCS) $(ART_OPOUT_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

.PHONY: art-libs
art-libs: $(ALIB)/libart.a $(ALIB)/libartsys.a $(ALIB)/libicuuc.a $(ALIB)/libicui18n.a $(ALIB)/libfdlibm.a

# --- libjavacore (libcore 6.0 natives) ----------------------------------------
# Without NativeBN (needs BoringSSL) and ExpatParser (needs expat): topic
# fw6-crypto builds them (mk/topic-fw6-crypto.mk); shim/art/javacore_register.cc
# declares them weak and skips them when they are not linked in.
AJC := $(A60)/libcore/luni/src/main/native
AJC_SRCS := $(filter-out %/Register.cpp %/java_math_NativeBN.cpp %/org_apache_harmony_xml_ExpatParser.cpp, \
              $(shell tools/mk_srcs.py $(AJC)/sub.mk LOCAL_SRC_FILES $(AJC)/)) \
            $(A60)/libcore/dalvik/src/main/native/org_apache_harmony_dalvik_NativeTestTarget.cpp \
            shim/art/javacore_register.cc
# Linux UAPI headers (linux/rtnetlink.h, linux/if_ether.h ...) for the
# constants; musl in Emscripten has none. Searched after the system headers.
A_UAPI := -idirafter $(A60)/bionic/libc/kernel/uapi -idirafter $(A60)/bionic/libc/kernel/uapi/asm-x86
AJC_FLAGS := $(A_INCLUDES) -I$(A60)/system/core/base/include $(ICU55_INCLUDES) -I$(AJC) \
             -DU_USING_ICU_NAMESPACE=0 $(A_UAPI)
$(AOBJ)/$(A60)/libcore/%: AFLAGS := $(AJC_FLAGS)
$(AOBJ)/shim/art/javacore_register.cc.o: AFLAGS := $(AJC_FLAGS)
$(ALIB)/libjavacore.a: $(call aobj,$(AJC_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- support code shared with the Dalvik build (liblog, libc extras) ---------
ASHIM_SRCS := shim/dalvik/android_log.cc shim/dalvik/libc_extras.cc shim/art/libc_extras_art.cc
$(AOBJ)/shim/dalvik/% $(AOBJ)/shim/art/libc_extras_art.cc.o: AFLAGS := $(A_INCLUDES) $(A_UAPI)
$(ALIB)/libart_shim.a: $(call aobj,$(ASHIM_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- Java: core-libart.jar and the test programs that need only libcore ------
AFW := $(ABLD)/framework
$(AFW)/core-libart.jar: tools/build_core_libart_jar.sh
	tools/build_core_libart_jar.sh $(AFW)
# VmTest, Hello and Bench use only libcore. Topics add tests that need more
# (ART_JAVA_TESTS +=, and the class jars to compile them against:
# ART_TESTS_CLASSPATH +=, both with $(AFW)/arc-tests.jar as a prerequisite).
ART_JAVA_TESTS := $(addprefix tests/java/arcwasm/,VmTest.java Hello.java Bench.java \
                    BigHeapTest.java ArtTestNatives.java ArtSpeedTest.java)
ART_TESTS_CLASSPATH :=
# Second dex: code built with dx --no-optimize (tools/build_dx_test_dex.sh,
# topic art-string-init).
ART_DX_TESTS_DEX := tests/java/dx/classes2.dex
$(AFW)/arc-tests.jar: $(ART_JAVA_TESTS) $(ART_DX_TESTS_DEX) $(AFW)/core-libart.jar
	rm -rf $(ABLD)/tests-java && mkdir -p $(ABLD)/tests-java/classes $(ABLD)/tests-java/dex
	javac -source 8 -target 8 -Xlint:-options -encoding UTF-8 \
	    -bootclasspath $(AFW)/core-libart-java/core-libart-classes.jar \
	    $(if $(strip $(ART_TESTS_CLASSPATH)),-classpath $(subst $() ,:,$(strip $(ART_TESTS_CLASSPATH)))) \
	    -d $(ABLD)/tests-java/classes $(ART_JAVA_TESTS)
	cd $(ABLD)/tests-java/classes && jar cf ../classes.jar .
	"$(D8)" --release --min-api 23 --lib $(AFW)/core-libart-java/core-libart-classes.jar \
	    $(addprefix --lib ,$(ART_TESTS_CLASSPATH)) \
	    --output $(ABLD)/tests-java/dex $(ABLD)/tests-java/classes.jar
	cp $(ART_DX_TESTS_DEX) $(ABLD)/tests-java/dex/classes2.dex
	rm -f $@ && cd $(ABLD)/tests-java/dex && zip -q "$(abspath $@)" classes.dex classes2.dex

# --- /system image -----------------------------------------------------------
ASYSTEM_IMAGE := $(ABLD)/system.img
$(ASYSTEM_IMAGE): $(AFW)/core-libart.jar $(AFW)/arc-tests.jar tools/make_system_image.py
	python3 tools/make_system_image.py $@ \
	    --add $(AFW)/core-libart.jar:system/framework/core-libart.jar \
	    --add $(AFW)/arc-tests.jar:system/framework/arc-tests.jar \
	    --add $(ICU55)/stubdata/icudt55l.dat:system/usr/icu/icudt55l.dat \
	    $(ASYSTEM_ADD)

# --- artvm -------------------------------------------------------------------
ART_MAIN_OBJ := $(call aobj,art/artvm.cc art/test_natives.cc)
$(ART_MAIN_OBJ): AFLAGS := $(A_INCLUDES)
ART_VFS_OBJ := $(call obj,art/vfs_setup.cc)
ART_LIBS := $(ALIB)/libart.a $(ALIB)/libjavacore.a $(ALIB)/libartsys.a \
            $(ALIB)/libicui18n.a $(ALIB)/libicuuc.a $(ALIB)/libfdlibm.a $(ALIB)/libart_shim.a
# Memory grows on demand up to 4 GB: ART reserves its heap with mmap, and
# TikTok-sized apps need more than 2 GB (addresses above 2 GB must work).
ART_STACK ?= 1MB
ART_LDFLAGS := -pthread -sPROXY_TO_PTHREAD -sUSE_ZLIB=1 -sINITIAL_MEMORY=256MB \
               -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB \
               -sSTACK_SIZE=$(ART_STACK) -sDEFAULT_PTHREAD_STACK_SIZE=1MB -sPTHREAD_POOL_SIZE=12 \
               -sEXIT_RUNTIME=1 -sASSERTIONS=1 --profiling-funcs --js-library $(OPFS_JS) --js-library $(CLOCK_JS) \
               --preload-file $(ASYSTEM_IMAGE)@/arc/system.img \
               --extern-pre-js $(DVM_PRE_JS) --pre-js art/artvm_pre.js
$(ABLD)/artvm.js: $(ART_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) $(ART_LIBS) \
                  $(BUILD)/libposix_translation.a $(ASYSTEM_IMAGE) $(OPFS_JS) $(CLOCK_JS) $(DVM_PRE_JS) art/artvm_pre.js mk/topic-art.mk
	@mkdir -p $(dir $@)
	$(EMXX) $(ART_LDFLAGS) -sENVIRONMENT=node,worker \
	    $(ART_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) \
	    -Wl,--start-group $(ART_LIBS) $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
	$(call wasm_opt,$(@:.js=.wasm))

.PHONY: art art-test art-bigheap-test
art: $(ABLD)/artvm.js
art-test: art
	cd $(ABLD) && $(NODE) artvm.js -cp /system/framework/arc-tests.jar arcwasm.VmTest
# Objects above 2 GB of wasm memory (a 2.5 GB heap).
art-bigheap-test: art
	cd $(ABLD) && $(NODE) artvm.js $(ART_BIGHEAP_OPTS) -cp /system/framework/arc-tests.jar arcwasm.BigHeapTest
# (fewer, larger heap growth steps: a GC every 8 MB would mark the whole heap
# a few hundred times on the way to 2.3 GB)
ART_BIGHEAP_OPTS := -Xmx2560m -XX:HeapGrowthLimit=2560m -XX:HeapMinFree=64m -XX:HeapMaxFree=512m

# --- artvm for the browser -----------------------------------------------------
#   make art-web   # build/art-web/: serve with tools/serve.py, open
#                  # index.html?arg=-cp&arg=/system/framework/arc-tests.jar&arg=arcwasm.VmTest
AWEB := $(BUILD)/art-web
$(AWEB)/artvm.js: $(ART_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) $(ART_LIBS) \
                  $(BUILD)/libposix_translation.a $(ASYSTEM_IMAGE) $(OPFS_JS) $(CLOCK_JS) $(DVM_PRE_JS) art/artvm_pre.js mk/topic-art.mk
	@mkdir -p $(dir $@)
	$(EMXX) $(ART_LDFLAGS) -sENVIRONMENT=web,worker \
	    $(ART_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) \
	    -Wl,--start-group $(ART_LIBS) $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
	$(call wasm_opt,$(@:.js=.wasm))
$(AWEB)/index.html: art/web/index.html
	@mkdir -p $(dir $@)
	cp $< $@
$(AWEB)/report.js: shim/report.js
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: art-web
art-web: $(AWEB)/artvm.js $(AWEB)/index.html $(AWEB)/report.js
