# Topic "userspace": the stock AOSP 4.4.4 system apps (Calculator, DeskClock,
# Settings, Launcher2, SystemUI from build/system-apks44) on this stack. See
# docs/topics/userspace.md. Here: the frameworks/base/core/jni modules they
# need that the earlier topics don't build, registered by
# shim/userspace/register_userspace.cc (called from register_framework.cc).
#
#   android_text_format_Time + Time.cpp   android.text.format.Time natives;
#                                         bionic's tz functions over musl
#                                         (shim/userspace/tztime.cc)
#   android_view_VelocityTracker          over libinput's VelocityTracker
#                                         (topic input)
#   timezone.cc                           persist.sys.timezone from the
#                                         browser's Intl zone, with bionic's
#                                         tzdata in /system/usr/share/zoneinfo
#   android_database_*                    android.database.sqlite and
#                                         CursorWindow over external/sqlite
#                                         (patches/aosp44/userspace.projects)
#                                         and libandroidfw's CursorWindow
#
# Included by the Makefile after the earlier topics (uses their variables).

US_JNI_SRCS := $(addprefix $(FWB)/core/jni/,android_text_format_Time.cpp Time.cpp \
                 android_view_VelocityTracker.cpp) \
               shim/userspace/tztime.cc shim/userspace/register_userspace.cc \
               shim/userspace/timezone.cc \
               $(addprefix $(FWB)/core/jni/,android_database_SQLiteConnection.cpp \
                 android_database_SQLiteGlobal.cpp android_database_SQLiteDebug.cpp \
                 android_database_SQLiteCommon.cpp android_database_CursorWindow.cpp) \
               $(FWB)/libs/androidfw/CursorWindow.cpp
$(call dobj,$(US_JNI_SRCS)): XFLAGS := -Ishim/userspace/include $(GJNI_XFLAGS) $(BINDER_INC) \
    -DLOG_TAG=\"UserspaceJNI\"
US_SQLITE := $(A44)/external/sqlite
# dist/Android.mk's common_sqlite_flags and the device ones.
US_SQLITE_FLAGS := -DNDEBUG=1 -DHAVE_USLEEP=1 -DSQLITE_HAVE_ISNAN \
  -DSQLITE_DEFAULT_JOURNAL_SIZE_LIMIT=1048576 -DSQLITE_THREADSAFE=2 -DSQLITE_TEMP_STORE=3 \
  -DSQLITE_POWERSAFE_OVERWRITE=1 -DSQLITE_DEFAULT_FILE_FORMAT=4 -DSQLITE_DEFAULT_AUTOVACUUM=1 \
  -DSQLITE_ENABLE_MEMORY_MANAGEMENT=1 -DSQLITE_ENABLE_FTS3 -DSQLITE_ENABLE_FTS3_BACKWARDS \
  -DSQLITE_ENABLE_FTS4 -DSQLITE_OMIT_BUILTIN_TEST -DSQLITE_OMIT_COMPILEOPTION_DIAGS \
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_FILE_PERMISSIONS=0600 \
  -DUSE_PREAD64 -Dfdatasync=fdatasync -DHAVE_MALLOC_USABLE_SIZE
US_SQLITE_SRCS := $(US_SQLITE)/dist/sqlite3.c \
                  $(addprefix $(US_SQLITE)/android/,sqlite3_android.cpp PhoneNumberUtils.cpp \
                    OldPhoneNumberUtils.cpp)
$(call dobj,$(US_SQLITE)/dist/sqlite3.c): XFLAGS := $(D_INCLUDES) $(US_SQLITE_FLAGS) -w
$(call dobj,$(filter %.cpp,$(US_SQLITE_SRCS))): XFLAGS := $(FW_INCLUDES) $(ICU_INCLUDES) \
    -idirafter $(US_SQLITE)/dist -DLOG_TAG=\"sqlite3_android\"
# The JNI includes <sqlite3.h> and <sqlite3_android.h>; -idirafter: dist/ has a
# file "version" that would shadow libc++'s <version>.
$(call dobj,$(filter $(FWB)/core/jni/android_database_%,$(US_JNI_SRCS))): \
    XFLAGS := $(GJNI_XFLAGS) $(BINDER_INC) -idirafter $(US_SQLITE)/dist -idirafter $(US_SQLITE)/android \
    -DLOG_TAG=\"SQLiteJNI\" -DLOG_NDEBUG=1
$(call dobj,$(FWB)/libs/androidfw/CursorWindow.cpp): XFLAGS := $(APK_CFLAGS) $(BINDER_INC) \
    -DLOG_TAG=\"CursorWindow\"
US_JNI_SRCS += $(US_SQLITE_SRCS)

$(BUILD)/libuserspace_jni.a: $(call dobj,$(US_JNI_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

US_LIBS := $(BUILD)/libuserspace_jni.a
DALVIK_LIBS += $(US_LIBS)
GWEB_LIBS += $(US_LIBS)
$(BUILD)/dalvik/dalvikvm.js $(DWEB)/dalvikvm.js $(GWEB)/dalvikvm.js: $(US_LIBS)

# libcore's ZoneInfoDB reads the zones from here (TimeZone.getTimeZone).
# tools/fetch_tzdata.sh's newer copy (2016a) when present; else 4.4's 2014a.
US_TZDATA := $(firstword $(wildcard third_party/tzdata/tzdata) $(A44)/bionic/libc/zoneinfo/tzdata)
DSYSTEM_ADD += --add $(US_TZDATA):system/usr/share/zoneinfo/tzdata
$(DSYSTEM_IMAGE): $(US_TZDATA)

# The stock system apps of each stack (not committed): AOSP 4.4.4's from
# tools/fetch_system_apks.sh into Dalvik's /system/app, Android 6.0's from
# tools/fetch_system_apks6.sh into ART's. Kept apart from build/extra-apks,
# which goes into both images: the two sets share APK names.
SYSTEM_APKS44 := $(wildcard $(BUILD)/system-apks44/*.apk)
DSYSTEM_ADD += $(foreach a,$(SYSTEM_APKS44),--add $(a):system/app/$(notdir $(a)))
$(BUILD)/dalvik/system.img: $(SYSTEM_APKS44)
SYSTEM_APKS6 := $(wildcard $(BUILD)/system-apks6/*.apk)
ASYSTEM_ADD += $(foreach a,$(SYSTEM_APKS6),--add $(a):system/app/$(notdir $(a)))
$(ASYSTEM_IMAGE): $(SYSTEM_APKS6)
# ART's libcore reads the same tzdata (ZoneInfoDB): DeskClock's analog clock.
ASYSTEM_ADD += --add $(US_TZDATA):system/usr/share/zoneinfo/tzdata
$(ASYSTEM_IMAGE): $(US_TZDATA)

# Natives the Android 6.0 system apps need (shim/userspace6, libraries linked
# into artvm: shim/art/static_jni_libs_art.cc): SystemUI's keyguard sounds
# (libsoundpool.so: SoundPool, silent; the file in /system/lib is empty,
# System.loadLibrary wants one).
US6_SRCS := shim/userspace6/register_userspace6.cc
$(call fsobj,$(US6_SRCS)): FSFLAGS := $(FG_FW_INCLUDES_BASE)
$(FSLIB)/libuserspace6_jni.a: $(call fsobj,$(US6_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
FW6_JNI_LIBS += $(FSLIB)/libuserspace6_jni.a
$(ABLD)/artvm.js $(AWEB)/artvm.js $(FGWEB)/artvm.js $(BUILD)/pwa/a60/artvm.js: \
    $(FSLIB)/libuserspace6_jni.a
ASYSTEM_ADD += --add $(FS)/empty.so:system/lib/libsoundpool.so
# What goes into the images is listed here (ASYSTEM_ADD, DSYSTEM_ADD): a change
# remakes them.
$(ASYSTEM_IMAGE) $(DSYSTEM_IMAGE): mk/topic-userspace.mk

# framework-res 6.0.1 numbered as the image's the 6.0 system apps come from
# (tools/make_public_ids.py): with build/system-apks6/framework-res-ids.txt
# (tools/fetch_system_apks6.sh), an overlay pins every resource's id.
US6_IDS := $(wildcard $(BUILD)/system-apks6/framework-res-ids.txt)
ifneq ($(US6_IDS),)
US6_IDS_DIR := $(ABLD)/framework-res-ids
$(US6_IDS_DIR)/values/public-arcwasm.xml: $(US6_IDS) tools/make_public_ids.py
	python3 tools/make_public_ids.py $(US6_IDS) $(US6_IDS_DIR)
F6RES_OVERLAY := -S $(US6_IDS_DIR)
$(F6RES_OUT)/aapt.stamp: $(US6_IDS_DIR)/values/public-arcwasm.xml
endif

# --- HTC Sense 5.5 on the Dalvik stack ------------------------------------------
# tools/fetch_system_apks_htc.sh (from an HTC One firmware zip, not committed)
# makes build/system-apkshtc: HTC's launcher, SystemUI and package installer,
# the classes of its framework libraries AOSP lacks (boot jars appended by
# its build.prop line) and its framework resources (packages 0x02, 0x03,
# added to every AssetManager: patches/aosp44/userspace.patch). Its image is
# the 4.4 one with those instead of the AOSP 4.4 apps:
#   make dalvik-htc   # build/dalvik-htc/dalvikvm.js (Node), its system.img
HTC_DIR := $(BUILD)/system-apkshtc
ifneq ($(wildcard $(HTC_DIR)/build.prop),)
HTC_BLD := $(BUILD)/dalvik-htc
HSYSTEM_IMAGE := $(HTC_BLD)/system.img
HTC_APPS := $(wildcard $(HTC_DIR)/app/*.apk)
HTC_FW := $(wildcard $(HTC_DIR)/framework/*)
# DSYSTEM_ADD without the AOSP 4.4 apps and build.prop.
htc_system_add = $(patsubst --add=%,--add %,$(filter-out --add=$(BUILD)/system-apks44/% \
                   --add=framework/build.prop:%,$(subst --add ,--add=,$(DSYSTEM_ADD))))
$(HTC_BLD)/build.prop: framework/build.prop $(HTC_DIR)/build.prop
	@mkdir -p $(dir $@)
	cat $^ > $@
$(HSYSTEM_IMAGE): $(DSYSTEM_IMAGE) $(HTC_APPS) $(HTC_FW) $(HTC_BLD)/build.prop mk/topic-userspace.mk
	python3 tools/make_system_image.py $@ \
	    --add $(DFW)/core.jar:system/framework/core.jar \
	    $(foreach j,$(FW_BOOT_JARS),--add $(DFW)/$(j).jar:system/framework/$(j).jar) \
	    --add $(ICU)/stubdata/icudt51l-default.dat:system/usr/icu/icudt51l.dat \
	    $(htc_system_add) $(addprefix --add ,$(DSYSTEM_EXTRA)) \
	    --add $(HTC_BLD)/build.prop:system/build.prop \
	    $(foreach a,$(HTC_APPS),--add $(a):system/app/$(notdir $(a))) \
	    $(foreach f,$(HTC_FW),--add $(f):system/framework/$(notdir $(f)))
# Under Node, /data in memory (shim/opfs/memory_opfs.js): Prism's databases;
# memory grows as the PWA's does (run it with -Xmx256m, as the page does).
$(HTC_BLD)/dalvikvm.js: $(DVM_MAIN_OBJ) $(DVM_VFS_OBJ) $(KERNEL_OBJ) $(DALVIK_LIBS) \
                        $(BUILD)/libposix_translation.a $(HSYSTEM_IMAGE) $(OPFS_JS) $(CLOCK_JS) $(DVM_PRE_JS) \
                        shim/opfs/memory_opfs.js
	@mkdir -p $(dir $@)
	$(EMXX) $(subst $(DSYSTEM_IMAGE)@,$(HSYSTEM_IMAGE)@,$(DVM_LDFLAGS)) -sENVIRONMENT=node,worker \
	    --pre-js shim/opfs/memory_opfs.js -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB \
	    $(DVM_MAIN_OBJ) $(DVM_VFS_OBJ) $(KERNEL_OBJ) \
	    -Wl,--start-group $(DALVIK_LIBS) $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
.PHONY: dalvik-htc
dalvik-htc: $(HTC_BLD)/dalvikvm.js

# The PWA's third system (mk/topic-pwa.mk): graphics-web's dalvikvm with this
# image.
PWAHTC_FILES := htc/dalvikvm.js htc/dalvikvm.wasm htc/dalvikvm.data
PWA_FILES += $(PWAHTC_FILES)
$(PWA)/sw.js: $(addprefix $(PWA)/,$(PWAHTC_FILES))
pwa-web: $(addprefix $(PWA)/,$(PWAHTC_FILES))
$(PWA)/htc/dalvikvm.js: $(DVM_MAIN_OBJ) $(DVM_VFS_OBJ) $(KERNEL_OBJ) $(GFB_GRALLOC_OBJ) $(GWEB_LIBS) \
                        $(BUILD)/libposix_translation.a $(HSYSTEM_IMAGE) $(OPFS_JS) $(CLOCK_JS) \
                        shim/opfs/memory_opfs.js shim/opfs/idb_opfs.js shim/memory/fit_memory.js \
                        shim/gles/remote_webgl.js mk/topic-pwa.mk
	@mkdir -p $(dir $@)
	$(EMXX) $(subst $(DSYSTEM_IMAGE)@,$(HSYSTEM_IMAGE)@,$(GWEB_LDFLAGS)) \
	    $(DVM_MAIN_OBJ) $(DVM_VFS_OBJ) $(KERNEL_OBJ) $(GFB_GRALLOC_OBJ) \
	    -Wl,--start-group $(GWEB_LIBS) $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
$(PWA)/htc/dalvikvm.wasm $(PWA)/htc/dalvikvm.data: $(PWA)/htc/dalvikvm.js ;
endif
