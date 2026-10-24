# --- pwa-web: the userspace systems as an installable web app (topic userspace) ---
#   make pwa-web   # build/pwa/: a static site; open its index.html over HTTPS
#                  # (or http://localhost), e.g. tools/serve.py 8140 build/pwa
# A phone page (demo/pwa/index.html) that runs, as the user picks:
#   a44/  Android 4.4 on Dalvik: graphics-web (Launcher2 + SystemUI from the
#         stock 4.4 apps, build/system-apks44)
#   a60/  Android 6.0 on ART: artvm linked for phones (below; Launcher3 and
#         the 6.0 apps, build/system-apks6)
#   htc/  HTC Sense 5.5 on Dalvik, in builds with build/system-apkshtc
#         (mk/topic-userspace.mk)
# with a manifest, icons and a service worker that caches the page and the
# picked system for offline use and adds the cross-origin isolation headers
# (docs/topics/userspace.md, "PWA"). The service worker's cache version is the
# hash of the other files, so any rebuilt file makes installed copies update.
PWA := $(BUILD)/pwa
PWA44_FILES := a44/dalvikvm.js a44/dalvikvm.wasm a44/dalvikvm.data
PWA60_FILES := a60/artvm.js a60/artvm.wasm a60/artvm.data
PWA_PAGE_FILES := index.html manifest.webmanifest senses-mask.png
PWA_ICONS := icon-180.png icon-192.png icon-512.png
PWA_FILES := $(PWA44_FILES) $(PWA60_FILES) report.js $(PWA_PAGE_FILES) $(PWA_ICONS)

# dalvikvm.wasm and .data are made by the dalvikvm.js link.
$(addprefix $(PWA)/,$(PWA44_FILES)): $(PWA)/a44/%: $(BUILD)/graphics-web/dalvikvm.js
	@mkdir -p $(dir $@)
	cp $(BUILD)/graphics-web/$* $@
$(PWA)/report.js: $(BUILD)/graphics-web/report.js
	cp $< $@
$(addprefix $(PWA)/,$(PWA_PAGE_FILES)): $(PWA)/%: demo/pwa/%
	@mkdir -p $(dir $@)
	cp $< $@
$(addprefix $(PWA)/,$(PWA_ICONS)): $(PWA)/icon-%.png: tools/make_pwa_icon.py
	@mkdir -p $(dir $@)
	python3 $< $* $@
$(PWA)/sw.js: demo/pwa/sw.js $(addprefix $(PWA)/,$(PWA_FILES))
	sed "s/__VERSION__/$$(cat $(addprefix $(PWA)/,$(PWA_FILES)) | shasum -a 256 | cut -c1-12)/" $< > $@

.PHONY: pwa-web
pwa-web: graphics-web $(addprefix $(PWA)/,$(PWA_FILES)) $(PWA)/sw.js

# Phones (iOS 15.2+), for both systems:
# - the display without WebGL in workers (Safari < 17): frames drawn by the
#   page's main thread (shim/graphics/framebuffer_gralloc.cc), so the canvas
#   is not handed to main()'s thread at startup;
# - /data where OPFS cannot write (Safari < 16.4): the same in IndexedDB,
#   on the device's storage (shim/opfs/idb_opfs.js), which
#   library_arc_opfs.js falls back to; the in-memory OPFS (not kept) only
#   where IndexedDB fails too.
# - memory: as much as the device allows, up to 2 GB (shim/memory/
#   fit_memory.js; the page sizes the Java heap by what it got). On ART it
#   comes with the memory budget's ceiling (mk/topic-mem-budget.mk: iPhones
#   1536 MB).
# - apps' GL threads without WebGL in workers (Safari < 17): their WebGL2
#   runs on the page's main thread (shim/gles/remote_webgl.js).
PWA_PHONE_LDFLAGS := '-sOFFSCREENCANVASES_TO_PTHREAD=""' -sMIN_SAFARI_VERSION=150200 \
                     --pre-js shim/opfs/memory_opfs.js --pre-js shim/opfs/idb_opfs.js \
                     --pre-js shim/gles/remote_webgl.js --js-library shim/gles/library_remote_webgl.js
# The page's catalog of compiled ARM translation packs for new threads
# (topic arm-chains, shim/ndk/jit_catalog.js).
PWA_PHONE_LDFLAGS += --pre-js shim/ndk/jit_catalog.js
GWEB_LDFLAGS += --pre-js shim/opfs/memory_opfs.js --pre-js shim/opfs/idb_opfs.js \
                --pre-js shim/memory/fit_memory.js --pre-js shim/gles/remote_webgl.js \
                -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB
$(BUILD)/graphics-web/dalvikvm.js: shim/opfs/memory_opfs.js shim/opfs/idb_opfs.js \
                                   shim/memory/fit_memory.js shim/gles/remote_webgl.js mk/topic-pwa.mk
# Malloc that reports the damage it finds before it aborts (Emscripten's
# dlmalloc with shim/memory/dlmalloc_report.c, in place of its own): what
# overwrote the heap, where only a phone shows it.
# It is dlmalloc with a record of the memory it never handed out (topic
# wasm-heap-trim: calloc writes no zeros there), generated from Emscripten's
# by tools/dlmalloc_arc.py.
PWA_MALLOC_OBJ := $(BUILD)/pwaobj/dlmalloc_report.o
$(BUILD)/pwaobj/dlmalloc_arc.c: $(EMSDK)/upstream/emscripten/system/lib/dlmalloc.c tools/dlmalloc_arc.py
	@mkdir -p $(dir $@)
	python3 tools/dlmalloc_arc.py $< $@
$(PWA_MALLOC_OBJ): shim/memory/dlmalloc_report.c $(BUILD)/pwaobj/dlmalloc_arc.c mk/topic-pwa.mk
	@mkdir -p $(dir $@)
	$(EMCC) -pthread -O2 -fno-builtin -Wno-unused-but-set-variable -UNDEBUG -DDLMALLOC_DEBUG \
	    -I$(BUILD)/pwaobj -I$(EMSDK)/upstream/emscripten/system/lib -c $< -o $@
GWEB_LDFLAGS += -sMALLOC=none $(PWA_MALLOC_OBJ)
$(BUILD)/graphics-web/dalvikvm.js $(PWA)/htc/dalvikvm.js: $(PWA_MALLOC_OBJ)

# Android 6.0 for phones: fw6-graphics-web's artvm with the phone flags and at
# most 2 GB of wasm memory, less where the device refuses that (fit_memory.js;
# Launcher3 and an app take about 400 MB; the desktop build may grow to 4 GB
# for TikTok). The Java heap's limit follows (demo/pwa/index.html).
$(PWA)/a60/artvm.js: $(ART_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) $(FG_GRALLOC_FB) \
                     $(BUILD)/libgraphics_translation.a $(ALIB)/libart.a $(FW6_JNI_LIBS) \
                     $(FW6_LIBS) $(BUILD)/libposix_translation.a $(ASYSTEM_IMAGE) $(OPFS_JS) $(CLOCK_JS) \
                     shim/input/browser_input.js art/artvm_pre.js shim/opfs/memory_opfs.js \
                     shim/opfs/idb_opfs.js \
                     shim/memory/fit_memory.js shim/gles/remote_webgl.js shim/gles/library_remote_webgl.js \
                     $(MBB)/libmembudget.a shim/membudget/mem_plan.js shim/membudget/mem_budget_pre.js mk/topic-pwa.mk
	@mkdir -p $(dir $@)
	$(EMXX) $(FGWEB_LDFLAGS) $(PWA_PHONE_LDFLAGS) -sMAXIMUM_MEMORY=2GB \
	    $(ART_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) $(FG_GRALLOC_FB) \
	    -Wl,--start-group $(filter-out $(FG_FB_HEADLESS),$(ART_LIBS)) \
	    $(BUILD)/libgraphics_translation.a $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
$(PWA)/a60/artvm.wasm $(PWA)/a60/artvm.data: $(PWA)/a60/artvm.js ;
