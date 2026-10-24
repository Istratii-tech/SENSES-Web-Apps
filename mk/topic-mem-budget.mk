# Topic "mem-budget": the app's memory budget (docs/topics/mem-budget.md,
# shim/membudget/mem_budget.h): the budget and the wasm memory's ceiling at
# start (mem_budget_pre.js), what memory holds (arc_mem_sample),
# /proc/meminfo, the heap sizes of the budget's device class, trims of this
# layer's caches; on the 8.1 stack also the system server's MemoryMonitor
# (framework/java-arcwasm8/arcwasm/server) over them.
#
#   make mem-budget-test   # MemoryTest on ART 8.1 in Node
#
# libmembudget.a (both ART stacks): the budget, the sample, /proc/meminfo,
# the VM's start (art/artvm.cc, art8/artvm8.cc call it weakly: linked with
# --whole-archive). libmembudget8.a (ART 8.1): the natives of
# arcwasm.server.MemoryBudget (they add themselves to art8's table of static
# JNI libraries) and ART 8.1's MemMaps by kind.

MBB := $(BUILD)/membudget
MB_OBJS := $(call obj,shim/membudget/mem_budget.cc shim/membudget/proc_meminfo.cc)
$(MB_OBJS): shim/membudget/mem_budget.h
$(MBB)/libmembudget.a: $(MB_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^

# --- ART 6.0.1 (no monitor: its system server is topic fw6-system's) ---------------------
ART_LIBS += -Wl,--whole-archive $(MBB)/libmembudget.a -Wl,--no-whole-archive
# The ceiling is made by shim/memory/fit_memory.js (topic userspace; one
# wrapper of WebAssembly.Memory for both: the plan's ceiling, less where the
# device refuses it).
ART_LDFLAGS += --pre-js shim/membudget/mem_plan.js --pre-js shim/membudget/mem_budget_pre.js --pre-js shim/memory/fit_memory.js   # (FGWEB_LDFLAGS derives from it)
$(ABLD)/artvm.js $(AWEB)/artvm.js $(FGWEB)/artvm.js: $(MBB)/libmembudget.a \
    shim/membudget/mem_plan.js shim/membudget/mem_budget_pre.js shim/memory/fit_memory.js

# --- ART 8.1 -------------------------------------------------------------------------------
ifneq ($(A8LIB),)
MB8_OBJS := $(call a8obj,shim/membudget/mem_budget_jni.cc shim/membudget/art_maps8.cc)
$(A8OBJ)/shim/membudget/mem_budget_jni.cc.o: A8FLAGS := $(A8_SYS_INCLUDES) -Ishim
$(A8OBJ)/shim/membudget/art_maps8.cc.o: A8FLAGS := $(ART8_FLAGS)
$(MB8_OBJS): shim/membudget/mem_budget.h
$(MBB)/libmembudget8.a: $(MB8_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@ && $(EMAR) rcs $@ $^
ART8_LIBS += -Wl,--whole-archive $(MBB)/libmembudget.a $(MBB)/libmembudget8.a -Wl,--no-whole-archive
ART8_LDFLAGS += --pre-js shim/membudget/mem_plan.js --pre-js shim/membudget/mem_budget_pre.js --pre-js shim/memory/fit_memory.js
$(A8BLD)/artvm8.js $(A8WEB)/artvm8.js $(BUILD)/fw8-graphics-web/artvm8.js: \
    $(MBB)/libmembudget.a $(MBB)/libmembudget8.a shim/membudget/mem_plan.js shim/membudget/mem_budget_pre.js \
    shim/memory/fit_memory.js
# System.loadLibrary("arcwasm_membudget") wants a file.
$(MBB)/empty.so:
	@mkdir -p $(dir $@)
	@: > $@
ASYSTEM8_ADD += --add $(MBB)/empty.so:system/lib/libarcwasm_membudget.so
$(A8SYSTEM_IMAGE): $(MBB)/empty.so

.PHONY: mem-budget-test
mem-budget-test: art8
	VM=art8 tools/run_java_tests.sh node MemoryTest
endif
