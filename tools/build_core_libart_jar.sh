#!/bin/sh
# Builds core-libart.jar (libcore 6.0 for ART, AOSP 6.0.1) as a DEX jar, the
# way libcore/JavaLibrary.mk builds module core-libart:
#   libcore {dalvik,dex,dom,json,luni,xml,libart} + ICU4J (minus localespi,
#   charset, translit), jarjar com.ibm.icu -> android.icu, ICU4J reading the
#   ICU4C data in /system/usr/icu,
#   javac (source/target 8, own bootclasspath) -> d8 (min-api 23, native
#   multidex: classes.dex, classes2.dex...) -> jar with the resources.
# javac 8+ emits interface bridge methods as default methods, which ART 6.0
# lacks; d8's desugaring (min-api < 24) moves them into the classes.
#
# Usage: build_core_libart_jar.sh OUT_DIR
# Env: ANDROID_SDK (default ~/Library/Android/sdk), D8 (path to d8).
set -e
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
A60=$ROOT/third_party/aosp60
LIBCORE=$A60/libcore
ICU4J=$A60/external/icu/icu4j
SDK=${ANDROID_SDK:-$HOME/Library/Android/sdk}
D8=${D8:-$(ls -d "$SDK"/build-tools/*/d8 | sort -V | tail -1)}

W=$OUT/core-libart-java
rm -rf "$W"
mkdir -p "$W/classes" "$W/empty" "$W/dex" "$W/res" "$W/jarjar"

# Sources. Files that mention com.ibm.icu get jarjar's rename in a copy
# (ICU4J itself and the few libcore classes that use it).
(cd "$LIBCORE" && find dalvik dex dom json luni xml libart -path '*/src/main/java/*' \
   -name '*.java' 2>/dev/null | sed "s|^|$LIBCORE/|") > "$W/all.txt"
ls -d "$ICU4J"/main/classes/*/src | grep -v -e /localespi/ -e /charset/ -e /translit/ \
  > "$W/icu4j-dirs.txt"
while read -r d; do find "$d" -name '*.java'; done < "$W/icu4j-dirs.txt" >> "$W/all.txt"
: > "$W/srcs.txt"
n=0
while read -r f; do
  if grep -q 'com\.ibm\.icu' "$f"; then
    n=$((n + 1))
    mkdir -p "$W/jarjar/$n"
    c="$W/jarjar/$n/$(basename "$f")"
    sed 's/com\.ibm\.icu/android.icu/g' "$f" > "$c"
    echo "\"$c\"" >> "$W/srcs.txt"
  else
    echo "\"$f\"" >> "$W/srcs.txt"
  fi
done < "$W/all.txt"

javac -source 8 -target 8 -bootclasspath "$W/empty" -encoding UTF-8 -nowarn \
  -Xmaxerrs 50 -Xlint:-options -d "$W/classes" @"$W/srcs.txt" 2>&1 | grep -v '^Note:' || true
[ -f "$W/classes/java/lang/Object.class" ] || { echo "javac failed" >&2; exit 1; }
(cd "$W/classes" && jar cf "$W/core-libart-classes.jar" .)
"$D8" --release --min-api 23 --output "$W/dex" "$W/core-libart-classes.jar"

# Resources (non-Java files under src/main/{java,resources}), as in
# JavaLibrary.mk, and ICU4J's under com/ibm/icu (renamed like the classes).
copy_res() {  # SRC_DIR DEST_PREFIX
  (cd "$1" && find . -type f ! -name '*.java' ! -name '*.html' \
     -exec sh -c 'mkdir -p "$0/$(dirname "$1")" && cp "$1" "$0/$1"' "$2" {} \;)
}
for d in dalvik dex dom json luni xml libart; do
  for s in java resources; do
    [ -d "$LIBCORE/$d/src/main/$s" ] && copy_res "$LIBCORE/$d/src/main/$s" "$W/res"
  done
done
while read -r d; do
  [ -d "$d/com/ibm/icu" ] || continue
  mkdir -p "$W/res/android/icu"
  copy_res "$d/com/ibm/icu" "$W/res/android/icu"
done < "$W/icu4j-dirs.txt"
# adjust_icudt_path.mk: ICU4J reads ICU4C's data file.
sed '/\.dataPath =/s|$| /system/usr/icu|' \
  "$ICU4J/main/classes/core/src/com/ibm/icu/ICUConfig.properties" \
  > "$W/res/android/icu/ICUConfig.properties"
cp "$W"/dex/classes*.dex "$W/res/"
rm -f "$OUT/core-libart.jar"
(cd "$W/res" && zip -qr "$OUT/core-libart.jar" .)
echo "built $OUT/core-libart.jar ($(ls "$W"/dex/classes*.dex | wc -l | tr -d ' ') dex files)"
