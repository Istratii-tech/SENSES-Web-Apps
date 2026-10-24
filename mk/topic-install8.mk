# --- Topic install8: SENSES Web Apps' app library on the VM side -------------------
# (docs/topics/install8.md). The library (senses/apps/PACKAGE/base.apk in the
# browser's storage) is mounted read-only at /mnt/apps ($ARC_APPS_DIR) and
# each stack's /data is its own directory there ($ARC_DATA_DIR: art/vfs_setup.cc,
# dalvik/launcher/vfs_setup.cc); the package managers of 4.4, 6.0.1 and 8.1
# install from it in place (arcwasm.server.AppLibrary); SystemServer --senses
# PACKAGE is the one-app mode (arcwasm.server.Senses); ART caches the apps'
# verified dex files in /data/dalvik-cache/arcwasm (patches/aosp{60,81}/install8.patch).
# The phone builds of the three stacks for the shell: mk/topic-senses-apps-vm.mk.
#
#   make install8-test   # InstallTest on the three stacks in Node

# Shared by the three system servers (4.4 takes framework/java-arcwasm/arcwasm/server
# whole): the library, the one-app mode, the HOME key (now also on 8.1).
INSTALL8_SHARED_JAVA := framework/java-arcwasm/arcwasm/server/AppLibrary.java \
                        framework/java-arcwasm/arcwasm/server/Senses.java \
                        framework/java-arcwasm/arcwasm/server/SystemKeys.java
$(FS_SERVICES_JAR) $(F8S_SERVICES_JAR): $(INSTALL8_SHARED_JAVA) mk/topic-install8.mk

# Under Node, Dalvik's getenv() sees the ARC_* variables too (ARC_DATA_DIR,
# ARC_APPS_DIR ...), as artvm's (art/artvm_pre.js).
$(BUILD)/dalvik/dalvikvm.js: DVM_LDFLAGS += --pre-js art/artvm_pre.js
$(BUILD)/dalvik/dalvikvm.js: art/artvm_pre.js

# --- InstallTest's app: versionCode 1 and 2, with an ARM library ------------------------
INSTALL8_APK_SRCS := $(shell find tests/apk-install -type f)
INSTALL8_APK_DIR := $(BUILD)/install8
INSTALL8_APKS := $(INSTALL8_APK_DIR)/v1.apk $(INSTALL8_APK_DIR)/v2.apk
INSTALL8_LIB := $(BUILD)/ndk/libs/armeabi-v7a/libndkhelper.so
$(INSTALL8_APK_DIR)/v%.apk: $(INSTALL8_APK_SRCS) $(INSTALL8_LIB) tools/build_test_apk.sh mk/topic-install8.mk
	rm -rf $(INSTALL8_APK_DIR)/src-v$* && mkdir -p $(INSTALL8_APK_DIR) && \
	    cp -R tests/apk-install $(INSTALL8_APK_DIR)/src-v$*
	sed -i.orig -e 's/android:versionCode="1"/android:versionCode="$*"/' \
	    -e 's/android:versionName="1.0"/android:versionName="$*.0"/' \
	    $(INSTALL8_APK_DIR)/src-v$*/AndroidManifest.xml
	AAPT="$(AAPT)" D8="$(D8)" ANDROID_JAR="$(ANDROID_JAR)" \
	    EXTRA_ASSETS="$(INSTALL8_LIB):lib/armeabi-v7a/libndkhelper.so" \
	    tools/build_test_apk.sh $(INSTALL8_APK_DIR)/src-v$* $(INSTALL8_APK_DIR)/work-v$* $@
# In each stack's image, where the test (the "shell") copies them from; not
# in /system/app (not scanned).
install8_add = $(foreach v,1 2,--add $(INSTALL8_APK_DIR)/v$(v).apk:system/etc/arcwasm/install-test/v$(v).apk)
DSYSTEM_ADD += $(install8_add)
ASYSTEM_ADD += $(install8_add)
ASYSTEM8_ADD += $(install8_add)
$(DSYSTEM_IMAGE) $(ASYSTEM_IMAGE) $(A8SYSTEM_IMAGE): $(INSTALL8_APKS)

# The test, in each stack's arc-tests.jar (Dalvik's takes tests/java whole).
ART_JAVA_TESTS += tests/java/arcwasm/InstallTest.java
ART8_JAVA_TESTS += tests/java/arcwasm/InstallTest.java
$(AFW)/arc-tests.jar $(A8FW)/arc-tests.jar: tests/java/arcwasm/InstallTest.java

.PHONY: install8-test
install8-test: dalvik art art8
	tools/run_java_tests.sh node InstallTest
	VM=art tools/run_java_tests.sh node InstallTest
	VM=art8 tools/run_java_tests.sh node InstallTest
