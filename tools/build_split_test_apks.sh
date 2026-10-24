#!/bin/sh
# Builds tests/apk-split as split APKs (topic apks-split): what an app
# bundle's .apks/.xapk holds, for arcwasm.SplitInstallTest (VM) and
# tests/apps/apks_split_test.js (the shell).
#
#   OUT/base.apk                    code, resources without density/language
#   OUT/split_config.mdpi.apk       aapt2 --split: values-mdpi, drawable-mdpi, mipmap-mdpi
#   OUT/split_config.xhdpi.apk      the same for xhdpi
#   OUT/split_config.ru.apk         values-ru
#   OUT/split_config.armeabi_v7a.apk  lib/armeabi-v7a/libndkhelper.so, no code
#   OUT/split_feature.apk           a feature part with code (classes.dex)
#
# Unsigned, not zipaligned (the package managers here do not check either).
#
# Usage: build_split_test_apks.sh VERSION_CODE OUT_DIR WORK_DIR
# Env: AAPT2, D8, ANDROID_JAR (defaults: newest in ~/Library/Android/sdk),
#      NDK_LIB (the armeabi-v7a library, default build/ndk/libs/armeabi-v7a/libndkhelper.so).
set -e
V=$1
OUT=$2
WORK=$3
[ -n "$WORK" ] || { echo "usage: $0 VERSION_CODE OUT_DIR WORK_DIR" >&2; exit 2; }
SRC=$(cd "$(dirname "$0")/../tests/apk-split" && pwd)
SDK=${ANDROID_SDK:-$HOME/Library/Android/sdk}
AAPT2=${AAPT2:-$(ls -d "$SDK"/build-tools/*/aapt2 | sort -V | tail -1)}
D8=${D8:-$(ls -d "$SDK"/build-tools/*/d8 | sort -V | tail -1)}
ANDROID_JAR=${ANDROID_JAR:-$(ls -d "$SDK"/platforms/android-*/android.jar | sort -V | tail -1)}
NDK_LIB=${NDK_LIB:-build/ndk/libs/armeabi-v7a/libndkhelper.so}

rm -rf "$WORK" "$OUT"
mkdir -p "$WORK/gen" "$WORK/classes" "$WORK/dex" "$WORK/fclasses" "$WORK/fdex" "$WORK/lib/lib/armeabi-v7a" "$OUT"
OUT=$(cd "$OUT" && pwd)

# The manifests at this version.
for m in base feature abi; do
  sed -e "s/android:versionCode=\"1\"/android:versionCode=\"$V\"/" \
      -e "s/android:versionName=\"1.0\"/android:versionName=\"$V.0\"/" \
      "$SRC/$m/AndroidManifest.xml" > "$WORK/$m-manifest.xml"
done

# Resources: base.apk and the configuration parts in one link.
"$AAPT2" compile --dir "$SRC/base/res" -o "$WORK/res.zip"
"$AAPT2" link -o "$OUT/base.apk" --manifest "$WORK/base-manifest.xml" -I "$ANDROID_JAR" \
  --java "$WORK/gen" "$WORK/res.zip" \
  --split "$OUT/split_config.mdpi.apk:mdpi" \
  --split "$OUT/split_config.xhdpi.apk:xhdpi" \
  --split "$OUT/split_config.ru.apk:ru"
"$AAPT2" link -o "$OUT/split_feature.apk" --manifest "$WORK/feature-manifest.xml" -I "$ANDROID_JAR"
"$AAPT2" link -o "$OUT/split_config.armeabi_v7a.apk" --manifest "$WORK/abi-manifest.xml" -I "$ANDROID_JAR"

# Code: the base's and the feature's dex.
dex() {  # SRC_DIR CLASSES DEX APK [EXTRA_SRC]
  find "$1" $5 -name '*.java' | sed 's|.*|"&"|' > "$2.txt"
  javac -source 8 -target 8 -Xlint:-options -encoding UTF-8 -bootclasspath "$ANDROID_JAR" \
    -d "$2" @"$2.txt"
  (cd "$2" && jar cf ../$(basename "$2").jar .)
  "$D8" --release --min-api 21 --lib "$ANDROID_JAR" --output "$3" "$2/../$(basename "$2").jar"
  (cd "$3" && zip -q "$4" classes.dex)
}
dex "$SRC/base/src" "$WORK/classes" "$WORK/dex" "$OUT/base.apk" "$WORK/gen"
dex "$SRC/feature/src" "$WORK/fclasses" "$WORK/fdex" "$OUT/split_feature.apk"

# The ABI part's library.
cp "$NDK_LIB" "$WORK/lib/lib/armeabi-v7a/libndkhelper.so"
(cd "$WORK/lib" && zip -q "$OUT/split_config.armeabi_v7a.apk" lib/armeabi-v7a/libndkhelper.so)
echo "built $OUT: $(cd "$OUT" && ls | tr '\n' ' ')"
