# Builds ARC's posix_translation (minus the Pepper backends) and its gtest
# suite for WebAssembly with Emscripten + pthreads.
#
#   make            # build build/posix_translation_test.js
#   make test       # build and run the suite under Node.js
#
# Paths are relative on purpose: the checkout lives under a directory with
# spaces in its name, which make handles poorly.

EMSDK ?= $(HOME)/emsdk
# The guest (ARM/arm64) compilers write this checkout's folder as "." into what
# they build (DWARF DW_AT_comp_dir, __FILE__): libraries that go into /system
# images (and so into a release) must not carry the builder's home folder.
# Quoted: the folder may have spaces.
PREFIX_MAP := -ffile-prefix-map="$(CURDIR)"=.
# Big inputs outside the checkout (archives of AOSP and the NDK, APKs for the
# sweeps): ARC_WORKSPACE (a big disk, from the environment), and the folder
# that holds the checkout (for a worktree: the main checkout's), ARC_SIBLINGS.
ARC_SIBLINGS := $(shell cd "$$(git rev-parse --git-common-dir 2>/dev/null || echo .git)/../.." 2>/dev/null && pwd)
ARC_WORKSPACE ?=
EMXX  := $(EMSDK)/upstream/emscripten/em++
NODE  ?= node

# Every VM's linked module (dalvikvm, artvm, artvm8, artvm10) then goes through
# binaryen's -Os (topics a10-size and a81-size, docs/topics/a10-size.md): the
# links are -O0, so the module as a whole was never optimized -- template
# copies with one body, small wrappers out of line. A third fewer functions;
# Safari keeps code and metadata of every function in every worker (iPhone:
# WebKit malloc outside the wasm memory). Names stay (-g: wasm stacks in
# logs); exports and the table's slots do not change (JS, the dex JIT and the
# ARM translator reach the module by them). A function with one caller is
# inlined only up to 1000 (binaryen's measure; -Os alone has no limit): else
# main grew to 190 KB and art::interpreter::Execute took in two of the four
# interpreter loops (180 KB), and Safari 16 on an iPhone started an app
# 1-4 s later (topic a81-size) -- at the same size of the module.
WASM_OPT := $(EMSDK)/upstream/bin/wasm-opt -Os --one-caller-inline-max-function-size 1000 -g \
            --enable-threads --enable-bulk-memory --enable-mutable-globals --enable-sign-ext \
            --enable-nontrapping-float-to-int --enable-reference-types
wasm_opt = $(WASM_OPT) $(1) -o $(1)

BUILD := build
UP    := upstream/src
GT    := third_party/googletest

DEFINES := -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -DARC_WASM=1 \
           -DARC_TARGET_PATH=\"/arc\" -DLOG_TAG=\"posix_translation\"
INCLUDES := -Ishim -I$(UP) \
            -I$(GT)/googletest/include -I$(GT)/googlemock/include
WARNINGS := -Wall -Wno-unused-parameter -Wno-unused-private-field \
            -Wno-unused-variable -Wno-unused-function \
            -Wno-deprecated-declarations -Wno-missing-field-initializers \
            -Wno-sign-compare -Wno-inconsistent-missing-override
CXXFLAGS := -std=gnu++17 -pthread -O1 -g1 $(DEFINES) $(INCLUDES) $(WARNINGS) \
            -include shim/arc_wasm_compat.h

# Pepper-backed handlers (pepper_file, tcp/udp_socket, host_resolver,
# external_file, crx_file, nacl_manifest_file) and the NaCl IRT hook layer
# (file_wrap.cc, initialization.cc, sockets_wrap.cc, syscall_wrap.cc) are
# intentionally not built. misc_wrap.cc is: the VFS starts threads through
# __wrap_pthread_create so they join the right emulated process.
PT_SRCS := \
  abstract_socket_namespace address_util cpu_file dev_alarm dev_ashmem \
  dev_null dev_urandom dev_zero device_file directory_file_stream \
  directory_manager epoll_stream fd_to_file_stream_map fence_stream \
  file_stream file_system_handler local_socket logd_socket_namespace \
  memory_region mount_point_manager passthrough path_util permission_info \
  procfs_file readonly_file readonly_fs_reader readonly_memory_file redirect \
  socket_stream socket_util statfs time_util virtual_file_system misc_wrap

COMMON_SRCS := process_emulator file_util options arc_strace plugin_handle \
               circular_buffer

SHIM_SRCS := shim/base/base_shim.cc shim/common/alog_shim.cc shim/common/trace_shim.cc \
             shim/ppapi/ppapi_shim.cc shim/native_client/nacl_shim.cc \
             shim/posix_translation/net_bridge.cc shim/posix_translation/net_sockets.cc \
             shim/posix_translation/net_resolver.cc shim/posix_translation/net_interfaces.cc \
             shim/posix_translation/init_shim.cc \
             shim/wasm_mm/wasm_mmap.cc shim/posix_translation/host_file.cc \
             shim/opfs/opfs_bridge.cc shim/opfs/opfs_file.cc \
             shim/multiproc/light_process.cc

LIB_SRCS := $(addprefix $(UP)/posix_translation/,$(addsuffix .cc,$(PT_SRCS))) \
            $(addprefix $(UP)/common/,$(addsuffix .cc,$(COMMON_SRCS))) \
            $(SHIM_SRCS)

# Tests for the Pepper-backed handlers are excluded together with them, as is
# syscall_wrap_test (it exercises the libc wrap layer, which is not built).
TEST_EXCLUDE := pepper_file_test tcp_socket_test external_file_test \
                crx_file_test virtual_file_system_host_resolver_test \
                syscall_wrap_test
TEST_SRCS := $(filter-out $(addprefix $(UP)/posix_translation/,$(addsuffix .cc,$(TEST_EXCLUDE))), \
               $(wildcard $(UP)/posix_translation/*_test.cc)) \
             $(filter-out $(UP)/posix_translation/test_util/posix_translation_test.cc \
                          $(UP)/posix_translation/test_util/sysconf_util.cc, \
               $(wildcard $(UP)/posix_translation/test_util/*.cc)) \
             shim/posix_translation/test_util/sysconf_util.cc \
             shim/posix_translation/test_util/init_for_test.cc \
             shim/posix_translation/test_util/memory_report.cc \
             shim/wasm_mm/libc_mm_host.cc \
             $(UP)/posix_translation/test_util/posix_translation_test.cc \
             $(UP)/ppapi_mocks/background_test.cc $(UP)/common/tests/option_test_helper.cc \
             shim/ppapi_mocks/ppapi_test.cc shim/ppapi_mocks/background_thread.cc

GTEST_SRCS := $(GT)/googletest/src/gtest-all.cc $(GT)/googlemock/src/gmock-all.cc
GTEST_FLAGS := -std=gnu++17 -pthread -O1 -g1 \
               -I$(GT)/googletest -I$(GT)/googletest/include \
               -I$(GT)/googlemock -I$(GT)/googlemock/include

obj = $(patsubst %.cc,$(BUILD)/obj/%.o,$(1))
LIB_OBJS   := $(call obj,$(LIB_SRCS))
TEST_OBJS  := $(call obj,$(TEST_SRCS))
GTEST_OBJS := $(call obj,$(GTEST_SRCS))

FS_IMAGE := $(BUILD)/fs/test_readonly_fs_image.img

# PROXY_TO_PTHREAD runs main() on a worker so the real main JS thread stays
# free to start workers and reclaim finished threads (it does both from its
# event loop). Without it, thread stacks pile up until exit.
LDFLAGS := -pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=24 -sINITIAL_MEMORY=512MB \
           -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB \
           -sEXIT_RUNTIME=1 -sENVIRONMENT=node,worker -sASSERTIONS=1 \
           --embed-file $(FS_IMAGE)@/arc/posix_translation_fs_images/test_readonly_fs_image.img

.PHONY: all lib test clean
all: $(BUILD)/posix_translation_test.js
lib: $(BUILD)/libposix_translation.a

$(BUILD)/obj/third_party/%.o: third_party/%.cc
	@mkdir -p $(dir $@)
	$(EMXX) $(GTEST_FLAGS) -c $< -o $@

$(BUILD)/obj/%.o: %.cc
	@mkdir -p $(dir $@)
	$(EMXX) $(CXXFLAGS) -c $< -o $@

# base::Lock is inline in every object (no header dependencies here): they
# are rebuilt when it changes (topic gc-pause's held-lock record).
$(LIB_OBJS): shim/base/synchronization/lock.h

$(BUILD)/libposix_translation.a: $(LIB_OBJS)
	rm -f $@ && $(EMSDK)/upstream/emscripten/emar rcs $@ $^

$(FS_IMAGE): tools/make_test_fs_image.py
	python3 tools/make_test_fs_image.py $(dir $@)

$(BUILD)/posix_translation_test.js: $(TEST_OBJS) $(BUILD)/libposix_translation.a $(GTEST_OBJS) $(FS_IMAGE) Makefile
	$(EMXX) $(LDFLAGS) $(TEST_OBJS) $(BUILD)/libposix_translation.a $(GTEST_OBJS) -o $@

test: $(BUILD)/posix_translation_test.js
	cd $(BUILD) && $(NODE) posix_translation_test.js $(GTEST_ARGS)

clean:
	rm -rf $(BUILD)

# --- graphics_translation + GLES 1.1 demo ----------------------------------
#
#   make demo        # build build/demo/ (serve with tools/serve.py: threads
#                    # need COOP/COEP headers)
#
# One threaded runtime: main() runs on a pthread (PROXY_TO_PTHREAD), which
# owns the WebGL2 context through an OffscreenCanvas transferred from the
# page, and posix_translation serves /system from an image via its VFS.

GT_UP   := upstream/mods/graphics_translation
GDEMO   := $(BUILD)/demo
GINCLUDES := -Ishim -I$(UP) -Iupstream/mods -Ithird_party/aosp/include \
             -I$(GT)/googletest/include
GCXXFLAGS := -std=gnu++17 -pthread -O2 -g1 $(DEFINES) $(GINCLUDES) $(WARNINGS) \
             -Wno-c++11-narrowing -Wno-unused-but-set-variable \
             -include shim/arc_wasm_compat.h \
             -include shim/graphics_translation/gl_compat.h
GT_SRCS := $(filter-out $(GT_UP)/egl/native_pepper.cpp $(GT_UP)/egl/%_test.cpp, \
             $(wildcard $(GT_UP)/egl/*.cpp)) \
           $(wildcard $(GT_UP)/gralloc/*.cpp) \
           $(filter-out $(GT_UP)/gles/%_test.cpp, $(wildcard $(GT_UP)/gles/*.cpp)) \
           shim/graphics_translation/egl/native_webgl.cpp \
           shim/ndk/ndk_glsl.cc \
           third_party/aosp/src/etc1.cpp \
           $(UP)/common/shared_object_tracker.cc \
           $(UP)/common/matrix.cc $(UP)/common/vector.cc

gobj = $(patsubst %,$(BUILD)/gobj/%.o,$(1))
GT_OBJS   := $(call gobj,$(GT_SRCS))
GDEMO_OBJ := $(call gobj,demo/gles1_demo.cpp)

$(GT_OBJS) $(GDEMO_OBJ): $(BUILD)/gobj/%.o: %
	@mkdir -p $(dir $@)
	$(EMXX) $(GCXXFLAGS) -c $< -o $@

$(BUILD)/libgraphics_translation.a: $(GT_OBJS)
	rm -f $@ && $(EMSDK)/upstream/emscripten/emar rcs $@ $^

SYSTEM_IMAGE := $(BUILD)/fs/system.img
# (+ a second small file, which the packing puts at an unaligned offset:
# shim/kernel/arc_kernel_test.cc maps it; and a jar whose classes.dex --
# any 148 KB, a source file -- --store-dex stores 64 KiB-aligned, as for
# ART 10's boot jars.)
KERNEL_TEST_JAR := $(BUILD)/fs/kernel-test.jar
$(KERNEL_TEST_JAR): shim/ndk/ndk_egl.cc
	@mkdir -p $(dir $@)
	python3 -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED).write(sys.argv[2], 'classes.dex')" $@ $<
$(SYSTEM_IMAGE): tools/make_system_image.py tools/make_test_fs_image.py shim/kernel/arc_kernel_test.cc $(KERNEL_TEST_JAR)
	python3 tools/make_system_image.py $@ --store-dex --add shim/kernel/arc_kernel_test.cc:system/etc/arc_kernel_test.cc \
	    --add $(KERNEL_TEST_JAR):system/framework/kernel-test.jar

GDEMO_LDFLAGS := -pthread -sPROXY_TO_PTHREAD -sOFFSCREENCANVAS_SUPPORT \
                 -sOFFSCREENCANVASES_TO_PTHREAD='\#canvas' \
                 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
                 -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB \
                 -sPTHREAD_POOL_SIZE=4 -sINITIAL_MEMORY=128MB \
                 -sENVIRONMENT=web,worker -sASSERTIONS=1 \
                 --embed-file $(SYSTEM_IMAGE)@/arc/system.img

KERNEL_OBJ := $(call obj,shim/kernel/arc_kernel.cc)

$(GDEMO)/gles1_demo.js: $(GDEMO_OBJ) $(KERNEL_OBJ) $(BUILD)/libgraphics_translation.a \
                        $(BUILD)/libposix_translation.a $(SYSTEM_IMAGE) Makefile \
                        shim/opfs/library_arc_opfs.js
	@mkdir -p $(dir $@)
	$(EMXX) $(GDEMO_LDFLAGS) --js-library shim/opfs/library_arc_opfs.js \
	    $(GDEMO_OBJ) $(KERNEL_OBJ) \
	    $(BUILD)/libgraphics_translation.a $(BUILD)/libposix_translation.a -o $@

$(GDEMO)/index.html: demo/index.html
	@mkdir -p $(dir $@)
	cp $< $@

$(GDEMO)/report.js $(BUILD)/opfs-test/report.js: shim/report.js
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: demo
demo: $(GDEMO)/gles1_demo.js $(GDEMO)/index.html $(GDEMO)/report.js

# --- libc -> VFS kernel layer ------------------------------------------------
#
#   make kernel-test   # build and run shim/kernel/arc_kernel_test.cc in Node

# The VM's monotonic clock, one for all threads (every program with the kernel).
CLOCK_JS := shim/kernel/library_arc_clock.js
KERNEL_TEST_SRCS := shim/kernel/arc_kernel.cc shim/kernel/arc_kernel_test.cc
KERNEL_TEST_OBJS := $(call obj,$(KERNEL_TEST_SRCS))

$(BUILD)/arc_kernel_test.js: $(KERNEL_TEST_OBJS) $(BUILD)/libposix_translation.a \
                             $(GTEST_OBJS) $(SYSTEM_IMAGE) $(CLOCK_JS) Makefile
	$(EMXX) -pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=8 \
	    -sINITIAL_MEMORY=256MB -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB \
	    -sEXIT_RUNTIME=1 -sENVIRONMENT=node,worker -sASSERTIONS=1 --js-library $(CLOCK_JS) \
	    --embed-file $(SYSTEM_IMAGE)@/arc/system.img \
	    $(KERNEL_TEST_OBJS) $(BUILD)/libposix_translation.a $(GTEST_OBJS) -o $@

.PHONY: kernel-test
kernel-test: $(BUILD)/arc_kernel_test.js
	cd $(BUILD) && $(NODE) arc_kernel_test.js $(GTEST_ARGS)
	cd $(BUILD) && $(NODE) arc_kernel_test.js --image-source=fs/system.img $(GTEST_ARGS)

# --- OPFS-backed /data --------------------------------------------------------
#
#   make opfs-test   # build build/opfs-test/; open it via tools/serve.py.
#                    # OPFS exists only in browsers, so there is no Node run.

OPFS_JS := shim/opfs/library_arc_opfs.js
OPFS_TEST_OBJS := $(call obj,shim/kernel/arc_kernel.cc shim/opfs/opfs_test.cc)

$(BUILD)/opfs-test/opfs_test.js: $(OPFS_TEST_OBJS) $(BUILD)/libposix_translation.a \
                                 $(GTEST_OBJS) $(OPFS_JS) $(CLOCK_JS) Makefile
	@mkdir -p $(dir $@)
	$(EMXX) -pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=8 \
	    -sINITIAL_MEMORY=256MB -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB \
	    -sEXIT_RUNTIME=1 -sENVIRONMENT=web,worker -sASSERTIONS=1 \
	    --js-library $(OPFS_JS) --js-library $(CLOCK_JS) \
	    $(OPFS_TEST_OBJS) $(BUILD)/libposix_translation.a $(GTEST_OBJS) -o $@

$(BUILD)/opfs-test/index.html: tests/browser/index.html
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: opfs-test
opfs-test: $(BUILD)/opfs-test/opfs_test.js $(BUILD)/opfs-test/index.html \
           $(BUILD)/opfs-test/report.js
include mk/dalvik.mk

# Per-topic build fragments (mk/topic-*.mk), included after the Dalvik and
# framework ones so they can use their variables.
-include $(sort $(wildcard mk/topic-*.mk))
