# The first slice of the Android framework (AOSP 4.4.4) on Dalvik:
# libutils (Looper on epoll), the libandroid_runtime JNI for android.util.Log,
# android.os.SystemClock and android.os.MessageQueue, and the framework's
# jars (tools/build_framework_jar.sh). Binder and Parcel: mk/topic-binder.mk.
# Included from mk/dalvik.mk, which links all of it into dalvikvm.

FWB := $(A44)/frameworks/base
FW_INCLUDES := $(D_INCLUDES) -I$(A44)/libnativehelper/include -I$(A44)/frameworks/native/include -I$(FWB)/include \
               -I$(FWB)/core/jni

# --- libutils (system/core/libutils) -------------------------------------------
# The host source list of its Android.mk, Looper included. CallStack and
# ProcessCallStack need libcorkscrew (stack unwinding); not built.
UTILS := $(A44)/system/core/libutils
UTILS_SRCS := $(filter-out %/CallStack.cpp %/ProcessCallStack.cpp, \
                $(shell tools/mk_srcs.py $(UTILS)/Android.mk commonSources $(UTILS)/)) \
              $(UTILS)/Looper.cpp
$(DOBJ)/$(UTILS)/%: XFLAGS := $(FW_INCLUDES) -DLIBUTILS_NATIVE=1
$(BUILD)/libutils.a: $(call dobj,$(UTILS_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- libandroid_runtime (frameworks/base/core/jni), first slice ------------------
# AndroidRuntime.cpp itself (VM startup, the full gRegJNI table) is replaced
# by shim/framework/{android_runtime,register_framework}.cc. libcutils'
# properties.c (host variant: no property service) backs Log.isLoggable.
AR_SRCS := $(addprefix $(FWB)/core/jni/,android_util_Log.cpp android_os_SystemClock.cpp \
             android_os_MessageQueue.cpp) \
           shim/framework/android_runtime.cc shim/framework/register_framework.cc \
           $(A44)/system/core/libcutils/properties.c
$(DOBJ)/$(FWB)/core/jni/%: XFLAGS := $(FW_INCLUDES)
# linux/android_alarm.h: the UAPI header posix_translation's /dev/alarm uses.
$(DOBJ)/$(FWB)/core/jni/android_os_SystemClock.cpp.o: XFLAGS := $(FW_INCLUDES) \
    -DLOG_TAG=\"SystemClock\" -Ishim/linux/..
$(DOBJ)/shim/framework/%: XFLAGS := $(FW_INCLUDES) -DLOG_TAG=\"AndroidRuntime\"
$(DOBJ)/$(A44)/system/core/libcutils/properties.c.o: XFLAGS := $(D_INCLUDES)
$(BUILD)/libandroid_runtime.a: $(call dobj,$(AR_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

FRAMEWORK_LIBS := $(BUILD)/libandroid_runtime.a $(BUILD)/libutils.a

# --- the framework's boot class path jars ---------------------------------------
# AOSP 4.4.4's framework.jar and framework2.jar built from the whole of
# frameworks/base, and the libraries they are compiled against, from source
# (tools/build_framework_jar.sh, docs/topics/framework-jar.md).
# FW_BOOT_JARS: BOOTCLASSPATH after core.jar, in AOSP 4.4.4's order (the
# same list is in dalvik/launcher/dalvikvm.cc).
FW_LIB_JARS := conscrypt bouncycastle okhttp core-junit ext
FW_BOOT_JARS := conscrypt okhttp core-junit bouncycastle ext framework framework2
FW_JARS := $(addprefix $(DFW)/,$(addsuffix .jar,$(FW_BOOT_JARS)))
# The javac output of each, for compiling and dexing against.
FW_CLASSES := $(foreach j,$(FW_LIB_JARS),$(DFW)/$(j)-java/$(j)-classes.jar) \
              $(DFW)/framework-java/framework-classes.jar \
              $(DFW)/framework-java/framework2-classes.jar
FW_LIB_SRCS := $(shell find $(A44)/libcore/crypto/src/main $(A44)/external/bouncycastle/bcprov/src/main \
                  $(A44)/external/okhttp/src/main $(A44)/external/okhttp/android/main $(A44)/external/junit/src \
                  $(A44)/external/nist-sip/java $(A44)/external/apache-http/src $(A44)/external/tagsoup/src \
                  $(A44)/external/libphonenumber/java/src -type f)
$(DFW)/framework-libs.stamp: tools/build_framework_jar.sh $(DFW)/core.jar $(FW_LIB_SRCS)
	tools/build_framework_jar.sh $(DFW) libs
	@touch $@
$(foreach j,$(FW_LIB_JARS),$(DFW)/$(j).jar $(DFW)/$(j)-java/$(j)-classes.jar): $(DFW)/framework-libs.stamp ;

FW_SRC_DIRS := $(addprefix $(FWB)/,$(addsuffix /java,core graphics location media media/mca/effect \
                 media/mca/filterfw media/mca/filterpacks drm opengl sax telephony wifi keystore))
FW_SRCS := $(shell find $(FW_SRC_DIRS) $(FWB)/packages/services -name '*.java' -o -name '*.aidl' \
             -o -name '*.logtags') $(FWB)/Android.mk $(FWB)/preloaded-classes
# (framework/java-arcwasm/arcwasm/server is services.jar: mk/topic-activity.mk.)
FW_JAVA := $(shell find framework/java-stubs framework/java-arcwasm -name '*.java' \
             -not -path 'framework/java-arcwasm/arcwasm/server/*')
# FW_GEN_JAVA: generated sources topics add (R.java: mk/topic-framework-res.mk),
# also listed as prerequisites of $(DFW)/framework.jar there.
$(DFW)/framework.jar: tools/build_framework_jar.sh tools/java_event_log_tags.py \
                      $(DFW)/framework-libs.stamp $(FW_SRCS) $(FW_JAVA)
	tools/build_framework_jar.sh $(DFW) framework $(FW_GEN_JAVA)
$(DFW)/framework2.jar $(DFW)/framework-java/framework-classes.jar \
  $(DFW)/framework-java/framework2-classes.jar: $(DFW)/framework.jar ;

.PHONY: framework-libs framework-jars
framework-jars: $(FW_JARS)
framework-libs: $(FRAMEWORK_LIBS) $(FW_JARS)
