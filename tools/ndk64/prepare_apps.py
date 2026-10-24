#!/usr/bin/env python3
"""Prepares apps' arm64-v8a libraries for arcwasm.Arm64AppsTest (topic ndk-arm64).

    prepare_apps.py OUTDIR NAME FILE...

FILE is an APK, or an .apks/.xapk bundle (its base.apk and split APKs). Writes
OUTDIR/NAME/lib/*.so (every lib/arm64-v8a/*.so) and OUTDIR/NAME/base.apk, a
zip of only the app's classes*.dex (what JNI_OnLoad's FindClass needs, without
the resources; dex version 039, which ART 8.1 does not open, is relabeled 038 --
the classes JNI_OnLoad looks up rarely use what 039 added). mk/topic-ndk-arm64.mk adds OUTDIR (build/ndk64-apps) to
artvm8's /system/ndk64-apps when it exists. The apps are not in git.
"""
import io
import os
import sys
import zipfile
import zlib


def as_dex038(data):
    """dex 039 relabeled 038, with the header's checksum (adler32) redone."""
    if data[:8] != b"dex\n039\0":
        return data
    data = bytearray(data)
    data[4:7] = b"038"
    data[8:12] = zlib.adler32(bytes(data[12:])).to_bytes(4, "little")
    return bytes(data)


def apks_of(path):
    """(name, ZipFile) of an APK, or of each APK inside a bundle."""
    z = zipfile.ZipFile(path)
    inner = [n for n in z.namelist() if n.endswith(".apk")]
    if not inner:
        yield os.path.basename(path), z
        return
    for n in inner:
        yield n, zipfile.ZipFile(io.BytesIO(z.read(n)))


def main():
    outdir, name, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    dest = os.path.join(outdir, name)
    os.makedirs(os.path.join(dest, "lib"), exist_ok=True)
    libs = dex = 0
    with zipfile.ZipFile(os.path.join(dest, "base.apk"), "w", zipfile.ZIP_DEFLATED) as out:
        for f in files:
            for apk_name, apk in apks_of(f):
                for n in apk.namelist():
                    if n.startswith("lib/arm64-v8a/") and n.endswith(".so") and n.count("/") == 2:
                        with open(os.path.join(dest, "lib", os.path.basename(n)), "wb") as so:
                            so.write(apk.read(n))
                        libs += 1
                    elif "/" not in n and n.startswith("classes") and n.endswith(".dex"):
                        out.writestr(n, as_dex038(apk.read(n)))
                        dex += 1
    print("%s: %d arm64-v8a libraries, %d dex files in %s" % (name, libs, dex, dest))


if __name__ == "__main__":
    main()
