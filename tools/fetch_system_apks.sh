#!/bin/sh
# Topic userspace: copies stock AOSP 4.4.4 system apps into build/system-apks44
# (not in git), from where `make dalvik app-web` puts them in /system/app.
# Source: the AOSP build of the sibling project arm44-web, in its Docker
# volume (guest/build.sh). The system image there is odexed (the code is in
# .odex files tied to that build's framework), so take the signed
# package.apk that still has classes.dex, from the build's intermediates.
#   tools/fetch_system_apks.sh [APP ...]   (default: the topic's five)
# Env: VOLUME (arm44-web-aosp), PRODUCT (vexpress).
set -eu
cd "$(dirname "$0")/.."
VOLUME=${VOLUME:-arm44-web-aosp}
PRODUCT=${PRODUCT:-vexpress}
[ $# -gt 0 ] || set -- Calculator DeskClock Settings Launcher2 SystemUI
mkdir -p build/system-apks44
docker run --rm -v "$VOLUME":/work:ro -v "$PWD/build/system-apks44":/out alpine:3 sh -euc '
  cd /work/aosp/out/target/product/'"$PRODUCT"'/obj/APPS
  for a in "$@"; do cp "${a}_intermediates/package.apk" "/out/$a.apk"; echo "$a.apk"; done
' sh "$@"
