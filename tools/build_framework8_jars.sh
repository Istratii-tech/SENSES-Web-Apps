#!/bin/sh
# Builds the boot class path jars of the Android 8.1 framework (AOSP
# android-8.1.0_r81) for ART 8.1 from source, as tools/build_framework6_jars.sh
# does for 6.0.1: javac source/target 8 -> d8 --min-api 27 (dex 038, native
# multidex: classes.dex, classes2.dex, ... in one jar) -> jar. core-oj and
# core-libart are topic art8's; javac compiles against their classes.
#
#   build_framework8_jars.sh OUT_DIR core
#       core-all-java/core-all-classes.jar   javac only, never dexed: libcore 8.1's
#                          core-all (openjdk_java_files, non_openjdk_java_files,
#                          android_icu4j, the lambda stubs), the class path of
#                          everything below until topic art8's classes are there
#                          (CORE8_CLASSES=jar:jar uses those instead)
#   build_framework8_jars.sh OUT_DIR libs
#       conscrypt.jar      external/conscrypt (common, platform, NativeConstants.java
#                          from BoringSSL's headers; org.conscrypt -> com.android.org.conscrypt)
#       okhttp.jar         external/okhttp (okhttp, urlconnection, android-support, okio,
#                          android/; com.squareup -> com.android, okio -> com.android.okhttp.okio)
#       bouncycastle.jar   bcprov without asn1/ocsp and AndroidDigestFactoryBouncyCastle
#                          (org.bouncycastle -> com.android.org.bouncycastle)
#       ext.jar            external/{nist-sip,tagsoup} + libphonenumber-platform
#                          (com.google -> com.android) with its data
#       apache-xml.jar     external/apache-xml
#   build_framework8_jars.sh OUT_DIR framework [GENERATED.java ...]
#       framework.jar      module "framework" of frameworks/base/Android.mk: every
#                          .java of FRAMEWORKS_BASE_SUBDIRS, its .aidl list, .logtags,
#                          core/proto (javastream), framework-protos (javanano of
#                          proto/src with its runtime, jarjar'd), the HIDL Java it links
#                          statically (hidl-gen), and the generated .java given (R.java etc.)
#   build_framework8_jars.sh OUT_DIR opt
#       legacy-test.jar, voip-common.jar, ims-common.jar, telephony-common.jar
#       (with its nano protos and radio HIDL), apache-xml's sibling
#       org.apache.http.legacy.boot.jar, android.hidl.base-V1.0-java.jar,
#       android.hidl.manager-V1.0-java.jar
#
# "libs" needs "core", "framework" needs "libs", "opt" needs "framework".
# Each jar NAME leaves NAME-java/NAME-classes.jar (the javac output) for
# compiling against it. javac runs with empty source paths; jarjar is done on
# the sources (see build_framework6_jars.sh).
# Env: ANDROID_SDK (default ~/Library/Android/sdk), D8, AIDL (default: next
# to D8), HOST (the host tools: aprotoc, protoc-gen-javastream, hidl-gen;
# default build/fw8/host), FW8_MIN_API (27).
set -e
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
what=$2
shift 2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
A81=$ROOT/third_party/aosp81
FWB=$A81/frameworks/base
SDK=${ANDROID_SDK:-$HOME/Library/Android/sdk}
D8=${D8:-$(ls -d "$SDK"/build-tools/*/d8 | sort -V | tail -1)}
AIDL=${AIDL:-$(dirname "$D8")/aidl}
HOST=${HOST:-$ROOT/build/fw8/host}
MIN_API=${FW8_MIN_API:-27}
nl='
'

# The boot class path javac and d8 compile against.
CORE=${CORE8_CLASSES:-$OUT/core-all-java/core-all-classes.jar}

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

# dex_jar NAME CLASSES_JAR LIBJARS [RESOURCE_DIR]: d8 CLASSES_JAR against the
# core classes and LIBJARS (colon-separated) into OUT/NAME.jar with the
# resources.
dex_jar() {
  name=$1 cls=$2 libpath=$3 res=$4
  rm -rf "$W/dex-$name" "$W/jar-$name"
  mkdir -p "$W/dex-$name" "$W/jar-$name"
  set --
  old=$IFS; IFS=:
  for l in $CORE $libpath; do set -- "$@" --lib "$l"; done
  IFS=$old
  "$D8" --release --min-api "$MIN_API" "$@" --output "$W/dex-$name" "$cls"
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
# as in the AOSP build (INCLUDE_DIRS newline-separated). The SDK's aidl
# wants the file's own package root among the import paths (AOSP 8.1's did
# not; frameworks/native's .aidl files have theirs in no include list), so
# that is added, from the normalized path and its package line.
aidl_all() {
  o=$1 incs=$2; shift 2
  old=$IFS; IFS=$nl
  inc=
  for d in $incs; do inc="$inc$nl-I$d"; done
  for f in "$@"; do
    f=$(cd "$(dirname "$f")" && pwd)/$(basename "$f")
    pkg=$(sed -n 's/^ *package *\([A-Za-z0-9_.]*\) *;.*/\1/p' "$f" | head -1 | tr . /)
    root=${f%/"$pkg"/*}
    # shellcheck disable=SC2086
    "$AIDL" --min_sdk_version="$MIN_API" $inc -I"$root" -o"$o" "$f"
  done
  IFS=$old
}

# hidl_java OUT_DIR LANGUAGE FQNAME...: hidl-gen -Ljava / -Ljava-constants of
# HIDL packages or types (android.hardware.* from hardware/interfaces,
# android.hidl.* from system/libhidl/transport).
hidl_java() {
  o=$1 lang=$2; shift 2
  for fq in "$@"; do
    "$HOST/hidl-gen" -o "$o" -L"$lang" -randroid.hardware:"$A81/hardware/interfaces" \
      -randroid.hidl:"$A81/system/libhidl/transport" "$fq"
  done
}

# nano_java OUT_DIR PROTO_ROOT PARAMS FILE...: javanano classes of FILEs
# (paths below PROTO_ROOT, the import root).
nano_java() {
  o=$1 r=$2 p=$3; shift 3
  mkdir -p "$o"
  (cd "$r" && "$HOST/aprotoc" -I. -I"$A81/external/protobuf/src" \
     --javanano_out="${p:+$p:}$o" "$@")
}

# FRAMEWORKS_BASE_SUBDIRS (build/make/core/pathmap.mk in AOSP 8.1), one per
# line (paths may contain spaces), and the .aidl include path of module
# framework (FRAMEWORKS_BASE_JAVA_SRC_DIRS and LOCAL_AIDL_INCLUDES).
FW_DIRS=$(for d in core graphics location media media/mca/effect media/mca/filterfw \
                   media/mca/filterpacks drm opengl sax telecomm telephony wifi lowpan \
                   keystore rs; do
            echo "$FWB/$d/java"; done)
FW_AIDL_INCS="$FW_DIRS
$FWB/packages/services/PacProcessor
$FWB/packages/services/Proxy
$A81/frameworks/native/aidl/binder
$A81/frameworks/av/camera/aidl
$A81/frameworks/av/drm/libmediadrm/aidl
$A81/frameworks/native/aidl/gui
$A81/system/netd/server/binder
$A81/system/update_engine/binder_bindings"

build_core() {
  W=$OUT/core-all-java; rm -rf "$W"; mkdir -p "$W"
  L=$A81/libcore
  { for v in openjdk_java_files non_openjdk_java_files openjdk_lambda_stub_files; do
      python3 "$ROOT/tools/mk_expand.py" $v "$L/" -- "$L/openjdk_java_files.mk" \
        "$L/non_openjdk_java_files.mk"
    done
    find "$A81/external/icu/android_icu4j/src/main/java" -name '*.java'
  } | sort -u | sed 's|.*|"&"|' > "$W/srcs.txt"
  rm -rf "$W/classes" "$W/empty"; mkdir -p "$W/classes" "$W/empty"
  javac -J-Xmx4g -source 8 -target 8 -bootclasspath "$W/empty" -sourcepath "$W/empty" \
    -encoding UTF-8 -nowarn -Xlint:-options -d "$W/classes" @"$W/srcs.txt" > "$W/javac.log" 2>&1 || {
      grep -v '^Note:' "$W/javac.log" >&2; echo "javac failed for core-all" >&2; exit 1; }
  (cd "$W/classes" && jar cf "$W/core-all-classes.jar" .)
  echo "built $W/core-all-classes.jar ($(wc -l < "$W/srcs.txt" | tr -d ' ') files, for javac only)"
}

build_libs() {
  # conscrypt: NativeConstants.java comes from a host program built against
  # BoringSSL's headers (conscrypt_generate_constants).
  W=$OUT/conscrypt-java; rm -rf "$W"; mkdir -p "$W/src" "$W/gen/org/conscrypt"
  C=$A81/external/conscrypt
  c++ -I"$A81/external/boringssl/src/include" -o "$W/generate_constants" \
    "$C/constants/src/gen/cpp/generate_constants.cpp"
  "$W/generate_constants" > "$W/gen/org/conscrypt/NativeConstants.java"
  for d in common/src/main/java platform/src/main/java; do
    jarjar_copy "$C/$d" "$W/src" org.conscrypt com.android.org.conscrypt
  done
  jarjar_copy "$W/gen" "$W/src" org.conscrypt com.android.org.conscrypt
  srcs "$W/src" > "$W/srcs.txt"
  javac_lib conscrypt
  dex_jar conscrypt "$W/conscrypt-classes.jar" ""

  # okhttp (external/okhttp/Android.mk, okhttp_system_src_files).
  W=$OUT/okhttp-java; rm -rf "$W"; mkdir -p "$W/a" "$W/src"
  K=$A81/external/okhttp
  for d in okhttp/src/main/java okhttp-urlconnection/src/main/java \
           okhttp-android-support/src/main/java okio/okio/src/main/java; do
    (cd "$K/$d" && find . -name '*.java' -exec sh -c \
       'mkdir -p "$0/$(dirname "$1")" && cp "$1" "$0/$1"' "$W/a" {} \;)
  done
  find "$W/a" -name Platform.java -exec rm {} +
  (cd "$K/android/main/java" && find . -name '*.java' -exec sh -c \
     'mkdir -p "$0/$(dirname "$1")" && cp "$1" "$0/$1"' "$W/a" {} \;)
  jarjar_copy "$W/a" "$W/b" com.squareup com.android
  jarjar_copy "$W/b" "$W/src" okio com.android.okhttp.okio
  srcs "$W/src" > "$W/srcs.txt"
  javac_lib okhttp
  dex_jar okhttp "$W/okhttp-classes.jar" ""

  # bouncycastle (android_bcprov_src_files).
  W=$OUT/bouncycastle-java; rm -rf "$W"; mkdir -p "$W/src"
  jarjar_copy "$A81/external/bouncycastle/bcprov/src/main/java" "$W/src" \
    org.bouncycastle com.android.org.bouncycastle
  rm -r "$W/src/org/bouncycastle/crypto/digests/AndroidDigestFactoryBouncyCastle.java" \
        "$W/src/org/bouncycastle/asn1/ocsp"
  srcs "$W/src" > "$W/srcs.txt"
  javac_lib bouncycastle
  dex_jar bouncycastle "$W/bouncycastle-classes.jar" ""

  # ext (frameworks/base/Android.mk, ext_dirs) with libphonenumber-platform
  # (jarjar com.google -> com.android; its data files move along).
  W=$OUT/ext-java; rm -rf "$W"; mkdir -p "$W/res" "$W/pn" "$W/pnres"
  E=$A81/external
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
  X=$A81/external/apache-xml/src/main/java
  srcs "$X" > "$W/srcs.txt"
  resources "$W/res" "$X"
  javac_lib apache-xml
  dex_jar apache-xml "$W/apache-xml-classes.jar" "" "$W/res"
}

# LOCAL_JAVA_LIBRARIES of module framework.
FW_LIBS() {
  echo "$(classes conscrypt):$(classes okhttp):$(classes bouncycastle):$(classes ext)"
}

build_framework() {
  W=$OUT/framework-java; rm -rf "$W"
  mkdir -p "$W/aidl" "$W/logtags" "$W/res" "$W/proto" "$W/nano" "$W/hidl"
  for f in "$@"; do echo "\"$(cd "$(dirname "$f")" && pwd)/$(basename "$f")\""; done > "$W/gen.txt"
  # The .aidl files of module framework (listed before its LOCAL_MODULE;
  # some are in other projects: ../native, ../av, ../../system).
  awk '/^LOCAL_MODULE := framework$/ { exit } /\.aidl/ { sub(/\\$/, "", $1); print $1 }' \
    "$FWB/Android.mk" | sed "s|^|$FWB/|" > "$W/aidl.txt"
  old=$IFS; IFS=$nl
  # shellcheck disable=SC2046
  aidl_all "$W/aidl" "$FW_AIDL_INCS$nl$FWB/lowpan/java" $(cat "$W/aidl.txt")
  IFS=$old
  sed -n 's/^ *\(core\/java\/.*\.logtags\).*/\1/p' "$FWB/Android.mk" | while read -r f; do
    pkg=$(sed -n 's/^option java_package \(.*\);*$/\1/p' "$FWB/$f" | tr -d ';' | tr . /)
    python3 "$ROOT/tools/java_event_log_tags.py" "$FWB/$f" "$W/logtags/$pkg/$(basename "$f" .logtags).java"
  done
  # core/proto: LOCAL_PROTOC_OPTIMIZE_TYPE := stream (field ids for
  # android.util.proto.ProtoOutputStream), imports relative to the tree top.
  (cd "$A81" && "$HOST/aprotoc" --plugin=protoc-gen-javastream="$HOST/protoc-gen-javastream" \
     -I. -Iexternal/protobuf/src --javastream_out="$W/proto" \
     $(cd "$A81" && find frameworks/base/core/proto -name '*.proto' | sort))
  # framework-protos (frameworks/base/proto): javanano with its runtime,
  # com.google.protobuf.nano -> com.android.framework.protobuf.nano.
  nano_java "$W/nano-a" "$FWB/proto/src" "" $(cd "$FWB/proto/src" && find . -name '*.proto' | sort)
  cp -R "$A81/external/protobuf/javanano/src/main/java/." "$W/nano-a/"
  jarjar_copy "$W/nano-a" "$W/nano" com.google.protobuf.nano com.android.framework.protobuf.nano
  # HIDL: the *-java-constants static libraries and hardware/interfaces/cas/1.0/CasHal.mk
  # (android.hardware.cas@1.0 and android.hidl.base@1.0's Java).
  hidl_java "$W/hidl" java-constants android.hardware.health@1.0 android.hardware.thermal@1.0 \
    android.hardware.tv.input@1.0 android.hardware.usb@1.0 android.hardware.usb@1.1 \
    android.hardware.vibrator@1.0 android.hardware.vibrator@1.1 android.hardware.wifi@1.0
  hidl_java "$W/hidl" java android.hidl.base@1.0 android.hardware.cas@1.0
  { echo "$FW_DIRS" | while read -r d; do srcs "$d"; done
    srcs "$W/aidl" "$W/logtags" "$W/proto" "$W/nano" "$W/hidl"
    cat "$W/gen.txt"
  } > "$W/srcs.txt"
  javac_lib framework "$(FW_LIBS)"
  # "List of classes and interfaces which should be loaded by the Zygote."
  cp "$FWB/config/preloaded-classes" "$W/res/" 2>/dev/null || true
  dex_jar framework "$W/framework-classes.jar" "$(FW_LIBS)" "$W/res"
}

# opt_lib NAME DIR CLASSPATH [EXTRA_SRC_DIR...]: a frameworks/opt module:
# src/java's .java, its I*.aidl and .logtags (all-Iaidl-files-under,
# all-logtags-files-under) and the extra (generated) sources.
opt_lib() {
  name=$1 dir=$2 cp=$3; shift 3
  W=$OUT/$name-java; mkdir -p "$W/aidl" "$W/logtags"
  old=$IFS; IFS=$nl
  # shellcheck disable=SC2046
  aidl_all "$W/aidl" "$dir/src/java$nl$FW_AIDL_INCS" $(find "$dir/src/java" -name 'I*.aidl' | sort)
  IFS=$old
  find "$dir/src/java" -name '*.logtags' | while read -r f; do
    pkg=$(sed -n 's/^option java_package \(.*\);*$/\1/p' "$f" | tr -d ';' | tr . /)
    python3 "$ROOT/tools/java_event_log_tags.py" "$f" "$W/logtags/$pkg/$(basename "$f" .logtags).java"
  done
  srcs "$dir/src/java" "$W/aidl" "$W/logtags" "$@" > "$W/srcs.txt"
  javac_lib "$name" "$cp"
  dex_jar "$name" "$W/$name-classes.jar" "$cp"
}

build_opt() {
  fw="$(FW_LIBS):$(classes framework)"
  # legacy-test (frameworks/base/legacy-test): junit.framework, android.test.
  W=$OUT/legacy-test-java; rm -rf "$W"; mkdir -p "$W"
  srcs "$FWB/legacy-test/src" > "$W/srcs.txt"
  javac_lib legacy-test "$fw"
  dex_jar legacy-test "$W/legacy-test-classes.jar" "$fw"

  rm -rf "$OUT/voip-common-java" "$OUT/ims-common-java" "$OUT/telephony-common-java"
  opt_lib voip-common "$A81/frameworks/opt/net/voip" "$fw"
  opt_lib ims-common "$A81/frameworks/opt/net/ims" "$fw"
  # telephony-common: + nano protos of proto/ (store_unknown_fields,
  # enum_style=java) with the nano runtime (jarjar to
  # com.android.internal.telephony.protobuf.nano) and the radio HIDL Java
  # (android.hardware.radio-V1.1-java-static, radio.deprecated-V1.0-java-static).
  # LOCAL_JAVA_LIBRARIES has services (system_server's) for one constant,
  # NetworkStatsService.VT_INTERFACE: compiled against a stub with it.
  W=$OUT/telephony-common-java; mkdir -p "$W/stub/com/android/server/net" "$W/stubc" "$W/hidl"
  T=$A81/frameworks/opt/telephony
  nano_java "$W/nano-a" "$T/proto" store_unknown_fields=true,enum_style=java \
    $(cd "$T/proto" && find . -name '*.proto' | sort)
  cp -R "$A81/external/protobuf/javanano/src/main/java/." "$W/nano-a/"
  jarjar_copy "$W/nano-a" "$W/nano" com.google.protobuf.nano com.android.internal.telephony.protobuf.nano
  hidl_java "$W/hidl" java android.hardware.radio@1.0 android.hardware.radio@1.1 \
    android.hardware.radio.deprecated@1.0
  grep -h 'VT_INTERFACE =' "$FWB/services/core/java/com/android/server/net/NetworkStatsService.java" |
    sed 's/^/package com.android.server.net; public class NetworkStatsService { /; s/$/ }/' \
    > "$W/stub/com/android/server/net/NetworkStatsService.java"
  javac -source 8 -target 8 -Xlint:-options -bootclasspath "$CORE" -d "$W/stubc" \
    "$W/stub/com/android/server/net/NetworkStatsService.java"
  opt_lib telephony-common "$T" "$fw:$(classes voip-common):$(classes ims-common):$W/stubc" \
    "$W/nano" "$W/hidl"

  # org.apache.http.legacy.boot (external/apache-http/Android.mk): src and
  # android, against the SDK (here: the framework).
  W=$OUT/org.apache.http.legacy.boot-java; rm -rf "$W"; mkdir -p "$W"
  srcs "$A81/external/apache-http/src" "$A81/external/apache-http/android" > "$W/srcs.txt"
  javac_lib org.apache.http.legacy.boot "$fw"
  dex_jar org.apache.http.legacy.boot "$W/org.apache.http.legacy.boot-classes.jar" "$fw"

  # android.hidl.base-V1.0-java, android.hidl.manager-V1.0-java
  # (system/libhidl/transport/{base,manager}/1.0/Android.mk), against the framework.
  W=$OUT/android.hidl.base-V1.0-java-java; rm -rf "$W"; mkdir -p "$W/hidl"
  hidl_java "$W/hidl" java android.hidl.base@1.0
  srcs "$W/hidl" > "$W/srcs.txt"
  javac_lib android.hidl.base-V1.0-java "$fw"
  dex_jar android.hidl.base-V1.0-java "$W/android.hidl.base-V1.0-java-classes.jar" "$fw"
  base=$W/android.hidl.base-V1.0-java-classes.jar
  W=$OUT/android.hidl.manager-V1.0-java-java; rm -rf "$W"; mkdir -p "$W/hidl"
  hidl_java "$W/hidl" java android.hidl.manager@1.0
  srcs "$W/hidl" > "$W/srcs.txt"
  javac_lib android.hidl.manager-V1.0-java "$fw:$base"
  dex_jar android.hidl.manager-V1.0-java "$W/android.hidl.manager-V1.0-java-classes.jar" "$fw:$base"
}

case $what in
  core) build_core ;;
  libs) build_libs ;;
  framework) build_framework "$@" ;;
  opt) build_opt ;;
  *) echo "usage: $0 OUT_DIR core|libs|framework|opt [GENERATED.java ...]" >&2; exit 2 ;;
esac
