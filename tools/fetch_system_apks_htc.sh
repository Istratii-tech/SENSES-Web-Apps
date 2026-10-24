#!/bin/sh
# HTC Sense 5.5 (HTC One m7, Android 4.4.2) on the Dalvik stack (topic
# userspace): its launcher (Prism, with BlinkFeed), SystemUI and package
# installer from a firmware zip, into build/system-apkshtc/ for the HTC system
# image (mk/topic-userspace.mk):
#
#   app/*.apk              the apps, code deodexed where it is only in an .odex
#   framework/htc-*.jar    the classes of HTC's framework libraries that AOSP's
#                          framework lacks (com.htc.*, HTC's additions to
#                          android.*, its telephony), boot jars of the image
#                          (ro.arcwasm.bootclasspath.append)
#   framework/com.htc.resources.apk, framework/framework-res-htc.apk
#                          HTC's framework resources (packages 0x02, 0x03),
#                          added to every AssetManager (patches/aosp44)
#   app/com.htc.*.res.apk  resource-only packages of HTC's framework
#   build.prop             lines the image appends to /system/build.prop
#   missing.txt            the framework members HTC's code uses that ours
#                          lacks (below)
#
# On the way, in smali:
# - classes of framework/java-htc (stand-ins for HTC classes that cannot work
#   here, e.g. for their ARM natives) replace HTC's of the same name, or are
#   added (htc-fw.jar);
# - uses of framework members HTC added to AOSP classes become calls of
#   default-answering stubs (tools/htc_fix_missing.py);
# - a few fixes of code paths a device never takes (tools/htc_smali_fixes.py);
# - references to framework-res (0x01) by HTC's numbers become ours where they
#   differ (tools/remap_res_ids.py: code, resources.arsc, XML).
#
# Usage: tools/fetch_system_apks_htc.sh FIRMWARE_ZIP
#   FIRMWARE_ZIP  a flashable HTC One ROM zip with /system (tested:
#                 One_4.19.401.8_odexed.zip, 4.19.401.8 CL304164)
# Needs build/dalvik/framework-res/framework-res.apk and our boot jars (make
# dalvik), aapt and d8 (AAPT, D8, default the newest Android SDK
# build-tools'), javac, and java with SMALI_DIR (default build/smali-tools)
# holding baksmali-2.5.2.jar and smali-2.5.2.jar.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ZIP=$1
SMALI_DIR=${SMALI_DIR:-$ROOT/build/smali-tools}
AAPT=${AAPT:-$(ls -d "$HOME"/Library/Android/sdk/build-tools/*/aapt "$HOME"/Android/Sdk/build-tools/*/aapt 2>/dev/null | tail -1)}
D8=${D8:-$(ls -d "$HOME"/Library/Android/sdk/build-tools/*/d8 "$HOME"/Android/Sdk/build-tools/*/d8 2>/dev/null | tail -1)}
OUT=$ROOT/build/system-apkshtc
W=$ROOT/build/htc-work
DFW=$ROOT/build/dalvik/framework
# v registers only (tools/htc_fix_missing.py rewrites register lists).
baksmali() { cmd=$1; shift; java -jar "$SMALI_DIR/baksmali-2.5.2.jar" "$cmd" --parameter-registers false "$@"; }
smali() { java -jar "$SMALI_DIR/smali-2.5.2.jar" "$@"; }
remap() { python3 "$ROOT/tools/remap_res_ids.py" "$@"; }
# The apps: firmware path, and the name in /system/app.
APPS="priv-app/Prism:Prism priv-app/SystemUI:SystemUI app/PureC_PackageInstaller:PackageInstaller"
# HTC's framework libraries, each its own boot jar (one dex holds at most
# 64K methods): the jars whose classes AOSP's framework lacks go in.
HTC_JARS="htc-ext:HTCExtension,HTCCommonctrl htc-fw:framework,framework2,framework3,telephony-common,voip-common,com.htc.fusion.fx,com.htc.lockscreen.fusion,com.htc.android.bluetooth"
HTC_RES="com.htc.resources.apk framework-res-htc.apk"
# Resource-only packages in HTC's /system/framework, installed like apps:
# the home screen's and BlinkFeed's views (createPackageContext).
HTC_RES_PKGS="com.htc.android.home.res com.htc.weather.res com.htc.socialnetwork.res"

rm -rf "$W" "$OUT" && mkdir -p "$W/z" "$OUT/app" "$OUT/framework"
unzip -q -o "$ZIP" 'system/framework/*' 'system/priv-app/Prism.*' 'system/priv-app/SystemUI.*' \
    'system/app/PureC_PackageInstaller.*' 'system/build.prop' -d "$W/z"
SYS=$W/z/system
grep -m1 -E '^ro\.build\.description=' "$SYS/build.prop"

# Framework-res ids: HTC's -> ours.
dump_ids() {
  "$AAPT" dump resources "$1" | grep 'spec resource' |
    sed -E 's/.*spec resource (0x[0-9a-f]+) android:([^:]+):.*/\1 \2/'
}
dump_ids "$SYS/framework/framework-res.apk" > "$W/htc-ids.txt"
dump_ids "$ROOT/build/dalvik/framework-res/framework-res.apk" > "$W/our-ids.txt"
remap map "$W/htc-ids.txt" "$W/our-ids.txt" "$W/idmap.txt"

# Our boot jars, as smali: their classes and members.
for j in "$DFW"/*.jar; do
  baksmali d -a 19 -o "$W/ours/$(basename "$j" .jar)" "$j"
done
find "$W/ours" -name '*.smali' -exec awk 'FNR == 1 {print $NF}' {} + | sort -u \
  > "$W/our-classes.txt"

# The stand-ins, as smali.
mkdir -p "$W/java-htc/classes" "$W/java-htc/dex"
CP=$(ls "$DFW"/*-java/*-classes.jar | tr '\n' ':')
find "$ROOT/framework/java-htc" -name '*.java' | sed 's/.*/"&"/' > "$W/java-htc/srcs.txt"
javac -source 8 -target 8 -Xlint:-options -encoding UTF-8 -bootclasspath "$CP" \
    -d "$W/java-htc/classes" @"$W/java-htc/srcs.txt"
(cd "$W/java-htc/classes" && jar cf ../classes.jar .)
set --
for j in "$DFW"/*-java/*-classes.jar; do set -- "$@" --lib "$j"; done
"$D8" --release --min-api 19 "$@" --output "$W/java-htc/dex" "$W/java-htc/classes.jar"
baksmali d -a 19 -o "$W/java-htc/smali" "$W/java-htc/dex/classes.dex"

# HTC's boot jars: the classes ours lack, or our stand-ins for them; the
# stand-ins of classes HTC has none of go into the last jar.
last=$(echo $HTC_JARS | awk '{print $NF}'); last=${last%%:*}
for spec in $HTC_JARS; do
  name=${spec%%:*}
  mkdir -p "$W/$name"
  for jar in $(echo "${spec#*:}" | tr , ' '); do
    baksmali d -a 19 -o "$W/$name/all-$jar" "$SYS/framework/$jar.jar"
  done
  # By each file's .class line: on a case-insensitive file system baksmali
  # renames classes whose names differ only in case.
  python3 - "$W/our-classes.txt" "$W/$name" "$W/java-htc/smali" \
      "$( [ "$name" = "$last" ] && echo last )" <<'PY'
import os, shutil, sys
def classes(root):
    out = {}
    for d, _, files in os.walk(root):
        for f in files:
            path = os.path.join(d, f)
            with open(path) as fh:
                out[fh.readline().split()[-1]] = path
    return out
ours = set(open(sys.argv[1]).read().split())
work, standins, last = sys.argv[2], classes(sys.argv[3]), sys.argv[4]
dst = os.path.join(work, 'smali')
os.makedirs(dst)
n = 0
done = set()
lost = set()
for jar in sorted(os.listdir(work)):
    if not jar.startswith('all-'):
        continue
    for cls, path in classes(os.path.join(work, jar)).items():
        if cls in ours:
            # HTC's version of one of our classes: the HTC interfaces it
            # implements and ours does not (tools/htc_fix_missing.py --lost).
            for line in open(path):
                if line.startswith('.implements'):
                    iface = line.split()[-1]
                    if iface not in ours:
                        lost.add(iface)
                elif line.startswith('.method'):
                    break
            continue
        if cls in done:
            continue
        done.add(cls)
        n += 1
        if cls in standins:
            print('stand-in: ' + cls)
            path = standins[cls]
        shutil.copy(path, os.path.join(dst, '%d.smali' % n))
with open(os.path.join(work, 'classes.txt'), 'w') as f:
    f.write('\n'.join(sorted(done)) + '\n')
with open(os.path.join(work, 'lost-interfaces.txt'), 'w') as f:
    f.write('\n'.join(sorted(lost)) + '\n')
if last:  # the stand-ins no HTC jar has a class for
    shipped = set()
    for t in os.listdir(os.path.dirname(work)):
        p = os.path.join(os.path.dirname(work), t, 'classes.txt')
        if os.path.exists(p):
            shipped |= set(open(p).read().split())
    for cls, path in standins.items():
        if cls not in shipped:
            n += 1
            print('added: ' + cls)
            shutil.copy(path, os.path.join(dst, '%d.smali' % n))
PY
  rm -rf "$W/$name"/all-*
done

# The apps: their dex (deodexed against HTC's framework where the APK has
# none).
for spec in $APPS; do
  path=${spec%%:*}; name=${spec#*:}
  src=$SYS/$path.apk
  if unzip -l "$src" | grep -q ' classes.dex$'; then
    baksmali d -a 19 -o "$W/app-$name/smali" "$src"
  else
    # -a 18: with 19, baksmali 2.5.2 resolves none of the quickened field
    # offsets of this firmware's odex files; with 18 SystemUI.odex deodexes to
    # the code of SystemUI.apk's own classes.dex (checked: same instructions).
    baksmali deodex -a 18 -d "$SYS/framework" -o "$W/app-$name/smali" "$SYS/$path.odex"
  fi
done

# HTC's code paths a device never takes, fixed (tools/htc_smali_fixes.py).
set --
for spec in $HTC_JARS; do set -- "$@" "$W/${spec%%:*}/smali"; done
for spec in $APPS; do set -- "$@" "$W/app-${spec#*:}/smali"; done
python3 "$ROOT/tools/htc_smali_fixes.py" "$@"

# Uses of members our framework lacks -> arcwasm.htc.Missing (in the last jar).
set -- "$W/$last/smali"
for spec in $HTC_JARS; do
  name=${spec%%:*}; [ "$name" = "$last" ] || set -- "$@" "$W/$name/smali"
done
for spec in $APPS; do set -- "$@" "$W/app-${spec#*:}/smali"; done
cat "$W"/*/lost-interfaces.txt | sort -u > "$W/lost-interfaces.txt"
python3 "$ROOT/tools/htc_fix_missing.py" "$W/$last/smali" --ours "$W/ours" --fix "$@" \
  --lost "$W/lost-interfaces.txt" > "$W/missing.txt"
echo "$(grep -c . "$W/lost-interfaces.txt") HTC interfaces our classes do not implement"
head -1 "$W/missing.txt"
cp "$W/missing.txt" "$OUT/missing.txt"

# Assemble: renumbered framework-res ids.
for spec in $HTC_JARS; do
  name=${spec%%:*}
  remap smali "$W/idmap.txt" "$W/$name/smali"
  smali assemble -a 19 -o "$W/$name/classes.dex" "$W/$name/smali"
  (cd "$W/$name" && zip -q "$OUT/framework/$name.jar" classes.dex)
  echo "$OUT/framework/$name.jar ($(find "$W/$name/smali" -name '*.smali' | wc -l | tr -d ' ') classes)"
done
for res in $HTC_RES; do
  remap apk "$W/idmap.txt" "$SYS/framework/$res" "$OUT/framework/$res"
done
for pkg in $HTC_RES_PKGS; do
  remap apk "$W/idmap.txt" "$SYS/framework/$pkg.apk" "$OUT/app/$pkg.apk"
done
for spec in $APPS; do
  path=${spec%%:*}; name=${spec#*:}
  remap smali "$W/idmap.txt" "$W/app-$name/smali"
  smali assemble -a 19 -o "$W/app-$name/classes.dex" "$W/app-$name/smali"
  remap apk "$W/idmap.txt" "$SYS/$path.apk" "$OUT/app/$name.apk"
  (cd "$W/app-$name" && zip -q "$OUT/app/$name.apk" classes.dex)
  echo "$OUT/app/$name.apk ($(wc -c < "$W/app-$name/classes.dex" | tr -d ' ') bytes of dex)"
done

{
  echo "# HTC Sense (tools/fetch_system_apks_htc.sh)"
  printf 'ro.arcwasm.bootclasspath.append='
  for spec in $HTC_JARS; do printf '/system/framework/%s.jar:' "${spec%%:*}"; done | sed 's/:$//'
  echo
} > "$OUT/build.prop"
cat "$OUT/build.prop"
rm -rf "$W"
