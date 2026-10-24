# Topic "window": real Android windows on the page. See docs/topics/window.md.
#
#   shim/window   the in-process SurfaceFlinger (layers, transactions, CPU
#                 composition into the display of shim/graphics/framebuffer.h,
#                 software vsync), the JNI of android.view.{Surface,
#                 SurfaceControl,SurfaceSession} over it, libgui's
#                 DisplayEventReceiver over it, registration and start of the
#                 Java services
#   AOSP          android_view_DisplayEventReceiver.cpp (core/jni), BitTube
#                 and IDisplayEventConnection (libgui), Rect (libui)
#   Java          framework/java-arcwasm/arcwasm/server: WindowManagerService
#                 (IWindowManager), Session (IWindowSession), WindowState,
#                 DisplayManagerService, InputMethodManagerService (compiled
#                 into framework2.jar with the rest of framework/java-arcwasm)
#   android.policy.jar   frameworks/base/policy (PolicyManager's Policy:
#                 PhoneWindow, PhoneLayoutInflater, PhoneFallbackEventHandler),
#                 on the boot class path
#
# Included by the Makefile after mk/dalvik.mk and the earlier topics (input).

LIBGUI := $(A44)/frameworks/native/libs/gui
WINDOW_SRCS := shim/window/surface_flinger.cc shim/window/android_view_Surface.cc \
               shim/window/display_event_receiver.cc shim/window/register_window.cc \
               $(FWB)/core/jni/android_view_DisplayEventReceiver.cpp \
               $(LIBGUI)/BitTube.cpp $(LIBGUI)/IDisplayEventConnection.cpp \
               $(A44)/frameworks/native/libs/ui/Rect.cpp
# Skia and GraphicsJNI for the Canvas swap in Surface.lockCanvas, libbinder
# for Parcel and the display token, libinput for InputChannel.
WINDOW_XFLAGS := $(GJNI_XFLAGS) $(BINDER_INC) -I$(A44)/hardware/libhardware/include -Ishim/window
$(call dobj,$(WINDOW_SRCS)): XFLAGS := $(WINDOW_XFLAGS)
$(DOBJ)/$(FWB)/core/jni/android_view_DisplayEventReceiver.cpp.o: \
    XFLAGS := $(WINDOW_XFLAGS) -DLOG_TAG=\"DisplayEventReceiver\"
$(BUILD)/libwindow.a: $(call dobj,$(WINDOW_SRCS))
	@rm -f $@ && $(EMAR) rcs $@ $^

WINDOW_LIBS := $(BUILD)/libwindow.a
DALVIK_LIBS += $(WINDOW_LIBS)
GWEB_LIBS += $(WINDOW_LIBS)
$(BUILD)/dalvik/dalvikvm.js $(DWEB)/dalvikvm.js $(GWEB)/dalvikvm.js: $(WINDOW_LIBS)

# --- android.policy.jar --------------------------------------------------------------
# As frameworks/base/policy/Android.mk: all of policy/src, compiled against
# the framework; one classes.dex like the other boot jars. Topic "activity"
# builds the same jar (POLICY_JAR, mk/topic-activity.mk, included before this
# file): then that rule is the one, and this block steps aside.
ifeq ($(POLICY_JAR),)
POLICY_SRCS := $(shell find $(FWB)/policy/src -name '*.java')
POLICY_W := $(DFW)/android.policy-java
$(DFW)/android.policy.jar: $(POLICY_SRCS) $(DFW)/framework.jar mk/topic-window.mk
	rm -rf $(POLICY_W) && mkdir -p $(POLICY_W)/classes $(POLICY_W)/dex
	find $(FWB)/policy/src -name '*.java' | sed 's|.*|"&"|' > $(POLICY_W)/srcs.txt
	javac -source 8 -target 8 -Xlint:-options -encoding UTF-8 -nowarn \
	    -bootclasspath $(BOOT_CLASSES) -d $(POLICY_W)/classes @$(POLICY_W)/srcs.txt
	cd $(POLICY_W)/classes && jar cf ../android.policy-classes.jar .
	"$(D8)" --release --min-api 19 --lib $(DFW)/core-java/core-classes.jar \
	    $(addprefix --lib ,$(FW_CLASSES)) \
	    --output $(POLICY_W)/dex $(POLICY_W)/android.policy-classes.jar
	rm -f $@ && cd $(POLICY_W)/dex && zip -q "$(abspath $@)" classes.dex

DSYSTEM_EXTRA += $(DFW)/android.policy.jar:system/framework/android.policy.jar
$(DSYSTEM_IMAGE): $(DFW)/android.policy.jar
endif

# --- window-web: the graphics-web build with the window demo -------------------------
#   make window-web   # build/graphics-web/window.html: WindowDemo (a click
#                     # counter in a window over a full-screen one);
#                     # ?selftest=1 for the scripted DOM check (Safari)
$(GWEB)/window.html: demo/window/index.html
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: window-libs window-test window-web
window-libs: $(WINDOW_LIBS)
window-web: graphics-web $(GWEB)/window.html
window-test: dalvik
	cd $(BUILD)/dalvik && $(NODE) dalvikvm.js -cp /system/framework/arc-tests.jar arcwasm.WindowTest
