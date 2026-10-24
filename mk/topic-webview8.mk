# Topic "webview8": a real WebView on the Android 8.1 stack (docs/topics/webview8.md).
#
#   framework/java-arcwasm8/arcwasm/webview   HostWebView (WebViewProvider), PageLoader,
#                                             Geometry, HostBridge: in services.jar
#                                             (tools/build_services_jar.sh services8 takes
#                                             the whole directory)
#   shim/webview/webview_jni.cc              HostBridge's natives, the static JNI library
#                                             libarcwasm_webview.so of every artvm8
#   shim/webview/webview_host.js             the page side: frames over the display, a
#                                             --pre-js of the page builds (fw8-graphics-web,
#                                             SENSES Web Apps' a81)
#   tests/webview/                           WebViewTest's pages, served as build/webview-test/
#
#   make webview8-test-pages
#   VM=art8 tools/run_java_tests.sh node WebViewTest      # "no host WebView": the stub
#   VM=art8 PAGE=fw8-graphics-web tools/run_java_tests.sh safari WebViewTest
#
# Included after mk/topic-fw8-graphics.mk, mk/topic-fw8-system.mk and
# mk/topic-senses-apps-vm.mk (alphabetical order): F8S*, G8WEB*, SAVM are defined.

ifneq ($(A8LIB),)
WV8 := $(BUILD)/webview8
WV8_SRCS := shim/webview/webview_jni.cc
WV8_OBJS := $(patsubst %,$(WV8)/obj/%.o,$(WV8_SRCS))
$(WV8)/obj/%.cc.o: %.cc mk/topic-webview8.mk
	@mkdir -p $(dir $@)
	@echo "  CXX  $<"
	@$(EMXX) $(F8_CXXFLAGS) $(F8S_FLAGS) -c $< -o $@
$(WV8)/libarcwasm_webview.a: $(WV8_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART8_LIBS += -Wl,--whole-archive $(WV8)/libarcwasm_webview.a -Wl,--no-whole-archive
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js $(G8WEB)/artvm8.js: $(WV8)/libarcwasm_webview.a
# System.load wants a file in /system/lib (an empty one; the library is linked in).
ASYSTEM8_ADD += --add $(F8S)/empty.so:system/lib/libarcwasm_webview.so

# The page builds: the host.
G8WEB_LDFLAGS += --pre-js shim/webview/webview_host.js
$(G8WEB)/artvm8.js: shim/webview/webview_host.js
$(SAVM)/a81/artvm8.js: shim/webview/webview_host.js $(WV8)/libarcwasm_webview.a

# WebViewTest's pages (same origin as the test page in a browser).
WV8_PAGES := $(notdir $(wildcard tests/webview/*))
$(BUILD)/webview-test/%: tests/webview/%
	@mkdir -p $(dir $@)
	cp $< $@
.PHONY: webview8-test-pages
webview8-test-pages: $(addprefix $(BUILD)/webview-test/,$(WV8_PAGES))
$(A8FW)/arc-tests.jar: $(addprefix $(BUILD)/webview-test/,$(WV8_PAGES))
endif
