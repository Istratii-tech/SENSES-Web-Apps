# Topic "framework-res": AOSP 4.4.4's framework resources
# (frameworks/base/core/res) as /system/framework/framework-res.apk
# (docs/topics/framework-res.md). Every AssetManager adds it first
# (AssetManager::addDefaultAssets, $ANDROID_ROOT/framework/framework-res.apk),
# so @android:... references in app resources resolve and
# Resources.getSystem() has the "android" package.
# Included from the Makefile after mk/dalvik.mk and mk/framework.mk.

SDK ?= $(HOME)/Library/Android/sdk
AAPT ?= $(lastword $(sort $(wildcard $(SDK)/build-tools/*/aapt)))
ZIPALIGN ?= $(lastword $(sort $(wildcard $(SDK)/build-tools/*/zipalign)))

FRES := $(FWB)/core/res
FRES_OUT := $(BUILD)/dalvik/framework-res
FRAMEWORK_RES_APK := $(FRES_OUT)/framework-res.apk

# What goes in. The full set is 19.5 MB (resources.arsc with ~70 locales is
# 8.6 MB, PNGs of six densities 8.4 MB) and every page load downloads the
# whole /system image (dalvikvm.data), so by default only English strings and
# one density are kept (4.6 MB). Empty = keep everything, e.g.
#   make dalvik FRAMEWORK_RES_LOCALES= FRAMEWORK_RES_DENSITY=
# FRAMEWORK_RES_LOCALES: aapt -c list (en,ru,...). FRAMEWORK_RES_DENSITY: aapt
# --preferred-density, keeps that density of each drawable (or the closest
# one when it has none); mdpi matches the default Configuration (density 0).
FRAMEWORK_RES_LOCALES ?= en
FRAMEWORK_RES_DENSITY ?= mdpi

# -x: package id 0x01 (core/res/Android.mk: LOCAL_AAPT_FLAGS := -x). No -I:
# this is the package everything else is built against.
FRAMEWORK_RES_FLAGS := -x --min-sdk-version 19 --target-sdk-version 19 \
    --version-code 19 --version-name 4.4.4 \
    $(if $(FRAMEWORK_RES_LOCALES),-c $(FRAMEWORK_RES_LOCALES)) \
    $(if $(FRAMEWORK_RES_DENSITY),--preferred-density $(FRAMEWORK_RES_DENSITY))

# The same aapt run writes the R classes (-m -J: one directory per package),
# so their ids are the ones in framework-res.apk. With the manifest package
# "android" aapt puts the symbols declared in res/values/public.xml (plus
# R.styleable) in android.R and every symbol in com.android.internal.R;
# --private-symbols com.android.internal is aapt's default for "android",
# spelled out. Neither the apk nor the ids depend on -c/--preferred-density.
# framework-core.jar gets them as AOSP's framework.jar does
# (frameworks/base/Android.mk): android/R.java, android/Manifest.java and
# com/android/internal/R.java.
FRES_GEN := $(FRES_OUT)/gen
FRES_JAVA := $(FRES_GEN)/android/R.java $(FRES_GEN)/android/Manifest.java \
    $(FRES_GEN)/com/android/internal/R.java

# Rebuild when the flags change (the file is rewritten only then).
$(FRES_OUT)/aapt-flags: FORCE
	@mkdir -p $(dir $@)
	@echo '$(FRAMEWORK_RES_FLAGS)' | cmp -s - $@ || echo '$(FRAMEWORK_RES_FLAGS)' > $@

FRES_FILES := $(FRES)/AndroidManifest.xml $(shell find $(FRES)/res $(FRES)/assets -type f)
# aapt prints ~1300 "has no default translation" warnings (strings that exist
# only in some locales); they go to aapt.log.
# One run makes the apk and the R classes; make 3.81 has no grouped targets,
# so both hang off a stamp.
$(FRES_OUT)/aapt.stamp: $(FRES_FILES) $(FRES_OUT)/aapt-flags
	@rm -rf $(FRES_GEN) && mkdir -p $(FRES_GEN)
	"$(AAPT)" package -f $(FRAMEWORK_RES_FLAGS) -M $(FRES)/AndroidManifest.xml \
	    -S $(FRES)/res -A $(FRES)/assets -F $(FRAMEWORK_RES_APK).unaligned \
	    -m -J $(FRES_GEN) --private-symbols com.android.internal 2> $(FRES_OUT)/aapt.log \
	    || { tail -20 $(FRES_OUT)/aapt.log; exit 1; }
	"$(ZIPALIGN)" -f 4 $(FRAMEWORK_RES_APK).unaligned $(FRAMEWORK_RES_APK)
	@rm -f $(FRAMEWORK_RES_APK).unaligned
	@echo "built $(FRAMEWORK_RES_APK) ($$(wc -c < $(FRAMEWORK_RES_APK) | tr -d ' ') bytes)"
	@touch $@
$(FRAMEWORK_RES_APK) $(FRES_JAVA): $(FRES_OUT)/aapt.stamp ;

FW_GEN_JAVA += $(FRES_JAVA)
$(DFW)/framework.jar: $(FRES_JAVA)

DSYSTEM_ADD += --add $(FRAMEWORK_RES_APK):system/framework/framework-res.apk
$(DSYSTEM_IMAGE): $(FRAMEWORK_RES_APK)

.PHONY: framework-res framework-res-test FORCE
framework-res: $(FRAMEWORK_RES_APK)
framework-res-test: dalvik
	cd $(BUILD)/dalvik && $(NODE) dalvikvm.js -cp /system/framework/arc-tests.jar arcwasm.FrameworkResTest
