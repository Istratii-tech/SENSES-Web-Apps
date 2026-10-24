# The milestone page: an APK's launcher Activity started by the in-process
# system_server, on the page and clickable (graphics-web/app.html).
#   make app-web   # then open http://localhost:8125/graphics-web/app.html
#                  # (?selftest=1 taps the test app's button three times)
# Paths are spelled out: topic makefiles are included in name order, so
# GWEB (mk/topic-graphics.mk) is not defined yet here.
$(BUILD)/graphics-web/app.html: demo/app/index.html
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: app-web
app-web: graphics-web $(BUILD)/graphics-web/app.html

# Extra APKs for experiments: every build/extra-apks/NAME.apk goes into the
# /system image as system/app/NAME.apk, where PackageManagerService scans
# it (not committed; e.g. third-party apps under test). Relink after
# adding or removing one: make -B $(DSYSTEM_IMAGE) dalvik.
EXTRA_APKS := $(wildcard $(BUILD)/extra-apks/*.apk)
DSYSTEM_ADD += $(foreach a,$(EXTRA_APKS),--add $(a):system/app/$(notdir $(a)))
$(BUILD)/dalvik/system.img: $(EXTRA_APKS)
