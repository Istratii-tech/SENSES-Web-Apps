#!/bin/sh
# Writes the AOSP changes made for TOPIC to patches/aosp81/TOPIC.patch.
#
#   save_aosp81_patch.sh TOPIC --start   once, before the first edit (tree clean)
#   save_aosp81_patch.sh TOPIC           after editing; run again after more edits
#
# The patch is everything in third_party/aosp81 since tag before/TOPIC:
# later commits plus uncommitted and new files. Base changes belong in
# patches/aosp81.patch (git -C third_party/aosp81 diff pristine base).
set -e
cd "$(dirname "$0")/.."
DEST=third_party/aosp81
topic=$1
[ -n "$topic" ] || { echo "usage: $0 TOPIC [--start]" >&2; exit 2; }
if [ "$2" = "--start" ]; then
  if [ -n "$(git -C "$DEST" status --porcelain)" ]; then
    echo "$DEST has uncommitted changes; commit or save them first" >&2
    exit 1
  fi
  git -C "$DEST" tag -f "before/$topic" >/dev/null
  echo "started topic $topic at $(git -C "$DEST" rev-parse --short HEAD)"
  exit 0
fi
git -C "$DEST" rev-parse -q --verify "refs/tags/before/$topic" >/dev/null || {
  echo "no tag before/$topic: run '$0 $topic --start' before editing" >&2
  exit 1
}
mkdir -p patches/aosp81
git -C "$DEST" add -A --intent-to-add
git -C "$DEST" diff "before/$topic" > "patches/aosp81/$topic.patch"
echo "wrote patches/aosp81/$topic.patch ($(grep -c '^diff --git' "patches/aosp81/$topic.patch") files)"
