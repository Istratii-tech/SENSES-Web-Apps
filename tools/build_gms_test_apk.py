#!/usr/bin/env python3
"""Builds GmsTest's APK (topic gms, docs/topics/gms.md) from tests/apk-gms
and the client libraries tools/fetch_maven.py fetched -- what Gradle does for
an app with AAR dependencies, reduced:

  - manifests: the app's, with each library's <uses-permission>,
    <permission> and <application> children merged in (same-name components
    merged, ${applicationId} replaced, tools:node="remove" honoured);
  - resources: every AAR's res/ compiled by aapt2 and linked as overlays
    under the app's, R.java for the app and each library package;
  - code: javac (the app and the R classes) against android.jar and the
    libraries, the libraries' classes deduplicated (the first artifact
    wins, as the resolver ordered them), d8 to classes*.dex (multidex for
    the min API);
  - the libraries' Java resources (META-INF/services/..., *.properties) at
    the APK's root, as Gradle's java resource merge does.

Unsigned (our package managers do not verify).

Usage: build_gms_test_apk.py --deps LIST --src DIR --work DIR --out APK
           --android-jar JAR --aapt2 AAPT2 --d8 D8 [--min-api N]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile

A = "{http://schemas.android.com/apk/res/android}"
T = "{http://schemas.android.com/tools}"
ET.register_namespace("android", A[1:-1])
ET.register_namespace("tools", T[1:-1])

COMPONENTS = ("activity", "activity-alias", "service", "receiver", "provider")


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        sys.exit("failed (%d): %s" % (r.returncode, " ".join(cmd[:6]) + " ..."))


def read_deps(path):
    deps = []
    for line in open(path):
        line = line.rstrip("\n")
        if not line:
            continue
        coord, kind, p = line.split(" ", 2)
        deps.append((coord, kind, p))
    return deps


def strip_tools(el):
    for k in list(el.attrib):
        if k.startswith(T):
            del el.attrib[k]
    for c in el:
        strip_tools(c)


def subst(el, app_id):
    for k, v in el.attrib.items():
        if "${applicationId}" in v:
            el.attrib[k] = v.replace("${applicationId}", app_id)
    for c in el:
        subst(c, app_id)


def merge_manifests(app_manifest, lib_manifests, out):
    tree = ET.parse(app_manifest)
    root = tree.getroot()
    app_id = root.get("package")
    application = root.find("application")
    by_name = {}
    for c in application:
        n = c.get(A + "name")
        if n:
            by_name[(c.tag, n if not n.startswith(".") else app_id + n)] = c
    perms = {(p.tag, p.get(A + "name")) for p in root if p.tag in ("uses-permission", "permission")}
    lib_packages = []
    for lm in lib_manifests:
        lroot = ET.parse(lm).getroot()
        pkg = lroot.get("package")
        if pkg:
            lib_packages.append(pkg)
        subst(lroot, app_id)
        for p in lroot:
            if p.tag in ("uses-permission", "permission", "uses-permission-sdk-23"):
                key = (p.tag, p.get(A + "name"))
                if key not in perms and p.get(T + "node") != "remove":
                    perms.add(key)
                    strip_tools(p)
                    application.addprevious(p) if hasattr(application, "addprevious") else \
                        root.insert(list(root).index(application), p)
        lapp = lroot.find("application")
        if lapp is None:
            continue
        for c in lapp:
            if c.tag not in COMPONENTS + ("meta-data", "uses-library"):
                continue
            if c.get(T + "node") == "remove":
                continue
            n = c.get(A + "name")
            if n and n.startswith("."):
                n = pkg + n
                c.set(A + "name", n)
            key = (c.tag, n)
            strip_tools(c)
            if key in by_name:
                # Same component in two libraries (ComponentDiscoveryService:
                # each library adds its registrars' meta-data): children merged.
                have = by_name[key]
                names = {(x.tag, x.get(A + "name")) for x in have}
                for x in c:
                    if (x.tag, x.get(A + "name")) not in names or x.tag == "intent-filter":
                        have.append(x)
                continue
            if c.tag == "uses-library" and c.get(A + "required") != "false":
                continue  # (none of ours needs a shared library)
            by_name[key] = c
            application.append(c)
    for q in root.findall("queries"):
        root.remove(q)
    for el in root.iter():
        for x in list(el):
            if x.tag in ("property", "queries", "profileable"):
                el.remove(x)
    tree.write(out, encoding="utf-8", xml_declaration=True)
    return app_id, lib_packages


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--deps", required=True)
    ap.add_argument("--src", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--android-jar", required=True)
    ap.add_argument("--aapt2", required=True)
    ap.add_argument("--d8", required=True)
    ap.add_argument("--min-api", default="26")
    args = ap.parse_args()

    work = os.path.abspath(args.work)
    shutil.rmtree(work, ignore_errors=True)
    for d in ("res", "gen", "classes", "dex", "javares"):
        os.makedirs(os.path.join(work, d))
    deps = read_deps(args.deps)

    jars, res_dirs, manifests = [], [], []
    for coord, kind, p in deps:
        if kind == "aar":
            if os.path.isfile(os.path.join(p, "classes.jar")):
                jars.append(os.path.join(p, "classes.jar"))
            libs = os.path.join(p, "libs")
            if os.path.isdir(libs):
                jars += [os.path.join(libs, j) for j in sorted(os.listdir(libs)) if j.endswith(".jar")]
            res = os.path.join(p, "res")
            if os.path.isdir(res) and any(os.scandir(res)):
                res_dirs.append((coord, res))
            if os.path.isfile(os.path.join(p, "AndroidManifest.xml")):
                manifests.append(os.path.join(p, "AndroidManifest.xml"))
        else:
            jars.append(p)

    # --- manifest
    merged = os.path.join(work, "AndroidManifest.xml")
    app_id, lib_packages = merge_manifests(os.path.join(args.src, "AndroidManifest.xml"),
                                           manifests, merged)

    # --- resources: libraries as overlays (later wins), the app's last
    flats = []
    for i, (coord, res) in enumerate(res_dirs):
        z = os.path.join(work, "res", "%03d.zip" % i)
        run([args.aapt2, "compile", "--dir", res, "-o", z])
        flats.append(z)
    app_z = os.path.join(work, "res", "app.zip")
    run([args.aapt2, "compile", "--dir", os.path.join(args.src, "res"), "-o", app_z])
    unsigned = os.path.join(work, "res.apk")
    extra = sorted(set(p for p in lib_packages if p != app_id))
    cmd = [args.aapt2, "link", "-o", unsigned, "-I", args.android_jar, "--manifest", merged,
           "--java", os.path.join(work, "gen"), "--auto-add-overlay",
           "--min-sdk-version", args.min_api, "--target-sdk-version", "27",
           "--extra-packages", ":".join(extra), app_z]
    for z in flats:
        cmd += ["-R", z]
    run(cmd)

    # --- code
    srcs = []
    for d in (os.path.join(args.src, "src"), os.path.join(work, "gen")):
        for r, _, fs in os.walk(d):
            srcs += [os.path.join(r, f) for f in fs if f.endswith(".java")]
    with open(os.path.join(work, "srcs.txt"), "w") as f:
        f.write("\n".join('"%s"' % s for s in srcs))
    cp = ":".join(jars)
    run(["javac", "-source", "8", "-target", "8", "-Xlint:-options", "-nowarn", "-encoding", "UTF-8",
         "-bootclasspath", args.android_jar, "-cp", cp, "-d", os.path.join(work, "classes"),
         "@" + os.path.join(work, "srcs.txt")])

    # One jar: the app's classes, then the libraries' (first wins).
    allj = os.path.join(work, "all.jar")
    seen = set()
    javares = {}
    with zipfile.ZipFile(allj, "w", zipfile.ZIP_DEFLATED) as out:
        for r, _, fs in os.walk(os.path.join(work, "classes")):
            for f in fs:
                full = os.path.join(r, f)
                name = os.path.relpath(full, os.path.join(work, "classes"))
                seen.add(name)
                out.write(full, name)
        for j in jars:
            with zipfile.ZipFile(j) as z:
                for e in z.infolist():
                    n = e.filename
                    if e.is_dir():
                        continue
                    if n.endswith(".class"):
                        if n in seen or n.startswith("META-INF/") or n == "module-info.class" \
                                or "/R.class" in n or re.search(r"/R\$[^/]+\.class$", n):
                            continue  # (R classes: the app's link made them)
                        seen.add(n)
                        out.writestr(n, z.read(n))
                    elif not (n.startswith("META-INF/") and (n.endswith((".SF", ".RSA", ".DSA",
                              ".MF", ".kotlin_module", ".version", ".pro")) or "/proguard/" in n
                              or "/com.android.tools/" in n)):
                        javares.setdefault(n, z.read(n))
    run([args.d8, "--release", "--min-api", args.min_api, "--lib", args.android_jar,
         "--output", os.path.join(work, "dex"), allj])

    # --- the APK
    tmp = args.out + ".tmp"
    shutil.copy(unsigned, tmp)
    with zipfile.ZipFile(tmp, "a", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(os.listdir(os.path.join(work, "dex"))):
            z.write(os.path.join(work, "dex", f), f)
        for n, data in sorted(javares.items()):
            if n not in z.namelist():
                z.writestr(n, data)
    os.replace(tmp, args.out)
    dexes = [f for f in os.listdir(os.path.join(work, "dex")) if f.endswith(".dex")]
    print("built %s: %d libraries, %d dex files, %d bytes" % (
        args.out, len(deps), len(dexes), os.path.getsize(args.out)))


if __name__ == "__main__":
    main()
