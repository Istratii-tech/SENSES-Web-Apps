# Topic "apk": loading an APK's code and resources (docs/topics/apk.md).
# libandroidfw (ResTable, AssetManager, ZipFileRO, Asset) and the JNI of
# android.content.res.{AssetManager,StringBlock,XmlBlock} and
# android.os.SystemProperties, linked into dalvikvm; the test APK
# (tests/apk) goes into the /system image as /system/app/ArcWasmTestApp.apk,
# its classes.dex also on its own as /system/app/ArcWasmTestApp.dex.
# Included from the Makefile after mk/dalvik.mk and mk/framework.mk.

# --- libandroidfw (frameworks/base/libs/androidfw) -----------------------------
# The host source list of its Android.mk; the device-only ones (backup
# helpers, CursorWindow) need libbinder.
AFW := $(FWB)/libs/androidfw
AFW_SRCS := $(shell tools/mk_srcs.py $(AFW)/Android.mk commonSources $(AFW)/)
# LOG_NDEBUG=1 as in a release build: LOG_FATAL_IF and ALOGV compile out.
# Otherwise AssetManager::getResTable() aborts for the system AssetManager,
# which has no asset paths without /system/framework/framework-res.apk.
APK_CFLAGS := $(FW_INCLUDES) -DLOG_NDEBUG=1
$(DOBJ)/$(AFW)/%: XFLAGS := $(APK_CFLAGS) -DLOG_TAG=\"asset\"
$(BUILD)/libandroidfw.a: $(call dobj,$(AFW_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- JNI (frameworks/base/core/jni) ------------------------------------------
APK_JNI_SRCS := $(addprefix $(FWB)/core/jni/,android_util_AssetManager.cpp \
                  android_util_StringBlock.cpp android_util_XmlBlock.cpp \
                  android_os_SystemProperties.cpp) \
                shim/androidfw/parcel_file_descriptor.cc
$(DOBJ)/shim/androidfw/%: XFLAGS := $(APK_CFLAGS)
$(call dobj,$(filter $(FWB)/%,$(APK_JNI_SRCS))): XFLAGS := $(APK_CFLAGS)
$(BUILD)/libandroid_runtime_apk.a: $(call dobj,$(APK_JNI_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

APK_LIBS := $(BUILD)/libandroid_runtime_apk.a $(BUILD)/libandroidfw.a
# DALVIK_LIBS is expanded in the link recipes, so appending here reaches them;
# the prerequisites are added separately.
DALVIK_LIBS += $(APK_LIBS)
$(BUILD)/dalvik/dalvikvm.js $(DWEB)/dalvikvm.js: $(APK_LIBS)

# --- test APK (tests/apk) ------------------------------------------------------
# aapt from the SDK compiles the resources against android.jar of any
# installed platform; javac + d8 (min-api 19, as for core.jar) make the dex.
SDK ?= $(HOME)/Library/Android/sdk
AAPT ?= $(lastword $(sort $(wildcard $(SDK)/build-tools/*/aapt)))
ANDROID_JAR ?= $(lastword $(sort $(wildcard $(SDK)/platforms/android-*/android.jar)))
TEST_APK := $(BUILD)/dalvik/apk/ArcWasmTestApp.apk
TEST_APK_FILES := $(shell find tests/apk -type f)
# A font asset for Typeface.createFromAsset (GraphicsTest), taken from AOSP.
TEST_APK_FONT := $(A44)/frameworks/base/data/fonts/DroidSansMono.ttf
$(TEST_APK): $(TEST_APK_FILES) $(TEST_APK_FONT) tools/build_test_apk.sh
	AAPT="$(AAPT)" D8="$(D8)" ANDROID_JAR="$(ANDROID_JAR)" \
	    EXTRA_ASSETS="$(TEST_APK_FONT):assets/fonts/DroidSansMono.ttf" \
	    tools/build_test_apk.sh tests/apk $(BUILD)/dalvik/apk $@

DSYSTEM_ADD += --add $(TEST_APK):system/app/ArcWasmTestApp.apk \
               --add $(BUILD)/dalvik/apk/dex/classes.dex:system/app/ArcWasmTestApp.dex
$(DSYSTEM_IMAGE): $(TEST_APK)

.PHONY: apk-libs test-apk apk-test
apk-libs: $(APK_LIBS)
test-apk: $(TEST_APK)
apk-test: dalvik
	cd $(BUILD)/dalvik && $(NODE) dalvikvm.js -cp /system/framework/arc-tests.jar arcwasm.ApkTest
