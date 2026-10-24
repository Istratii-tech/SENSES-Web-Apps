#!/bin/sh
# Builds the boot class path jars of AOSP 4.4.4's framework from source, the
# way the AOSP build does (frameworks/base/Android.mk and the libraries it
# compiles against), each compiled like tools/build_core_jar.sh builds
# core.jar: javac source/target 8 -> d8 --min-api 19 with desugaring -> jar
# with one classes.dex (Dalvik loads one dex per jar).
#
#   build_framework_jar.sh OUT_DIR libs
#       conscrypt.jar     libcore/crypto (jarjar: org.conscrypt -> com.android.org.conscrypt)
#       bouncycastle.jar  external/bouncycastle bcprov (org.bouncycastle -> com.android.org.bouncycastle)
#       okhttp.jar        external/okhttp (com.squareup -> com.android)
#       core-junit.jar    external/junit (core-junit-files + junit.extensions)
#       ext.jar           external/{nist-sip,apache-http,tagsoup,libphonenumber}
#   build_framework_jar.sh OUT_DIR framework [GENERATED.java ...]
#       framework.jar     android.*        } module framework-base: every .java
#       framework2.jar    com.*, javax.*   } under FRAMEWORKS_BASE_SUBDIRS, its
#                         .aidl list (SDK aidl tool), .logtags, the generated
#                         .java given (R.java etc. from framework-res), plus
#                         framework/java-arcwasm (but not arcwasm/server and
#                         arcwasm/webview: services.jar, mk/topic-activity.mk) and
#                         framework/java-stubs
#
# OUT_DIR holds core-java/ (tools/build_core_jar.sh) already; "framework"
# needs "libs" built. Each jar NAME leaves NAME-java/NAME-classes.jar (the
# javac output) for compiling against it.
# javac runs with empty source paths: anything not compiled or on the class
# path is an error rather than a silent pull-in.
# jarjar is done on the sources (the rule's prefix rewritten everywhere it
# starts a name), which is what jarjar does to classes and class-name strings.
# Env: ANDROID_SDK (default ~/Library/Android/sdk), D8, AIDL (default: next to D8).
set -e
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
what=$2
shift 2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
A44=$ROOT/third_party/aosp44
FWB=$A44/frameworks/base
SDK=${ANDROID_SDK:-$HOME/Library/Android/sdk}
D8=${D8:-$(ls -d "$SDK"/build-tools/*/d8 | sort -V | tail -1)}
AIDL=${AIDL:-$(dirname "$D8")/aidl}
CORE=$OUT/core-java/core-classes.jar
[ -f "$CORE" ] || { echo "$CORE missing; build core.jar first" >&2; exit 1; }

# classes NAME: the javac output jar of library NAME.
classes() { echo "$OUT/$1-java/$1-classes.jar"; }

# javac_lib NAME CLASSPATH: compiles $W/srcs.txt (one quoted path per line:
# the checkout's path may contain spaces) into $W/NAME-classes.jar.
javac_lib() {
  rm -rf "$W/classes" "$W/empty"
  mkdir -p "$W/classes" "$W/empty"
  javac -J-Xmx3g -source 8 -target 8 -bootclasspath "$CORE" -sourcepath "$W/empty" \
    -cp "${2:-$W/empty}" -encoding UTF-8 -nowarn -Xlint:-options -Xmaxerrs 1000 \
    -d "$W/classes" @"$W/srcs.txt" > "$W/javac.log" 2>&1 || {
      grep -v '^Note:' "$W/javac.log" >&2; echo "javac failed for $1" >&2; exit 1; }
  grep -v '^Note:' "$W/javac.log" || true
  (cd "$W/classes" && jar cf "$W/$1-classes.jar" .)
}

# dex_jar NAME CLASSES_JAR LIBJARS [RESOURCE_DIR]: d8 CLASSES_JAR against
# core and LIBJARS (colon-separated) into OUT/NAME.jar with the resources.
dex_jar() {
  name=$1 cls=$2 libpath=$3 res=$4
  rm -rf "$W/dex-$name" "$W/jar-$name"
  mkdir -p "$W/dex-$name" "$W/jar-$name"
  set -- --lib "$CORE"
  old=$IFS; IFS=:
  for l in $libpath; do set -- "$@" --lib "$l"; done
  IFS=$old
  "$D8" --release --min-api 19 "$@" --output "$W/dex-$name" "$cls"
  [ -f "$W/dex-$name/classes2.dex" ] && { echo "$name: more than one dex" >&2; exit 1; }
  [ -n "$res" ] && cp -R "$res/." "$W/jar-$name/"
  cp "$W/dex-$name/classes.dex" "$W/jar-$name/"
  rm -f "$OUT/$name.jar"
  (cd "$W/jar-$name" && zip -qr "$OUT/$name.jar" .)
  echo "built $OUT/$name.jar ($(wc -c < "$W/dex-$name/classes.dex" | tr -d ' ') bytes of dex)"
}

# jarjar_copy SRC_DIR DEST_DIR FROM TO: copies the .java files under SRC_DIR
# with every name starting with package FROM moved to package TO.
jarjar_copy() {
  from=$(echo "$3" | sed 's/\./\\./g')
  (cd "$1" && find . -name '*.java') | while read -r f; do
    mkdir -p "$2/$(dirname "$f")"
    sed -E "s/(^|[^.A-Za-z0-9_])$from([.;\"])/\1$4\2/g" "$1/$f" > "$2/$f"
  done
}

# srcs DIR...: every .java file under DIRs, one quoted path per line.
srcs() { find "$@" -name '*.java' ! -name '.*' | sort | sed 's|.*|"&"|'; }

# resources DEST DIR...: copies the non-Java files under DIRs, as
# LOCAL_JAVA_RESOURCE_DIRS does.
resources() {
  d=$1; shift
  for r in "$@"; do
    (cd "$r" && find . -type f ! -name '*.java' ! -name '*.html' ! -name '.*' \
       -exec sh -c 'mkdir -p "$0/$(dirname "$1")" && cp "$1" "$0/$1"' "$d" {} \;)
  done
}

build_libs() {
  # conscrypt (libcore/JavaLibrary.mk), natives in libjavacrypto (not built).
  W=$OUT/conscrypt-java; rm -rf "$W"; mkdir -p "$W/src"
  jarjar_copy "$A44/libcore/crypto/src/main/java" "$W/src" org.conscrypt com.android.org.conscrypt
  srcs "$W/src" > "$W/srcs.txt"
  javac_lib conscrypt
  dex_jar conscrypt "$W/conscrypt-classes.jar" ""

  # bouncycastle (external/bouncycastle/Android.mk, android_bcprov_src_files).
  W=$OUT/bouncycastle-java; rm -rf "$W"; mkdir -p "$W/src"
  jarjar_copy "$A44/external/bouncycastle/bcprov/src/main/java" "$W/src" \
    org.bouncycastle com.android.org.bouncycastle
  rm "$W/src/org/bouncycastle/crypto/digests/AndroidDigestFactoryBouncyCastle.java"
  srcs "$W/src" > "$W/srcs.txt"
  javac_lib bouncycastle "$(classes conscrypt)"
  dex_jar bouncycastle "$W/bouncycastle-classes.jar" "$(classes conscrypt)"

  # okhttp (external/okhttp/Android.mk: the Android Platform.java).
  W=$OUT/okhttp-java; rm -rf "$W"; mkdir -p "$W/src" "$W/src-android"
  jarjar_copy "$A44/external/okhttp/src/main/java" "$W/src" com.squareup com.android
  jarjar_copy "$A44/external/okhttp/android/main/java" "$W/src-android" com.squareup com.android
  find "$W/src" -name Platform.java -exec rm {} +
  srcs "$W/src" "$W/src-android" > "$W/srcs.txt"
  javac_lib okhttp "$(classes conscrypt)"
  dex_jar okhttp "$W/okhttp-classes.jar" "$(classes conscrypt)"

  # core-junit (external/junit/Android.mk and Common.mk).
  W=$OUT/core-junit-java; rm -rf "$W"; mkdir -p "$W"
  J=$A44/external/junit
  { srcs "$J/src/junit/extensions"
    "$ROOT/tools/mk_srcs.py" "$J/Common.mk" core-junit-files "$J/" | sed 's|.*|"&"|'
  } > "$W/srcs.txt"
  javac_lib core-junit
  dex_jar core-junit "$W/core-junit-classes.jar" ""

  # ext (frameworks/base/Android.mk, ext_dirs; resources: libphonenumber data).
  W=$OUT/ext-java; rm -rf "$W"; mkdir -p "$W/res"
  E=$A44/external
  srcs "$E/nist-sip/java" "$E/apache-http/src" "$E/tagsoup/src" \
       "$E/libphonenumber/java/src" > "$W/srcs.txt"
  resources "$W/res" "$E/libphonenumber/java/src"
  javac_lib ext "$(classes conscrypt)"
  dex_jar ext "$W/ext-classes.jar" "" "$W/res"
}

build_framework() {
  W=$OUT/framework-java; rm -rf "$W"; mkdir -p "$W/aidl" "$W/logtags" "$W/res"
  for f in "$@"; do echo "\"$(cd "$(dirname "$f")" && pwd)/$(basename "$f")\""; done > "$W/gen.txt"
  # FRAMEWORKS_BASE_SUBDIRS (build/core/pathmap.mk in AOSP 4.4.4), one
  # per line (paths may contain spaces).
  dirs=$(for d in core graphics location media media/mca/effect media/mca/filterfw \
                  media/mca/filterpacks drm opengl sax telephony wifi keystore; do
           echo "$FWB/$d/java"; done)
  nl='
'
  # The .aidl files of module framework-base, compiled one by one with every
  # source directory on the import path (LOCAL_AIDL_INCLUDES), plus the
  # directories of the three in packages/services (the SDK's aidl insists).
  awk '/^LOCAL_MODULE := framework-base/ { exit } /\.aidl/ { sub(/\\$/, "", $1); print $1 }' \
    "$FWB/Android.mk" > "$W/aidl.txt"
  while read -r f; do
    old=$IFS; IFS=$nl
    set --
    for d in $dirs; do set -- "$@" "-I$d"; done
    IFS=$old
    for d in PacProcessor Proxy; do set -- "$@" "-I$FWB/packages/services/$d"; done
    IFS=$old
    "$AIDL" --min_sdk_version=19 "$@" -o"$W/aidl" "$FWB/$f"
  done < "$W/aidl.txt"
  for f in core/java/android/content/EventLogTags.logtags \
           core/java/android/speech/tts/EventLogTags.logtags \
           core/java/android/webkit/EventLogTags.logtags; do
    pkg=$(dirname "${f#core/java/}")
    python3 "$ROOT/tools/java_event_log_tags.py" "$FWB/$f" "$W/logtags/$pkg/EventLogTags.java"
  done
  { echo "$dirs" | while read -r d; do srcs "$d"; done
    srcs "$W/aidl" "$W/logtags" "$ROOT/framework/java-arcwasm" "$ROOT/framework/java-stubs" |
      grep -v -e '/framework/java-arcwasm/arcwasm/server/' -e '/framework/java-arcwasm/arcwasm/webview/'
    cat "$W/gen.txt"
  } > "$W/srcs.txt"
  libs="$(classes conscrypt):$(classes bouncycastle):$(classes okhttp):$(classes core-junit):$(classes ext)"
  javac_lib framework-base "$libs"
  # Split like frameworks/base/Android.mk: framework = android.*,
  # framework2 = com.*, javax.* (here: everything else, arcwasm.* too).
  rm -rf "$W/c1" "$W/c2"; mkdir -p "$W/c1" "$W/c2"
  mv "$W/classes/android" "$W/c1/"
  (cd "$W/classes" && cp -R . "$W/c2/")
  (cd "$W/c1" && jar cf "$W/framework-classes.jar" .)
  (cd "$W/c2" && jar cf "$W/framework2-classes.jar" .)
  # "List of classes and interfaces which should be loaded by the Zygote."
  cp "$FWB/preloaded-classes" "$W/res/"
  dex_jar framework "$W/framework-classes.jar" "$libs:$W/framework2-classes.jar" "$W/res"
  dex_jar framework2 "$W/framework2-classes.jar" "$libs:$W/framework-classes.jar"
}

case $what in
  libs) build_libs ;;
  framework) build_framework "$@" ;;
  *) echo "usage: $0 OUT_DIR libs|framework [GENERATED.java ...]" >&2; exit 2 ;;
esac
