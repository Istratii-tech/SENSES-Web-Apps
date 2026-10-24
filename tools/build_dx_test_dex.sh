#!/bin/sh
# Builds tests/java/dx/classes2.dex (checked in; the second dex of ART's
# arc-tests.jar, mk/topic-art.mk) from tests/java/dx/arcwasm/*.java with dx
# --no-optimize: dx's register copies of a new-instance before String.<init>
# (docs/topics/art-string-init.md), which d8 never emits. dx is in
# build-tools 30 and older; set DX_JAR to use another dx.jar.
set -e
cd "$(dirname "$0")/.."
DX_JAR=${DX_JAR:-${ANDROID_HOME:-$HOME/Library/Android/sdk}/build-tools/30.0.3/lib/dx.jar}
[ -f "$DX_JAR" ] || { echo "no dx.jar at $DX_JAR (set DX_JAR)" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
javac --release 8 -Xlint:-options -encoding UTF-8 -d "$tmp/classes" tests/java/dx/arcwasm/*.java
java -cp "$DX_JAR" com.android.dx.command.Main --dex --no-optimize \
    --output="$tmp/classes.dex" "$tmp/classes"
cp "$tmp/classes.dex" tests/java/dx/classes2.dex
echo "wrote tests/java/dx/classes2.dex ($(wc -c < tests/java/dx/classes2.dex | tr -d ' ') bytes)"
