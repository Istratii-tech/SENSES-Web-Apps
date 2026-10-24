# --- SENSES Web Apps: the VMs the shell runs apps on (topic install8) --------------
#   make senses-apps-vm   # build/senses-apps/vm/{a44,a60,a81,a10}/ + vm.json
# One build per stack, for phones and desktops alike (docs/topics/install8.md,
# "Contract with the shell": the files, Module.arguments and ENV):
#   a44/  Android 4.4 on Dalvik      dalvikvm.{js,wasm,data}
#   a60/  Android 6.0.1 on ART       artvm.{js,wasm,data}
#   a81/  Android 8.1 on ART 8.1     artvm8.{js,wasm,data}   (the default stack)
#   a10/  Android 10 on ART 10       artvm10.{js,wasm,data}  (apps with minSdk 28-29,
#                                    arm64-only apps; topic senses-a10)
# Their /system images have no launcher, SystemUI or system apps and none of
# the experiments' APKs (build/extra-apks*, build/system-apks*): the apps come
# from the shell's library (/mnt/apps). As Web Systems' phone builds a44/a60
# (mk/topic-pwa.mk, not used here): the display without WebGL in workers
# (Safari < 17: frames on the page's main thread), /data in IndexedDB where
# OPFS cannot write (iOS 16.1), apps' WebGL on the page's main thread, the
# wasm memory as large as the device allows (shim/memory/fit_memory.js) under
# the memory budget's ceiling (mem-budget: iPhones 1536 MB).
SAVM := $(BUILD)/senses-apps/vm

# A stack's image additions without system/app/* and the APKs of InstallTest
# and SplitInstallTest.
savm_add = $(patsubst --add=%,--add %,$(foreach a,$(subst --add ,--add=,$(1)),$(if \
             $(findstring :system/app/,$(a))$(findstring :system/etc/arcwasm/install-test/,$(a))$(findstring \
             :system/etc/arcwasm/split-test/,$(a)),,$(a))))

# --- a44: Dalvik 4.4 (graphics-web's link) ---------------------------------------------
$(SAVM)/a44/system.img: $(DSYSTEM_IMAGE) mk/topic-senses-apps-vm.mk
	@mkdir -p $(dir $@)
	python3 tools/make_system_image.py $@ \
	    --add $(DFW)/core.jar:system/framework/core.jar \
	    $(foreach j,$(FW_BOOT_JARS),--add $(DFW)/$(j).jar:system/framework/$(j).jar) \
	    --add $(ICU)/stubdata/icudt51l-default.dat:system/usr/icu/icudt51l.dat \
	    $(call savm_add,$(DSYSTEM_ADD)) $(addprefix --add ,$(DSYSTEM_EXTRA))
# The function table is lazy here too, as on a81 (below).
$(SAVM)/a44/dalvikvm.js: $(GWEB)/dalvikvm.js $(SAVM)/a44/system.img $(AUDIO_PRE_JS) \
                         tools/wasm_lazy_table.py tools/slim_thread_js.py
	@mkdir -p $(dir $@)
	$(EMXX) $(subst $(DSYSTEM_IMAGE),$(SAVM)/a44/system.img,$(GWEB_LDFLAGS)) \
	    --pre-js $(AUDIO_PRE_JS) $(DVM_MAIN_OBJ) $(DVM_VFS_OBJ) $(KERNEL_OBJ) $(GFB_GRALLOC_OBJ) \
	    -Wl,--start-group $(GWEB_LIBS) $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
	$(call wasm_opt,$(SAVM)/a44/dalvikvm.wasm)
	python3 tools/wasm_lazy_table.py $(SAVM)/a44/dalvikvm.wasm $@
	EMSDK=$(EMSDK) python3 tools/slim_thread_js.py $@
$(SAVM)/a44/dalvikvm.wasm $(SAVM)/a44/dalvikvm.data: $(SAVM)/a44/dalvikvm.js ;

# --- a60: ART 6.0.1 (fw6-graphics-web's link) -------------------------------------------
$(SAVM)/a60/system.img: $(ASYSTEM_IMAGE) mk/topic-senses-apps-vm.mk
	@mkdir -p $(dir $@)
	python3 tools/make_system_image.py $@ \
	    --add $(AFW)/core-libart.jar:system/framework/core-libart.jar \
	    --add $(ICU55)/stubdata/icudt55l.dat:system/usr/icu/icudt55l.dat \
	    $(call savm_add,$(ASYSTEM_ADD))
# The function table is lazy here too, as on a81 (below).
$(SAVM)/a60/artvm.js: $(FGWEB)/artvm.js $(SAVM)/a60/system.img shim/opfs/memory_opfs.js \
                      shim/opfs/idb_opfs.js shim/gles/remote_webgl.js shim/gles/library_remote_webgl.js \
                      shim/audio/web_audio.js tools/wasm_lazy_table.py tools/slim_thread_js.py
	@mkdir -p $(dir $@)
	$(EMXX) $(subst $(ASYSTEM_IMAGE),$(SAVM)/a60/system.img,$(FGWEB_LDFLAGS)) $(PWA_PHONE_LDFLAGS) \
	    --pre-js shim/audio/web_audio.js \
	    $(ART_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) $(FG_GRALLOC_FB) \
	    -Wl,--start-group $(filter-out $(FG_FB_HEADLESS),$(ART_LIBS)) \
	    $(BUILD)/libgraphics_translation.a $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
	$(call wasm_opt,$(SAVM)/a60/artvm.wasm)
	python3 tools/wasm_lazy_table.py $(SAVM)/a60/artvm.wasm $@
	EMSDK=$(EMSDK) python3 tools/slim_thread_js.py $@
$(SAVM)/a60/artvm.wasm $(SAVM)/a60/artvm.data: $(SAVM)/a60/artvm.js ;

# --- a81: ART 8.1 (fw8-graphics-web's link) ---------------------------------------------
$(SAVM)/a81/system.img: $(A8SYSTEM_IMAGE) mk/topic-senses-apps-vm.mk
	@mkdir -p $(dir $@)
	python3 tools/make_system_image.py $@ $(A8_IMAGE_OPTS) \
	    --add $(A8BLD)/build.prop:system/build.prop \
	    --add $(A8FW)/core-oj.jar:system/framework/core-oj.jar \
	    --add $(A8FW)/core-libart.jar:system/framework/core-libart.jar \
	    $(A8_ICU_ADD) \
	    --add $(A8_TZ)/iana/tzdata:system/usr/share/zoneinfo/tzdata \
	    --add $(A8_TZ)/android/tzlookup.xml:system/usr/share/zoneinfo/tzlookup.xml \
	    $(call savm_add,$(ASYSTEM8_ADD))
# The function table is filled per thread as it is used (tools/wasm_lazy_table.py,
# docs/topics/ios-mem.md): Safari 16 made a JS function of each of its 30000
# elements in every thread, ~12 MB a thread.
# Every stack's JS is then minified (ASCII only) and wrapped in one function
# (tools/slim_thread_js.py, docs/topics/ios-threads.md): each thread is a
# worker that runs it, ~10 -> ~6.5 MB a worker on an iPhone.
$(SAVM)/a81/artvm8.js: $(G8WEB)/artvm8.js $(SAVM)/a81/system.img shim/opfs/memory_opfs.js \
                       shim/opfs/idb_opfs.js shim/gles/remote_webgl.js shim/gles/library_remote_webgl.js \
                       shim/audio/web_audio.js tools/wasm_lazy_table.py tools/slim_thread_js.py
	@mkdir -p $(dir $@)
	$(EMXX) $(subst $(A8SYSTEM_IMAGE),$(SAVM)/a81/system.img,$(G8WEB_LDFLAGS)) $(PWA_PHONE_LDFLAGS) \
	    --pre-js shim/audio/web_audio.js \
	    $(ART8_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) $(G8_GRALLOC_FB) \
	    -Wl,--start-group $(filter-out $(G8_FB_HEADLESS),$(ART8_LIBS)) \
	    $(BUILD)/libgraphics_translation.a $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
	$(call wasm_opt,$(SAVM)/a81/artvm8.wasm)
	python3 tools/wasm_lazy_table.py $(SAVM)/a81/artvm8.wasm $@
	EMSDK=$(EMSDK) python3 tools/slim_thread_js.py $@
$(SAVM)/a81/artvm8.wasm $(SAVM)/a81/artvm8.data: $(SAVM)/a81/artvm8.js ;

# --- a10: ART 10 (fw10-graphics-web's link) --------------------------------------------
# As a81, plus what ART10_LDFLAGS lacks and ART8_LDFLAGS has (topic mem-budget's
# page side: its plan and fit_memory.js's ceiling). The image also leaves out
# the media tests' clips (system/media/arcwasm: ~10 MB).
ifneq ($(G10WEB),)
savm10_add = $(patsubst --add=%,--add %,$(foreach a,$(subst --add ,--add=,$(1)),$(if \
               $(findstring :system/app/,$(a))$(findstring :system/etc/arcwasm/install-test/,$(a))$(findstring \
               :system/etc/arcwasm/split-test/,$(a))$(findstring :system/media/arcwasm/,$(a)),,$(a))))
$(SAVM)/a10/system.img: $(A10SYSTEM_IMAGE) mk/topic-senses-apps-vm.mk
	@mkdir -p $(dir $@)
	python3 tools/make_system_image.py $@ $(A10_IMAGE_OPTS) \
	    --add $(A10BLD)/build.prop:system/build.prop \
	    --add $(A10FW)/core-oj.jar:system/framework/core-oj.jar \
	    --add $(A10FW)/core-libart.jar:system/framework/core-libart.jar \
	    $(A10_ICU_ADD) \
	    --add $(A10_TZ)/iana/tzdata:system/etc/tz/tzdata \
	    --add $(A10_TZ)/android/tzlookup.xml:system/etc/tz/tzlookup.xml \
	    --add $(A10_TZ)/version/tz_version:system/etc/tz/tz_version \
	    $(call savm10_add,$(ASYSTEM10_ADD))
$(SAVM)/a10/artvm10.js: $(G10WEB)/artvm10.js $(SAVM)/a10/system.img shim/opfs/memory_opfs.js \
                        shim/opfs/idb_opfs.js shim/gles/remote_webgl.js shim/gles/library_remote_webgl.js \
                        shim/audio/web_audio.js shim/membudget/mem_plan.js shim/membudget/mem_budget_pre.js shim/memory/fit_memory.js \
                        tools/wasm_lazy_table.py tools/slim_thread_js.py
	@mkdir -p $(dir $@)
	$(EMXX) $(subst $(A10SYSTEM_IMAGE),$(SAVM)/a10/system.img,$(G10WEB_LDFLAGS)) $(PWA_PHONE_LDFLAGS) \
	    --pre-js shim/audio/web_audio.js --pre-js shim/membudget/mem_plan.js --pre-js shim/membudget/mem_budget_pre.js \
	    --pre-js shim/memory/fit_memory.js \
	    $(ART10_MAIN_OBJ) $(ART_VFS_OBJ) $(KERNEL_OBJ) $(G10_GRALLOC_FB) \
	    -Wl,--start-group $(filter-out $(G10_FB_HEADLESS),$(ART10_LIBS)) \
	    $(BUILD)/libgraphics_translation.a $(BUILD)/libposix_translation.a -Wl,--end-group -o $@
	$(call wasm_opt,$(SAVM)/a10/artvm10.wasm)
	python3 tools/wasm_lazy_table.py $(SAVM)/a10/artvm10.wasm $@
	EMSDK=$(EMSDK) python3 tools/slim_thread_js.py $@
$(SAVM)/a10/artvm10.wasm $(SAVM)/a10/artvm10.data: $(SAVM)/a10/artvm10.js ;
SAVM_A10_FILES := a10/artvm10.js a10/artvm10.wasm a10/artvm10.data
endif

# --- vm.json: what the shell loads for each stack -----------------------------------------
SAVM_FILES := a44/dalvikvm.js a44/dalvikvm.wasm a44/dalvikvm.data \
              a60/artvm.js a60/artvm.wasm a60/artvm.data \
              a81/artvm8.js a81/artvm8.wasm a81/artvm8.data $(SAVM_A10_FILES)
$(SAVM)/vm.json: $(addprefix $(SAVM)/,$(SAVM_FILES)) tools/senses_apps_vm_json.py
	python3 tools/senses_apps_vm_json.py $(SAVM) > $@.tmp && mv $@.tmp $@

# A test page for this side of the contract (not the shell): an app library
# in the storage, one app per VM in an iframe (demo/senses-apps-vm).
SAVM_PAGE := index.html run.html apk.js library-worker.js memprobe.html limitprobe.html opfsprobe.html opfsprobe-worker.js cageprobe.html
$(addprefix $(SAVM)/,$(SAVM_PAGE)): $(SAVM)/%: demo/senses-apps-vm/%
	@mkdir -p $(dir $@)
	cp $< $@
$(SAVM)/idb_opfs.js: shim/opfs/idb_opfs.js
	@mkdir -p $(dir $@)
	cp $< $@
$(SAVM)/report.js: shim/report.js
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: senses-apps-vm
senses-apps-vm: $(addprefix $(SAVM)/,$(SAVM_FILES)) $(SAVM)/vm.json \
                $(addprefix $(SAVM)/,$(SAVM_PAGE)) $(SAVM)/idb_opfs.js $(SAVM)/report.js
