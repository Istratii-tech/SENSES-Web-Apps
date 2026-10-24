# --- Topic apks-split: apps delivered as split APKs (.apks/.xapk/.apkm) -----------------
# (docs/topics/apks-split.md). The shell (demo/apps) unpacks such a file into
# the library as senses/apps/PACKAGE/{base.apk, split_SPLITNAME.apk ...,
# obb/*.obb}; the package managers of 6.0.1 and 8.1 install the directory as
# one package (AppLibrary, PackageManagerService); an .xapk's OBB files are
# mounted read-only at the app's OBB directory ($ARC_APP_OBB, art/vfs_setup.cc,
# dalvik/launcher/vfs_setup.cc). Named to be included after topic-art*.mk and
# topic-install8.mk (the images and test lists it adds to).
#
#   make apks-split-test   # SplitInstallTest in Node on 6.0.1 and 8.1 (and 4.4's check)
#   make apks-split-shell-test  # the shell (demo/apps: apk.js, install.js, store-worker.js)
#                               # under Node on .apks/.xapk/.apkm made from the test app,
#                               # then ART 8.1 runs what it installed (--e2e)
#   make apks-split-apks   # build/apks-split/v1, v2: the test app's parts

# --- SplitInstallTest's app (tests/apk-split): versions 1 and 2, six APKs each ----------
APKS_SPLIT_DIR := $(BUILD)/apks-split
APKS_SPLIT_SRCS := $(shell find tests/apk-split -type f)
APKS_SPLIT_PARTS := base.apk split_config.armeabi_v7a.apk split_config.mdpi.apk \
                    split_config.ru.apk split_config.xhdpi.apk split_feature.apk
APKS_SPLIT_STAMPS := $(APKS_SPLIT_DIR)/v1/.built $(APKS_SPLIT_DIR)/v2/.built
$(APKS_SPLIT_DIR)/v%/.built: $(APKS_SPLIT_SRCS) $(INSTALL8_LIB) tools/build_split_test_apks.sh \
                             mk/topic-senses-apks-split.mk
	AAPT2="$(AAPT2)" D8="$(D8)" ANDROID_JAR="$(ANDROID_JAR)" NDK_LIB="$(INSTALL8_LIB)" \
	    tools/build_split_test_apks.sh $* $(APKS_SPLIT_DIR)/v$* $(APKS_SPLIT_DIR)/work-v$*
	touch $@
# In each stack's image (system/etc/arcwasm/split-test/vN/), where the test
# (playing the shell) copies them from; the phone builds leave them out
# (mk/topic-senses-apps-vm.mk).
apks_split_add = $(foreach v,1 2,$(foreach p,$(APKS_SPLIT_PARTS),\
                   --add $(APKS_SPLIT_DIR)/v$(v)/$(p):system/etc/arcwasm/split-test/v$(v)/$(p)))
DSYSTEM_ADD += $(apks_split_add)
ASYSTEM_ADD += $(apks_split_add)
ASYSTEM8_ADD += $(apks_split_add)
$(DSYSTEM_IMAGE) $(ASYSTEM_IMAGE) $(A8SYSTEM_IMAGE): $(APKS_SPLIT_STAMPS)

# The test, in each stack's arc-tests.jar (Dalvik's takes tests/java whole).
ART_JAVA_TESTS += tests/java/arcwasm/SplitInstallTest.java
ART8_JAVA_TESTS += tests/java/arcwasm/SplitInstallTest.java
$(AFW)/arc-tests.jar $(A8FW)/arc-tests.jar: tests/java/arcwasm/SplitInstallTest.java

.PHONY: apks-split-apks apks-split-test apks-split-shell-test
apks-split-apks: $(APKS_SPLIT_STAMPS)
apks-split-shell-test: $(APKS_SPLIT_STAMPS) art8
	node tests/apps/apks_split_test.js --e2e
apks-split-test: dalvik art art8
	tools/run_java_tests.sh node SplitInstallTest
	VM=art tools/run_java_tests.sh node SplitInstallTest
	VM=art8 tools/run_java_tests.sh node SplitInstallTest
