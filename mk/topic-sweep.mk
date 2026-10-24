# Topic "apps-sweep" (docs/topics/apps-sweep.md): tools/app_sweep.py starts
# many real apps headless in Node and sorts out what happens; here what the
# sweep found missing in the shared runtime, and its smoke run.
#
#   make app-sweep-smoke   # a fixed few apps (tools/app_sweep_smoke.txt) on
#                          # each stack: each must reach what the list says
#
# (topic-sweep.mk, not topic-apps-sweep.mk: included after topic-art8/art10,
# whose ART8_LIBS/ART10_LIBS it adds to.)

SWB := $(BUILD)/sweep

# --- media natives the framework's classes load (ART 8.1 and 10) ---------------------------
# shim/sweep/soundpool_jni.cc: SoundPool, silent, loads reported.
# shim/sweep/audioeffect_jni.cc: AudioEffect and Visualizer, no effects.
# Libraries linked into the VMs (they add themselves to the tables of static
# JNI libraries); on 8.1 System.loadLibrary looks for the file first.
SW_JNI_SRCS := shim/sweep/soundpool_jni.cc shim/sweep/audioeffect_jni.cc
SW_JNI_LIBS := libsoundpool.so libaudioeffect_jni.so
$(SWB)/empty.so:
	@mkdir -p $(dir $@)
	@: > $@

ifneq ($(A8LIB),)
SW8_OBJS := $(call a8obj,$(SW_JNI_SRCS))
$(SW8_OBJS): A8FLAGS := $(A8_SYS_INCLUDES)
$(SWB)/libsweep8.a: $(SW8_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART8_LIBS += -Wl,--whole-archive $(SWB)/libsweep8.a -Wl,--no-whole-archive
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js $(G8WEB)/artvm8.js: $(SWB)/libsweep8.a
ASYSTEM8_ADD += $(foreach l,$(SW_JNI_LIBS),--add $(SWB)/empty.so:system/lib/$(l))
$(A8SYSTEM_IMAGE): $(SWB)/empty.so mk/topic-sweep.mk
endif

ifneq ($(A10OBJ),)
SW10_OBJS := $(call a10obj,$(SW_JNI_SRCS))
$(SW10_OBJS): A10FLAGS := $(A10_SYS_INCLUDES)
$(SWB)/libsweep10.a: $(SW10_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART10_LIBS += -Wl,--whole-archive $(SWB)/libsweep10.a -Wl,--no-whole-archive
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js $(G10WEB)/artvm10.js: $(SWB)/libsweep10.a
endif

# --- the smoke run --------------------------------------------------------------------------
# The APKs are not in git: SWEEP_APKS is where tools/app_sweep_smoke.txt's
# files are (a missing one is skipped).
SWEEP_APKS ?= $(ARC_WORKSPACE)
.PHONY: app-sweep-smoke
app-sweep-smoke: dalvik art art8 art10
	python3 -I tools/app_sweep.py --smoke tools/app_sweep_smoke.txt \
	    --out $(BUILD)/app-sweep/smoke -j 2 "$(SWEEP_APKS)"
