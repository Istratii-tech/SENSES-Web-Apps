# Topic "ndk-imports": what NDK libraries built for Android 5-6 (API 21+)
# import from the system beyond Android 4.4's set (docs/topics/ndk-imports.md):
# bionic additions, libz, libEGL/GLES 3.0, libandroid, libjnigraphics,
# libOpenSLES. The files join the bridge library of topic ndk-bridge
# (mk/topic-ndk-bridge.mk, included before this one: its archive and unit
# test take every prerequisite, $^).
#
#   make ndk-check-imports SO=lib.so          # one library (ndk-bridge's target)
#   make ndk-check-all-imports DIR=lib/armeabi-v7a   # every .so of a directory

NDK_IMPORTS_SRCS := $(addprefix shim/ndk/,ndk_bionic.cc ndk_fd.cc ndk_locale.cc ndk_syscall.cc \
                      ndk_zlib.cc ndk_egl.cc ndk_android.cc ndk_opensles.cc ndk_libc8.cc \
                      ndk_glsl.cc ndk_android8.cc)
$(call dobj,$(NDK_IMPORTS_SRCS)): XFLAGS := $(NDK_FLAGS) -Ishim/gles
$(BUILD)/libndk_bridge.a: $(call dobj,$(NDK_IMPORTS_SRCS))
$(NDK_UNIT): $(call dobj,$(NDK_IMPORTS_SRCS))

# libandroid's framework side (AssetManager, Looper, the ANativeWindow of a
# Surface): framework headers, dalvikvm only. The bridge refers to it
# weakly, so -u pulls it out of the archive. (Recursive: WINDOW_XFLAGS and
# APK_CFLAGS come from topics included later or earlier.)
NDK_FW_SRCS := shim/ndk/ndk_android_fw.cc
$(call dobj,$(NDK_FW_SRCS)): XFLAGS = $(WINDOW_XFLAGS) -std=gnu++17 -DLOG_NDEBUG=1 -Ishim/ndk
# The guest's OpenSL ES plays on libmedia's AudioTrack (topic gles-audio).
$(call dobj,shim/ndk/ndk_audio_fw.cc): XFLAGS = $(AUDIO_XFLAGS)
$(BUILD)/libndk_bridge.a: $(call dobj,$(NDK_FW_SRCS) shim/ndk/ndk_audio_fw.cc)
NDK_FW_LDFLAGS := -Wl,-u,ndk_android_fw_linked -Wl,-u,ndk_audio_fw_linked
DVM_LDFLAGS += $(NDK_FW_LDFLAGS)
GWEB_LDFLAGS += $(NDK_FW_LDFLAGS)
# The unit test links the GL and EGL thunks too (WebGL2 is never reached in Node).
NDK_UNIT_LDFLAGS += -sUSE_ZLIB=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2

.PHONY: ndk-check-all-imports
ndk-check-all-imports: $(NDK_UNIT)
	tools/ndk_check_all_imports.py "$(DIR)"

# --- tests: tests/ndk6 (libndk6test.so in an APK of its own) and arcwasm.Ndk6Test ---------
# The library is built like topic ndk-bridge's test libraries (clang + lld,
# compiler-rt helpers: build/ndk/armeabi-v7a/libndkrt.a), but against the
# NDK's android-18 headers and stub libraries, which have EGL, GLES 3.0,
# OpenSL ES and libandroid; what they lack is declared in the source.
NDK18_SYSROOT := $(BUILD)/ndk/9/platforms/android-18/arch-arm/usr
$(NDK18_SYSROOT)/include/jni.h:
	@mkdir -p $(BUILD)/ndk
	tar xzf "$(NDK_ARCHIVE)" -C $(BUILD)/ndk 9/platforms/android-18/arch-arm
	touch $@
NDK6_CC := clang $(PREFIX_MAP) $(NDKT_ARCH_armeabi-v7a) -fpic -nostdinc -isystem $(NDK18_SYSROOT)/include \
           -isystem $(shell clang -print-resource-dir)/include -fno-exceptions -ffunction-sections \
           -fstack-protector -O2 -g -D_GNU_SOURCE -Wno-incompatible-library-redeclaration
NDK6_LIB := $(BUILD)/ndk/libs6/armeabi-v7a/libndk6test.so
$(BUILD)/ndk/armeabi-v7a/obj/ndk6test.c.o: tests/ndk6/ndk6test.c | $(NDK18_SYSROOT)/include/jni.h
	@mkdir -p $(dir $@)
	$(NDK6_CC) -c $< -o $@
$(NDK6_LIB): $(BUILD)/ndk/armeabi-v7a/obj/ndk6test.c.o $(BUILD)/ndk/armeabi-v7a/libndkrt.a
	@mkdir -p $(dir $@)
	$(NDKT_LD) -soname libndk6test.so -o $@ $(NDK18_SYSROOT)/lib/crtbegin_so.o $^ \
	    -L$(NDK18_SYSROOT)/lib -lEGL -lGLESv3 -lOpenSLES -landroid -ljnigraphics -lz -llog \
	    -lc -lm -ldl $(NDK18_SYSROOT)/lib/crtend_so.o

NDK6_APK_SRCS := $(shell find tests/ndk6/apk -type f)
NDK6_APK := $(BUILD)/dalvik/apk-ndk6/ArcWasmNdk6Test.apk
$(NDK6_APK): $(NDK6_APK_SRCS) $(NDK6_LIB) tools/build_test_apk.sh
	AAPT="$(AAPT)" D8="$(D8)" ANDROID_JAR="$(ANDROID_JAR)" \
	    EXTRA_ASSETS="$(NDK6_LIB):lib/armeabi-v7a/libndk6test.so" \
	    tools/build_test_apk.sh tests/ndk6/apk $(BUILD)/dalvik/apk-ndk6/work $@
DSYSTEM_ADD += --add $(NDK6_APK):system/app/ArcWasmNdk6Test.apk
$(DSYSTEM_IMAGE): $(NDK6_APK)

.PHONY: ndk6-test-lib ndk6-test
ndk6-test-lib: $(NDK6_LIB)
ndk6-test: dalvik
	cd $(BUILD)/dalvik && $(NODE) dalvikvm.js -cp /system/framework/arc-tests.jar arcwasm.Ndk6Test
