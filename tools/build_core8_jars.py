#!/usr/bin/env python3
"""Builds the core library of Android 8.1 (libcore 8.1, AOSP 8.1.0_r81) as DEX
jars for ART 8.1, the way libcore/JavaLibrary.mk builds its modules:

  core-all   openjdk_java_files + non_openjdk_java_files + android_icu4j
             (only to compile against; its classes jar is kept for the tests)
  core-oj    openjdk_java_files (OpenJDK-based java.*, javax.*, sun.*) with
             the resources of luni/src/main/java and ojluni/src/main/resources
  core-libart  non_openjdk_java_files (dalvik.*, libcore.*, android.system,
             org.json, kxml ...) + android_icu4j (android.icu) with its
             resources

javac (source/target 8, empty bootclasspath) compiles core-all once; the
classes are split into core-oj and core-libart by the source file each was
compiled from (its SourceFile attribute and package). d8 (--min-api 26: dex
038, default and static interface methods stay as they are; lambdas are
desugared as always) dexes each against the other (--lib).

Usage: build_core8_jars.py AOSP81_DIR OUT_DIR
Env: D8 (path to d8; default: newest build-tools d8 of ANDROID_SDK or
~/Library/Android/sdk).
"""
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile


def mk_vars(path, env):
    """Assignments (:=, +=) of a libcore .mk file of file lists, with $(var)
    references expanded from |env| and earlier assignments."""
    text = open(path).read().replace('\\\n', ' ')
    for line in text.splitlines():
        line = line.split('#', 1)[0]
        m = re.match(r'\s*([A-Za-z_][A-Za-z0-9_]*)\s*(:=|\+=)(.*)', line)
        if not m:
            continue
        items = []
        for tok in m.group(3).split():
            ref = re.fullmatch(r'\$\(([A-Za-z_][A-Za-z0-9_]*)\)', tok)
            items += env.get(ref.group(1), []) if ref else [tok]
        env[m.group(1)] = (env.get(m.group(1), []) if m.group(2) == '+=' else []) + items
    return env


def source_file_of(class_bytes):
    """(this class's internal name, SourceFile attribute) of a class file."""
    d = class_bytes
    pos = 10
    count = struct.unpack('>H', d[8:10])[0]
    utf8 = {}
    classes = {}
    i = 1
    while i < count:
        tag = d[pos]
        if tag == 1:
            n = struct.unpack('>H', d[pos + 1:pos + 3])[0]
            utf8[i] = d[pos + 3:pos + 3 + n].decode('utf-8', 'replace')
            pos += 3 + n
        elif tag == 7:
            classes[i] = struct.unpack('>H', d[pos + 1:pos + 3])[0]
            pos += 3
        elif tag in (8, 16, 19, 20):
            pos += 3
        elif tag == 15:
            pos += 4
        elif tag in (3, 4, 9, 10, 11, 12, 17, 18):
            pos += 5
        elif tag in (5, 6):
            pos += 9
            i += 1
        else:
            raise ValueError('constant pool tag %d' % tag)
        i += 1
    this_class = struct.unpack('>H', d[pos + 2:pos + 4])[0]
    pos += 6
    n_ifaces = struct.unpack('>H', d[pos:pos + 2])[0]
    pos += 2 + 2 * n_ifaces

    def skip_members(pos):
        n = struct.unpack('>H', d[pos:pos + 2])[0]
        pos += 2
        for _ in range(n):
            pos += 6
            n_attrs = struct.unpack('>H', d[pos:pos + 2])[0]
            pos += 2
            for _ in range(n_attrs):
                pos += 6 + struct.unpack('>I', d[pos + 2:pos + 6])[0]
        return pos

    pos = skip_members(skip_members(pos))
    n_attrs = struct.unpack('>H', d[pos:pos + 2])[0]
    pos += 2
    source = None
    for _ in range(n_attrs):
        name = utf8.get(struct.unpack('>H', d[pos:pos + 2])[0])
        length = struct.unpack('>I', d[pos + 2:pos + 6])[0]
        if name == 'SourceFile':
            source = utf8[struct.unpack('>H', d[pos + 6:pos + 8])[0]]
        pos += 6 + length
    return utf8[classes[this_class]], source


def package_relative(path):
    """java/lang/Object.java of .../src/main/java/java/lang/Object.java: the
    path from the package root (the first component that starts a package
    directory is found from the file's package declaration)."""
    with open(path, encoding='utf-8', errors='replace') as f:
        m = re.search(r'^\s*package\s+([\w.]+)\s*;', f.read(), re.M)
    pkg = m.group(1).replace('.', '/') + '/' if m else ''
    return pkg + os.path.basename(path)


def copy_resources(src_dir, dest):
    """Non-Java files under src_dir (as LOCAL_JAVA_RESOURCE_DIRS)."""
    for root, _, files in os.walk(src_dir):
        for f in files:
            if f.endswith('.java') or f.endswith('.html'):
                continue
            rel = os.path.relpath(os.path.join(root, f), src_dir)
            os.makedirs(os.path.join(dest, os.path.dirname(rel)), exist_ok=True)
            shutil.copy(os.path.join(root, f), os.path.join(dest, rel))


def find_d8():
    if os.environ.get('D8'):
        return os.environ['D8']
    sdk = os.environ.get('ANDROID_SDK', os.path.expanduser('~/Library/Android/sdk'))
    tools = sorted(glob.glob(os.path.join(sdk, 'build-tools', '*', 'd8')),
                   key=lambda p: [int(x) if x.isdigit() else x
                                  for x in re.split(r'[.-]', p.split('/')[-2])])
    return tools[-1]


def jar_dir(src_dir, jar):
    if os.path.exists(jar):
        os.remove(jar)
    with zipfile.ZipFile(jar, 'w', zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(src_dir):
            for f in sorted(files):
                full = os.path.join(root, f)
                z.write(full, os.path.relpath(full, src_dir))


def main(aosp, out):
    libcore = os.path.join(aosp, 'libcore')
    icu4j = os.path.join(aosp, 'external/icu/android_icu4j')
    os.makedirs(out, exist_ok=True)
    w = os.path.join(out, 'core8-java')
    shutil.rmtree(w, ignore_errors=True)
    for d in ('classes', 'empty', 'oj', 'libart', 'dex-oj', 'dex-libart', 'res-oj', 'res-libart'):
        os.makedirs(os.path.join(w, d))

    env = mk_vars(os.path.join(libcore, 'openjdk_java_files.mk'), {})
    env = mk_vars(os.path.join(libcore, 'non_openjdk_java_files.mk'), env)
    oj = [os.path.join(libcore, f) for f in env['openjdk_java_files']]
    nonoj = [os.path.join(libcore, f) for f in env['non_openjdk_java_files']]
    icu = sorted(glob.glob(os.path.join(icu4j, 'src/main/java/**/*.java'), recursive=True))
    oj = list(dict.fromkeys(oj))
    libart_srcs = list(dict.fromkeys(nonoj + icu))

    with open(os.path.join(w, 'srcs.txt'), 'w') as f:
        for p in oj + libart_srcs:
            f.write('"%s"\n' % p)
    print('javac: %d files (core-oj %d, core-libart %d)' % (len(oj) + len(libart_srcs), len(oj),
                                                           len(libart_srcs)), flush=True)
    r = subprocess.run(['javac', '-source', '8', '-target', '8', '-bootclasspath',
                        os.path.join(w, 'empty'), '-encoding', 'UTF-8', '-nowarn', '-g',
                        '-Xmaxerrs', '50', '-Xlint:-options', '-J-Xmx3g',
                        '-d', os.path.join(w, 'classes'), '@' + os.path.join(w, 'srcs.txt')],
                       stderr=subprocess.PIPE, text=True)
    errors = '\n'.join(l for l in r.stderr.splitlines() if not l.startswith('Note:'))
    if r.returncode != 0:
        print(errors, file=sys.stderr)
        raise SystemExit('javac failed')

    # Which half each class belongs to: by its source file.
    oj_rel = {package_relative(p) for p in oj}
    libart_rel = {package_relative(p) for p in libart_srcs}
    classes = os.path.join(w, 'classes')
    counts = {'oj': 0, 'libart': 0}
    for root, _, files in os.walk(classes):
        for f in files:
            full = os.path.join(root, f)
            name, source = source_file_of(open(full, 'rb').read())
            rel = os.path.dirname(name) + '/' + source if '/' in name else source
            if rel in oj_rel:
                half = 'oj'
            elif rel in libart_rel:
                half = 'libart'
            else:
                raise SystemExit('class %s: source %s is in neither half' % (name, rel))
            dest = os.path.join(w, half, os.path.relpath(full, classes))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copy(full, dest)
            counts[half] += 1
    print('classes: core-oj %(oj)d, core-libart %(libart)d' % counts, flush=True)
    all_jar = os.path.join(out, 'core-all-classes.jar')
    jar_dir(classes, all_jar)
    for half in ('oj', 'libart'):
        jar_dir(os.path.join(w, half), os.path.join(w, half + '-classes.jar'))

    d8 = find_d8()
    for half, other in (('oj', 'libart'), ('libart', 'oj')):
        subprocess.run([d8, '--release', '--min-api', '26',
                        '--lib', os.path.join(w, other + '-classes.jar'),
                        '--output', os.path.join(w, 'dex-' + half),
                        os.path.join(w, half + '-classes.jar')], check=True)

    # Resources, as LOCAL_JAVA_RESOURCE_DIRS of each module.
    copy_resources(os.path.join(libcore, 'luni/src/main/java'), os.path.join(w, 'res-oj'))
    copy_resources(os.path.join(libcore, 'ojluni/src/main/resources'), os.path.join(w, 'res-oj'))
    copy_resources(os.path.join(icu4j, 'resources'), os.path.join(w, 'res-libart'))
    for half, jar in (('oj', 'core-oj.jar'), ('libart', 'core-libart.jar')):
        for dex in glob.glob(os.path.join(w, 'dex-' + half, 'classes*.dex')):
            shutil.copy(dex, os.path.join(w, 'res-' + half))
        jar_dir(os.path.join(w, 'res-' + half), os.path.join(out, jar))
        n = len(glob.glob(os.path.join(w, 'dex-' + half, 'classes*.dex')))
        print('built %s (%d dex files)' % (os.path.join(out, jar), n))


if __name__ == '__main__':
    main(*sys.argv[1:])
