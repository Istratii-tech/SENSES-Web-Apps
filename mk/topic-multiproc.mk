# Topic multiproc: components of another android:process run in light
# processes of the one VM (docs/topics/multiproc.md).
#
#   shim/multiproc/light_process.cc       pids, the calling thread's process
#                                         (in libposix_translation: getpid(),
#                                         the binder driver, /proc use it)
#   shim/multiproc/light_process_wrap.cc  pthread_create/exit/_exit/abort/kill
#                                         wrappers (art8, art10 only)
#   shim/multiproc/lproc_jni.cc           arcwasm.server.LightProcess and
#                                         Binder.arcProxyFor natives, per
#                                         framework (libbinder 8.1, 10)
#   shim/binder/dev_binder.cc             one binder process per light process
#   framework/java-multiproc              LightProcess (services.jar 8.1, 10)
#   patches/aosp81/multiproc.patch,
#   patches/aosp10/multiproc.patch        Binder, Looper, ActivityThread,
#                                         ApplicationLoaders, ServiceManager,
#                                         Runtime.exit, java_vm_ext (ART)
#
#   make multiproc-test     MultiProcessTest on art8 and art10 in Node

MP_WRAP_LDFLAGS := -Wl,--wrap=pthread_create -Wl,--wrap=exit -Wl,--wrap=_exit -Wl,--wrap=abort \
                   -Wl,--wrap=kill
MP_JAVA := framework/java-multiproc/arcwasm/server/LightProcess.java

# --- 8.1 -------------------------------------------------------------------------------
ifneq ($(F8LIB),)
MP8_OBJS := $(call f8obj,shim/multiproc/lproc_jni.cc shim/multiproc/light_process_wrap.cc)
$(MP8_OBJS): F8XFLAGS := $(F8_JNI_FLAGS) -Ishim -DLOG_TAG=\"multiproc\"
$(MP8_OBJS): shim/multiproc/include/arc_lproc.h
$(F8LIB)/libmultiproc8.a: $(MP8_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART8_LIBS += -Wl,--whole-archive $(F8LIB)/libmultiproc8.a -Wl,--no-whole-archive
ART8_LDFLAGS += $(MP_WRAP_LDFLAGS)
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js $(BUILD)/fw8-graphics-web/artvm8.js: $(F8LIB)/libmultiproc8.a
endif
ifneq ($(F8S_SERVICES_JAR),)
$(F8S_SERVICES_JAR): $(MP_JAVA)
endif

# --- 10 --------------------------------------------------------------------------------
ifneq ($(F10LIB),)
MP10_OBJS := $(call f10obj,shim/multiproc/lproc_jni.cc shim/multiproc/light_process_wrap.cc)
$(MP10_OBJS): F10XFLAGS := $(F10_JNI_FLAGS) -Ishim -DLOG_TAG=\"multiproc\"
$(MP10_OBJS): shim/multiproc/include/arc_lproc.h
$(F10LIB)/libmultiproc10.a: $(MP10_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART10_LIBS += -Wl,--whole-archive $(F10LIB)/libmultiproc10.a -Wl,--no-whole-archive
ART10_LDFLAGS += $(MP_WRAP_LDFLAGS)
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js $(BUILD)/fw10-graphics-web/artvm10.js: \
    $(F10LIB)/libmultiproc10.a
endif
ifneq ($(F10S_SERVICES_JAR),)
$(F10S_SERVICES_JAR): $(MP_JAVA)
endif

# --- the test app (tests/apkmp: minSdk 26, no lambdas) and MultiProcessTest ----------
# One APK for 8.1 and 10 (dex 038 runs on both); in their /system/app.
MP_APK := $(BUILD)/multiproc/ArcWasmMultiProc.apk
$(MP_APK): $(shell find tests/apkmp -type f) tools/build_test_apk.sh
	MIN_SDK=26 TARGET_SDK=27 AAPT="$(AAPT)" D8="$(D8)" ANDROID_JAR="$(ANDROID_JAR)" \
	    tools/build_test_apk.sh tests/apkmp $(BUILD)/multiproc/work $@
ifneq ($(A8LIB),)
ASYSTEM8_ADD += --add $(MP_APK):system/app/ArcWasmMultiProc.apk
$(A8SYSTEM_IMAGE): $(MP_APK)
endif
ifneq ($(A10SYSTEM_IMAGE),)
ASYSTEM10_ADD += --add $(MP_APK):system/app/ArcWasmMultiProc.apk
$(A10SYSTEM_IMAGE): $(MP_APK)
# (art8's arc-tests.jar takes every tests/java8 test by its wildcard.)
ART10_JAVA_TESTS += tests/java8/arcwasm/MultiProcessTest.java
$(A10FW)/arc-tests.jar: tests/java8/arcwasm/MultiProcessTest.java
endif

# The driver, getpid() and /proc/PID/ (in every VM) follow the registry.
$(DEV_BINDER_OBJ) $(KERNEL_OBJ) $(TT_PROC_OBJ): shim/multiproc/include/arc_lproc.h

.PHONY: multiproc-test
multiproc-test: art8 art10
	VM=art8 tools/run_java_tests.sh node MultiProcessTest
	VM=art10 tools/run_java_tests.sh node MultiProcessTest
