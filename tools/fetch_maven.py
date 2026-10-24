#!/usr/bin/env python3
"""Fetches Maven artifacts and their runtime dependencies (a small resolver:
compile/runtime scopes, nearest version wins, no exclusions beyond the POM's
own) from Google's Maven repository and Maven Central into a cache directory,
and unpacks each AAR's classes.jar, res/ and AndroidManifest.xml next to it.

Used by topic gms (docs/topics/gms.md) for the GmsTest APK's real client
libraries (play-services-*, firebase-*, Play Billing, Play Core): third-party
binaries are fetched at build time, never committed.

Usage: fetch_maven.py CACHE_DIR OUT_LIST group:artifact:version ...
  CACHE_DIR  where .pom/.aar/.jar files go (kept between runs)
  OUT_LIST   written: one line per resolved artifact,
             "group:artifact:version KIND PATH" (KIND aar or jar; for an AAR
             PATH is the unpacked directory)
"""
import os
import re
import sys
import subprocess
import xml.etree.ElementTree as ET
import zipfile

REPOS = [
    "https://dl.google.com/dl/android/maven2",
    "https://repo1.maven.org/maven2",
]
NS = "{http://maven.apache.org/POM/4.0.0}"


def fetch(cache, group, artifact, version, ext):
    rel = "%s/%s/%s/%s-%s.%s" % (group.replace(".", "/"), artifact, version,
                                 artifact, version, ext)
    dest = os.path.join(cache, rel)
    if os.path.exists(dest):
        return dest
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    for repo in REPOS:
        # curl: the system's certificate store (python.org's Python has none).
        r = subprocess.run(["curl", "-sSfL", "-m", "120", "-o", dest + ".tmp",
                            "%s/%s" % (repo, rel)], stderr=subprocess.DEVNULL)
        if r.returncode == 0:
            os.rename(dest + ".tmp", dest)
            return dest
    return None


def text(el, tag):
    c = el.find(NS + tag)
    return c.text.strip() if c is not None and c.text else None


class Pom:
    def __init__(self, cache, group, artifact, version):
        path = fetch(cache, group, artifact, version, "pom")
        if path is None:
            raise SystemExit("no POM for %s:%s:%s" % (group, artifact, version))
        self.root = ET.parse(path).getroot()
        self.packaging = text(self.root, "packaging") or "jar"
        self.props = {"project.version": version, "version": version}
        props = self.root.find(NS + "properties")
        if props is not None:
            for p in props:
                self.props[p.tag.replace(NS, "")] = (p.text or "").strip()
        self.managed = {}
        parent = self.root.find(NS + "parent")
        if parent is not None:
            pg, pa, pv = text(parent, "groupId"), text(parent, "artifactId"), text(parent, "version")
            try:
                pp = Pom(cache, pg, pa, pv)
                for k, v in pp.props.items():
                    self.props.setdefault(k, v)
                self.managed.update(pp.managed)
            except SystemExit:
                pass
        dm = self.root.find(NS + "dependencyManagement")
        if dm is not None:
            for d in dm.iter(NS + "dependency"):
                g, a, v = text(d, "groupId"), text(d, "artifactId"), text(d, "version")
                if g and a and v:
                    self.managed[(self.sub(g), self.sub(a))] = self.sub(v)

    def sub(self, s):
        return re.sub(r"\$\{([^}]+)\}", lambda m: self.props.get(m.group(1), m.group(0)), s)

    def deps(self):
        out = []
        deps = self.root.find(NS + "dependencies")
        if deps is None:
            return out
        for d in deps.findall(NS + "dependency"):
            scope = text(d, "scope") or "compile"
            if scope not in ("compile", "runtime") or text(d, "optional") == "true":
                continue
            g, a = self.sub(text(d, "groupId")), self.sub(text(d, "artifactId"))
            v = text(d, "version")
            v = self.sub(v) if v else self.managed.get((g, a))
            if v is None:
                continue
            # Ranges ("[18.0.0]", "[1.0,2.0)"): the lower bound.
            m = re.match(r"^[\[(]\s*([^,\])]+)", v)
            if m:
                v = m.group(1).strip()
            out.append((g, a, v))
        return out


def vkey(v):
    return [int(x) if x.isdigit() else x for x in re.split(r"[.\-]", v)]


def main():
    cache, out_list, coords = sys.argv[1], sys.argv[2], sys.argv[3:]
    # Breadth first: the nearest declaration wins, ties to the higher version
    # (Gradle would take the highest everywhere; close enough for a test APK).
    chosen = {}
    queue = [tuple(c.split(":")) for c in coords]
    depth = {(g, a): 0 for g, a, _ in queue}
    order = []
    while queue:
        g, a, v = queue.pop(0)
        key = (g, a)
        if key in chosen:
            if vkey(v) > vkey(chosen[key]):
                chosen[key] = v
            continue
        chosen[key] = v
        order.append(key)
        for dg, da, dv in Pom(cache, g, a, v).deps():
            queue.append((dg, da, dv))
    with open(out_list, "w") as f:
        for g, a in order:
            v = chosen[(g, a)]
            pom = Pom(cache, g, a, v)
            kind = "aar" if pom.packaging == "aar" else "jar"
            path = fetch(cache, g, a, v, kind)
            if path is None and kind == "jar":
                kind, path = "aar", fetch(cache, g, a, v, "aar")
            if path is None:
                if pom.packaging == "pom":
                    continue
                raise SystemExit("no artifact for %s:%s:%s" % (g, a, v))
            if kind == "aar":
                d = path[:-4]
                if not os.path.isdir(d):
                    with zipfile.ZipFile(path) as z:
                        z.extractall(d + ".tmp")
                    os.rename(d + ".tmp", d)
                path = d
            f.write("%s:%s:%s %s %s\n" % (g, a, v, kind, path))
            print("%s:%s:%s %s" % (g, a, v, kind))


if __name__ == "__main__":
    main()
