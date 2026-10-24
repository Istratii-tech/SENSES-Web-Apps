# Topic "binder": Binder IPC inside the one WebAssembly process, as ARC did
# it. See docs/topics/binder.md.
#
#   /dev/binder   shim/binder/dev_binder.cc: the binder driver in
#                 posix_translation's VFS (mounted by dalvik/launcher/vfs_setup.cc)
#   libbinder     frameworks/native/libs/binder (ProcessState, IPCThreadState,
#                 Parcel, BpBinder/BBinder, IServiceManager, ...)
#   servicemanager  frameworks/native/cmds/servicemanager on its own thread
#                 as a separate binder process (shim/binder/servicemanager_main.c)
#   JNI           android_util_Binder.cpp, android_os_Parcel.cpp,
#                 android_util_EventLog.cpp (frameworks/base/core/jni) and
#                 shim/binder/binder_runtime.cc (servicemanager, Java thread
#                 hook, binder thread pool); libcutils' threads.c (thread_store
#                 for libbinder's BufferedTextOutput)
#   test natives  tests/native/binder_test_jni.cc for arcwasm.BinderTest
#
# Included by the Makefile after mk/dalvik.mk and mk/framework.mk.

BINDER_INC := -Ishim/binder/include
LIBBINDER  := $(A44)/frameworks/native/libs/binder
SVCMGR     := $(A44)/frameworks/native/cmds/servicemanager

# --- the driver (posix_translation side, its flags) ----------------------------
DEV_BINDER_OBJ := $(BUILD)/obj/shim/binder/dev_binder.o
$(DEV_BINDER_OBJ): shim/binder/dev_binder.cc shim/binder/dev_binder.h \
                   shim/binder/include/arc_binder.h shim/binder/include/linux/binder.h
	@mkdir -p $(dir $@)
	$(EMXX) $(CXXFLAGS) $(BINDER_INC) -c $< -o $@
$(BUILD)/libdev_binder.a: $(DEV_BINDER_OBJ)
	@rm -f $@ && $(EMAR) rcs $@ $^
# vfs_setup.cc includes shim/binder/dev_binder.h.
$(DVM_VFS_OBJ): shim/binder/dev_binder.h

# --- libbinder -------------------------------------------------------------------
LIBBINDER_SRCS := $(shell tools/mk_srcs.py $(LIBBINDER)/Android.mk sources $(LIBBINDER)/)
# LOG_NDEBUG: without it libbinder logs every command it exchanges.
$(DOBJ)/$(LIBBINDER)/%: XFLAGS := $(FW_INCLUDES) $(BINDER_INC) -DLOG_NDEBUG=1
$(BUILD)/libbinder.a: $(call dobj,$(LIBBINDER_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- servicemanager ----------------------------------------------------------------
SVCMGR_SRCS := $(SVCMGR)/service_manager.c $(SVCMGR)/binder.c shim/binder/servicemanager_main.c
# binder.c relies on string.h coming in through bionic headers.
$(DOBJ)/$(SVCMGR)/%: XFLAGS := $(D_INCLUDES) $(BINDER_INC) -include string.h \
    -Dmain=arc_servicemanager_unused_main
$(DOBJ)/shim/binder/servicemanager_main.c.o: XFLAGS := $(D_INCLUDES) $(BINDER_INC) -I$(SVCMGR)
$(BUILD)/libservicemanager.a: $(call dobj,$(SVCMGR_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- JNI: android.os.Binder/Parcel, android.util.EventLog -------------------------
BINDER_JNI_SRCS := $(addprefix $(FWB)/core/jni/,android_util_Binder.cpp android_os_Parcel.cpp \
                     android_util_EventLog.cpp) \
                   shim/binder/binder_runtime.cc shim/binder/liblog_events.cc \
                   $(A44)/system/core/libcutils/threads.c
$(call dobj,$(addprefix $(FWB)/core/jni/,android_util_Binder.cpp android_os_Parcel.cpp)): \
    XFLAGS := $(FW_INCLUDES) $(BINDER_INC) -DLOG_NDEBUG=1
$(DOBJ)/shim/binder/binder_runtime.cc.o $(DOBJ)/shim/binder/liblog_events.cc.o: \
    XFLAGS := $(FW_INCLUDES) $(BINDER_INC) -DLOG_TAG=\"AndroidRuntime\"
$(BUILD)/libbinder_jni.a: $(call dobj,$(BINDER_JNI_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- natives of arcwasm.BinderTest (System.load of a static library) --------------
BINDER_TEST_SRCS := tests/native/binder_test_jni.cc
$(DOBJ)/tests/native/binder_test_jni.cc.o: XFLAGS := $(FW_INCLUDES) $(BINDER_INC) -I$(SVCMGR) \
    -DLOG_TAG=\"BinderTest\"
$(BUILD)/libarcwasm_binder_test.a: $(call dobj,$(BINDER_TEST_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

BINDER_LIBS := $(BUILD)/libbinder_jni.a $(BUILD)/libbinder.a $(BUILD)/libservicemanager.a \
               $(BUILD)/libarcwasm_binder_test.a $(BUILD)/libdev_binder.a
# Linked into dalvikvm with the rest (inside --start-group/--end-group).
DALVIK_LIBS += $(BINDER_LIBS)
$(BUILD)/dalvik/dalvikvm.js $(DWEB)/dalvikvm.js: $(BINDER_LIBS)

.PHONY: binder-libs binder-test
binder-libs: $(BINDER_LIBS)
binder-test: dalvik
	cd $(BUILD)/dalvik && $(NODE) dalvikvm.js -cp /system/framework/arc-tests.jar arcwasm.BinderTest
