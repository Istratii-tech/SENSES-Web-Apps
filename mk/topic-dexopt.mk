# Topic "dexopt" (docs/topics/dexopt.md): Dalvik's dexopt without fork().
#
# Jars in /system are preoptimized at build time ("dexpreopt", as for an
# Android device image): build/dexopt/dexpreopt.js runs dexopt's code under
# Node, one process per jar, and writes NAME.odex (verified and quickened
# bytecode, register maps), which goes into the /system image next to
# NAME.jar. Code without an .odex (apps loaded at run time, or an .odex that
# no longer matches the boot class path) is verified and optimized in the
# running VM, one class at a time (patches/aosp44/dexopt.patch).
#
#   make dexpreopt         # the .odex files (part of make dalvik)
#   make DEXPREOPT=0 ...   # no .odex in /system (remove build/dalvik/system.img)
#   make dalvik-bench      # arcwasm.Bench under Node
#
# Included from the Makefile after mk/dalvik.mk and mk/framework.mk.

DEXOPT := $(BUILD)/dexopt
DEXPREOPT ?= 1

# --- the tool ---------------------------------------------------------------------
DEXPREOPT_OBJS := $(call dobj,shim/dexopt/dexpreopt.cc) $(call obj,shim/dexopt/memory_file.cc)
$(call dobj,shim/dexopt/dexpreopt.cc): XFLAGS := $(D_DVM_FLAGS)
DEXPREOPT_PRE_JS := shim/dexopt/dexpreopt_pre.js
# dalvikvm's link flags without the packaged /system image: the tool reads
# its image from $ARC_SYSTEM_IMAGE (dexpreopt_pre.js).
DEXPREOPT_LDFLAGS := $(filter-out --$(DSYSTEM_PACKAGING)-file $(DSYSTEM_IMAGE)@/arc/system.img \
                       --extern-pre-js $(DVM_PRE_JS),$(DVM_LDFLAGS)) \
                     -sENVIRONMENT=node,worker -sFORCE_FILESYSTEM=1 \
                     -sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE='$$UTF8ToString' \
                     --pre-js $(DEXPREOPT_PRE_JS)
# DALVIK_LIBS once all topic fragments have added theirs.
.SECONDEXPANSION:
$(DEXOPT)/dexpreopt.js: $(DEXPREOPT_OBJS) $(DVM_VFS_OBJ) $(KERNEL_OBJ) $$(DALVIK_LIBS) \
                        $(BUILD)/libposix_translation.a $(OPFS_JS) $(CLOCK_JS) $(DEXPREOPT_PRE_JS) \
                        mk/topic-dexopt.mk
	@mkdir -p $(dir $@)
	$(EMXX) $(DEXPREOPT_LDFLAGS) $(DEXPREOPT_OBJS) $(DVM_VFS_OBJ) $(KERNEL_OBJ) \
	    -Wl,--start-group $(DALVIK_LIBS) $(BUILD)/libposix_translation.a -Wl,--end-group -o $@

# --- what gets preoptimized ---------------------------------------------------------
# The boot class path dalvikvm runs with: an .odex records it and is not used
# if it differs at run time.
DEXOPT_BCP := $(shell python3 tools/dexopt_bootclasspath.py dalvik/launcher/dalvikvm.cc)
ifeq ($(DEXOPT_BCP),)
$(error mk/topic-dexopt.mk: no BOOTCLASSPATH in dalvik/launcher/dalvikvm.cc)
endif
# /system paths of the jars and APKs with code, and where the build makes
# them: the boot class path (/system/framework/NAME.jar from $(DFW)/NAME.jar),
# the test jar and the test APK.
DEXOPT_JARS := $(subst :, ,$(DEXOPT_BCP)) /system/framework/arc-tests.jar \
               /system/app/ArcWasmTestApp.apk
dexopt_src = $(if $(filter /system/app/ArcWasmTestApp.apk,$(1)),$(TEST_APK),$(DFW)/$(notdir $(1)))
dexopt_odex = $(DEXOPT)/odex$(basename $(1)).odex
DEXOPT_ODEX := $(foreach j,$(DEXOPT_JARS),$(call dexopt_odex,$(j)))

# The tool's /system: just the jars, at their run-time paths.
$(DEXOPT)/input.img: $(foreach j,$(DEXOPT_JARS),$(call dexopt_src,$(j))) \
                     tools/make_system_image.py mk/topic-dexopt.mk
	@mkdir -p $(dir $@)
	python3 tools/make_system_image.py $@ \
	    $(foreach j,$(DEXOPT_JARS),--add $(call dexopt_src,$(j)):$(patsubst /%,%,$(j)))

define dexopt_rule
$(call dexopt_odex,$(1)): $(DEXOPT)/dexpreopt.js $(DEXOPT)/input.img
	@mkdir -p $$(dir $$@) && rm -f $$@
	ARC_SYSTEM_IMAGE=$(DEXOPT)/input.img $(NODE) $(DEXOPT)/dexpreopt.js \
	    $(1) $$@ $(DEXOPT_BCP) 2>&1 | grep -v '^I/posix_translation' ; test -s $$@
endef
$(foreach j,$(DEXOPT_JARS),$(eval $(call dexopt_rule,$(j))))

.PHONY: dexpreopt
dexpreopt: $(DEXOPT_ODEX)

ifeq ($(DEXPREOPT),1)
DSYSTEM_ADD += $(foreach j,$(DEXOPT_JARS),--add $(call dexopt_odex,$(j)):$(patsubst /%,%,$(basename $(j))).odex)
$(DSYSTEM_IMAGE): $(DEXOPT_ODEX)
endif

# --- benchmark ------------------------------------------------------------------------
.PHONY: dalvik-bench
dalvik-bench: dalvik
	cd $(BUILD)/dalvik && $(NODE) dalvikvm.js -Xdexoptstats \
	    -cp /system/framework/arc-tests.jar arcwasm.Bench 10
