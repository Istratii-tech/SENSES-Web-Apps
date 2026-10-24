#!/bin/sh
# Fetches the Khronos Vulkan headers and registry (Vulkan-Headers, Apache-2.0
# or MIT) into third_party/vulkan-headers for topic vulkan
# (docs/topics/vulkan.md): include/vulkan/*.h for the ICD (shim/vulkan) and
# the guest test libraries, registry/vk.xml for tools/vulkan/gen_vk.py.
#
# Set VULKAN_HEADERS_ARCHIVE to a downloaded Vulkan-Headers-$VERSION.tar.gz
# to use it instead of GitHub.
set -e
cd "$(dirname "$0")/.."
VERSION=1.3.296
DEST=third_party/vulkan-headers
if [ -f "$DEST/.version" ] && [ "$(cat "$DEST/.version")" = "$VERSION" ]; then
  exit 0
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if [ -n "$VULKAN_HEADERS_ARCHIVE" ]; then
  cp "$VULKAN_HEADERS_ARCHIVE" "$TMP/vh.tar.gz"
else
  curl -sSfL -o "$TMP/vh.tar.gz" \
    "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v$VERSION.tar.gz"
fi
tar xzf "$TMP/vh.tar.gz" -C "$TMP"
rm -rf "$DEST"
mkdir -p "$DEST"
cp -R "$TMP/Vulkan-Headers-$VERSION/include" "$TMP/Vulkan-Headers-$VERSION/registry" \
      "$TMP/Vulkan-Headers-$VERSION/LICENSE.md" "$DEST/"
echo "$VERSION" > "$DEST/.version"
echo "Vulkan-Headers $VERSION in $DEST"
