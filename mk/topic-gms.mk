# --- Topic gms: Google's services as the user installs them -----------------------------
# (docs/topics/gms.md). Nothing of Google's is in any image: a stack is a phone
# without Google Play until the user adds microG or Google's own APKs in
# SENSES Web Apps ("Сервисы Google": demo/apps/google.js), which the shell puts
# in the stack's /data as /data/google/PACKAGE/; the package managers of 8.1
# and 10 install them as privileged system apps, with microG's signature
# spoofing for a user-installed microG only (arcwasm.server.Gms).
#
# Tests (8.1 and 10, Node; tools/run_java_tests.sh):
#   GmsTest        no Google services: apps' "no Google Play" paths, no crash
#   GmsMicrogTest  microG installed as the shell does it (the official release,
#                  fetched here into $(GMS_DIR)/microg, never committed)
#   GmsOffTest     microG installed so, the app's switch off (ARC_GMS=0): as GmsTest
# all with an app on the real client libraries (tests/apk-gms; the AARs and
# jars tools/fetch_maven.py fetches from Google's Maven and Maven Central into
# $(GMS_MAVEN), not committed; the first build needs the network).
#
#   make gms-test

GMS_DIR := $(BUILD)/gms

# Gms.java in the system servers of 6.0.1, 8.1 and 10 (4.4's takes
# framework/java-arcwasm/arcwasm/server whole).
GMS_JAVA := framework/java-arcwasm/arcwasm/server/Gms.java
$(FS_SERVICES_JAR) $(F8S_SERVICES_JAR) $(F10S_SERVICES_JAR): $(GMS_JAVA) mk/topic-gms.mk

# --- GmsTest's app: the client libraries from Maven ----------------------------------
GMS_MAVEN ?= $(GMS_DIR)/maven
GMS_TEST_LIBS := com.google.android.gms:play-services-base:18.10.0 \
                 com.google.android.gms:play-services-ads-identifier:18.2.0 \
                 com.google.firebase:firebase-messaging:25.0.1 \
                 com.google.firebase:firebase-analytics:23.0.0 \
                 com.google.android.gms:play-services-auth:21.4.0 \
                 com.google.android.gms:play-services-location:21.3.0 \
                 com.google.android.gms:play-services-maps:19.2.0 \
                 com.google.android.gms:play-services-safetynet:18.1.0 \
                 com.android.billingclient:billing:8.0.0 \
                 com.google.android.play:review:2.0.2 \
                 com.google.android.play:app-update:2.1.0 \
                 com.google.android.play:integrity:1.4.0 \
                 com.google.android.play:asset-delivery:2.3.0
GMS_TEST_DEPS := $(GMS_DIR)/test-deps.txt
GMS_TEST_APK := $(GMS_DIR)/ArcWasmGmsTest.apk
$(GMS_TEST_DEPS): tools/fetch_maven.py mk/topic-gms.mk
	@mkdir -p $(GMS_MAVEN)
	python3 -I tools/fetch_maven.py "$(GMS_MAVEN)" $@.tmp $(GMS_TEST_LIBS) > $(GMS_DIR)/fetch.log
	mv $@.tmp $@
$(GMS_TEST_APK): $(GMS_TEST_DEPS) $(shell find tests/apk-gms -type f) tools/build_gms_test_apk.py
	python3 -I tools/build_gms_test_apk.py --deps $(GMS_TEST_DEPS) --src tests/apk-gms \
	    --work $(GMS_DIR)/test-work --out $@ --android-jar $(ANDROID_JAR) \
	    --aapt2 $(AAPT2) --d8 $(D8) --min-api 26

# The test app is an ordinary app (/system/app of the test images; the phone
# builds leave system/app out).
ASYSTEM8_ADD += --add $(GMS_TEST_APK):system/app/ArcWasmGmsTest.apk
ASYSTEM10_ADD += --add $(GMS_TEST_APK):system/app/ArcWasmGmsTest.apk
$(A8SYSTEM_IMAGE) $(A10SYSTEM_IMAGE): $(GMS_TEST_APK)

# --- microG for GmsMicrogTest: the official release, checked -------------------------
MICROG_VERSION := v0.3.17.252432
MICROG_RELEASE := https://github.com/microg/GmsCore/releases/download/$(MICROG_VERSION)
MICROG_APKS := $(GMS_DIR)/microg/com.google.android.gms-252432034.apk \
               $(GMS_DIR)/microg/com.android.vending-84022634.apk
MICROG_SHA256 := e5e6cba078bdec812b5450599350e5c1a12c8b7438da5bf77123af170f8c1a35:com.google.android.gms-252432034.apk \
                 9b8616011cc8f026bcbbe094df850794abb9d6d9f23214d7a8f8c4d1033ea012:com.android.vending-84022634.apk
$(GMS_DIR)/microg/%.apk: mk/topic-gms.mk
	@mkdir -p $(GMS_DIR)/microg
	curl -sSfL -o $@.tmp $(MICROG_RELEASE)/$*.apk
	echo "$(subst :,  ,$(filter %:$*.apk,$(MICROG_SHA256)))" | sed "s|  $*.apk|  $@.tmp|" | shasum -a 256 -c -
	mv $@.tmp $@
.PHONY: microg-apks
microg-apks: $(MICROG_APKS)

# --- the tests ---------------------------------------------------------------------------
ART8_JAVA_TESTS += tests/java8/arcwasm/GmsTest.java
ART10_JAVA_TESTS += tests/java8/arcwasm/GmsTest.java
$(A8FW)/arc-tests.jar $(A10FW)/arc-tests.jar: tests/java8/arcwasm/GmsTest.java

.PHONY: gms-test
gms-test: art8 art10 microg-apks
	VM=art8 tools/run_java_tests.sh node GmsTest GmsMicrogTest GmsOffTest
	VM=art10 tools/run_java_tests.sh node GmsTest GmsMicrogTest GmsOffTest
