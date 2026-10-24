#!/bin/sh
# Builds the boot class path jars of the Android 6.0.1 framework (AOSP
# android-6.0.1_r81) for ART from source, the way the AOSP build does each
# module, compiled like tools/build_core_libart_jar.sh builds core-libart.jar:
# javac source/target 8 -> d8 --min-api 23 with desugaring (native multidex:
# classes.dex, classes2.dex, ... in one jar, which ART loads) -> jar.
#
#   build_framework6_jars.sh OUT_DIR libs
#       conscrypt.jar      external/conscrypt (src/main, src/platform, NativeConstants.java
#                          generated from BoringSSL's headers; org.conscrypt -> com.android.org.conscrypt)
#       okhttp.jar         external/okhttp (+ okio, urlconnection, android-support;
#                          com.squareup -> com.android, okio -> com.android.okhttp.okio)
#       core-junit.jar     external/junit (core-junit-files + junit.extensions)
#       bouncycastle.jar   external/bouncycastle bcprov (org.bouncycastle -> com.android.org.bouncycastle)
#       ext.jar            external/{nist-sip,tagsoup} + libphonenumber-platform
#                          (com.google -> com.android) with its data
#       apache-xml.jar     external/apache-xml
#   build_framework6_jars.sh OUT_DIR framework [GENERATED.java ...]
#       framework.jar      module "framework" of frameworks/base/Android.mk: every
#                          .java under FRAMEWORKS_BASE_SUBDIRS, its .aidl list (SDK
#                          aidl tool), .logtags, the generated .java given (R.java
#                          etc. from framework-res)
#   build_framework6_jars.sh OUT_DIR opt
#       voip-common.jar, ims-common.jar, telephony-common.jar  frameworks/opt/...
#       org.apache.http.legacy.boot.jar                        external/apache-http
#
# OUT_DIR holds core-libart-java/ (tools/build_core_libart_jar.sh) already;
# "framework" needs "libs", "opt" needs "framework". Each jar NAME leaves
# NAME-java/NAME-classes.jar (the javac output) for compiling against it.
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
A60=$ROOT/third_party/aosp60
FWB=$A60/frameworks/base
SDK=${ANDROID_SDK:-$HOME/Library/Android/sdk}
D8=${D8:-$(ls -d "$SDK"/build-tools/*/d8 | sort -V | tail -1)}
AIDL=${AIDL:-$(dirname "$D8")/aidl}
CORE=$OUT/core-libart-java/core-libart-classes.jar
[ -f "$CORE" ] || { echo "$CORE missing; build core-libart.jar first" >&2; exit 1; }
nl='
'

# classes NAME: the javac output jar of library NAME.
classes() { echo "$OUT/$1-java/$1-classes.jar"; }

# javac_lib NAME CLASSPATH: compiles $W/srcs.txt (one quoted path per line:
# the checkout's path may contain spaces) into $W/NAME-classes.jar.
javac_lib() {
  rm -rf "$W/classes" "$W/empty"
  mkdir -p "$W/classes" "$W/empty"
  javac -J-Xmx4g -source 8 -target 8 -bootclasspath "$CORE" -sourcepath "$W/empty" \
    -cp "${2:-$W/empty}" -encoding UTF-8 -nowarn -Xlint:-options -Xmaxerrs 1000 \
    -d "$W/classes" @"$W/srcs.txt" > "$W/javac.log" 2>&1 || {
      grep -v '^Note:' "$W/javac.log" >&2; echo "javac failed for $1" >&2; exit 1; }
  grep -v '^Note:' "$W/javac.log" || true
  (cd "$W/classes" && jar cf "$W/$1-classes.jar" .)
}

# dex_jar NAME CLASSES_JAR LIBJARS [RESOURCE_DIR]: d8 CLASSES_JAR against
# core-libart and LIBJARS (colon-separated) into OUT/NAME.jar with the
# resources. More than 64K methods make classes2.dex... (ART's multidex).
dex_jar() {
  name=$1 cls=$2 libpath=$3 res=$4
  rm -rf "$W/dex-$name" "$W/jar-$name"
  mkdir -p "$W/dex-$name" "$W/jar-$name"
  set -- --lib "$CORE"
  old=$IFS; IFS=:
  for l in $libpath; do set -- "$@" --lib "$l"; done
  IFS=$old
  "$D8" --release --min-api 23 "$@" --output "$W/dex-$name" "$cls"
  [ -n "$res" ] && cp -R "$res/." "$W/jar-$name/"
  cp "$W/dex-$name"/classes*.dex "$W/jar-$name/"
  rm -f "$OUT/$name.jar"
  (cd "$W/jar-$name" && zip -qr "$OUT/$name.jar" .)
  echo "built $OUT/$name.jar ($(ls "$W/dex-$name"/classes*.dex | wc -l | tr -d ' ') dex," \
       "$(cat "$W/dex-$name"/classes*.dex | wc -c | tr -d ' ') bytes)"
}

# jarjar_copy SRC_DIR DEST_DIR FROM TO: copies the .java files under SRC_DIR
# with every name starting with package FROM moved to package TO (dotted
# names, and slashed ones for resource paths: /com/google/ -> /com/android/).
jarjar_copy() {
  from=$(echo "$3" | sed 's/\./\\./g')
  sfrom=$(echo "$3" | sed 's|\.|/|g') sto=$(echo "$4" | sed 's|\.|/|g')
  (cd "$1" && find . -name '*.java') | while read -r f; do
    mkdir -p "$2/$(dirname "$f")"
    sed -E -e "s/(^|[^.A-Za-z0-9_])$from([.;\"])/\1$4\2/g" \
        -e "s|\"/$sfrom/|\"/$sto/|g" "$1/$f" > "$2/$f"
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
       ! -name '*.aidl' ! -name '*.logtags' \
       -exec sh -c 'mkdir -p "$0/$(dirname "$1")" && cp "$1" "$0/$1"' "$d" {} \;)
  done
}

# aidl_all OUT_DIR INCLUDE_DIRS FILE...: Java from .aidl, one call per file
# as in the AOSP build (INCLUDE_DIRS newline-separated).
aidl_all() {
  o=$1 incs=$2; shift 2
  old=$IFS; IFS=$nl
  inc=
  for d in $incs; do inc="$inc$nl-I$d"; done
  for f in "$@"; do
    # shellcheck disable=SC2086
    "$AIDL" --min_sdk_version=23 $inc -o"$o" "$f"
  done
  IFS=$old
}

# FRAMEWORKS_BASE_SUBDIRS (build/core/pathmap.mk in AOSP 6.0.1), one per line
# (paths may contain spaces). Also the .aidl include path of every module.
FW_DIRS=$(for d in core graphics location media media/mca/effect media/mca/filterfw \
                   media/mca/filterpacks drm opengl sax telecomm telephony wifi keystore rs; do
            echo "$FWB/$d/java"; done)
FW_AIDL_INCS="$FW_DIRS$nl$FWB/packages/services/PacProcessor$nl$FWB/packages/services/Proxy"

build_libs() {
  # conscrypt: NativeConstants.java comes from a host program built against
  # BoringSSL's headers (conscrypt_generate_constants).
  W=$OUT/conscrypt-java; rm -rf "$W"; mkdir -p "$W/src" "$W/gen/org/conscrypt"
  C=$A60/external/conscrypt
  c++ -I"$A60/external/boringssl/src/include" -o "$W/generate_constants" \
    "$C/src/gen/native/generate_constants.cc"
  "$W/generate_constants" > "$W/gen/org/conscrypt/NativeConstants.java"
  for d in src/main/java src/platform/java; do
    jarjar_copy "$C/$d" "$W/src" org.conscrypt com.android.org.conscrypt
  done
  jarjar_copy "$W/gen" "$W/src" org.conscrypt com.android.org.conscrypt
  srcs "$W/src" > "$W/srcs.txt"
  javac_lib conscrypt
  dex_jar conscrypt "$W/conscrypt-classes.jar" ""

  # okhttp (external/okhttp/Android.mk, okhttp_system_src_files).
  W=$OUT/okhttp-java; rm -rf "$W"; mkdir -p "$W/a" "$W/src"
  K=$A60/external/okhttp
  for d in okhttp/src/main/java okhttp-urlconnection/src/main/java \
           okhttp-android-support/src/main/java okio/okio/src/main/java android/main/java; do
    (cd "$K/$d" && find . -name '*.java' -exec sh -c \
       'mkdir -p "$0/$(dirname "$1")" && cp "$1" "$0/$1"' "$W/a" {} \;)
    [ "$d" = okio/okio/src/main/java ] && find "$W/a" -name Platform.java -exec rm {} +
  done
  jarjar_copy "$W/a" "$W/b" com.squareup com.android
  jarjar_copy "$W/b" "$W/src" okio com.android.okhttp.okio
  srcs "$W/src" > "$W/srcs.txt"
  javac_lib okhttp "$(classes conscrypt)"
  dex_jar okhttp "$W/okhttp-classes.jar" "$(classes conscrypt)"

  # core-junit (external/junit/Android.mk and Common.mk).
  W=$OUT/core-junit-java; rm -rf "$W"; mkdir -p "$W"
  J=$A60/external/junit
  { srcs "$J/src/junit/extensions"
    "$ROOT/tools/mk_srcs.py" "$J/Common.mk" core-junit-files "$J/" | sed 's|.*|"&"|'
  } > "$W/srcs.txt"
  javac_lib core-junit
  dex_jar core-junit "$W/core-junit-classes.jar" ""

  # bouncycastle (android_bcprov_src_files).
  W=$OUT/bouncycastle-java; rm -rf "$W"; mkdir -p "$W/src"
  jarjar_copy "$A60/external/bouncycastle/bcprov/src/main/java" "$W/src" \
    org.bouncycastle com.android.org.bouncycastle
  rm "$W/src/org/bouncycastle/crypto/digests/AndroidDigestFactoryBouncyCastle.java"
  srcs "$W/src" > "$W/srcs.txt"
  javac_lib bouncycastle "$(classes conscrypt)"
  dex_jar bouncycastle "$W/bouncycastle-classes.jar" "$(classes conscrypt)"

  # ext (frameworks/base/Android.mk, ext_dirs) with libphonenumber-platform
  # (jarjar com.google -> com.android; its data files move along).
  W=$OUT/ext-java; rm -rf "$W"; mkdir -p "$W/res" "$W/pn" "$W/pnres"
  E=$A60/external
  for d in libphonenumber/src geocoder/src internal/prefixmapper/src; do
    jarjar_copy "$E/libphonenumber/$d" "$W/pn" com.google com.android
    resources "$W/pnres" "$E/libphonenumber/$d"
  done
  [ -d "$W/pnres/com/google" ] && { mkdir -p "$W/res/com/android"; cp -R "$W/pnres/com/google/." "$W/res/com/android/"; }
  srcs "$E/nist-sip/java" "$E/tagsoup/src" "$W/pn" > "$W/srcs.txt"
  javac_lib ext
  dex_jar ext "$W/ext-classes.jar" "" "$W/res"

  # apache-xml (external/apache-xml/Android.mk), resources from src/main/java.
  W=$OUT/apache-xml-java; rm -rf "$W"; mkdir -p "$W/res"
  X=$A60/external/apache-xml/src/main/java
  srcs "$X" > "$W/srcs.txt"
  resources "$W/res" "$X"
  javac_lib apache-xml
  dex_jar apache-xml "$W/apache-xml-classes.jar" "" "$W/res"
}

FW_LIBS() {
  echo "$(classes conscrypt):$(classes okhttp):$(classes core-junit):$(classes bouncycastle):$(classes ext)"
}

build_framework() {
  W=$OUT/framework-java; rm -rf "$W"; mkdir -p "$W/aidl" "$W/logtags" "$W/res"
  for f in "$@"; do echo "\"$(cd "$(dirname "$f")" && pwd)/$(basename "$f")\""; done > "$W/gen.txt"
  # The .aidl files of module framework (listed before its LOCAL_MODULE).
  awk '/^LOCAL_MODULE := framework$/ { exit } /\.aidl/ { sub(/\\$/, "", $1); print $1 }' \
    "$FWB/Android.mk" | sed "s|^|$FWB/|" > "$W/aidl.txt"
  old=$IFS; IFS=$nl
  # shellcheck disable=SC2046
  aidl_all "$W/aidl" "$FW_AIDL_INCS" $(cat "$W/aidl.txt")
  IFS=$old
  for f in core/java/android/content/EventLogTags.logtags \
           core/java/android/speech/tts/EventLogTags.logtags \
           core/java/android/webkit/EventLogTags.logtags \
           core/java/com/android/internal/logging/EventLogTags.logtags; do
    pkg=$(dirname "${f#core/java/}")
    python3 "$ROOT/tools/java_event_log_tags.py" "$FWB/$f" "$W/logtags/$pkg/EventLogTags.java"
  done
  { echo "$FW_DIRS" | while read -r d; do srcs "$d"; done
    srcs "$W/aidl" "$W/logtags"
    cat "$W/gen.txt"
  } > "$W/srcs.txt"
  javac_lib framework "$(FW_LIBS)"
  # "List of classes and interfaces which should be loaded by the Zygote."
  cp "$FWB/preloaded-classes" "$W/res/"
  dex_jar framework "$W/framework-classes.jar" "$(FW_LIBS)" "$W/res"
}

# opt_lib NAME DIR CLASSPATH: a frameworks/opt module: src/java's .java, its
# I*.aidl and .logtags (all-Iaidl-files-under, all-logtags-files-under).
opt_lib() {
  name=$1 dir=$2 cp=$3
  W=$OUT/$name-java; rm -rf "$W"; mkdir -p "$W/aidl" "$W/logtags"
  old=$IFS; IFS=$nl
  # shellcheck disable=SC2046
  aidl_all "$W/aidl" "$dir/src/java$nl$FW_AIDL_INCS" $(find "$dir/src/java" -name 'I*.aidl' | sort)
  IFS=$old
  find "$dir/src/java" -name '*.logtags' | while read -r f; do
    pkg=$(sed -n 's/^option java_package \(.*\);*$/\1/p' "$f" | tr -d ';' | tr . /)
    python3 "$ROOT/tools/java_event_log_tags.py" "$f" "$W/logtags/$pkg/$(basename "$f" .logtags).java"
  done
  srcs "$dir/src/java" "$W/aidl" "$W/logtags" > "$W/srcs.txt"
  javac_lib "$name" "$cp"
  dex_jar "$name" "$W/$name-classes.jar" "$cp"
}

build_opt() {
  fw="$(FW_LIBS):$(classes framework)"
  opt_lib voip-common "$A60/frameworks/opt/net/voip" "$fw"
  opt_lib ims-common "$A60/frameworks/opt/net/ims" "$fw"
  opt_lib telephony-common "$A60/frameworks/opt/telephony" \
    "$fw:$(classes voip-common):$(classes ims-common)"
  # org.apache.http.legacy.boot (external/apache-http/Android.mk): src and
  # android, against conscrypt and the SDK (here: the framework).
  W=$OUT/org.apache.http.legacy.boot-java; rm -rf "$W"; mkdir -p "$W"
  srcs "$A60/external/apache-http/src" "$A60/external/apache-http/android" > "$W/srcs.txt"
  javac_lib org.apache.http.legacy.boot "$fw"
  dex_jar org.apache.http.legacy.boot "$W/org.apache.http.legacy.boot-classes.jar" "$fw"
}

case $what in
  libs) build_libs ;;
  framework) build_framework "$@" ;;
  opt) build_opt ;;
  *) echo "usage: $0 OUT_DIR libs|framework|opt [GENERATED.java ...]" >&2; exit 2 ;;
esac
