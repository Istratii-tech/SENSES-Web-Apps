#!/bin/sh
# Fetches the AOSP android-6.0.1_r81 projects the Android 6 port (ART +
# framework 6.0.1) builds from into third_party/aosp60 and applies the
# arc-wasm patches. Same scheme as tools/fetch_aosp44.sh (Android 4.4).
#
# Patches and extra projects are split by topic so that work on separate
# topics (in separate branches) does not conflict:
#   patches/aosp60.patch            base changes (optional)
#   patches/aosp60/TOPIC.projects   extra AOSP projects TOPIC needs, one per line
#   patches/aosp60/TOPIC.patch      TOPIC's changes, applied after the base
#                                   (topics in the order of their names)
#
# third_party/aosp60 is a small git repository: tag "pristine" is the
# unmodified sources, then one commit per patch (tag "base", then
# "topic/TOPIC" for each topic). `git -C third_party/aosp60 diff pristine`
# shows every arc-wasm change. To change AOSP code for a topic, run
# tools/save_aosp60_patch.sh TOPIC --start once, edit, then
# tools/save_aosp60_patch.sh TOPIC to (re)write patches/aosp60/TOPIC.patch.
#
# Run it again after pulling: it fetches projects that are missing and
# applies topic patches that are not applied yet.
#
# Downloads use googlesource's per-project snapshot archives. To use
# archives that are already on disk, laid out as <project>.tar.gz (e.g.
# external/icu.tar.gz), set AOSP60_ARCHIVES to their directory. By default
# the shared archive directory next to the checkout is used when it exists
# (downloaded once through the LAN host; see docs/HANDOFF.md).
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
TAG=android-6.0.1_r81
DEST=third_party/aosp60
# Archives live next to the checkout (for a worktree: next to the main checkout).
SIBLINGS=$(cd "$(git rev-parse --git-common-dir)/../.." 2>/dev/null && pwd)
AOSP60_ARCHIVES=${AOSP60_ARCHIVES-$SIBLINGS/aosp60-archives}
[ -d "$AOSP60_ARCHIVES" ] || AOSP60_ARCHIVES=
PROJECTS="art libcore libnativehelper bionic system/core external/icu
          external/fdlibm external/zlib external/safe-iop frameworks/native
          frameworks/base"
for f in patches/aosp60/*.projects; do
  [ -f "$f" ] && PROJECTS="$PROJECTS $(grep -v '^#' "$f")"
done
# Large parts of frameworks/base the port does not build from (docs, media
# test clips, sounds, videos, the preload tool's data, test apps and system
# packages): about 750 MB of its 1 GB, removed right after unpacking.
PRUNE_frameworks_base="docs media/tests data/sounds data/videos tools/preload
                       tests core/tests services/tests media/mca packages"
# Pruned paths that are needed after all, unpacked again by themselves:
# framework.jar is built from media/mca and two .aidl directories of
# packages/services (frameworks/base/Android.mk, module framework-base).
RESTORE="frameworks/base/media/mca frameworks/base/packages/services/PacProcessor/com
         frameworks/base/packages/services/Proxy/com"

git_() { git -C "$DEST" -c user.name=arc-wasm -c user.email=arc-wasm@localhost "$@"; }

# Unpacks project $1 into directory $2; only path $3 in it if given.
fetch() {
  mkdir -p "$2/$1"
  if [ -n "$AOSP60_ARCHIVES" ]; then
    echo "unpacking $1 $3"
    tar xzf "$AOSP60_ARCHIVES/$1.tar.gz" -C "$2/$1" ${3:+"./$3"}
  else
    echo "downloading $1 $3"
    curl -fsSL --retry 5 \
      "https://android.googlesource.com/platform/$1/+archive/refs/tags/$TAG.tar.gz" \
      | tar xzf - -C "$2/$1" ${3:+"./$3"}
  fi
  [ -z "$3" ] || return 0
  prune=$(eval echo "\$PRUNE_$(echo "$1" | tr /- __)")
  for d in $prune; do
    rm -rf "$2/$1/$d"
  done
}

if [ ! -d "$DEST/.git" ]; then
  rm -rf "$DEST.tmp"
  mkdir -p "$DEST.tmp"
  for p in $PROJECTS; do
    fetch "$p" "$DEST.tmp"
  done
  mv "$DEST.tmp" "$DEST"
  git -C "$DEST" init -q
  git_ add -A
  git_ commit -q -m "AOSP $TAG (pristine): $(echo $PROJECTS)"
  git_ tag pristine
  if [ -f "$ROOT/patches/aosp60.patch" ]; then
    git_ apply --whitespace=nowarn "$ROOT/patches/aosp60.patch"
  fi
  git_ add -A
  git_ commit -q --allow-empty -m "arc-wasm: patches/aosp60.patch"
  git_ tag base
else
  # Projects added since the last run: unpack and commit them pristine.
  for p in $PROJECTS; do
    [ -d "$DEST/$p" ] && continue
    fetch "$p" "$DEST"
    git_ add -A -- "$p"
    git_ commit -q -m "AOSP $TAG (pristine): $p" -- "$p"
  done
fi
for r in $RESTORE; do
  [ -e "$DEST/$r" ] && continue
  fetch frameworks/base "$DEST" "${r#frameworks/base/}"
  git_ add -A -- "$r"
  git_ commit -q -m "AOSP $TAG (pristine): $r" -- "$r"
done

# Topics in the order of their names (C locale), so that a topic is applied
# after the one its name extends (art before art-speed; the glob would put
# art-speed.patch first).
for topic in $(for p in patches/aosp60/*.patch; do
                 [ -f "$p" ] && basename "$p" .patch
               done | LC_ALL=C sort); do
  patch=patches/aosp60/$topic.patch
  git_ rev-parse -q --verify "refs/tags/topic/$topic" >/dev/null && continue
  git_ tag -f "before/$topic" >/dev/null
  git_ apply --whitespace=nowarn "$ROOT/$patch"
  git_ add -A
  git_ commit -q --allow-empty -m "arc-wasm: $patch"
  git_ tag "topic/$topic"
  echo "applied $patch"
done
echo "$DEST ready ('git -C $DEST diff pristine' shows all arc-wasm changes)"
