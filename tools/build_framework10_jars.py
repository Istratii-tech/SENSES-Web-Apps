#!/usr/bin/env python3
"""Builds the boot class path jars of the Android 10 framework (AOSP
android-10.0.0_r47) for ART 10 from source, the way Soong builds their
modules (the 8.1 counterpart is tools/build_framework8_jars.sh, which reads
8.1's Android.mk files). core-oj and core-libart are topic art10's
(tools/build_core10_jars.py); everything here compiles against its
core-all-classes.jar. javac (source/target 8) -> d8 --min-api 29 (dex 039,
native multidex) -> jar with the module's resources. Soong's jarjar rules are
applied to the sources (package renames of copies), as fw6/fw8 did.

  build_framework10_jars.py OUT libs
      conscrypt.jar      external/conscrypt (repackaged/: com.android.org.conscrypt;
                         NativeConstants.java from BoringSSL's headers)
      okhttp.jar         external/okhttp (repackaged/: com.android.okhttp)
      bouncycastle.jar   external/bouncycastle (repackaged/: com.android.org.bouncycastle)
      apache-xml.jar     external/apache-xml (+ resources)
      ext.jar            libphonenumber-platform, nist-sip, tagsoup, rappor (against
                         jsr305), libtextclassifier-java (frameworks/base/Android.bp,
                         module ext)
  build_framework10_jars.py OUT framework GENERATED.java...
      framework.jar      module framework (framework-defaults): .java of
                         FRAMEWORKS_BASE_SUBDIRS, its .aidl list and :*_aidl
                         filegroups of other projects, .logtags,
                         framework-javastream-protos, framework-statslog-gen
                         (stats-log-api-gen), :platform-properties and
                         :apex-properties (sysprop_java), its static_libs
                         (apex_aidl_interface-java, framework-protos and
                         devicepolicyprotosnano: javanano; game-driver-protos:
                         protobuf lite; the HIDL Java of hidl-gen), jarjar
                         android.hidl.** -> android.internal.hidl.**, and the
                         generated .java given (R.java etc.)
      updatable-media.jar  frameworks/base/media (the media APEX's
                         MediaSession2...), compiled with framework.jar in
                         one javac run (they refer to each other: Soong
                         breaks the cycle with stubs) and split off by class
  build_framework10_jars.py OUT opt
      telephony-common.jar, voip-common.jar, ims-common.jar,
      android.test.base.jar, org.apache.http.legacy.jar (not on the boot
      class path in 10: the shared library of apps that ask for it)

"libs" needs topic art10's jars, "framework" needs "libs", "opt" needs
"framework". Each jar NAME leaves NAME-java/NAME-classes.jar (javac output)
for compiling against it.
Env: AOSP10 (default third_party/aosp10), CORE10_CLASSES (the class path of
the core library; default build/art10/framework/core-all-classes.jar), HOST
(aprotoc, protoc-gen-javastream, hidl-gen, stats-log-api-gen, sysprop_java;
default build/fw10/host), D8, AIDL (default: next to D8), ANDROID_SDK.
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import zipfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.dont_write_bytecode = True  # (no __pycache__ in tools/)
sys.path.insert(0, TOOLS)
from bp_srcs import Parser  # noqa: E402

A10 = os.path.abspath(os.environ.get('AOSP10', os.path.join(ROOT, 'third_party/aosp10')))
FWB = os.path.join(A10, 'frameworks/base')
CORE = os.path.abspath(os.environ.get('CORE10_CLASSES',
                                      os.path.join(ROOT, 'build/art10/framework/core-all-classes.jar')))
HOST = os.path.abspath(os.environ.get('HOST', os.path.join(ROOT, 'build/fw10/host')))
MIN_API = '29'


def find_d8():
    if os.environ.get('D8'):
        return os.environ['D8']
    sdk = os.environ.get('ANDROID_SDK', os.path.expanduser('~/Library/Android/sdk'))
    tools = sorted(glob.glob(os.path.join(sdk, 'build-tools', '*', 'd8')),
                   key=lambda p: [int(x) if x.isdigit() else x
                                  for x in re.split(r'[.-]', p.split('/')[-2])])
    return tools[-1]


D8 = find_d8()
AIDL = os.environ.get('AIDL', os.path.join(os.path.dirname(D8), 'aidl'))


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        raise SystemExit('failed (%d): %s' % (r.returncode, ' '.join(cmd)[:400]))
    return r


# --- Android.bp ---------------------------------------------------------------------------
_bp_cache = {}


def bp_modules(bp):
    if bp not in _bp_cache:
        _bp_cache[bp] = {m.get('name'): m for m in Parser(open(bp).read()).parse().modules}
    return _bp_cache[bp]


def prop(bp, name, key):
    """A list property of module |name| of |bp| (nested keys dotted)."""
    node = bp_modules(bp)[name]
    for k in key.split('.')[:-1]:
        node = node.get(k, {})
    v = node.get(key.split('.')[-1], [])
    return v if isinstance(v, list) else [v]


# Where the :filegroups the modules here refer to are (Soong finds them
# by name anywhere in the tree).
FILEGROUPS = {
    'libbluetooth-binder-aidl': 'system/bt/binder/Android.bp',
    'libbinder_aidl': 'frameworks/native/libs/binder/Android.bp',
    'libcamera_client_aidl': 'frameworks/av/camera/Android.bp',
    'libcamera_client_framework_aidl': 'frameworks/av/camera/Android.bp',
    'keystore_aidl': 'system/security/keystore/Android.bp',
    'libaudioclient_aidl': 'frameworks/av/media/libaudioclient/Android.bp',
    'libupdate_engine_aidl': 'system/update_engine/Android.bp',
    'storaged_aidl': 'system/core/storaged/Android.bp',
    'vold_aidl': 'system/vold/Android.bp',
    'gsiservice_aidl': 'system/gsid/Android.bp',
    'installd_aidl': 'frameworks/native/cmds/installd/Android.bp',
    'dumpstate_aidl': 'frameworks/native/cmds/dumpstate/Android.bp',
    'incidentcompanion_aidl': 'frameworks/native/libs/incidentcompanion/Android.bp',
    'apex-properties': 'system/apex/apexd/sysprop/Android.bp',
    'platform-properties': 'system/libsysprop/srcs/Android.bp',
    'libincident_aidl': 'frameworks/base/Android.bp',
    'statsd_aidl': 'frameworks/base/Android.bp',
    'libvibrator_aidl': 'frameworks/base/Android.bp',
    'updatable-media-srcs': 'frameworks/base/media/Android.bp',
    'mediasession2-srcs': 'frameworks/base/media/Android.bp',
    'conscrypt_java_files': 'external/conscrypt/Android.bp',
    'conscrypt_public_api_files': 'external/conscrypt/Android.bp',
    'bouncycastle_java_files': 'external/bouncycastle/Android.bp',
    'opt-telephony-common-srcs': 'frameworks/opt/telephony/Android.bp',
}


def srcs_of(bp, name, key='srcs', special=None):
    """The files of property |key| of module |name| (absolute paths): globs
    expanded relative to the module's directory, :filegroups followed
    (FILEGROUPS), exclude_srcs removed. A :name in |special| is kept as it is
    (generated sources the caller makes)."""
    base = os.path.dirname(bp)
    out = []
    for item in prop(bp, name, key):
        if item.startswith(':'):
            if special is not None and item[1:] in special:
                out.append(item)
                continue
            fg = FILEGROUPS.get(item[1:])
            if fg is None:
                print('warning: filegroup %s of %s is not known; left out' % (item, name),
                      file=sys.stderr)
                continue
            out += srcs_of(os.path.join(A10, fg), item[1:], 'srcs', special)
        elif '*' in item:
            out += sorted(glob.glob(os.path.join(base, item), recursive=True))
        else:
            out.append(os.path.join(base, item))
    if key == 'srcs':
        excluded = set()
        for item in prop(bp, name, 'exclude_srcs'):
            excluded |= set(glob.glob(os.path.join(base, item), recursive=True)) \
                if '*' in item else {os.path.join(base, item)}
        out = [f for f in out if f not in excluded]
    return list(dict.fromkeys(out))


# --- compile, dex, jar ------------------------------------------------------------------------
def classes(out, name):
    return os.path.join(out, name + '-java', name + '-classes.jar')


def jar_dir(src_dir, jar):
    if os.path.exists(jar):
        os.remove(jar)
    with zipfile.ZipFile(jar, 'w', zipfile.ZIP_DEFLATED) as z:
        for root, _, files in sorted(os.walk(src_dir)):
            for f in sorted(files):
                full = os.path.join(root, f)
                z.write(full, os.path.relpath(full, src_dir))


JAVAC_TO_JAR = os.path.join(TOOLS, 'javac_jar', 'JavacToJar.java')


def javac(w, name, srcs, cp=(), extra=()):
    """javac -source 8 of |srcs| into w/NAME-classes.jar. The classes go
    straight into the jar (tools/javac_jar/JavacToJar.java): the file system
    of a Mac is case-insensitive, and some classes of a package differ only
    in case."""
    os.makedirs(w, exist_ok=True)
    empty = os.path.join(w, 'empty')
    os.makedirs(empty, exist_ok=True)
    with open(os.path.join(w, 'srcs.txt'), 'w') as f:
        for s in srcs:
            f.write('"%s"\n' % s)
    print('javac %s: %d files' % (name, len(srcs)), flush=True)
    jar = os.path.join(w, name + '-classes.jar')
    r = subprocess.run(['java', '-Xmx6g', JAVAC_TO_JAR, jar, os.path.join(w, 'srcs.txt'),
                        '-source', '8', '-target', '8', '-bootclasspath', CORE,
                        '-sourcepath', empty, '-cp', ':'.join(cp) if cp else empty,
                        '-encoding', 'UTF-8', '-nowarn', '-Xlint:-options', '-Xmaxerrs', '400',
                        '-XDstringConcat=inline', '-parameters', '-g'] + list(extra),
                       stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        raise SystemExit('javac failed for ' + name)
    return jar


def dex_jar(out, name, cls_jar, libs=(), res=None):
    """d8 |cls_jar| against the core classes and |libs| into OUT/NAME.jar with
    the resources of directory |res|."""
    w = os.path.join(out, name + '-java')
    dex = os.path.join(w, 'dex')
    jd = os.path.join(w, 'jar')
    shutil.rmtree(dex, ignore_errors=True)
    shutil.rmtree(jd, ignore_errors=True)
    os.makedirs(dex)
    os.makedirs(jd)
    cmd = [D8, '--release', '--min-api', MIN_API, '--lib', CORE]
    for l in libs:
        cmd += ['--lib', l]
    run(cmd + ['--output', dex, cls_jar])
    if res and os.path.isdir(res):
        shutil.copytree(res, jd, dirs_exist_ok=True)
    dexes = sorted(glob.glob(os.path.join(dex, 'classes*.dex')))
    for d in dexes:
        shutil.copy(d, jd)
    jar_dir(jd, os.path.join(out, name + '.jar'))
    print('built %s (%d dex, %d bytes)' % (os.path.join(out, name + '.jar'), len(dexes),
                                          sum(os.path.getsize(d) for d in dexes)), flush=True)


def copy_resources(src_dir, dest):
    """Non-Java files under src_dir (java_resource_dirs)."""
    for root, _, files in os.walk(src_dir):
        for f in files:
            if f.endswith(('.java', '.html', '.aidl', '.logtags')) or f.startswith('.'):
                continue
            rel = os.path.relpath(os.path.join(root, f), src_dir)
            os.makedirs(os.path.join(dest, os.path.dirname(rel)), exist_ok=True)
            shutil.copy(os.path.join(root, f), os.path.join(dest, rel))


def jarjar(files, dest, rules):
    """Copies Java files |files| (absolute paths) into |dest| (by package
    path) with package renames |rules| [(from, to)] applied to their text:
    the source form of Soong's jarjar_rules (rule FROM.** TO.@1)."""
    out = []
    for f in files:
        text = open(f, encoding='utf-8', errors='surrogateescape').read()
        for frm, to in rules:
            text = re.sub(r'(?<![\w.])' + re.escape(frm) + r'(?=[.;"*])', to, text)
            text = text.replace('"/' + frm.replace('.', '/') + '/', '"/' + to.replace('.', '/') + '/')
        m = re.search(r'^\s*package\s+([\w.]+)\s*;', text, re.M)
        pkg = m.group(1).replace('.', '/') if m else ''
        d = os.path.join(dest, pkg)
        os.makedirs(d, exist_ok=True)
        p = os.path.join(d, os.path.basename(f))
        with open(p, 'w', encoding='utf-8', errors='surrogateescape') as o:
            o.write(text)
        out.append(p)
    return out


def java_files(d):
    return sorted(glob.glob(os.path.join(d, '**', '*.java'), recursive=True))


# --- generators ---------------------------------------------------------------------------
def package_root(f):
    """The directory the package of .aidl/.java file |f| starts in."""
    with open(f, encoding='utf-8', errors='replace') as h:
        m = re.search(r'^\s*package\s+([\w.]+)\s*;', h.read(), re.M)
    if not m:
        return os.path.dirname(f)
    pkg = m.group(1).replace('.', '/')
    d = os.path.dirname(os.path.abspath(f))
    return d[:-len(pkg) - 1] if d.endswith('/' + pkg) else os.path.dirname(f)


def aidl_all(out_dir, includes, files):
    """Java of .aidl |files| (one aidl call each, as Soong), with |includes|
    and each file's own package root (the SDK's aidl wants it among the
    import paths). Declarations of parcelables only make no code: skipped
    (structured parcelables, with their fields, do). The SDK's aidl writes
    android.compat.annotation.UnsupportedAppUsage (its package since 11) for
    @UnsupportedAppUsage, with an overrideSourcePosition element: in 10
    that is android.annotation's, without it."""
    os.makedirs(out_dir, exist_ok=True)
    inc = ['-I' + d for d in includes]
    n = 0
    for f in files:
        with open(f, encoding='utf-8', errors='replace') as h:
            text = re.sub(r'//[^\n]*|/\*.*?\*/', '', h.read(), flags=re.S)
        if not re.search(r'\binterface\b|\bparcelable\s+\w+\s*\{', text):
            continue
        run([AIDL, '--min_sdk_version=' + MIN_API, '--transaction_names'] + inc +
            ['-I' + package_root(f), '-o' + out_dir, f], stdout=subprocess.DEVNULL)
        n += 1
    for j in java_files(out_dir):
        text = open(j, encoding='utf-8').read()
        if 'android.compat.annotation.' in text:
            text = text.replace('android.compat.annotation.', 'android.annotation.')
            text = re.sub(r'overrideSourcePosition="[^"]*"\s*,?\s*', '', text)
            text = re.sub(r'(@android\.annotation\.UnsupportedAppUsage)\(\s*\)', r'\1', text)
            with open(j, 'w', encoding='utf-8') as o:
                o.write(text)
    print('aidl: %d files' % n, flush=True)


def logtags(out_dir, files):
    for f in files:
        pkg = ''
        for line in open(f):
            m = re.match(r'option java_package ([\w.]+)', line)
            if m:
                pkg = m.group(1)
        dest = os.path.join(out_dir, pkg.replace('.', '/'),
                            os.path.basename(f)[:-len('.logtags')] + '.java')
        run([sys.executable, '-I', os.path.join(TOOLS, 'java_event_log_tags.py'), f, dest])


def hidl_java(out_dir, lang, fqnames):
    """hidl-gen -Ljava / -Ljava-constants of HIDL packages."""
    for fq in fqnames:
        run([os.path.join(HOST, 'hidl-gen'), '-o', out_dir, '-L' + lang,
             '-randroid.hardware:' + os.path.join(A10, 'hardware/interfaces'),
             '-randroid.hidl:' + os.path.join(A10, 'system/libhidl/transport'), fq])


def proto_java(out_dir, proto_root, files, kind, params=''):
    """aprotoc --javanano_out / --java_out=lite of |files| (relative to
    |proto_root|, the import root)."""
    os.makedirs(out_dir, exist_ok=True)
    flag = {'nano': '--javanano_out', 'lite': '--java_out'}[kind]
    opts = 'lite' if kind == 'lite' else params
    run([os.path.join(HOST, 'aprotoc'), '-I.', '-I' + os.path.join(A10, 'external/protobuf/src'),
         '%s=%s%s' % (flag, opts + ':' if opts else '', out_dir)] + files, cwd=proto_root)


def protos(d):
    return sorted(os.path.relpath(p, d) for p in glob.glob(os.path.join(d, '**', '*.proto'),
                                                            recursive=True))


NANO_RUNTIME = 'external/protobuf/javanano/src/main/java'


# --- libs -------------------------------------------------------------------------------------
def build_libs(out):
    # conscrypt: NativeConstants.java comes from a host program built against
    # BoringSSL's headers (conscrypt_generate_constants), for package
    # com.android.org.conscrypt.
    c = os.path.join(A10, 'external/conscrypt')
    w = os.path.join(out, 'conscrypt-java')
    shutil.rmtree(w, ignore_errors=True)
    gen = os.path.join(w, 'gen/com/android/org/conscrypt')
    os.makedirs(gen)
    run(['c++', '-I' + os.path.join(A10, 'external/boringssl/src/include'), '-o',
         os.path.join(w, 'generate_constants'),
         os.path.join(c, 'constants/src/gen/cpp/generate_constants.cc')])
    with open(os.path.join(gen, 'NativeConstants.java'), 'w') as f:
        run([os.path.join(w, 'generate_constants'), 'com.android.org.conscrypt'], stdout=f)
    bp = os.path.join(c, 'Android.bp')
    srcs = srcs_of(bp, 'conscrypt', special={'conscrypt_generated_constants'})
    srcs = [s for s in srcs if not s.startswith(':')] + [os.path.join(gen, 'NativeConstants.java')]
    dex_jar(out, 'conscrypt', javac(w, 'conscrypt', srcs))

    # okhttp, bouncycastle: repackaged sources (srcgen) as they are.
    for name, bp_dir, cp in (('okhttp', 'external/okhttp', [classes(out, 'conscrypt')]),
                             ('bouncycastle', 'external/bouncycastle', [])):
        w = os.path.join(out, name + '-java')
        shutil.rmtree(w, ignore_errors=True)
        os.makedirs(w)
        bp = os.path.join(A10, bp_dir, 'Android.bp')
        dex_jar(out, name, javac(w, name, srcs_of(bp, name), cp), cp)

    # apache-xml, resources from src/main/java.
    x = os.path.join(A10, 'external/apache-xml')
    w = os.path.join(out, 'apache-xml-java')
    shutil.rmtree(w, ignore_errors=True)
    os.makedirs(w)
    copy_resources(os.path.join(x, 'src/main/java'), os.path.join(w, 'res'))
    dex_jar(out, 'apache-xml', javac(w, 'apache-xml', srcs_of(os.path.join(x, 'Android.bp'),
                                                                 'apache-xml')),
            res=os.path.join(w, 'res'))

    # ext (frameworks/base/Android.bp): libphonenumber-platform (with its
    # data), nist-sip, tagsoup, rappor (against jsr305's annotations, javac
    # only), libtextclassifier-java.
    w = os.path.join(out, 'ext-java')
    shutil.rmtree(w, ignore_errors=True)
    os.makedirs(w)
    e = os.path.join(A10, 'external')
    srcs = []
    pn = os.path.join(e, 'libphonenumber/Android.bp')
    srcs += srcs_of(pn, 'libphonenumber-platform')
    for r in prop(pn, 'libphonenumber-platform', 'java_resource_dirs'):
        copy_resources(os.path.join(e, 'libphonenumber', r), os.path.join(w, 'res'))
    srcs += srcs_of(os.path.join(e, 'nist-sip/Android.bp'), 'nist-sip')
    srcs += srcs_of(os.path.join(e, 'tagsoup/Android.bp'), 'tagsoup')
    srcs += srcs_of(os.path.join(e, 'rappor/Android.bp'), 'rappor')
    srcs += srcs_of(os.path.join(e, 'libtextclassifier/Android.bp'), 'libtextclassifier-java')
    # javax.annotation (external/jsr305) for rappor: compile-time only (libs).
    jsr = os.path.join(e, 'jsr305/Android.bp')
    jsr_jar = javac(os.path.join(w, 'jsr'), 'jsr305', srcs_of(jsr, 'jsr305'))
    dex_jar(out, 'ext', javac(w, 'ext', srcs, [jsr_jar]), [jsr_jar], os.path.join(w, 'res'))


def lib_classes(out):
    return [classes(out, n) for n in ('conscrypt', 'okhttp', 'bouncycastle', 'apache-xml', 'ext')]


# --- framework ---------------------------------------------------------------------------------
# module framework's static_libs that hidl-gen makes (hidl_interface java and
# java-constants libraries).
FW_HIDL_JAVA = ['android.hidl.base@1.0', 'android.hidl.safe_union@1.0', 'android.hardware.cas@1.1', 'android.hardware.cas@1.0',
                'android.hardware.contexthub@1.0', 'android.hardware.radio@1.0',
                'android.hardware.radio@1.1', 'android.hardware.radio@1.2', 'android.hardware.radio@1.3',
                'android.hardware.radio@1.4', 'android.hardware.thermal@1.0',
                'android.hardware.thermal@1.1', 'android.hardware.thermal@2.0',
                'android.hardware.usb.gadget@1.0', 'android.hardware.vibrator@1.0',
                'android.hardware.vibrator@1.1', 'android.hardware.vibrator@1.2',
                'android.hardware.vibrator@1.3']
FW_HIDL_CONSTANTS = ['android.hardware.health@1.0', 'android.hardware.thermal@1.0',
                     'android.hardware.tv.input@1.0', 'android.hardware.usb@1.0',
                     'android.hardware.usb@1.1', 'android.hardware.usb@1.2', 'android.hardware.wifi@1.0']
# framework's jarjar_rules (jarjar_rules_hidl.txt).
FW_JARJAR = [('android.hidl', 'android.internal.hidl')]


def build_framework(out, generated):
    bp = os.path.join(FWB, 'Android.bp')
    w = os.path.join(out, 'framework-java')
    shutil.rmtree(w, ignore_errors=True)
    os.makedirs(w)
    gen = os.path.join(w, 'gen')
    special = {'framework-javastream-protos', 'framework-statslog-gen'}
    srcs = srcs_of(bp, 'framework-defaults', special=special)
    # The media APEX's classes: compiled here, split off below.
    media_bp = os.path.join(FWB, 'media/Android.bp')
    media = srcs_of(media_bp, 'updatable-media')
    srcs += media
    java = [s for s in srcs if s.endswith('.java')]
    aidl = [s for s in srcs if s.endswith('.aidl')]
    tags = [s for s in srcs if s.endswith('.logtags')]
    sysprop = [s for s in srcs if s.endswith('.sysprop')]
    other = [s for s in srcs if not s.endswith(('.java', '.aidl', '.logtags', '.sysprop'))
             and not s.startswith(':')]
    if other:
        print('warning: sources left out: %s' % other[:10], file=sys.stderr)

    # .aidl: the module's include dirs (export_include_dirs relative to it,
    # include_dirs to the tree), the media module's.
    incs = [os.path.join(FWB, d) for d in prop(bp, 'framework-defaults', 'aidl.export_include_dirs')]
    incs += [os.path.join(A10, d) for d in prop(bp, 'framework-defaults', 'aidl.include_dirs')]
    incs += [os.path.join(FWB, 'media/apex/java')]
    aidl_all(os.path.join(gen, 'aidl'), incs, aidl)
    logtags(os.path.join(gen, 'logtags'), tags)

    # framework-javastream-protos (tools/genprotos.sh): core/proto, imports
    # relative to the tree top.
    jstream = os.path.join(gen, 'javastream')
    os.makedirs(jstream)
    core_protos = sorted(os.path.relpath(p, A10) for p in
                         glob.glob(os.path.join(FWB, 'core/proto/**/*.proto'), recursive=True))
    run([os.path.join(HOST, 'aprotoc'),
         '--plugin=protoc-gen-javastream=' + os.path.join(HOST, 'protoc-gen-javastream'),
         '-Iexternal/protobuf/src', '-I.', '--javastream_out=' + jstream] + core_protos, cwd=A10)
    # framework-statslog-gen.
    sl = os.path.join(gen, 'statslog/android/util')
    os.makedirs(sl)
    run([os.path.join(HOST, 'stats-log-api-gen'), '--java', os.path.join(sl, 'StatsLogInternal.java')])
    # :platform-properties, :apex-properties.
    sp = os.path.join(gen, 'sysprop')
    os.makedirs(sp)
    for s in sysprop:
        run([os.path.join(HOST, 'sysprop_java'), '--java-output-dir', sp, s])

    # static_libs. framework-protos, devicepolicyprotosnano: javanano with its
    # runtime, jarjar com.google.protobuf.nano -> com.android.framework.protobuf.nano.
    nano = os.path.join(gen, 'nano-a')
    proto_java(nano, os.path.join(FWB, 'proto'), ['src/' + f for f in protos(os.path.join(FWB, 'proto/src'))],
               'nano')
    dp = os.path.join(FWB, 'core/proto/android/stats/devicepolicy')
    proto_java(nano, A10, sorted(os.path.relpath(p, A10) for p in glob.glob(os.path.join(dp, '*.proto'))),
               'nano')
    nano_srcs = jarjar(java_files(nano) + java_files(os.path.join(A10, NANO_RUNTIME)),
                       os.path.join(gen, 'nano'),
                       [('com.google.protobuf.nano', 'com.android.framework.protobuf.nano')])
    # game-driver-protos: protobuf lite (sdk_version 28) with the lite
    # runtime, jarjar com.google.protobuf -> com.android.framework.protobuf.
    lite = os.path.join(gen, 'lite-a')
    proto_java(lite, os.path.join(FWB, 'graphics/proto'), ['game_driver.proto'], 'lite')
    pb_bp = os.path.join(A10, 'external/protobuf/Android.bp')
    lite_srcs = jarjar(java_files(lite) + srcs_of(pb_bp, 'libprotobuf-java-lite'),
                       os.path.join(gen, 'lite'),
                       [('com.google.protobuf', 'com.android.framework.protobuf')])
    # apex_aidl_interface-java (aidl_interface of system/apex/apexd).
    apex_bp = os.path.join(A10, 'system/apex/apexd/Android.bp')
    apex_aidl = srcs_of(apex_bp, 'apex_aidl_interface')
    aidl_all(os.path.join(gen, 'apex-aidl'), [os.path.join(A10, 'system/apex/apexd/aidl')],
             apex_aidl)
    # The HIDL Java.
    hidl = os.path.join(gen, 'hidl')
    hidl_java(hidl, 'java', FW_HIDL_JAVA)
    hidl_java(hidl, 'java-constants', FW_HIDL_CONSTANTS)

    # jarjar android.hidl.** -> android.internal.hidl.** over the whole
    # module: files that mention android.hidl are copied with it renamed.
    all_java = (java + java_files(os.path.join(gen, 'aidl')) + java_files(os.path.join(gen, 'logtags')) +
                java_files(jstream) + java_files(os.path.join(gen, 'statslog')) + java_files(sp) +
                nano_srcs + lite_srcs + java_files(os.path.join(gen, 'apex-aidl')) + java_files(hidl) +
                [os.path.abspath(g) for g in generated])
    hidl_files = [f for f in all_java if 'android.hidl' in open(f, encoding='utf-8', errors='replace').read()]
    renamed = jarjar(hidl_files, os.path.join(gen, 'jarjar'), FW_JARJAR)
    hidl_set = set(hidl_files)
    all_java = [f for f in all_java if f not in hidl_set] + renamed
    # (Soong's default class path of platform modules has the jars of the
    # runtime and conscrypt APEXes besides libs: ext.)
    cls_jar = javac(w, 'framework-all', all_java, lib_classes(out))

    # Split: updatable-media's classes (top-level classes of its sources and
    # their inner classes) out of framework.jar.
    # (an .aidl's Java has the class name of the .aidl)
    media_names = {os.path.splitext(os.path.relpath(f, package_root(f)))[0] for f in media}
    os.makedirs(os.path.dirname(classes(out, 'updatable-media')), exist_ok=True)
    moved = 0
    with zipfile.ZipFile(cls_jar) as src, \
            zipfile.ZipFile(classes(out, 'framework'), 'w', zipfile.ZIP_DEFLATED) as fw, \
            zipfile.ZipFile(classes(out, 'updatable-media'), 'w', zipfile.ZIP_DEFLATED) as um:
        for e in src.infolist():
            if e.is_dir():
                continue
            top = e.filename[:-len('.class')].split('$')[0]
            if top in media_names:
                um.writestr(e.filename, src.read(e))
                moved += 1
            else:
                fw.writestr(e.filename, src.read(e))
    print('updatable-media: %d classes split off' % moved, flush=True)
    os.remove(cls_jar)
    res = os.path.join(w, 'res')
    os.makedirs(res)
    shutil.copy(os.path.join(FWB, 'config/preloaded-classes'), res)
    fw_libs = lib_classes(out) + [classes(out, 'updatable-media')]
    dex_jar(out, 'framework', classes(out, 'framework'), fw_libs, res)
    dex_jar(out, 'updatable-media', classes(out, 'updatable-media'), [classes(out, 'framework')])


# --- opt ---------------------------------------------------------------------------------------
def build_opt(out):
    fw = [classes(out, 'framework')] + lib_classes(out)
    # android.test.base (frameworks/base/test-base): junit.framework, android.test.
    w = os.path.join(out, 'android.test.base-java')
    shutil.rmtree(w, ignore_errors=True)
    os.makedirs(w)
    tb = os.path.join(FWB, 'test-base/Android.bp')
    dex_jar(out, 'android.test.base', javac(w, 'android.test.base', srcs_of(tb, 'android.test.base'), fw),
            fw)

    def opt_lib(name, d, cp, extra=()):
        w = os.path.join(out, name + '-java')
        os.makedirs(w, exist_ok=True)
        src = os.path.join(A10, d, 'src/java')
        aidl_all(os.path.join(w, 'aidl'), [src, os.path.join(FWB, 'core/java'),
                                           os.path.join(FWB, 'telephony/java')],
                 sorted(glob.glob(os.path.join(src, '**', 'I*.aidl'), recursive=True)))
        logtags(os.path.join(w, 'logtags'), sorted(glob.glob(os.path.join(src, '**', '*.logtags'),
                                                             recursive=True)))
        srcs = java_files(src) + java_files(os.path.join(w, 'aidl')) + \
            java_files(os.path.join(w, 'logtags')) + list(extra)
        dex_jar(out, name, javac(w, name, srcs, cp), cp)

    for n in ('voip-common', 'ims-common', 'telephony-common'):
        shutil.rmtree(os.path.join(out, n + '-java'), ignore_errors=True)
    opt_lib('voip-common', 'frameworks/opt/net/voip', fw)
    opt_lib('ims-common', 'frameworks/opt/net/ims', fw)
    # telephony-common: + telephony-protos (javanano, store_unknown_fields,
    # enum_style=java, jarjar to com.android.internal.telephony.protobuf.nano),
    # ecc-protos-lite (packages/services/Telephony, the same with jarjar to
    # com.android.phone.ecc.nano) and the shallow radio.config/radio.deprecated
    # HIDL Java; framework's jarjar rules (android.hidl.** ->
    # android.internal.hidl.**, as framework.jar has them). Its libs has
    # services (system_server's) for NetworkStatsService.VT_INTERFACE:
    # compiled against a stub with it.
    t = os.path.join(A10, 'frameworks/opt/telephony')
    w = os.path.join(out, 'telephony-common-java')
    os.makedirs(w)
    params = 'store_unknown_fields=true,enum_style=java'
    nano = os.path.join(w, 'nano-a')
    proto_java(nano, os.path.join(t, 'proto'), ['src/' + f for f in protos(os.path.join(t, 'proto/src'))], 'nano',
               params)
    nano_srcs = jarjar(java_files(nano) + java_files(os.path.join(A10, NANO_RUNTIME)),
                       os.path.join(w, 'nano'),
                       [('com.google.protobuf.nano', 'com.android.internal.telephony.protobuf.nano')])
    ecc = os.path.join(w, 'ecc-a')
    proto_java(ecc, os.path.join(A10, 'packages/services/Telephony/ecc/conversion_toolset_v1/proto'),
               ['protobuf_ecc_data.proto'], 'nano', params)
    ecc_srcs = jarjar(java_files(ecc) + java_files(os.path.join(A10, NANO_RUNTIME)),
                      os.path.join(w, 'ecc'), [('com.google.protobuf.nano', 'com.android.phone.ecc.nano')])
    hidl = os.path.join(w, 'hidl')
    hidl_java(hidl, 'java', ['android.hardware.radio.config@1.0', 'android.hardware.radio.config@1.1',
                             'android.hardware.radio.config@1.2', 'android.hardware.radio.deprecated@1.0'])
    # (the shallow libraries leave out the packages they import, which
    # framework.jar has.)
    shallow = [f for f in java_files(hidl) if '/radio/config/' in f or '/radio/deprecated/' in f]
    stub = os.path.join(w, 'stub/com/android/server/net')
    os.makedirs(stub)
    with open(os.path.join(stub, 'NetworkStatsService.java'), 'w') as f:
        vt = [l.strip() for l in open(os.path.join(FWB, 'services/core/java/com/android/server/net/'
                                                        'NetworkStatsService.java'))
              if 'VT_INTERFACE =' in l]
        f.write('package com.android.server.net;\npublic class NetworkStatsService {\n    %s\n}\n' % vt[0])
    stub_jar = javac(os.path.join(w, 'stubc'), 'services-stub', [os.path.join(stub, 'NetworkStatsService.java')])
    src = os.path.join(t, 'src/java')
    aidl_all(os.path.join(w, 'aidl'), [src, os.path.join(FWB, 'core/java'), os.path.join(FWB, 'telephony/java')],
             sorted(glob.glob(os.path.join(src, '**', 'I*.aidl'), recursive=True)))
    logtags(os.path.join(w, 'logtags'), sorted(glob.glob(os.path.join(src, '**', '*.logtags'), recursive=True)))
    srcs = (java_files(src) + java_files(os.path.join(w, 'aidl')) + java_files(os.path.join(w, 'logtags')) +
            nano_srcs + ecc_srcs + shallow)
    hidl_files = [f for f in srcs if 'android.hidl' in open(f, encoding='utf-8', errors='replace').read()]
    hidl_set = set(hidl_files)
    srcs = [f for f in srcs if f not in hidl_set] + jarjar(hidl_files, os.path.join(w, 'jarjar'), FW_JARJAR)
    cp = fw + [classes(out, 'voip-common'), classes(out, 'ims-common')]
    dex_jar(out, 'telephony-common', javac(w, 'telephony-common', srcs, cp + [stub_jar]), cp)

    # org.apache.http.legacy (external/apache-http): the shared library apps
    # ask for with <uses-library>, against the framework.
    ah = os.path.join(A10, 'external/apache-http')
    w = os.path.join(out, 'org.apache.http.legacy-java')
    shutil.rmtree(w, ignore_errors=True)
    os.makedirs(w)
    dex_jar(out, 'org.apache.http.legacy',
            javac(w, 'org.apache.http.legacy', srcs_of(os.path.join(ah, 'Android.bp'),
                                                       'org.apache.http.legacy'), fw), fw)


def main(argv):
    out = os.path.abspath(argv[0])
    os.makedirs(out, exist_ok=True)
    what = argv[1]
    if what == 'libs':
        build_libs(out)
    elif what == 'framework':
        build_framework(out, argv[2:])
    elif what == 'opt':
        build_opt(out)
    else:
        raise SystemExit('usage: build_framework10_jars.py OUT libs|framework|opt [GENERATED.java...]')


if __name__ == '__main__':
    main(sys.argv[1:])
