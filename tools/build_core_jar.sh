#!/bin/sh
# Builds core.jar (libcore for Dalvik, AOSP 4.4.4) as a DEX jar:
#   javac (source/target 8, own bootclasspath) -> d8 (min-api 19) -> jar.
# javac 8+ emits interface bridge methods as default methods, which Dalvik
# lacks; d8's desugaring moves them into the implementing classes.
#
# Usage: build_core_jar.sh OUT_DIR
# Env: ANDROID_SDK (default ~/Library/Android/sdk), D8 (path to d8).
set -e
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIBCORE=$ROOT/third_party/aosp44/libcore
SDK=${ANDROID_SDK:-$HOME/Library/Android/sdk}
D8=${D8:-$(ls -d "$SDK"/build-tools/*/d8 | sort -V | tail -1)}

W=$OUT/core-java
rm -rf "$W"
mkdir -p "$W/classes" "$W/empty" "$W/dex" "$W/res"
cd "$LIBCORE"
find dalvik dex dom json luni xml libdvm -path '*/src/main/java/*' -name '*.java' \
  | sort > "$W/srcs.txt"
javac -source 8 -target 8 -bootclasspath "$W/empty" -encoding UTF-8 -nowarn \
  -Xlint:-options -d "$W/classes" @"$W/srcs.txt" 2>&1 | grep -v '^Note:' || true
[ -f "$W/classes/java/lang/Object.class" ] || { echo "javac failed" >&2; exit 1; }
(cd "$W/classes" && jar cf "$W/core-classes.jar" .)
"$D8" --release --min-api 19 --output "$W/dex" "$W/core-classes.jar"

# Resources (non-Java files under src/main/java), as in JavaLibrary.mk.
for d in dalvik dex dom json luni xml libdvm; do
  [ -d $d/src/main/java ] || continue
  (cd $d/src/main/java && find . -type f ! -name '*.java' ! -name '*.html' \
     -exec sh -c 'mkdir -p "$0/$(dirname "$1")" && cp "$1" "$0/$1"' "$W/res" {} \;)
done
cp "$W/dex/classes.dex" "$W/res/"
rm -f "$OUT/core.jar"
(cd "$W/res" && zip -qr "$OUT/core.jar" .)
echo "built $OUT/core.jar"
