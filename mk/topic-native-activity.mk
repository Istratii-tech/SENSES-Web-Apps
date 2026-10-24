# Topic "native-activity": android.app.NativeActivity for apps' ARM code
# (docs/topics/native-activity.md). shim/ndk/ndk_native_activity.cc gives
# the natives gRegJNI refers to weakly (register_android_app_NativeActivity)
# and libandroid's ANativeActivity_*, AInputQueue/AInputEvent and
# AConfiguration for guest code; the last two are AOSP's libandroid sources
# (frameworks/base/native/android/{input,configuration}.cpp) built for the
# host (with every declaration of the NDK headers, which hide the newer
# ones by API level, so that all the functions are extern "C"). Built
# against each framework like ndk_android_fw.cc (8.1: topic fw8-graphics'
# G8 rules; 6.0.1: fw6-system's; 10: fw10-system's) and linked into its VM
# with the NDK bridge.
#
#   make native-activity-apk   # tests/native-activity: the test APK
#   VM=art8 tools/run_java_tests.sh node NativeActivityTest

ifneq ($(A8LIB),)
NA8_SRCS := shim/ndk/ndk_native_activity.cc $(addprefix $(G8_FWB)/native/android/,input.cpp \
              configuration.cpp)
$(call g8obj,$(NA8_SRCS)): G8FLAGS := $(G8_FW_INCLUDES_BASE) -std=gnu++17 -I$(G8_JNI) \
    -I$(G8_JNI)/include -Ishim/ndk -include android/looper.h -D__ANDROID_API__=10000
$(G8LIB)/libfw8_native_activity.a: $(call g8obj,$(NA8_SRCS))
	$(g8_archive)
ART8_LIBS += $(G8LIB)/libfw8_native_activity.a
ART8_LDFLAGS += -Wl,-u,ndk_native_activity_linked
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js $(BUILD)/fw8-graphics-web/artvm8.js: \
    $(G8LIB)/libfw8_native_activity.a
endif

# 6.0.1 (topic fw6-system's objects and flags; NA_FW6: 6.0's loadNativeCode).
NA6_SRCS := shim/ndk/ndk_native_activity.cc $(addprefix $(F6FWB)/native/android/,input.cpp \
              configuration.cpp)
$(call fsobj,$(NA6_SRCS)): FSFLAGS := $(FG_FW_INCLUDES_BASE) -std=gnu++17 -I$(F6FWB)/core/jni \
    -Ishim/ndk -include android/looper.h -D__ANDROID_API__=10000 -DNA_FW6=1
$(FSLIB)/libfw6_native_activity.a: $(call fsobj,$(NA6_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
FW6_LIBS += $(FSLIB)/libfw6_native_activity.a
ART_LDFLAGS += -Wl,-u,ndk_native_activity_linked
$(ABLD)/artvm.js $(AWEB)/artvm.js $(FGWEB)/artvm.js: $(FSLIB)/libfw6_native_activity.a

# 10 (topic fw10-system's objects, fw10-graphics' headers: Surfaces and input).
# The 10 stack had no frame destination for guest windows (ANativeWindow_lock
# and guest EGL window surfaces): shim/fw6-system/layer_target6.cc, as 8.1
# builds it (it only uses ANativeWindow's perform/query), joins it here.
ifneq ($(G10_FW_INCLUDES),)
NA10_SRCS := shim/ndk/ndk_native_activity.cc $(addprefix $(A10)/frameworks/base/native/android/,input.cpp \
               configuration.cpp)
$(call f10sobj,$(NA10_SRCS)): F10SXFLAGS := $(G10_FW_INCLUDES_BASE) -std=gnu++17 $(F10_JNI_FLAGS) \
    -Ishim/ndk -include android/looper.h -D__ANDROID_API__=10000 -DARC_FW10=1
$(call f10sobj,shim/fw6-system/layer_target6.cc): F10SXFLAGS := $(G10_FW_INCLUDES_BASE) -Ishim/gles
$(call f10sobj,$(NA10_SRCS) shim/fw6-system/layer_target6.cc): | $(G10)/gen/hidl-cpp/hidl.stamp \
    $(F10_HIDL_GENCPP)/hidl.stamp
$(F10SLIB)/libfw10_native_activity.a: $(call f10sobj,$(NA10_SRCS) shim/fw6-system/layer_target6.cc)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART10_LIBS += $(F10SLIB)/libfw10_native_activity.a
ART10_LDFLAGS += -Wl,-u,ndk_native_activity_linked -Wl,-u,arcwasm_layer_target_for_window
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js $(G10WEB)/artvm10.js: $(F10SLIB)/libfw10_native_activity.a
endif

.PHONY: native-activity-libs
native-activity-libs: $(G8LIB)/libfw8_native_activity.a $(FSLIB)/libfw6_native_activity.a \
    $(if $(G10_FW_INCLUDES),$(F10SLIB)/libfw10_native_activity.a)

# --- the test: tests/native-activity -> /system/app/ArcWasmNativeActivityTest.apk ------------
# libnatest.so (natest.c and AOSP's android_native_app_glue) is built like
# topic ndk-imports' libndk6test.so: NDK r9's android-18 sysroot.
# (NDK18_SYSROOT, NDK6_CC, NDKT_LD: mk/topic-ndk-*.mk, included later; hence $$.)
NA8 := $(BUILD)/native-activity
NA8_LIB := $(NA8)/libs/armeabi-v7a/libnatest.so
NA8_OBJS := $(NA8)/natest.c.o $(NA8)/glue.c.o
$(NA8)/natest.c.o: tests/native-activity/natest.c tests/native-activity/glue/android_native_app_glue.h \
    | $$(NDK18_SYSROOT)/include/jni.h
	@mkdir -p $(dir $@)
	$(NDK6_CC) -Itests/native-activity/glue -c $< -o $@
$(NA8)/glue.c.o: tests/native-activity/glue/android_native_app_glue.c \
    tests/native-activity/glue/android_native_app_glue.h | $$(NDK18_SYSROOT)/include/jni.h
	@mkdir -p $(dir $@)
	$(NDK6_CC) -Itests/native-activity/glue -c $< -o $@
$(NA8_LIB): $(NA8_OBJS) $(BUILD)/ndk/armeabi-v7a/libndkrt.a
	@mkdir -p $(dir $@)
	$(NDKT_LD) -soname libnatest.so -o $@ $(NDK18_SYSROOT)/lib/crtbegin_so.o $^ \
	    -L$(NDK18_SYSROOT)/lib -lEGL -lGLESv2 -landroid -llog -lc -lm -ldl \
	    $(NDK18_SYSROOT)/lib/crtend_so.o
NA8_APK := $(NA8)/ArcWasmNativeActivityTest.apk
$(NA8_APK): $(shell find tests/native-activity/apk -type f) $(NA8_LIB) tools/build_test_apk.sh
	MIN_SDK=9 TARGET_SDK=27 AAPT="$(AAPT)" ANDROID_JAR="$(ANDROID_JAR)" \
	    EXTRA_ASSETS="$(NA8_LIB):lib/armeabi-v7a/libnatest.so" \
	    tools/build_test_apk.sh tests/native-activity/apk $(NA8)/work $@
.PHONY: native-activity-apk
native-activity-apk: $(NA8_APK)
ifneq ($(A8LIB),)
ASYSTEM8_ADD += --add $(NA8_APK):system/app/ArcWasmNativeActivityTest.apk
$(A8SYSTEM_IMAGE): $(NA8_APK)
endif
# The same test on 6.0.1 and 10 (written for all three: tests/java8 is only
# 8.1's wildcard); NativeActivityArm64Test, the arm64 app's run, on 10 too.
NA_TEST_SRC := tests/java8/arcwasm/NativeActivityTest.java
ART_JAVA_TESTS += $(NA_TEST_SRC)
$(AFW)/arc-tests.jar: $(NA_TEST_SRC)
ASYSTEM_ADD += --add $(NA8_APK):system/app/ArcWasmNativeActivityTest.apk
$(ASYSTEM_IMAGE): $(NA8_APK)
ifneq ($(G10_FW_INCLUDES),)
NA_TEST64_SRC := tests/java8/arcwasm/NativeActivityArm64Test.java
ART10_JAVA_TESTS += $(NA_TEST_SRC) $(NA_TEST64_SRC)
$(A10FW)/arc-tests.jar: $(NA_TEST_SRC) $(NA_TEST64_SRC)
ASYSTEM10_ADD += --add $(NA8_APK):system/app/ArcWasmNativeActivityTest.apk
$(A10SYSTEM_IMAGE): $(NA8_APK)
endif

# --- the same app for arm64-v8a: /system/app/ArcWasmNativeActivityTestArm64.apk ------------
# Package org.arcwasm.natest.arm64 with only lib/arm64-v8a/libnatest.so, so
# the package service picks the 64-bit ABI (8.1 and 10, the stacks with the
# AArch64 CPU). Built like topic ndk-arm64's test libraries: the host's clang
# for aarch64 against AOSP 8.1's bionic and NDK headers, linked against
# stand-ins of the system libraries (tools/ndk64/gen_stub_libs.py).
# (A64_CC, A64_LD, NDK64T_CFLAGS, crt_so.o: mk/topic-ndk-arm64.mk, later.)
NA64 := $(NA8)/arm64
NA64_STUBS := libc.so libm.so libdl.so liblog.so libandroid.so libEGL.so libGLESv2.so
NA64_CFLAGS = $(NDK64T_CFLAGS) -isystem $(A81)/frameworks/native/include \
    $(addprefix -isystem $(A81)/frameworks/native/libs/,nativewindow/include arect/include) \
    -isystem $(A81)/frameworks/native/opengl/include -Itests/native-activity/glue
$(NA64)/stubs/libc.so.S: tools/ndk64/gen_stub_libs.py tests/ndk/android81-ndk-symbols.txt
	@mkdir -p $(NA64)/stubs
	python3 tools/ndk64/gen_stub_libs.py tests/ndk/android81-ndk-symbols.txt $(NA64)/stubs $(NA64_STUBS)
$(addprefix $(NA64)/stubs/,$(addsuffix .S,$(filter-out libc.so,$(NA64_STUBS)))): $(NA64)/stubs/libc.so.S
	@test -f $@
$(NA64)/stubs/%.so: $(NA64)/stubs/%.so.S
	$(A64_CC) -c $< -o $<.o
	$(A64_LD) -shared -soname $*.so -o $@ $<.o
$(NA64)/natest.c.o: tests/native-activity/natest.c tests/native-activity/glue/android_native_app_glue.h
	@mkdir -p $(dir $@)
	$(A64_CC) $(NA64_CFLAGS) -c $< -o $@
$(NA64)/glue.c.o: tests/native-activity/glue/android_native_app_glue.c \
    tests/native-activity/glue/android_native_app_glue.h
	@mkdir -p $(dir $@)
	$(A64_CC) $(NA64_CFLAGS) -c $< -o $@
NA64_LIB := $(NA64)/libs/arm64-v8a/libnatest.so
$(NA64_LIB): $(NA64)/natest.c.o $(NA64)/glue.c.o $(BUILD)/ndk64/obj/crt_so.o \
    $(addprefix $(NA64)/stubs/,$(NA64_STUBS))
	@mkdir -p $(dir $@)
	$(A64_LD) -shared --no-undefined --hash-style=both -z max-page-size=4096 -L$(NA64)/stubs \
	    -soname libnatest.so -o $@ $(filter %.o,$^) -landroid -lEGL -lGLESv2 -llog -lc -lm -ldl
NA64_APK := $(NA8)/ArcWasmNativeActivityTestArm64.apk
$(NA64_APK): $(shell find tests/native-activity/apk -type f) $(NA64_LIB) tools/build_test_apk.sh
	rm -rf $(NA64)/src && mkdir -p $(NA64) && cp -R tests/native-activity/apk $(NA64)/src
	sed -i.orig 's/package="org.arcwasm.natest"/package="org.arcwasm.natest.arm64"/' \
	    $(NA64)/src/AndroidManifest.xml && rm $(NA64)/src/AndroidManifest.xml.orig
	MIN_SDK=21 TARGET_SDK=27 AAPT="$(AAPT)" ANDROID_JAR="$(ANDROID_JAR)" \
	    EXTRA_ASSETS="$(NA64_LIB):lib/arm64-v8a/libnatest.so" \
	    tools/build_test_apk.sh $(NA64)/src $(NA64)/work $@
native-activity-apk: $(NA64_APK)
ifneq ($(A8LIB),)
ASYSTEM8_ADD += --add $(NA64_APK):system/app/ArcWasmNativeActivityTestArm64.apk
$(A8SYSTEM_IMAGE): $(NA64_APK)
endif
ifneq ($(G10_FW_INCLUDES),)
ASYSTEM10_ADD += --add $(NA64_APK):system/app/ArcWasmNativeActivityTestArm64.apk
$(A10SYSTEM_IMAGE): $(NA64_APK)
endif
