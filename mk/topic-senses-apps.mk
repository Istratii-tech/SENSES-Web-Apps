# --- senses-apps-web: SENSES Web Apps, the library of Android apps (topic senses-shell) ---
#   make senses-apps-web   # build/senses-apps/: a static site; open its index.html over
#                          # HTTPS (or http://localhost), e.g. tools/serve.py 8231 build/senses-apps
# A PWA (demo/apps, docs/topics/senses-shell.md): the installed apps, APKs installed
# from Files (read in the browser: demo/apps/apk.js), each app on its system,
# full screen, offline. The systems, one folder each (also the names of their
# /data in the library, senses/data/<id>):
#   a81/  Android 8.1 on ART 8.1 (the default)
#   a10/  Android 10 on ART 10 (apps with minSdk 28-29 and arm64-only ones)
#   a60/  Android 6.0 on ART 6.0.1
#   a44/  Android 4.4 on Dalvik
# (copies of build/senses-apps/vm/*, topic install8)
# SENSES_STACKS picks which go into the build (stacks.json tells the page).
# Each app on an origin of its own (topic per-app-origin, docs/topics/per-app-origin.md):
# an origins.json beside index.html on the host lists them ({"ports": [...]} or
# {"hosts": "app{n}.example.com", "count": N}); not built here (tools/serve.py
# makes one for ARC_APP_PORTS).
# The service worker's cache version is the hash of the shell's files; each
# system's cache is keyed by its own files, so a new shell does not make
# phones download the systems again.
SAPPS := $(BUILD)/senses-apps
SENSES_STACKS ?= a81 a10 a60 a44
SAPPS_PAGE := index.html run.html memtest.html origin.html manifest.webmanifest senses.css senses.js apk.js install.js \
              google.js files.js icon.js store.js store-worker.js origins.js senses-mask.png icon-180.png icon-192.png icon-512.png \
              icon-maskable-512.png
SAPPS_SHELL := $(SAPPS_PAGE) idb_opfs.js report.js mem_plan.js stacks.json
SAPPS_A81 := a81/artvm8.js a81/artvm8.wasm a81/artvm8.data
SAPPS_A10 := a10/artvm10.js a10/artvm10.wasm a10/artvm10.data
# Tint (SPIR-V -> WGSL, docs/topics/vulkan.md) next to the 8.1 and 10 VMs, when
# tools/vulkan/build_tint.sh made it: without it Vulkan on WebGPU (Safari 26,
# iOS 26) has a device but no shaders. (mk/topic-vulkan.mk copies it into $(SAVM).)
SAPPS_TINT := $(notdir $(wildcard $(BUILD)/vulkan/tint/arc_tint.js $(BUILD)/vulkan/tint/arc_tint.wasm))
SAPPS_A81 += $(addprefix a81/,$(SAPPS_TINT))
SAPPS_A10 += $(addprefix a10/,$(SAPPS_TINT))
SAPPS_A60 := a60/artvm.js a60/artvm.wasm a60/artvm.data
SAPPS_A44 := a44/dalvikvm.js a44/dalvikvm.wasm a44/dalvikvm.data
SAPPS_SYS_FILES := $(if $(filter a81,$(SENSES_STACKS)),$(SAPPS_A81)) \
                   $(if $(filter a10,$(SENSES_STACKS)),$(SAPPS_A10)) \
                   $(if $(filter a60,$(SENSES_STACKS)),$(SAPPS_A60)) \
                   $(if $(filter a44,$(SENSES_STACKS)),$(SAPPS_A44))

$(addprefix $(SAPPS)/,$(SAPPS_PAGE)): $(SAPPS)/%: demo/apps/%
	@mkdir -p $(dir $@)
	cp $< $@
# The library in IndexedDB where OPFS cannot write: the same store as the VM's
# /data there (the worker imports it).
$(SAPPS)/idb_opfs.js: shim/opfs/idb_opfs.js
	@mkdir -p $(dir $@)
	cp $< $@
$(SAPPS)/report.js: shim/report.js
	@mkdir -p $(dir $@)
	cp $< $@
# The VM's memory plan (shim/membudget/mem_plan.js): run.html makes the VM's
# memory with the ceiling the VM will plan (topic device-mem-budget).
$(SAPPS)/mem_plan.js: shim/membudget/mem_plan.js
	@mkdir -p $(dir $@)
	cp $< $@
$(SAPPS)/stacks.json: mk/topic-senses-apps.mk FORCE_SAPPS_STACKS
	@mkdir -p $(dir $@)
	@printf '{"stacks": [%s]}\n' "$$(printf '"%s",' $(SENSES_STACKS) | sed 's/,$$//')" > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@; rm -f $@.tmp
.PHONY: FORCE_SAPPS_STACKS

# The systems: topic install8's VM builds for this shell (mk/topic-senses-apps-vm.mk:
# the phone switches, /system images without system apps; the apps come from the
# library, /mnt/apps).
$(addprefix $(SAPPS)/,$(SAPPS_A81) $(SAPPS_A10) $(SAPPS_A60) $(SAPPS_A44)): $(SAPPS)/%: $(SAVM)/%
	@mkdir -p $(dir $@)
	cp $< $@

# The worker's versions: the shell's hash, and per system the hash of its
# script and wasm (the script carries the data package's size and id).
$(SAPPS)/sw.js: demo/apps/sw.js $(addprefix $(SAPPS)/,$(SAPPS_SHELL) $(SAPPS_SYS_FILES))
	sed -e "s/__VERSION__/$$(cat $(addprefix $(SAPPS)/,$(SAPPS_SHELL)) demo/apps/sw.js | shasum -a 256 | cut -c1-12)/" \
	    -e "s/__SYSTEMS__/$$(for s in $(SENSES_STACKS); do printf '%s:%s ' $$s \
	        $$(cat $(SAPPS)/$$s/*.js $(SAPPS)/$$s/*.wasm | shasum -a 256 | cut -c1-12); done)/" $< > $@

.PHONY: senses-apps-web
senses-apps-web: $(addprefix $(SAPPS)/,$(SAPPS_SHELL) $(SAPPS_SYS_FILES)) $(SAPPS)/sw.js

# Files between apps and the phone, the page's side (topic app-files):
# demo/apps/files.js and the store worker's file operations under Node.
.PHONY: senses-apps-files-test
senses-apps-files-test:
	node tests/apps/files_test.js
