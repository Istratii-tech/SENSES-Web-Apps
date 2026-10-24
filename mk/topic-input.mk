# Topic "input": browser input as Android input events. See docs/topics/input.md.
#
#   libinput      frameworks/native/libs/input (Input, InputTransport with
#                 InputChannel on posix_translation's unix socketpairs,
#                 KeyCharacterMap/KeyLayoutMap/VirtualKeyMap, VelocityTracker)
#   JNI           android_view_{InputChannel,InputEventReceiver,InputEventSender,
#                 MotionEvent,KeyEvent,KeyCharacterMap,InputDevice}.cpp
#   shim/input    the browser's input devices: a queue the page's main thread
#                 fills without blocking, a dispatcher thread publishing on the
#                 registered InputChannel (browser_input.cc), arcwasm.input
#                 natives and registration (register_input.cc), and the page
#                 side (browser_input.js, a --pre-js of the browser builds)
#   /system       usr/keylayout/Generic.kl, usr/keychars/{Generic,Virtual}.kcm
#
# Included by the Makefile after mk/dalvik.mk and the earlier topics.

LIBINPUT := $(A44)/frameworks/native/libs/input
# Android.mk's deviceSources (its commonSources plus the transport).
LIBINPUT_SRCS := $(addprefix $(LIBINPUT)/,Input.cpp InputDevice.cpp Keyboard.cpp \
                   KeyCharacterMap.cpp KeyLayoutMap.cpp VirtualKeyMap.cpp \
                   InputTransport.cpp VelocityControl.cpp VelocityTracker.cpp)
# ARC_WASM (in D_COMMON) turns on the Parcel code of Input.cpp and
# KeyCharacterMap.cpp, which is under HAVE_ANDROID_OS (patches/aosp44/input.patch).
$(DOBJ)/$(LIBINPUT)/%: XFLAGS := $(FW_INCLUDES) $(BINDER_INC) -DLOG_TAG=\"Input\"
$(BUILD)/libinput.a: $(call dobj,$(LIBINPUT_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- JNI (frameworks/base/core/jni) and shim/input --------------------------------
INPUT_JNI_SRCS := $(addprefix $(FWB)/core/jni/,android_view_InputChannel.cpp \
                    android_view_InputEventReceiver.cpp android_view_InputEventSender.cpp \
                    android_view_MotionEvent.cpp android_view_KeyEvent.cpp \
                    android_view_KeyCharacterMap.cpp android_view_InputDevice.cpp) \
                  shim/input/browser_input.cc shim/input/register_input.cc
# MotionEvent.transform takes an android.graphics.Matrix (Skia's SkMatrix).
INPUT_JNI_XFLAGS := $(GJNI_XFLAGS) $(BINDER_INC) -DLOG_TAG=\"InputJNI\"
$(call dobj,$(INPUT_JNI_SRCS)): XFLAGS := $(INPUT_JNI_XFLAGS)
$(BUILD)/libinput_jni.a: $(call dobj,$(INPUT_JNI_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

INPUT_LIBS := $(BUILD)/libinput_jni.a $(BUILD)/libinput.a
DALVIK_LIBS += $(INPUT_LIBS)
GWEB_LIBS += $(INPUT_LIBS)
$(BUILD)/dalvik/dalvikvm.js $(DWEB)/dalvikvm.js $(GWEB)/dalvikvm.js: $(INPUT_LIBS)

# The page side: listens to the canvas (Module.canvas) and queues DOM events
# into the dispatcher; in every browser build (a page without a canvas just
# does not attach it).
INPUT_PRE_JS := shim/input/browser_input.js
$(DWEB)/dalvikvm.js $(GWEB)/dalvikvm.js: $(INPUT_PRE_JS) mk/topic-input.mk
$(DWEB)/dalvikvm.js: DVM_LDFLAGS += --pre-js $(INPUT_PRE_JS)
GWEB_LDFLAGS += --pre-js $(INPUT_PRE_JS)

# /system: the key layout and key character maps of the browser keyboard
# and of the virtual keyboard.
KEYBOARDS := $(FWB)/data/keyboards
DSYSTEM_EXTRA += $(KEYBOARDS)/Generic.kl:system/usr/keylayout/Generic.kl \
                 $(KEYBOARDS)/Generic.kcm:system/usr/keychars/Generic.kcm \
                 $(KEYBOARDS)/Virtual.kcm:system/usr/keychars/Virtual.kcm
$(DSYSTEM_IMAGE): $(KEYBOARDS)/Generic.kl $(KEYBOARDS)/Generic.kcm $(KEYBOARDS)/Virtual.kcm

# --- input-web: the graphics-web build with an input page ---------------------------
#   make input-web   # build/graphics-web/input.html: InputDemo (dots under the
#                    # pointers, typed text); ?selftest=1 for the scripted DOM check
$(GWEB)/input.html: demo/input/index.html
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: input-libs input-test input-web
input-libs: $(INPUT_LIBS)
input-web: graphics-web $(GWEB)/input.html
input-test: dalvik
	cd $(BUILD)/dalvik && $(NODE) dalvikvm.js -cp /system/framework/arc-tests.jar arcwasm.InputTest
