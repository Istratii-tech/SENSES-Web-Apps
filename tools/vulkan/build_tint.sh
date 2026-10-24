#!/bin/sh
# Builds Tint's SPIR-V reader and WGSL writer into a wasm module of its own
# for topic vulkan (docs/topics/vulkan.md): shim/vulkan/tint/arc_tint.cc
# -> OUT/arc_tint.js + OUT/arc_tint.wasm, which the page builds copy next
# to the VM (shim/vulkan/webgpu_host.js loads them when a device is made).
#
#   tools/vulkan/build_tint.sh WORK OUT
#
# WORK holds Dawn's sources (cloned at DAWN_REV with the SPIR-V headers,
# SPIRV-Tools and abseil revisions its DEPS names) and the CMake build;
# ~2 GB, so keep it on the external disk. Needs emsdk, cmake and ninja.
set -e
WORK=${1:?work directory}
OUT=${2:?output directory}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DAWN_REV=ff7c6960f62d4249f967a78448918eec21705a86
mkdir -p "$WORK" "$OUT"
OUT=$(cd "$OUT" && pwd)
WORK=$(cd "$WORK" && pwd)  # absolute: the prefix map must match the paths CMake gives

build_tint() {
cd "$WORK"
if [ ! -d dawn/.git ]; then
  git init -q dawn
  git -C dawn fetch -q --depth 1 https://dawn.googlesource.com/dawn "$DAWN_REV"
  git -C dawn checkout -q FETCH_HEAD
fi
dep() {  # dep PATH URL: the revision dawn/DEPS gives PATH
  rev=$(grep -A1 "'$1'" dawn/DEPS | sed -n "s/.*@\([0-9a-f]\{40\}\).*/\1/p" | head -1)
  if [ ! -d "dawn/$1/.git" ]; then
    git init -q "dawn/$1"
    git -C "dawn/$1" fetch -q --depth 1 "$2" "$rev"
    git -C "dawn/$1" checkout -q FETCH_HEAD
  fi
}
dep third_party/spirv-headers/src https://github.com/KhronosGroup/SPIRV-Headers
dep third_party/spirv-tools/src https://github.com/KhronosGroup/SPIRV-Tools
dep third_party/abseil-cpp https://chromium.googlesource.com/chromium/src/third_party/abseil-cpp
. "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1 || true
# Only Tint: under Emscripten Dawn's root also builds emdawnwebgpu and Dawn.
# And its third_party stops before SPIRV-Tools under Emscripten.
sed -i.orig 's/ OR DAWN_ENABLE_VULKAN OR EMSCRIPTEN)/ OR DAWN_ENABLE_VULKAN)/' dawn/CMakeLists.txt
python3 - dawn/third_party/CMakeLists.txt <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace("if (EMSCRIPTEN)\n    return()\nendif()", "# (arc-wasm: SPIRV-Tools for Tint's SPIR-V reader under Emscripten too)")
open(p, 'w').write(s)
PY
# Source paths in what is built (Tint's __FILE__ in its checks) relative to
# WORK, not the builder's folders. Quoted inside the flags: Ninja runs them
# through the shell, and WORK may have spaces.
MAP="-ffile-prefix-map=\"$WORK/\"="
emcmake cmake -S dawn -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="$MAP" -DCMAKE_CXX_FLAGS="$MAP" \
  -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_ENABLE_INSTALL=OFF \
  -DDAWN_FETCH_DEPENDENCIES=OFF -DDAWN_BUILD_PROTOBUF=OFF -DDAWN_USE_GLFW=OFF \
  -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DTINT_BUILD_BENCHMARKS=OFF \
  -DTINT_BUILD_SPV_READER=ON -DTINT_BUILD_WGSL_WRITER=ON -DTINT_BUILD_WGSL_READER=OFF \
  -DTINT_BUILD_GLSL_WRITER=OFF -DTINT_BUILD_GLSL_VALIDATOR=OFF -DTINT_BUILD_HLSL_WRITER=OFF \
  -DTINT_BUILD_MSL_WRITER=OFF -DTINT_BUILD_SPV_WRITER=OFF -DTINT_BUILD_NULL_WRITER=OFF \
  -DTINT_BUILD_IR_BINARY=OFF -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_SKIP_TESTS=ON > cmake.log
ninja -C build -j4 SPIRV-Tools-opt tint_lang_spirv_reader tint_lang_wgsl_writer > ninja.log
link_tint
}

link_tint() {
# Every static library of the build (the linker keeps what is used), and
# Tint's own defines and flags (as its CMake sets them: classes differ
# without them). Tint recurses deeply: Emscripten's 64 KB stack overflows
# into the heap (malloc aborts), so 4 MB.
LIBS=$(find build -name '*.a' | grep -v -e test -e bench | sort)
DEFS="-DNDEBUG -DDAWN_HAVE_UTILS_BUILD_CONFIG -DTINT_BUILD_SPV_READER=1 -DTINT_BUILD_WGSL_WRITER=1
  -DTINT_BUILD_WGSL_READER=0 -DTINT_BUILD_SPV_WRITER=0 -DTINT_BUILD_GLSL_WRITER=0 -DTINT_BUILD_HLSL_WRITER=0
  -DTINT_BUILD_MSL_WRITER=0 -DTINT_BUILD_NULL_WRITER=0 -DTINT_BUILD_IR_BINARY=0 -DTINT_BUILD_IS_LINUX=1
  -DTINT_BUILD_IS_MAC=0 -DTINT_BUILD_IS_WIN=0 -DTINT_ENABLE_IR_DUMPING=1 -DTINT_ENABLE_IR_VALIDATION_ASSERTS=1
  -fno-rtti -fno-exceptions"
em++ -std=c++20 -O2 $DEFS -ffile-prefix-map="$WORK/"= -ffile-prefix-map="$ROOT/"= -I dawn -I dawn/include -I build/gen -I build/gen/include \
  -I dawn/third_party/spirv-headers/src/include \
  "$ROOT/shim/vulkan/tint/arc_tint.cc" -Wl,--start-group $LIBS -Wl,--end-group \
  -sMODULARIZE=1 -sEXPORT_NAME=ArcTint -sENVIRONMENT=web,worker,node -sALLOW_MEMORY_GROWTH=1 \
  -sSTACK_SIZE=4MB \
  -sEXPORTED_FUNCTIONS=_arc_tint_spirv_to_wgsl,_arc_tint_free,_malloc,_free \
  -sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8,HEAPU32 \
  -o "$OUT/arc_tint.js"
ls -la "$OUT"/arc_tint.*
}

if [ "$3" = --link-only ]; then
  cd "$WORK"
  . "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1 || true
  link_tint
else
  build_tint
fi
