# Topic "senses-a10": topic webview8's real WebView on the Android 10 stack
# (docs/topics/senses-a10.md, docs/topics/webview8.md).
#
#   framework/java-arcwasm10/arcwasm/webview  HostWebView on 10's WebViewProvider (API 29:
#                                             the renderer process calls), PageLoader,
#                                             Geometry, HostBridge: in services.jar 10
#                                             (tools/build_services_jar.sh services10 takes
#                                             the whole directory)
#   shim/webview/webview_jni.cc              HostBridge's natives, here in artvm10's table
#                                             of static JNI libraries
#   shim/webview/webview_host.js             the page side, a --pre-js of the 10 page builds
#                                             (fw10-graphics-web, SENSES Web Apps' a10)
#
#   VM=art10 tools/run_java_tests.sh node WebViewTest         # "no host WebView": the stub
#   VM=art10 PAGE=fw10-graphics-web tools/run_java_tests.sh safari WebViewTest
#
# Included after mk/topic-fw10-*.mk and mk/topic-senses-apps-vm.mk (alphabetical
# order), before mk/topic-webview8.mk (its test pages).

ifneq ($(G10WEB),)
WV10 := $(BUILD)/webview10
WV10_OBJS := $(WV10)/obj/shim/webview/webview_jni.cc.o
$(WV10)/obj/%.cc.o: %.cc mk/topic-webview10.mk
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(F10_CXXFLAGS) $(F10S_FLAGS) -c $< -o $@
$(WV10)/libarcwasm_webview.a: $(WV10_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART10_LIBS += -Wl,--whole-archive $(WV10)/libarcwasm_webview.a -Wl,--no-whole-archive
$(A10BLD)/artvm10.js $(A10WEB)/artvm10.js $(G10WEB)/artvm10.js $(SAVM)/a10/artvm10.js: \
    $(WV10)/libarcwasm_webview.a
# System.load wants a file in /system/lib (an empty one; the library is linked in).
ASYSTEM10_ADD += --add $(F10S)/empty.so:system/lib/libarcwasm_webview.so

# The page builds: the host.
G10WEB_LDFLAGS += --pre-js shim/webview/webview_host.js
$(G10WEB)/artvm10.js $(SAVM)/a10/artvm10.js: shim/webview/webview_host.js

# WebViewTest (tests/java8: no 8.1-only API) on ART 10, with its pages.
# (WindowEnv: a window on 10's WindowServices, as ViewTest's.)
ART10_JAVA_TESTS += tests/java8/arcwasm/WebViewTest.java tests/java/arcwasm/WindowEnv.java
$(A10FW)/arc-tests.jar: tests/java8/arcwasm/WebViewTest.java tests/java/arcwasm/WindowEnv.java $(addprefix $(BUILD)/webview-test/,$(notdir \
    $(wildcard tests/webview/*)))
endif
