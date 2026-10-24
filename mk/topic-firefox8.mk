# Topic firefox8: Firefox 158 (org.mozilla.firefox, build/extra-apks8, not
# committed) on Android 8.1. docs/topics/firefox8.md.
#
# apps8/data-local-tmp/*: files SystemServer copies to /data/local/tmp at start
# ("adb push"), e.g. GeckoView's debug configuration.
FF8_DATA_LOCAL_TMP := $(wildcard apps8/data-local-tmp/*)
ifneq ($(A8LIB),)
ASYSTEM8_ADD += $(foreach f,$(FF8_DATA_LOCAL_TMP),--add $(f):system/etc/arcwasm/data-local-tmp/$(notdir $(f)))
$(A8SYSTEM_IMAGE): $(FF8_DATA_LOCAL_TMP)
endif
# (and on Android 10: the same configuration, topic a10-default -- without it
# GeckoView 10 starts its GPU process, which cannot run in the app's process)
ASYSTEM10_ADD += $(foreach f,$(FF8_DATA_LOCAL_TMP),--add $(f):system/etc/arcwasm/data-local-tmp/$(notdir $(f)))
$(A10SYSTEM_IMAGE): $(FF8_DATA_LOCAL_TMP)

# A wasm trap in a thread that runs ARM code reports the guest block, registers
# and last host calls (topic tiktok's report, also on ART 8.1).
ART8_LDFLAGS += --pre-js shim/app6/guest_trap_report.js
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js: shim/app6/guest_trap_report.js

# fw8-graphics-web's artvm8 (the pages' VM) relinks when the NDK bridge or the
# ARM CPU changes (ART8_LINK_DEPS was expanded before topics added their libs).
$(BUILD)/fw8-graphics-web/artvm8.js: $(BUILD)/libndk_cpu.a $(BUILD)/fw8s/lib/libndk_bridge_art8.a \
                                     shim/app6/guest_trap_report.js

# A local page for the browser (demo/firefox8): served by tools/serve.py as
# /firefox8-web/hello.html.
FF8WEB := $(BUILD)/firefox8-web
$(FF8WEB)/%.html: demo/firefox8/%.html
	@mkdir -p $(dir $@)
	cp $< $@
firefox8-web: $(FF8WEB)/hello.html $(FF8WEB)/colors.html $(FF8WEB)/anim.html $(FF8WEB)/jsbench.html
.PHONY: firefox8-web
