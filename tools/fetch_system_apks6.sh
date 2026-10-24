#!/bin/sh
# Android 6.0's own system apps for the ART stack (topic userspace): Launcher3,
# Calculator, DeskClock, Settings, SystemUI from Google's SDK emulator image
# (sys-img armeabi-v7a-23_r06, Android 6.0, API 23), deodexed, into
# build/system-apks6/ (mk/topic-userspace.mk puts those into ART's /system/app).
#
# The image is pre-optimized: each NAME.apk has no classes.dex, its code is in
# oat/arm/NAME.odex, quickened against that image's boot classpath
# (iget-quick, invoke-virtual-quick: field offsets and vtable indices of its
# framework). baksmali's deodex turns that back into plain instructions using
# the image's framework/arm/boot.oat, smali assembles classes.dex again, and
# the dex goes back into the APK (not re-signed: arc-wasm's PackageManager
# does not check signatures).
#
# Usage: tools/fetch_system_apks6.sh [IMAGE_ZIP]
#   IMAGE_ZIP  armeabi-v7a-23_r06.zip, default build/aosp6-sysimg/; from
#              https://dl.google.com/android/repository/sys-img/android/armeabi-v7a-23_r06.zip
#              (238 MB; SHA-1 as in the SDK's sys-img2-1.xml, checked here)
# Also writes build/system-apks6/framework-res-ids.txt: the resource ids of the
# image's framework-res, which the apps refer to by number; the ART build pins
# its framework-res to them (tools/make_public_ids.py, mk/topic-userspace.mk).
#
# Needs 7z (ext4 images), aapt (AAPT, default the newest Android SDK
# build-tools') and java with SMALI_DIR (default build/smali-tools)
# holding baksmali-2.5.2.jar and smali-2.5.2.jar
# (https://bitbucket.org/JesusFreke/smali/downloads).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ZIP=${1:-$ROOT/build/aosp6-sysimg/armeabi-v7a-23_r06.zip}
SHA1=7cf2ad756e54a3acfd81064b63cb0cb9dff2798d
SMALI_DIR=${SMALI_DIR:-$ROOT/build/smali-tools}
AAPT=${AAPT:-$(ls -d "$HOME"/Library/Android/sdk/build-tools/*/aapt "$HOME"/Android/Sdk/build-tools/*/aapt 2>/dev/null | tail -1)}
OUT=$ROOT/build/system-apks6
W=$ROOT/build/aosp6-sysimg/work
APPS="priv-app/Launcher3 app/Calculator app/DeskClock priv-app/Settings priv-app/SystemUI"

[ "$(shasum -a 1 "$ZIP" | cut -d' ' -f1)" = "$SHA1" ] || { echo "$ZIP: SHA-1 mismatch" >&2; exit 1; }
rm -rf "$W" && mkdir -p "$W" "$OUT"
7z x -y -o"$W" "$ZIP" armeabi-v7a/system.img > /dev/null
IMG=$W/armeabi-v7a/system.img
7z x -y -o"$W/sys" "$IMG" 'framework/arm/boot.oat' 'framework/framework-res.apk' > /dev/null
"$AAPT" dump resources "$W/sys/framework/framework-res.apk" | grep 'spec resource' |
  sed -E 's/.*spec resource (0x[0-9a-f]+) android:([^/]+)\/([^:]+):.*/\1 \2 \3/' \
  > "$OUT/framework-res-ids.txt"
echo "$OUT/framework-res-ids.txt ($(wc -l < "$OUT/framework-res-ids.txt" | tr -d ' ') resources)"
for app in $APPS; do
  name=$(basename "$app")
  7z e -y -o"$W/$name" "$IMG" "$app/$name.apk" "$app/oat/arm/$name.odex" > /dev/null
  java -jar "$SMALI_DIR/baksmali-2.5.2.jar" deodex -b "$W/sys/framework/arm/boot.oat" \
      -o "$W/$name/smali" "$W/$name/$name.odex"
  java -jar "$SMALI_DIR/smali-2.5.2.jar" assemble -a 23 -o "$W/$name/classes.dex" "$W/$name/smali"
  cp "$W/$name/$name.apk" "$OUT/$name.apk"
  (cd "$W/$name" && zip -q "$OUT/$name.apk" classes.dex)
  echo "$OUT/$name.apk ($(wc -c < "$W/$name/classes.dex" | tr -d ' ') bytes of dex)"
done
rm -rf "$W"
