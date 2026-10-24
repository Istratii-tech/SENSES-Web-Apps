# Topic tiktok (Android 6 phase 3): TikTok 47.1.4 on ART and the 6.0.1
# framework (docs/topics/tiktok.md). The APK itself is the user's
# (build/extra-apks6/TikTok.apk, never committed); this file only adds the
# topic's natives and test:
#   - SQLite (external/sqlite 6.0.1, android.database.sqlite's JNI) and the
#     natives of SystemSensorManager and NetworkUtils (shim/app6: no sensors,
#     one network), in libandroid_runtime_app6.a;
#   - AppSupportTest.
#
#   make tiktok-test     AppSupportTest on ART in Node

# --- APKs for ART only ------------------------------------------------------------------
# build/extra-apks6/NAME.apk (not committed) goes into ART's /system/app only,
# not into Dalvik's image as build/extra-apks/ does: apps for Android 6 (TikTok:
# 500 MB, which Dalvik's preloaded image cannot hold).
EXTRA_APKS6 := $(wildcard $(BUILD)/extra-apks6/*.apk)
ASYSTEM_ADD += $(foreach a,$(EXTRA_APKS6),--add $(a):system/app/$(notdir $(a)))
$(ASYSTEM_IMAGE): $(EXTRA_APKS6)

# --- SQLite and android.database ---------------------------------------------------------
# The device flags of external/sqlite/dist/Android.mk (pread64 and fdatasync
# as the plain calls). WAL's -shm index is a MAP_SHARED mmap; the kernel
# layer need not share it between processes, as the index lives in one
# unixShmNode per file for all of a process's connections.
TT_SQLITE := $(A60)/external/sqlite
TT_SQLITE_FLAGS := -DNDEBUG=1 -DHAVE_USLEEP=1 -DSQLITE_HAVE_ISNAN \
    -DSQLITE_DEFAULT_JOURNAL_SIZE_LIMIT=1048576 -DSQLITE_THREADSAFE=2 -DSQLITE_TEMP_STORE=3 \
    -DSQLITE_POWERSAFE_OVERWRITE=1 -DSQLITE_DEFAULT_FILE_FORMAT=4 -DSQLITE_DEFAULT_AUTOVACUUM=1 \
    -DSQLITE_ENABLE_MEMORY_MANAGEMENT=1 -DSQLITE_ENABLE_FTS3 -DSQLITE_ENABLE_FTS3_BACKWARDS \
    -DSQLITE_ENABLE_FTS4 -DSQLITE_OMIT_BUILTIN_TEST -DSQLITE_OMIT_COMPILEOPTION_DIAGS \
    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_FILE_PERMISSIONS=0600 -DSQLITE_ENABLE_ICU \
    -include shim/app6/sqlite_config.h -Wno-everything
TT_SQLITE_SRCS := $(TT_SQLITE)/dist/sqlite3.c \
    $(addprefix $(TT_SQLITE)/android/,sqlite3_android.cpp PhoneNumberUtils.cpp OldPhoneNumberUtils.cpp)
$(call f6obj,$(TT_SQLITE_SRCS)): mk/topic-tiktok.mk shim/app6/sqlite_config.h
$(call f6obj,$(TT_SQLITE_SRCS)): F6XFLAGS := $(TT_SQLITE_FLAGS) -idirafter $(TT_SQLITE)/dist \
    -I$(TT_SQLITE)/android -DLOG_TAG=\"sqlite3_android\"
TT_JNI_SRCS := $(addprefix $(F6FWB)/core/jni/,android_database_SQLiteConnection.cpp \
    android_database_SQLiteGlobal.cpp android_database_SQLiteDebug.cpp \
    android_database_SQLiteCommon.cpp android_database_CursorWindow.cpp) \
    shim/app6/sensors_net6_jni.cc
$(call f6obj,$(TT_JNI_SRCS)): F6XFLAGS := -idirafter $(TT_SQLITE)/dist -I$(TT_SQLITE)/android
TT_LIB := $(ALIB)/libandroid_runtime_app6.a
$(TT_LIB): $(call f6obj,$(TT_SQLITE_SRCS) $(TT_JNI_SRCS))
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
FW6_JNI_LIBS += $(TT_LIB)
$(ABLD)/artvm.js $(AWEB)/artvm.js $(FGWEB)/artvm.js: $(TT_LIB)

# --- tests on ART ----------------------------------------------------------------------
# AppSupportTest (tests/java6): class initialization across threads, the stub
# WebView provider, package queries before the Application exists.
TT_TEST_SRCS := tests/java6/arcwasm/AppSupportTest.java
# (WindowEnv, ViewTest: the test APK's resources and a Context; already in the jar.)
ART_JAVA_TESTS += $(TT_TEST_SRCS)
$(AFW)/arc-tests.jar: $(TT_TEST_SRCS)

.PHONY: tiktok-test
tiktok-test: art
	VM=art tools/run_java_tests.sh node AppSupportTest

# --- artvm --watchdog=SECONDS (art/watchdog.cc) -----------------------------------------
# What each ART thread is doing, every SECONDS, written past the kernel: for
# hangs, where the dump of SystemServer --dump cannot run any more.
TT_WATCHDOG_OBJ := $(call aobj,art/watchdog.cc)
$(TT_WATCHDOG_OBJ): AFLAGS := $(ART_FLAGS)
ART_MAIN_OBJ += $(TT_WATCHDOG_OBJ)
$(ABLD)/artvm.js $(AWEB)/artvm.js $(FGWEB)/artvm.js: $(TT_WATCHDOG_OBJ)

# --- which guest code a wasm trap came from (shim/app6/guest_trap_report.js) ------------
ART_LDFLAGS += --pre-js shim/app6/guest_trap_report.js   # (FGWEB_LDFLAGS derives from it)
$(ABLD)/artvm.js $(AWEB)/artvm.js $(FGWEB)/artvm.js: shim/app6/guest_trap_report.js

# --- /proc/self/ (shim/app6/proc_self.cc), mounted by art/vfs_setup.cc ---------------------
TT_PROC_OBJ := $(call obj,shim/app6/proc_self.cc)
ART_VFS_OBJ += $(TT_PROC_OBJ)
$(ABLD)/artvm.js $(AWEB)/artvm.js $(FGWEB)/artvm.js: $(TT_PROC_OBJ)
