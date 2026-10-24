#!/bin/sh
# Topic userspace: a newer bionic tzdata for /system/usr/share/zoneinfo
# (AOSP 4.4.4's is 2014a: before Russia's 2014 change Europe/Moscow is
# UTC+4). Same file format; AOSP 6.0.1's bionic has 2016a. Writes
# third_party/tzdata/tzdata (not in git), which mk/topic-userspace.mk uses
# instead of 4.4's when it exists.
#   AOSP60_ARCHIVES=/dir/with/bionic.tar.gz tools/fetch_tzdata.sh
set -eu
cd "$(dirname "$0")/.."
: "${AOSP60_ARCHIVES:?set AOSP60_ARCHIVES to the AOSP 6.0 archives (bionic.tar.gz)}"
mkdir -p third_party/tzdata
tar xzf "$AOSP60_ARCHIVES/bionic.tar.gz" -O libc/zoneinfo/tzdata > third_party/tzdata/tzdata.tmp
mv third_party/tzdata/tzdata.tmp third_party/tzdata/tzdata
printf '%s: ' third_party/tzdata/tzdata; head -c 11 third_party/tzdata/tzdata; echo
