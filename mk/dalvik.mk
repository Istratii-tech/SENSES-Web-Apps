# Dalvik (AOSP 4.4.4) for WebAssembly: ICU, libnativehelper, libjavacore,
# libdex, libdvm and a dalvikvm launcher, on top of the arc-wasm kernel.
# Sources come from third_party/aosp44 (tools/fetch_aosp44.sh).
#
#   make dalvik       # build/dalvik/dalvikvm.js (Node)

EMCC := $(EMSDK)/upstream/emscripten/emcc
EMAR := $(EMSDK)/upstream/emscripten/emar
A44  := third_party/aosp44
DOBJ := $(BUILD)/dobj
dobj  = $(patsubst %,$(DOBJ)/%.o,$(1))

D_COMMON := -pthread -O2 -g1 -D_GNU_SOURCE -DARC_WASM=1 -DANDROID_SMP=1 \
            -include shim/dalvik/AndroidConfig.h -include shim/dalvik/dalvik_wasm_compat.h \
            -Wno-unused-parameter -Wno-deprecated-declarations \
            -Wno-unused-variable -Wno-unused-function -Wno-sign-compare \
            -Wno-unused-but-set-variable -Wno-deprecated-register \
            -Wno-unknown-warning-option -Wno-c++11-narrowing \
            -Wno-missing-field-initializers -Wno-format
D_COMMON += -MMD -MP
D_CFLAGS   := $(D_COMMON) -std=gnu11
D_CXXFLAGS := $(D_COMMON) -std=gnu++14 -Wno-register

$(DOBJ)/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $<"
	@$(EMCC) $(D_CFLAGS) $(XFLAGS) -c $< -o $@

$(DOBJ)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(D_CXXFLAGS) $(XFLAGS) -c $< -o $@

$(DOBJ)/%.cc.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(D_CXXFLAGS) $(XFLAGS) -c $< -o $@

-include $(shell find $(DOBJ) -name '*.d' 2>/dev/null)

# --- ICU 51 (external/icu4c) -----------------------------------------------
ICU := $(A44)/external/icu4c
ICU_UC_SRCS   := $(shell tools/mk_srcs.py $(ICU)/common/Android.mk src_files $(ICU)/common/)
ICU_I18N_SRCS := $(filter %.c %.cpp,$(shell tools/mk_srcs.py $(ICU)/i18n/Android.mk src_files $(ICU)/i18n/))
ICU_INCLUDES  := -I$(ICU)/common -I$(ICU)/i18n

$(DOBJ)/$(ICU)/common/%: XFLAGS := $(ICU_INCLUDES) -DU_COMMON_IMPLEMENTATION \
    '-DICU_DATA_DIR_PREFIX_ENV_VAR="ANDROID_ROOT"' '-DICU_DATA_DIR="/usr/icu"' \
    -DU_HAVE_NL_LANGINFO_CODESET=0 -DU_TIMEZONE=timezone
$(DOBJ)/$(ICU)/i18n/%: XFLAGS := $(ICU_INCLUDES) -DU_I18N_IMPLEMENTATION

$(BUILD)/libicuuc.a: $(call dobj,$(ICU_UC_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^
$(BUILD)/libicui18n.a: $(call dobj,$(ICU_I18N_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

.PHONY: icu
icu: $(BUILD)/libicuuc.a $(BUILD)/libicui18n.a

# --- shared include paths ----------------------------------------------------
D_INCLUDES := -Ishim/dalvik/include -I$(A44)/system/core/include \
              -I$(A44)/libnativehelper/include/nativehelper \
              -I$(A44)/libcore/include -I$(A44)/external/safe-iop/include \
              -I$(A44)/dalvik -sUSE_ZLIB=1

# --- libnativehelper -----------------------------------------------------------
NH_SRCS := $(addprefix $(A44)/libnativehelper/,JNIHelp.cpp JniConstants.cpp toStringArray.cpp)
$(DOBJ)/$(A44)/libnativehelper/%: XFLAGS := $(D_INCLUDES)
$(BUILD)/libnativehelper.a: $(call dobj,$(NH_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- fdlibm (StrictMath) -----------------------------------------------------
FDLIBM_SRCS := $(shell tools/mk_srcs.py $(A44)/external/fdlibm/Android.mk src_files $(A44)/external/fdlibm/)
$(DOBJ)/$(A44)/external/fdlibm/%: XFLAGS := -D_IEEE_LIBM -D__LITTLE_ENDIAN -fno-strict-aliasing
$(BUILD)/libfdlibm.a: $(call dobj,$(FDLIBM_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libjavacore (libcore natives) -------------------------------------------
# Not built yet: NativeBN (needs OpenSSL), ExpatParser (needs expat),
# RawSocket (packet sockets; no browser equivalent). The
# arc-wasm Register.cpp declares every register_* weak and skips missing ones.
JC := $(A44)/libcore/luni/src/main/native
JC_SRCS := $(filter-out %/Register.cpp %/java_math_NativeBN.cpp %/org_apache_harmony_xml_ExpatParser.cpp \
               %/libcore_net_RawSocket.cpp, \
             $(shell tools/mk_srcs.py $(JC)/sub.mk LOCAL_SRC_FILES $(JC)/)) \
           $(A44)/libcore/dalvik/src/main/native/org_apache_harmony_dalvik_NativeTestTarget.cpp \
           shim/dalvik/javacore_register.cc
$(DOBJ)/$(A44)/libcore/%: XFLAGS := $(D_INCLUDES) $(ICU_INCLUDES) -I$(JC)
$(DOBJ)/shim/dalvik/javacore_register.cc.o: XFLAGS := $(D_INCLUDES) -I$(JC)
$(BUILD)/libjavacore.a: $(call dobj,$(JC_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libdex ------------------------------------------------------------------
DEX_SRCS := $(shell tools/mk_srcs.py $(A44)/dalvik/libdex/Android.mk dex_src_files $(A44)/dalvik/libdex/)
$(DOBJ)/$(A44)/dalvik/libdex/%: XFLAGS := $(D_INCLUDES)
$(BUILD)/libdex.a: $(call dobj,$(DEX_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libdvm ------------------------------------------------------------------
# The "unknown architecture" configuration of Dvm.mk: portable C interpreter
# (InterpC-portable + allstubs), no JIT, mark-sweep GC on dlmalloc mspaces.
# arch/generic/Call.cpp needs libffi; shim/dalvik/wasm_call.cc replaces it.
# No zygote (fork, SELinux): shim/dalvik/zygote_stub.cc replaces its natives.
DVM := $(A44)/dalvik/vm
# Entries like "mterp/out/InterpC-$(dvm_arch_variant).cpp" (unbalanced
# parenthesis, so not writable inline in filter-out).
DVM_ARCH_VARIANT_SRCS := %variant).cpp %variant).cpp.arm
DVM_SRCS := $(patsubst %.arm,%,$(filter-out compiler/% arch/% os/android.cpp \
              native/dalvik_system_Zygote.cpp \
              alloc/Copying.cpp.arm %.S mterp/out/InterpC-arm% mterp/out/InterpC-x86.cpp \
              mterp/out/InterpC-mips.cpp $(DVM_ARCH_VARIANT_SRCS), \
              $(shell tools/mk_srcs.py $(DVM)/Dvm.mk LOCAL_SRC_FILES)))
DVM_SRCS := $(addprefix $(DVM)/,$(sort $(DVM_SRCS) arch/generic/Hints.cpp mterp/out/InterpC-allstubs.cpp)) \
            shim/dalvik/wasm_call.cc shim/dalvik/zygote_stub.cc
D_DVM_FLAGS := $(D_INCLUDES) -I$(DVM) -Ishim/dalvik -DARCH_VARIANT=\"wasm32\" -DDVM_SHOW_EXCEPTION=1 \
    -DdvmAsmInstructionStart=0 -DdvmAsmInstructionEnd=0 \
    -DdvmAsmSisterStart=0 -DdvmAsmSisterEnd=0 -DDVM_NO_ASM_INTERP=1 \
    -fstrict-aliasing -DLOG_TAG=\"dalvikvm\"
$(DOBJ)/$(DVM)/%: XFLAGS := $(D_DVM_FLAGS)
$(DOBJ)/shim/dalvik/wasm_call.cc.o $(DOBJ)/shim/dalvik/zygote_stub.cc.o: XFLAGS := $(D_DVM_FLAGS)
$(BUILD)/libdvm.a: $(call dobj,$(DVM_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

.PHONY: dalvik-libs
dalvik-libs: icu $(BUILD)/libnativehelper.a $(BUILD)/libfdlibm.a \
             $(BUILD)/libjavacore.a $(BUILD)/libdex.a $(BUILD)/libdvm.a

# --- support code: liblog, cutils, static JNI libraries ----------------------
DSHIM_SRCS := shim/dalvik/android_log.cc shim/dalvik/cutils_atomic.c \
              shim/dalvik/static_jni_libs.cc shim/dalvik/libc_extras.cc \
              shim/dalvik/cutils_extras.cc \
              $(A44)/system/core/libcutils/ashmem-dev.c
$(DOBJ)/shim/dalvik/%: XFLAGS := $(D_INCLUDES)
# linux/ashmem.h: the same UAPI header posix_translation uses (shim/linux).
$(DOBJ)/$(A44)/system/core/libcutils/%: XFLAGS := $(D_INCLUDES) -Ishim/linux/..
$(BUILD)/libdalvik_shim.a: $(call dobj,$(DSHIM_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- Java: core.jar (libcore) and test programs --------------------------------
DFW := $(BUILD)/dalvik/framework
$(DFW)/core.jar: tools/build_core_jar.sh
	tools/build_core_jar.sh $(DFW)

include mk/framework.mk

D8 ?= $(lastword $(sort $(wildcard $(HOME)/Library/Android/sdk/build-tools/*/d8)))
JAVA_TESTS := $(shell find tests/java -name '*.java')
empty :=
BOOT_CLASSES := $(subst $(empty) $(empty),:,$(DFW)/core-java/core-classes.jar $(FW_CLASSES))
$(DFW)/arc-tests.jar: $(JAVA_TESTS) $(DFW)/core.jar $(FW_JARS)
	rm -rf $(BUILD)/dalvik/tests-java && mkdir -p $(BUILD)/dalvik/tests-java/classes $(BUILD)/dalvik/tests-java/dex
	javac -source 8 -target 8 -Xlint:-options -encoding UTF-8 \
	    -bootclasspath $(BOOT_CLASSES) \
	    -d $(BUILD)/dalvik/tests-java/classes $(JAVA_TESTS)
	cd $(BUILD)/dalvik/tests-java/classes && jar cf ../classes.jar .
	"$(D8)" --release --min-api 19 --lib $(DFW)/core-java/core-classes.jar \
	    $(addprefix --lib ,$(FW_CLASSES)) \
	    --output $(BUILD)/dalvik/tests-java/dex $(BUILD)/dalvik/tests-java/classes.jar
	rm -f $@ && cd $(BUILD)/dalvik/tests-java/dex && zip -q "$(abspath $@)" classes.dex

# --- /system image -----------------------------------------------------------
# Topic fragments (mk/topic-*.mk) add files with DSYSTEM_ADD += --add SRC:DEST
# or as HOST:IMAGE pairs in DSYSTEM_EXTRA, plus the host files as
# prerequisites of $(DSYSTEM_IMAGE).
DSYSTEM_IMAGE := $(BUILD)/dalvik/system.img
$(DSYSTEM_IMAGE): $(DFW)/core.jar $(FW_JARS) $(DFW)/arc-tests.jar \
                  tools/make_system_image.py
	python3 tools/make_system_image.py $@ \
	    --add $(DFW)/core.jar:system/framework/core.jar \
	    $(foreach j,$(FW_BOOT_JARS),--add $(DFW)/$(j).jar:system/framework/$(j).jar) \
	    --add $(DFW)/arc-tests.jar:system/framework/arc-tests.jar \
	    --add $(ICU)/stubdata/icudt51l-default.dat:system/usr/icu/icudt51l.dat \
	    $(DSYSTEM_ADD) $(addprefix --add ,$(DSYSTEM_EXTRA))

# --- dalvikvm ----------------------------------------------------------------
DVM_MAIN_OBJ := $(call dobj,dalvik/launcher/dalvikvm.cc)
$(DVM_MAIN_OBJ): XFLAGS := $(D_INCLUDES)
DVM_VFS_OBJ := $(call obj,dalvik/launcher/vfs_setup.cc)
DALVIK_LIBS := $(BUILD)/libdvm.a $(BUILD)/libdex.a $(BUILD)/libjavacore.a \
               $(BUILD)/libnativehelper.a $(BUILD)/libicui18n.a $(BUILD)/libicuuc.a \
               $(BUILD)/libfdlibm.a $(BUILD)/libdalvik_shim.a $(FRAMEWORK_LIBS)
# How the /system image reaches /arc/system.img (MEMFS) before main():
#   preload  dalvikvm.data next to dalvikvm.js, read (Node) or fetched
#            (browser) at startup: one copy of the image, in the JS heap
#   embed    inside dalvikvm.wasm: one file to ship, but the image is also
#            static data in wasm memory (two copies, ~+40 MB RSS in Node)
# Relink after changing it (make -B or remove dalvikvm.js).
DSYSTEM_PACKAGING ?= preload
# Under Node, finds dalvikvm.data next to dalvikvm.js from any directory.
DVM_PRE_JS := dalvik/launcher/locate_file.js
DVM_LDFLAGS := -pthread -sPROXY_TO_PTHREAD -sUSE_ZLIB=1 -sINITIAL_MEMORY=512MB \
               -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB -sPTHREAD_POOL_SIZE=8 \
               -sEXIT_RUNTIME=1 -sASSERTIONS=1 --profiling-funcs --js-library $(OPFS_JS) --js-library $(CLOCK_JS) \
               --$(DSYSTEM_PACKAGING)-file $(DSYSTEM_IMAGE)@/arc/system.img \
               --extern-pre-js $(DVM_PRE_JS)

$(BUILD)/dalvik/dalvikvm.js: $(DVM_MAIN_OBJ) $(DVM_VFS_OBJ) $(KERNEL_OBJ) $(DALVIK_LIBS) \
                             $(BUILD)/libposix_translation.a $(DSYSTEM_IMAGE) $(OPFS_JS) $(CLOCK_JS) $(DVM_PRE_JS) mk/dalvik.mk
	@mkdir -p $(dir $@)
	$(EMXX) $(DVM_LDFLAGS) -sENVIRONMENT=node,worker \
	    $(DVM_MAIN_OBJ) $(DVM_VFS_OBJ) $(KERNEL_OBJ) \
	    -Wl,--start-group $(DALVIK_LIBS) $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
	$(call wasm_opt,$(@:.js=.wasm))

.PHONY: dalvik dalvik-hello
dalvik: $(BUILD)/dalvik/dalvikvm.js
dalvik-hello: dalvik
	cd $(BUILD)/dalvik && $(NODE) dalvikvm.js -cp /system/framework/arc-tests.jar arcwasm.Hello one two

# --- dalvikvm for the browser --------------------------------------------------
#   make dalvik-web   # build/dalvik-web/: serve with tools/serve.py, open
#                     # index.html?arg=-cp&arg=/system/framework/arc-tests.jar&arg=arcwasm.VmTest
DWEB := $(BUILD)/dalvik-web
$(DWEB)/dalvikvm.js: $(DVM_MAIN_OBJ) $(DVM_VFS_OBJ) $(KERNEL_OBJ) $(DALVIK_LIBS) \
                     $(BUILD)/libposix_translation.a $(DSYSTEM_IMAGE) $(OPFS_JS) $(CLOCK_JS) $(DVM_PRE_JS) mk/dalvik.mk
	@mkdir -p $(dir $@)
	$(EMXX) $(DVM_LDFLAGS) -sENVIRONMENT=web,worker \
	    $(DVM_MAIN_OBJ) $(DVM_VFS_OBJ) $(KERNEL_OBJ) \
	    -Wl,--start-group $(DALVIK_LIBS) $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
	$(call wasm_opt,$(@:.js=.wasm))
$(DWEB)/index.html: dalvik/web/index.html
	@mkdir -p $(dir $@)
	cp $< $@
$(DWEB)/report.js: shim/report.js
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: dalvik-web
dalvik-web: $(DWEB)/dalvikvm.js $(DWEB)/index.html $(DWEB)/report.js
