#!/bin/sh
# Builds an APK from sources: aapt (resources, manifest, assets) against
# android.jar of an installed SDK platform, javac + d8 for classes.dex.
# Unsigned and not zipaligned: Dalvik and libandroidfw need neither.
#
# Usage: build_test_apk.sh SRC_DIR WORK_DIR OUT_APK
#   SRC_DIR holds AndroidManifest.xml, res/, assets/ and src/ (Java); without
#   src/ the APK has no classes.dex (android:hasCode="false").
# Env: AAPT, D8, ANDROID_JAR (defaults: newest in ~/Library/Android/sdk);
#      MIN_SDK (default 19), TARGET_SDK (default MIN_SDK): aapt's
#      --min/target-sdk-version and d8's --min-api;
#      EXTRA_ASSETS="HOST_FILE:assets/PATH ..." adds files from outside SRC_DIR
#      (e.g. AOSP fonts) to the APK's assets;
#      BOOTCP=a.jar:b.jar goes before android.jar for javac and d8 (a libcore
#      with java.lang.invoke's LambdaMetafactory, which android.jar lacks,
#      for Java 8 lambdas).
set -e
SRC=$1
WORK=$2
OUT=$3
[ -n "$OUT" ] || { echo "usage: $0 SRC_DIR WORK_DIR OUT_APK" >&2; exit 2; }
SDK=${ANDROID_SDK:-$HOME/Library/Android/sdk}
AAPT=${AAPT:-$(ls -d "$SDK"/build-tools/*/aapt | sort -V | tail -1)}
D8=${D8:-$(ls -d "$SDK"/build-tools/*/d8 | sort -V | tail -1)}
ANDROID_JAR=${ANDROID_JAR:-$(ls -d "$SDK"/platforms/android-*/android.jar | sort -V | tail -1)}
MIN_SDK=${MIN_SDK:-19}
TARGET_SDK=${TARGET_SDK:-$MIN_SDK}

rm -rf "$WORK"
mkdir -p "$WORK/gen" "$WORK/classes" "$WORK/dex"
OUT_ABS=$(cd "$(dirname "$OUT")" 2>/dev/null && pwd || (mkdir -p "$(dirname "$OUT")" && cd "$(dirname "$OUT")" && pwd))/$(basename "$OUT")

# Resources: resources.arsc, compiled XML (manifest, layouts), assets; R.java.
"$AAPT" package -f --min-sdk-version $MIN_SDK --target-sdk-version $TARGET_SDK \
  -M "$SRC/AndroidManifest.xml" -S "$SRC/res" -A "$SRC/assets" \
  -I "$ANDROID_JAR" -J "$WORK/gen" -F "$WORK/unsigned.apk"

cp "$WORK/unsigned.apk" "$OUT_ABS.tmp"
if [ -d "$SRC/src" ]; then
# Code: Java 8 bytecode, then dex for MIN_SDK (with desugaring, as for
# core.jar).
find "$SRC/src" "$WORK/gen" -name '*.java' | sed 's|.*|"&"|' > "$WORK/srcs.txt"
javac -source 8 -target 8 -Xlint:-options -encoding UTF-8 \
  -bootclasspath "${BOOTCP:+$BOOTCP:}$ANDROID_JAR" -d "$WORK/classes" @"$WORK/srcs.txt"
(cd "$WORK/classes" && jar cf ../classes.jar .)
set --
old=$IFS; IFS=:
for l in $BOOTCP; do set -- "$@" --lib "$l"; done
IFS=$old
"$D8" --release --min-api $MIN_SDK "$@" --lib "$ANDROID_JAR" --output "$WORK/dex" "$WORK/classes.jar"

(cd "$WORK/dex" && zip -q "$OUT_ABS.tmp" classes.dex)
fi
for pair in $EXTRA_ASSETS; do
  host=${pair%%:*}
  dest=${pair#*:}
  mkdir -p "$WORK/extra/$(dirname "$dest")"
  cp "$host" "$WORK/extra/$dest"
  (cd "$WORK/extra" && zip -q "$OUT_ABS.tmp" "$dest")
done
mv "$OUT_ABS.tmp" "$OUT_ABS"
echo "built $OUT ($(unzip -l "$OUT_ABS" | tail -1 | awk '{print $2}') files)"
