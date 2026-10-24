#!/bin/sh
# Fetches build dependencies that are not checked in.
#
#   tools/fetch_deps.sh
#
# * googletest 1.14.0 -> third_party/googletest (ignored by git)
#
# Emscripten itself is expected in ~/emsdk (override with EMSDK=...):
#   git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
#   ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest
# This tree was developed with Emscripten 6.0.12.
set -e
cd "$(dirname "$0")/.."

if [ ! -d third_party/googletest ]; then
  git -c advice.detachedHead=false clone -q --depth 1 --branch v1.14.0 \
      https://github.com/google/googletest.git third_party/googletest
  rm -rf third_party/googletest/.git
  echo "fetched googletest 1.14.0"
else
  echo "googletest already present"
fi

EMSDK_DIR="${EMSDK:-$HOME/emsdk}"
if [ ! -x "$EMSDK_DIR/upstream/emscripten/em++" ]; then
  echo "warning: no Emscripten at $EMSDK_DIR (see the comment in $0)" >&2
fi
