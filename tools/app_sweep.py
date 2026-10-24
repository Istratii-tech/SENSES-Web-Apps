#!/usr/bin/env python3
"""arc-wasm: start many real apps headless in Node and sort out what happens
(topic apps-sweep, docs/topics/apps-sweep.md).

For each APK / .apks / .xapk / .apkm given (files or directories):
  1. reads it as SENSES Web Apps' shell does (tools/app_sweep_meta.js over
     demo/apps/apk.js): package, minSdk, ABIs, the parts of a bundle, the
     stack the shell gives it (a10; a81 in a build without 10);
  2. puts it alone in a throwaway library (senses/apps/PKG/base.apk and
     split_*.apk as symlinks, nothing copied) next to a /data cloned from a
     per-stack template (the system packages already installed);
  3. runs `SystemServer --senses PKG --shot-shown=S:PNG` of that stack in
     Node (build/{dalvik,art,art8,art10}) with a time limit, the JIT on;
  4. sorts the run: "shown" (first frame of an app window), "started" (its
     launcher activity started, no frame), "crash" / "hang" / "install" ...
     with a cause (Java exception and its top frame in the app, native trap,
     unresolved symbol, missing service or API, GMS, multiprocess, Vulkan,
     OOM) from the VM's output;
  5. what did not show is run again with the JIT off and on the other
     stacks it could run on (--retry), to tell the runtime's faults from
     the app's.

  python3 -I tools/app_sweep.py [--out DIR] [-j 2] [--limit 240] [--only PKG,..]
      [--stacks a81,a10] [--retry jit,stack | none] [--rerun] FILE|DIR...

Results: OUT/results.json and OUT/results.csv (one row per app: the best
run first), OUT/table.md; per run OUT/runs/NAME/STACK-jitN.{log,png}.
Default OUT: $ARC_WORKSPACE/apps-sweep-out, else build/app-sweep-out (logs and
screenshots stay out of git), with results.* also copied to build/app-sweep/.
"""
import argparse
import csv
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

sys.dont_write_bytecode = True  # (tools/apk_survey.py is imported: no __pycache__ in tools/)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = (os.path.join(os.environ['ARC_WORKSPACE'], 'apps-sweep-out') if os.environ.get('ARC_WORKSPACE')
               else os.path.join(ROOT, 'build', 'app-sweep-out'))
STACKS = {
    # stack: (build dir, script, API, extra VM args before the class)
    'a44': ('dalvik', 'dalvikvm.js', 19, ['-Xmx256m']),
    'a60': ('art', 'artvm.js', 23, []),
    'a81': ('art8', 'artvm8.js', 27, []),
    'a10': ('art10', 'artvm10.js', 29, []),
}
STACK_ORDER = ['a81', 'a10', 'a60', 'a44']
EXTS = ('.apk', '.apks', '.xapk', '.apkm')
LIVE = set()  # the VMs running (their process groups go with the sweep)


def kill_live(*_):
    for p in list(LIVE):
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except OSError:
            pass
    os._exit(130)

# --- reading the apps ---------------------------------------------------------


def collect(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            for dirpath, _, names in os.walk(p):
                for n in sorted(names):
                    if n.lower().endswith(EXTS) and not n.startswith('.'):
                        files.append(os.path.join(dirpath, n))
        elif p.lower().endswith(EXTS):
            files.append(p)
    return files


def name_of(path):
    n = os.path.basename(path)
    n = re.sub(r'\.(apk|apks|xapk|apkm)$', '', n, flags=re.I)
    return re.sub(r'[^A-Za-z0-9._+-]+', '_', n)


def read_meta(path, cache):
    """The shell's view of the file (cached by size and mtime)."""
    st = os.stat(path)
    key = '%d-%d' % (st.st_size, int(st.st_mtime))
    f = os.path.join(cache, name_of(path) + '.json')
    try:
        with open(f) as fh:
            m = json.load(fh)
        if m.get('_key') == key:
            return m
    except (OSError, ValueError):
        pass
    out = subprocess.run(['node', os.path.join(ROOT, 'tools', 'app_sweep_meta.js'), path],
                         capture_output=True, text=True, timeout=600)
    try:
        m = json.loads(out.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        m = {'error': 'meta', 'message': (out.stderr or out.stdout)[-500:]}
    m['_key'] = key
    os.makedirs(cache, exist_ok=True)
    with open(f, 'w') as fh:
        json.dump(m, fh)
    return m


def static_hints(path, meta, cache):
    """What the manifest and the code promise (from the APK, not the run):
    android:process components, GMS, Vulkan, engines. Cheap: names in the
    zip and strings in the manifest/dex, not a full survey."""
    f = os.path.join(cache, name_of(path) + '.hints.json')
    try:
        with open(f) as fh:
            h = json.load(fh)
        if h.get('_key') == meta.get('_key'):
            return h
    except (OSError, ValueError):
        pass
    import zipfile
    h = {'_key': meta.get('_key'), 'engines': [], 'process': 0, 'gms': False,
         'vulkan': False, 'libs': 0}
    blobs = []
    man_blob = [b'']
    try:
        outer = zipfile.ZipFile(path)
        inner = []
        if meta.get('kind') not in (None, 'apk'):
            for p in meta.get('parts', []):
                try:
                    import io
                    inner.append(zipfile.ZipFile(io.BytesIO(outer.read(p['path']))))
                except Exception:
                    pass
        else:
            inner = [outer]
        names = []
        for z in inner:
            for n in z.namelist():
                names.append(n)
                if n == 'AndroidManifest.xml' and z is inner[0]:
                    man_blob[0] = z.read(n)
                elif re.match(r'classes\d*\.dex$', n):
                    blobs.append(z.read(n))
        libs = [n for n in names if n.startswith('lib/') and n.endswith('.so')]
        h['libs'] = len(libs)
        base = set(os.path.basename(n) for n in libs)
        eng = []
        if 'libunity.so' in base or 'libil2cpp.so' in base:
            eng.append('Unity')
        if 'libUE4.so' in base or 'libUnreal.so' in base:
            eng.append('Unreal')
        if 'libflutter.so' in base:
            eng.append('Flutter')
        if base & {'libreactnativejni.so', 'libhermes.so', 'libjsc.so', 'libreactnative.so'}:
            eng.append('ReactNative')
        if 'libgodot_android.so' in base:
            eng.append('Godot')
        if 'libgdx.so' in base:
            eng.append('libGDX')
        if 'libmonodroid.so' in base:
            eng.append('Xamarin')
        if 'libcocos2dcpp.so' in base or any('cocos' in b for b in base):
            eng.append('Cocos')
        if 'libSDL2.so' in base:
            eng.append('SDL')
        if 'libpython3.so' in base or any(b.startswith('libpython') for b in base):
            eng.append('Python')
        if 'libxul.so' in base:
            eng.append('Gecko')
        if any(n.startswith('assets/public/') and n.endswith('index.html') for n in names) \
                or 'assets/capacitor.config.json' in names:
            eng.append('Capacitor/Cordova')
        h['engines'] = eng
        dex = b''.join(blobs)
        h['gms'] = b'com/google/android/gms/common/GoogleApiAvailability' in dex \
            or b'com/google/android/gms/common/GooglePlayServicesUtil' in dex
        h['vulkan'] = any('vulkan' in b.lower() or b.startswith('libVkLayer') for b in base)
        h['kotlin'] = b'kotlin/jvm/internal' in dex
        h['compose'] = b'androidx/compose/runtime' in dex
        # components in another process (android:process; ":name" or a full name)
        if man_blob[0]:
            sys.path.insert(0, os.path.join(ROOT, 'tools'))
            import apk_survey
            m = apk_survey.parse_axml(man_blob[0])
            pkg = m['attrs'].get('package', '')
            procs = set()
            for app in apk_survey.walk(m, 'application'):
                for c in app['children']:
                    pr = c['attrs'].get('process')
                    if isinstance(pr, str) and pr and pr != pkg:
                        procs.add(pr)
                        if c['attrs'].get('isolatedProcess') is True:
                            h['isolated'] = h.get('isolated', 0) + 1
            h['process'] = len(procs)
            h['processes'] = sorted(procs)[:8]
            for el in apk_survey.walk(m, 'activity'):
                if el['attrs'].get('name') == 'android.app.NativeActivity':
                    h['nativeActivity'] = True
            for el in apk_survey.walk(m, 'meta-data'):
                if el['attrs'].get('name') == 'android.app.lib_name':
                    h['nativeActivity'] = True
    except Exception as e:  # a bad zip: the run says the rest
        h['error'] = str(e)
    with open(f, 'w') as fh:
        json.dump(h, fh)
    return h


# --- the library and /data of a run ------------------------------------------


def extract_parts(path, meta, cache):
    """A bundle's chosen parts as files (once, in the cache): [(file, split)]."""
    import zipfile
    d = os.path.join(cache, 'parts', name_of(path))
    stamp = os.path.join(d, '.key')
    out = []
    for p in meta['parts']:
        file = 'base.apk' if p['kind'] == 'base' else 'split_%s.apk' % p['split']
        out.append((os.path.join(d, file), file))
    try:
        with open(stamp) as fh:
            if fh.read() == meta['_key']:
                return out
    except OSError:
        pass
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    z = zipfile.ZipFile(path)
    for p, (f, _) in zip(meta['parts'], out):
        with z.open(p['path']) as src, open(f, 'wb') as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
    for o in meta.get('obb', []):
        od = os.path.join(d, 'obb')
        os.makedirs(od, exist_ok=True)
        with z.open(o['path']) as src, open(os.path.join(od, o['file']), 'wb') as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
    with open(stamp, 'w') as fh:
        fh.write(meta['_key'])
    return out


def clone_tree(src, dst):
    """APFS clone (cp -c): instant and free until written."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    r = subprocess.run(['cp', '-cR', src, dst], capture_output=True)
    if r.returncode != 0:
        shutil.copytree(src, dst, symlinks=True)


def vm_command(stack, pkg, opts):
    d, js, _, pre = STACKS[stack]
    return ['node', js] + pre + ['arcwasm.server.SystemServer', '--senses'] + opts + [pkg]


def vm_env(stack, rundir, jit, obb_pkg=None):
    env = dict(os.environ)
    env.update({'ARC_DATA_DIR': 'senses/data/' + stack, 'ARC_APPS_DIR': 'senses/apps',
                'ARC_NODE_OPFS_DIR': rundir})
    if not jit:
        env['ARC_JIT'] = '0'
    if obb_pkg:
        env['ARC_APP_OBB'] = obb_pkg
    return env


def make_template(stack, work, build):
    """/data of |stack| with the image's own packages installed: a run of
    SystemServer --senses for a package that is not in the (empty) library."""
    t = os.path.join(work, 'template-' + stack)
    if os.path.isdir(os.path.join(t, 'senses', 'data', stack, 'system')):
        return t
    shutil.rmtree(t, ignore_errors=True)
    os.makedirs(os.path.join(t, 'senses', 'apps'))
    d, js, _, _ = STACKS[stack]
    cmd = vm_command(stack, 'org.arcwasm.none', [])
    log = os.path.join(work, 'template-%s.log' % stack)
    with open(log, 'w') as fh:
        p = subprocess.Popen(cmd, cwd=os.path.join(build, d), env=vm_env(stack, t, True),
                             stdout=fh, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            p.wait(timeout=900)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            raise SystemExit('template %s: the VM did not finish (%s)' % (stack, log))
    if not os.path.isdir(os.path.join(t, 'senses', 'data', stack)):
        raise SystemExit('template %s: no /data (%s)' % (stack, log))
    return t


# --- a run ---------------------------------------------------------------------

END_EVENTS = re.compile(r'^SENSES: app (crash|exit|missing) ')


def memory_free_percent():
    try:
        out = subprocess.run(['memory_pressure'], capture_output=True, text=True,
                             timeout=30).stdout
        m = re.search(r'free percentage: (\d+)%', out)
        return int(m.group(1)) if m else 100
    except Exception:
        return 100


def png_blank(path):
    """A screenshot of one colour (or nearly): the window is there, nothing
    drawn (GL without WebGL in Node, a surface not yet posted)."""
    import struct
    import zlib
    try:
        with open(path, 'rb') as fh:
            data = fh.read()
        p, idat = 8, b''
        while p < len(data):
            n, kind = struct.unpack('>I4s', data[p:p + 8])
            if kind == b'IDAT':
                idat += data[p + 8:p + 8 + n]
            p += 12 + n
        raw = zlib.decompress(idat)
        return len(set(raw[:4 << 20])) <= 4
    except Exception:
        return False


def run_one(job, a):
    """One VM run -> dict (the log and screenshot in job['dir'])."""
    stack, jit, meta = job['stack'], job['jit'], job['meta']
    pkg = meta['package']
    tag = '%s-jit%d' % (stack, 1 if jit else 0)
    os.makedirs(job['dir'], exist_ok=True)
    log_path = os.path.join(job['dir'], tag + '.log')
    png_name = tag + '.png'
    rundir = os.path.join(a.runs, '%s-%s' % (job['name'], tag))
    shutil.rmtree(rundir, ignore_errors=True)
    os.makedirs(os.path.join(rundir, 'senses', 'apps', pkg))
    clone_tree(os.path.join(job['template'], 'senses', 'data', stack),
               os.path.join(rundir, 'senses', 'data', stack))
    lib = os.path.join(rundir, 'senses', 'apps', pkg)
    obb = None
    if meta.get('kind') == 'apk':
        os.symlink(job['path'], os.path.join(lib, 'base.apk'))
    else:
        for f, file in job['parts']:
            os.symlink(f, os.path.join(lib, file))
        od = os.path.join(os.path.dirname(job['parts'][0][0]), 'obb')
        if os.path.isdir(od):
            os.makedirs(os.path.join(lib, 'obb'))
            for o in os.listdir(od):
                os.symlink(os.path.join(od, o), os.path.join(lib, 'obb', o))
            obb = pkg
    shot = '/data/local/tmp/sweep.png'
    opts = ['--shot-shown=%d:%s' % (a.shot_after, shot),
            '--threads-at=%d' % max(30, a.limit - 20)]
    # (SWEEP_VM_OPTS: more SystemServer options, e.g. --mem-report=5)
    opts += os.environ.get('SWEEP_VM_OPTS', '').split()
    cmd = vm_command(stack, pkg, opts)
    d = STACKS[stack][0]
    t0 = time.time()
    events = {}
    lines = []
    # The VM writes to a file of the internal disk, read here as it grows:
    # through a pipe a slow reader (the log on a USB disk) fills it, and
    # Node's stdout then fails with EAGAIN and the VM exits.
    vm_log = os.path.join(rundir, 'vm.log')
    with open(vm_log, 'wb') as out, open(vm_log, 'rb') as tail:
        p = subprocess.Popen(cmd, cwd=os.path.join(a.build, d),
                             env=vm_env(stack, rundir, jit, obb), stdout=out,
                             stderr=subprocess.STDOUT, start_new_session=True)
        LIVE.add(p)
        deadline = t0 + a.limit
        stopped = ''
        partial = b''
        while True:
            chunk = tail.read()
            done = p.poll() is not None
            if chunk:
                partial += chunk
                *full, partial = partial.split(b'\n')
                for raw in full:
                    line = raw.decode('utf-8', 'replace')
                    if len(lines) < 200000:
                        lines.append(line)
                    now = time.time() - t0
                    if line.startswith('SENSES: app '):
                        ev = line.split(' ')[2]
                        events.setdefault(ev, round(now, 1))
                        if ev == 'shown':
                            # what the app does after its first frame (and the shot)
                            deadline = min(deadline, time.time() + a.shot_after + a.after_shown)
                        elif END_EVENTS.match(line):
                            deadline = min(deadline, time.time() + 4)
                            stopped = stopped or 'ended'
                    elif line.startswith('SystemServer: screenshot '):
                        events.setdefault('shot', round(now, 1))
                        deadline = min(deadline, time.time() + 2)
                        stopped = stopped or 'ended'
            elif done:
                break
            elif time.time() > deadline:
                stopped = stopped or 'limit'
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except OSError:
                    pass
                p.wait()
            else:
                time.sleep(0.3)
        if partial:
            lines.append(partial.decode('utf-8', 'replace'))
        status = p.returncode
        LIVE.discard(p)
    with open(log_path, 'w') as fh:
        fh.write('# %s\n# cwd build/%s, %s\n' % (' '.join(cmd), d,
                 'JIT on' if jit else 'ARC_JIT=0'))
    with open(log_path, 'ab') as fh, open(vm_log, 'rb') as src:
        shutil.copyfileobj(src, fh, 1 << 20)
    killed = [stopped]
    secs = round(time.time() - t0, 1)
    png = os.path.join(rundir, 'senses', 'data', stack, 'local', 'tmp', 'sweep.png')
    if os.path.isfile(png):
        shutil.copy(png, os.path.join(job['dir'], png_name))
    else:
        png_name = ''
    if not a.keep:
        shutil.rmtree(rundir, ignore_errors=True)
    r = classify(lines, events, pkg, status)
    r['blank'] = bool(png_name) and png_blank(os.path.join(job['dir'], png_name))
    r.update({'stack': stack, 'jit': 1 if jit else 0, 'secs': secs, 'exit': status,
              'stopped': killed[0], 'events': events, 'log': os.path.relpath(log_path, a.out),
              'png': os.path.relpath(os.path.join(job['dir'], png_name), a.out) if png_name else ''})
    return r


# --- sorting a run out ----------------------------------------------------------

def top_app_frame(trace, pkg):
    """The first frame of a Java stack trace in the app's code (else the first)."""
    frames = [l.strip()[3:] for l in trace if l.strip().startswith('at ')]
    prefix = '.'.join(pkg.split('.')[:2])
    skip = ('java.', 'javax.', 'android.', 'androidx.', 'com.android.', 'dalvik.', 'libcore.',
            'sun.', 'kotlin.', 'kotlinx.', 'arcwasm.', 'com.google.', 'okhttp3.', 'io.reactivex.')
    for f in frames:
        if f.startswith(prefix):
            return f
    for f in frames:
        if not f.startswith(skip):
            return f
    return frames[0] if frames else ''


EXC_RE = re.compile(r'((?:[a-zA-Z_$][\w$]*\.)+[A-Z][\w$]*(?:Exception|Error|Throwable))(?::\s?(.*))?$')

# (cause, pattern on any line) -- first match wins, most specific first.
CAUSES = [
    ('gms', re.compile(r'GooglePlayServices(?:NotAvailable|Repairable)|Google Play services|'
                       r'com\.google\.android\.gms(?!\.internal\.measurement)[\w.]*.*(?:not (?:found|installed|available)|'
                       r'NameNotFoundException|SecurityException)|Package com\.android\.vending', re.I)),
    ('vulkan', re.compile(r'libvulkan\.so|vkCreateInstance|VK_ERROR_|Vulkan (?:is )?not supported', re.I)),
    ('multiprocess', re.compile(r'android:process|another process|isolatedProcess|'
                                r'bindIsolatedService|app_zygote|Process\.myPid\(\) mismatch', re.I)),
    ('oom', re.compile(r'OutOfMemoryError|Cannot enlarge memory|out of memory|'
                       r'Aborted\(OOM\)|failed to grow memory|RangeError: (?:WebAssembly\.Memory|Array buffer allocation)', re.I)),
    ('unresolved', re.compile(r'guest called the unresolved symbol|cannot locate symbol|'
                              r'dlopen failed|UnsatisfiedLinkError')),
    ('native', re.compile(r'RuntimeError: |Aborted\(|unreachable|memory access out of bounds|'
                          r'ndk: guest (?:fault|crash|signal)|SIGSEGV|SIGILL|SIGBUS|'
                          r'Fatal signal|undefined instruction|wasm trap', re.I)),
    ('api', re.compile(r'Unimplemented|NoSuchMethodError|NoSuchFieldError|NoClassDefFoundError|'
                       r'ClassNotFoundException: (?:android|java|javax|dalvik)\.|'
                       r'AbstractMethodError|IncompatibleClassChangeError|VerifyError|'
                       r'ServiceManager.*not found|getService\(.*\) (?:failed|null)|'
                       r'ActivityNotFoundException|UnsupportedOperationException')),
]


BENIGN = re.compile(r'dlopen failed: library "lib(?:androidio|javacore|openjdk|nativehelper)\.so"|'
                    r'is not implemented: default answer|'
                    r'^java\.lang\.UnsupportedOperationException: I[A-Z]\w+\.\w+$|'
                    r'^\s+at arcwasm\.server\.(?:Unimplemented|base\.)|'
                    r'class_linker\.cc:\d+\] Rejecting re-init on previously-failed class')
# The VM dying under a shown app (a native abort, a failed assertion of the
# kernel, a wasm trap): a page's log has no exit status, a Node run's says
# so too.
DIED = re.compile(r'^F/[A-Za-z_]+: |Aborted\(|Pthread 0x[0-9a-f]+ sent an error|^RuntimeError: ')
GL_FAIL = re.compile(r'eglInitialize failed|EGL_NOT_INITIALIZED|EGL_BAD_DISPLAY|'
                     r'eglGetDisplay.*(?:fail|NO_DISPLAY)|no WebGL|'
                     r'Could not create an Android GL environment', re.I)


def classify(lines, events, pkg, exit_status=None):
    """-> {state, cause, detail, exception, frame}"""
    crash = None
    for l in lines:
        if l.startswith('SENSES: app crash '):
            crash = l[len('SENSES: app crash ') + len(pkg) + 1:]
            break
    # The first fatal Java exception: "FATAL EXCEPTION" blocks (the app's
    # uncaught handler) or "E/AndroidRuntime" lines, with their stack.
    exc, trace = '', []
    for i, l in enumerate(lines):
        if 'FATAL EXCEPTION' in l or ('AndroidRuntime' in l and 'Process:' in l):
            for k in range(i + 1, min(i + 60, len(lines))):
                s = lines[k]
                body = re.sub(r'^.*?AndroidRuntime[^:]*: ', '', s)
                if not exc:
                    m = EXC_RE.search(body)
                    if m and 'Process:' not in body:
                        exc = body.strip()
                        continue
                if exc:
                    if body.strip().startswith(('at ', 'Caused by', '...')):
                        trace.append(body)
                        if body.strip().startswith('Caused by'):
                            exc = exc + ' | ' + body.strip()[len('Caused by: '):]
                    elif trace:
                        break
            break
    frame = top_app_frame(trace, pkg)
    shown = 'shown' in events
    started = 'started' in events or 'restored' in events
    ev_end = None
    for e in ('crash', 'exit', 'missing'):
        if e in events:
            ev_end = e
    if ev_end == 'missing':
        state = 'install'
    elif shown and ev_end != 'crash':
        state = 'shown'
        if exit_status not in (None, 0, -9) or any(DIED.search(l) for l in lines):
            state = 'shown-crash'
    elif shown and ev_end == 'crash':
        state = 'shown-crash'
    elif ev_end == 'crash':
        state = 'crash'
    elif ev_end == 'exit':
        state = 'exit'
    elif started:
        state = 'started'
    else:
        state = 'no-start'
    # The cause: what the crash and its exception say first; for a VM that
    # died without one (a native fault), its last lines; a start that
    # neither crashed nor showed is a hang (the thread dump says where).
    # Log lines alone (FirebaseMessaging: "Google Play services missing")
    # are no cause: apps say that and go on.
    crash_at = next((i for i, l in enumerate(lines) if l.startswith('SENSES: app crash ')), None)
    # What the exception says first (its message and causes, its frames);
    # then the lines before the crash, or the VM's last lines when it died
    # without one -- less a few that apps survive (an optional dlopen).
    first_hay = [crash or '', exc] + trace
    context = []
    if crash_at is not None:
        context = lines[max(0, crash_at - 150):crash_at]
    elif not shown and not started or exit_status not in (None, 0, -9) or state == 'shown-crash':
        context = lines[-150:]
    context = [l for l in context if not BENIGN.search(l)]
    hay = first_hay + context
    cause = ''
    gl_line = next((l for l in lines if GL_FAIL.search(l)), '')
    if state != 'shown' and gl_line and not (crash or exc):
        # (no WebGL in Node: what GL takes down needs a run in a browser)
        cause = 'gl'
    elif state != 'shown' and (crash or exc or exit_status not in (None, 0, -9) or
                               state == 'shown-crash'):
        for part in (first_hay, context):
            for name, rx in CAUSES:
                if any(rx.search(h) for h in part if h):
                    cause = name
                    break
            if cause:
                break
        if not cause:
            cause = 'gl' if gl_line else 'java' if (exc or crash) else 'exit'
    elif state in ('started', 'no-start'):
        cause = 'gl' if gl_line else 'hang'
    detail = gl_line.strip()[:300] if cause == 'gl' else ''
    if cause in dict(CAUSES):
        rx = dict(CAUSES)[cause]
        for h in hay:
            if h and rx.search(h):
                detail = h.strip()[:300]
                break
    if not detail:
        detail = (crash or exc or '')[:300]
    if state == 'install':
        for l in lines:
            if 'PackageManager' in l and ('fail' in l.lower() or 'error' in l.lower()):
                detail = l.strip()[:300]
                break
    if cause == 'hang':
        # where the app's main thread is (SENSES-THREADS, --threads-at), else
        # the last thing it said
        main = thread_stack(lines, 'main')
        if main:
            detail = 'main: ' + ' < '.join(main[:4])
            return {'state': state, 'cause': cause, 'detail': detail[:300], 'exception': exc[:400],
                    'frame': top_app_frame(['at ' + f for f in main], pkg)[:200]}
        for l in reversed(lines):
            if l.strip() and not l.startswith(('W/posix_translation: OPFS', '#')):
                detail = l.strip()[:300]
                break
    gms_log = any('Google Play services' in l or 'GooglePlayServices' in l for l in lines)
    # no WebGL in Node: GL apps get no EGL display (they need a browser run)
    gl_fail = any(GL_FAIL.search(l) for l in lines)
    return {'state': state, 'cause': cause, 'detail': detail, 'exception': exc[:400],
            'frame': frame[:200], 'gmsLog': gms_log, 'glFail': gl_fail}


def thread_stack(lines, name):
    """The frames of thread |name| in the last SENSES-THREADS dump (of two
    "main" threads -- system_server's and the app's -- the app's: the one
    in its Looper or ActivityThread)."""
    try:
        start = max(i for i, l in enumerate(lines) if l == 'SENSES-THREADS')
    except ValueError:
        return []
    threads, cur = [], None
    for l in lines[start + 1:]:
        if l == 'SENSES-THREADS end':
            break
        if l.startswith('  "'):
            cur = [] if l.startswith('  "%s" ' % name) else None
            if cur is not None:
                threads.append(cur)
        elif cur is not None and l.strip().startswith('at '):
            cur.append(l.strip()[3:])
    for t in threads:
        if not any('arcwasm.server.SystemServer.main' in f for f in t):
            return t
    return threads[-1] if threads else []


STATE_RANK = {'shown': 6, 'shown-crash': 5, 'exit': 4, 'started': 3, 'crash': 2,
              'no-start': 1, 'install': 0, 'skip': -1}


def summarize(res):
    """res['best'] (the furthest run) and res['first'] (the shell's stack, JIT on)."""
    runs = res['runs']
    best = max(runs, key=lambda r: STATE_RANK.get(r.get('state'), -1)) if runs else {}
    res['best'] = {k: best.get(k) for k in ('state', 'cause', 'stack', 'jit', 'detail',
                                             'exception', 'frame', 'png', 'blank', 'glFail')}
    first = runs[0] if runs else {}
    res['first'] = {k: first.get(k) for k in ('state', 'cause', 'stack', 'jit', 'detail')}


def plan(meta, a):
    """The (stack, jit) runs to try in order: the shell's stack first."""
    if 'error' in meta:
        return []
    probs = meta.get('stackProblems', {})
    ok = [s for s in STACK_ORDER if s not in probs and s in a.stacks
          and meta.get('minSdk', 1) <= STACKS[s][2]]
    first = meta.get('stack') if meta.get('stack') in ok else (ok[0] if ok else None)
    if not first:
        return []
    runs = [(first, True)]
    if 'jit' in a.retry:
        runs.append((first, False))
    if 'stack' in a.retry:
        for s in ok:
            if s != first and s in ('a81', 'a10'):
                runs.append((s, True))
                break
    return runs


# --- the browser pass: what needs GL, in headless Chrome ---------------------

CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'
LOG_LINE = re.compile(r'^\d\d:\d\d:\d\d \[\w+\] ')


def browser_wanted(res):
    """Runs to repeat in a browser: GL failed in Node (no WebGL), the first
    frame was blank (a GL surface), or the app did not show cleanly (the
    browser is where it runs: Node draws in software, without WebGL)."""
    b = res.get('best', {})
    if res.get('kind') not in ('apk', None):
        return False
    return b.get('cause') == 'gl' or b.get('state') != 'shown' or \
        (b.get('blank') or b.get('glFail'))


def run_browser(job, a, server_log):
    """One app on the VM test page (build/senses-apps/vm/index.html) in
    headless Chrome, its own profile (an empty library and /data)."""
    stack, meta = job['stack'], job['meta']
    pkg = meta['package']
    tag = '%s-chrome' % stack
    os.makedirs(job['dir'], exist_ok=True)
    log_path = os.path.join(job['dir'], tag + '.log')
    serve_dir = os.path.join(a.build, 'senses-apps', '__sweep')
    os.makedirs(serve_dir, exist_ok=True)
    link = os.path.join(serve_dir, job['name'] + '.apk')
    if os.path.lexists(link):
        os.remove(link)
    os.symlink(job['path'], link)
    # The profile (and so the page's OPFS) in the system's temporary directory,
    # not under --runs: on an external disk every OPFS request took 1-26 s
    # and Saber's first frame 140-280 s, on the Mac's own disk 3 s (docs/topics/flutter.md).
    profile = tempfile.mkdtemp(prefix='app-sweep-chrome-')
    open(server_log, 'w').close()
    q = 'stack=%s&add=/__sweep/%s.apk&run=%s' % (stack, urllib.parse.quote(job['name']), pkg)
    if meta.get('orientation') == 'landscape':
        q += '&landscape=1'
    # (SWEEP_PAGE_QUERY: more of the page's query, e.g. &env=NDK_GL_ERRORS=20)
    q += os.environ.get('SWEEP_PAGE_QUERY', '')
    url = 'http://localhost:%d/vm/index.html?%s' % (a.port, q)
    cmd = [CHROME, '--headless=new', '--user-data-dir=' + profile,
           '--remote-debugging-port=%d' % a.cdp_port, '--window-size=540,960',
           '--no-first-run', '--no-default-browser-check', '--enable-unsafe-swiftshader',
           '--autoplay-policy=no-user-gesture-required', url]
    t0 = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    LIVE.add(p)
    events, lines = {}, []
    deadline = t0 + a.limit + 60  # (the page reads the APK into its storage first)
    shot_at = None
    png = os.path.join(job['dir'], tag + '.png')
    seen = 0
    try:
        while time.time() < deadline and p.poll() is None:
            time.sleep(1)
            try:
                with open(server_log, errors='replace') as fh:
                    new = fh.read().split('\n')
            except OSError:
                continue
            for raw in new[seen:-1]:
                line = LOG_LINE.sub('', raw)
                lines.append(line)
                m = re.search(r'SENSES: app (\w+)', line)
                if m:
                    ev = m.group(1)
                    events.setdefault(ev, round(time.time() - t0, 1))
                    if ev == 'shown' and shot_at is None:
                        shot_at = time.time() + a.shot_after
                    elif ev in ('crash', 'exit', 'missing'):
                        deadline = min(deadline, time.time() + 4)
            seen = max(seen, len(new) - 1)
            if shot_at and time.time() >= shot_at:
                subprocess.run(['node', os.path.join(ROOT, 'tools', 'headless_cdp.mjs'),
                                str(a.cdp_port), 'shot', png], capture_output=True, timeout=60)
                events.setdefault('shot', round(time.time() - t0, 1))
                deadline = min(deadline, time.time() + a.after_shown)
                shot_at = 0
        if 'shown' not in events:
            # where it is: a shot of the page anyway
            subprocess.run(['node', os.path.join(ROOT, 'tools', 'headless_cdp.mjs'),
                            str(a.cdp_port), 'shot', png], capture_output=True, timeout=60)
    finally:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except OSError:
            pass
        p.wait()
        LIVE.discard(p)
        shutil.rmtree(profile, ignore_errors=True)
        os.remove(link)
    with open(log_path, 'w') as fh:
        fh.write('# %s\n' % url)
        fh.write('\n'.join(lines) + '\n')
    r = classify(lines, events, pkg, None)
    r['blank'] = os.path.isfile(png) and png_blank(png)
    r.update({'stack': stack, 'jit': 1, 'where': 'chrome', 'secs': round(time.time() - t0, 1),
              'events': events, 'log': os.path.relpath(log_path, a.out),
              'png': os.path.relpath(png, a.out) if os.path.isfile(png) else ''})
    return r


def browser_pass(results, apps, a, results_path, save):
    server_log = os.path.join(a.build, 'app-sweep', 'browser-log.txt')
    os.makedirs(os.path.dirname(server_log), exist_ok=True)
    env = dict(os.environ, ARC_NET_ECHO='0')
    srv = subprocess.Popen([sys.executable, '-u', os.path.join(ROOT, 'tools', 'serve.py'),
                            str(a.port), os.path.join(a.build, 'senses-apps'), server_log],
                           env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           start_new_session=True)
    LIVE.add(srv)
    time.sleep(2)
    try:
        for app in apps:
            res = results.get(app['name'])
            if not res or (not a.only and not browser_wanted(res)) or \
                    (not a.rerun and any(r.get('where') == 'chrome' for r in res['runs'])):
                continue
            stack = res['best'].get('stack') or app['meta'].get('stack')
            while memory_free_percent() < a.min_free:
                time.sleep(10)
            job = {'name': app['name'], 'path': app['path'], 'meta': app['meta'], 'stack': stack,
                   'dir': os.path.join(a.out, 'runs', app['name'])}
            try:
                r = run_browser(job, a, server_log)
            except Exception as e:
                r = {'stack': stack, 'jit': 1, 'where': 'chrome', 'state': 'harness',
                     'cause': 'harness', 'detail': repr(e)[:300]}
            res['runs'] = [x for x in res['runs'] if x.get('where') != 'chrome'] + [r]
            summarize(res)
            save()
            print('app_sweep: %-40s %s chrome %-11s %-12s %5.0fs %s' % (
                app['name'][:40], stack, r['state'], r.get('cause', ''), r.get('secs', 0),
                r.get('detail', '')[:100]), flush=True)
    finally:
        os.killpg(srv.pid, signal.SIGKILL)
        LIVE.discard(srv)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('inputs', nargs='+')
    ap.add_argument('--out', default=DEFAULT_OUT)
    ap.add_argument('--build', default=os.path.join(ROOT, 'build'))
    ap.add_argument('--runs', default=None, help='throwaway libraries and /data (internal disk)')
    ap.add_argument('-j', type=int, default=2)
    ap.add_argument('--limit', type=int, default=180, help='seconds per run')
    ap.add_argument('--shot-after', type=int, default=12, help='screenshot S s after the first frame')
    ap.add_argument('--after-shown', type=int, default=8, help='then keep running S s')
    ap.add_argument('--stacks', default='a81,a10,a60,a44')
    ap.add_argument('--retry', default='jit,stack')
    ap.add_argument('--only', default='')
    ap.add_argument('--rerun', action='store_true', help='run again what has a result')
    ap.add_argument('--keep', action='store_true', help='keep each run\'s library and /data')
    ap.add_argument('--min-free', type=int, default=30, help='memory_pressure free %% to start a VM')
    ap.add_argument('--results', default=None, help='results.json (default OUT/results.json)')
    ap.add_argument('--browser', action='store_true',
                    help='run again in headless Chrome what needs GL (cause gl, a blank frame)')
    ap.add_argument('--port', type=int, default=8192, help='serve.py for --browser')
    ap.add_argument('--cdp-port', type=int, default=9333)
    ap.add_argument('--reclassify', action='store_true',
                    help='sort the runs out again from their logs (no VM)')
    ap.add_argument('--smoke', default=None, metavar='LIST',
                    help='a fixed subset: lines "STACK EXPECTED FILE" (FILE relative to the '
                         'first input directory); exit 1 if a run does not reach EXPECTED')
    a = ap.parse_args()
    signal.signal(signal.SIGTERM, kill_live)
    signal.signal(signal.SIGINT, kill_live)
    a.stacks = a.stacks.split(',')
    a.retry = a.retry.split(',') if a.retry != 'none' else []
    a.runs = a.runs or os.path.join(a.build, 'app-sweep', 'runs')
    a.out = os.path.abspath(a.out)
    os.makedirs(a.out, exist_ok=True)
    cache = os.path.join(a.out, 'meta')
    results_path = a.results or os.path.join(a.out, 'results.json')
    try:
        with open(results_path) as fh:
            results = json.load(fh)
    except (OSError, ValueError):
        results = {}
    only = set(x for x in a.only.split(',') if x)

    forced = {}
    expect = {}
    if a.smoke:
        files = []
        with open(a.smoke) as fh:
            for line in fh:
                line = line.split('#')[0].strip()
                if not line:
                    continue
                stack, want, rel = line.split(None, 2)
                f = os.path.join(a.inputs[0], rel)
                if not os.path.isfile(f):
                    print('app_sweep: smoke: %s not here, skipped' % f, flush=True)
                    continue
                files.append((f, stack))
                forced[name_of(f) + '@' + stack] = stack
                expect[name_of(f) + '@' + stack] = want
        a.rerun = True
        a.retry = []
    else:
        files = collect(a.inputs)
    apps = []
    for f in files:
        f, n = (f[0], name_of(f[0]) + '@' + f[1]) if isinstance(f, tuple) else (f, name_of(f))
        m = read_meta(f, cache)
        if only and n not in only and m.get('package') not in only:
            continue
        h = static_hints(f, m, cache) if 'error' not in m else {}
        if n in forced and 'error' not in m:
            m = dict(m, stack=forced[n])
        apps.append({'name': n, 'path': f, 'meta': m, 'hints': h})
    print('app_sweep: %d apps' % len(apps), flush=True)
    if a.reclassify:
        hints = {app['name']: app['hints'] for app in apps}
        for res in results.values():
            if res['name'] in hints:
                res['hints'] = {k: v for k, v in hints[res['name']].items() if not k.startswith('_')}
            for r in res['runs']:
                if not r.get('log'):
                    continue
                try:
                    with open(os.path.join(a.out, r['log']), errors='replace') as fh:
                        lines = fh.read().split('\n')[2:]
                except OSError:
                    continue
                r.update(classify(lines, r.get('events', {}), res['package'], r.get('exit')))
                if r.get('png'):
                    r['blank'] = png_blank(os.path.join(a.out, r['png']))
            summarize(res)
        with open(results_path, 'w') as fh:
            json.dump(results, fh, indent=1, sort_keys=True)
        write_reports(results, a, results_path)
        return

    templates = {}
    lock = threading.Lock()

    if a.browser:
        def save_b():
            with open(results_path + '.tmp', 'w') as fh:
                json.dump(results, fh, indent=1, sort_keys=True)
            os.replace(results_path + '.tmp', results_path)
        browser_pass(results, apps, a, results_path, save_b)
        write_reports(results, a, results_path)
        return
    queue = []
    for app in apps:
        r = results.get(app['name'])
        if r and not a.rerun and r.get('runs'):
            continue
        queue.append(app)

    def save():
        with lock:
            tmp = results_path + '.tmp'
            with open(tmp, 'w') as fh:
                json.dump(results, fh, indent=1, sort_keys=True)
            os.replace(tmp, results_path)

    def do_app(app):
        m = app['meta']
        res = {'name': app['name'], 'file': app['path'], 'package': m.get('package', ''),
               'version': m.get('versionName', ''), 'minSdk': m.get('minSdk'),
               'targetSdk': m.get('targetSdk'), 'abis': m.get('abis', []),
               'kind': m.get('kind', ''), 'shellStack': m.get('stack', ''),
               'hints': {k: v for k, v in app['hints'].items() if not k.startswith('_')},
               'runs': []}
        if 'error' in m:
            res['runs'].append({'state': 'skip', 'cause': 'apk', 'detail': m.get('message', '')})
        runs = plan(m, a)
        if not runs and 'error' not in m:
            res['runs'].append({'state': 'skip', 'cause': 'stack',
                                'detail': json.dumps(m.get('stackProblems', {}), ensure_ascii=False)})
        parts = extract_parts(app['path'], m, cache) if m.get('kind') not in (None, 'apk') \
            and 'error' not in m else None
        for stack, jit in runs:
            # retries only for what did not show
            if res['runs'] and res['runs'][-1].get('state') in ('shown',):
                break
            if res['runs'] and res['runs'][-1].get('cause') in ('gl', 'install'):
                break  # (no WebGL in Node: the browser pass; not in the library)
            if res['runs'] and not jit and res['runs'][-1].get('cause') in ('gms', 'vulkan'):
                continue
            with lock:
                if stack not in templates:
                    templates[stack] = make_template(stack, os.path.join(a.build, 'app-sweep'), a.build)
            while memory_free_percent() < a.min_free:
                time.sleep(10)
            job = {'name': app['name'], 'path': app['path'], 'meta': m, 'stack': stack, 'jit': jit,
                   'template': templates[stack], 'parts': parts,
                   'dir': os.path.join(a.out, 'runs', app['name'])}
            try:
                r = run_one(job, a)
            except Exception as e:
                r = {'stack': stack, 'jit': 1 if jit else 0, 'state': 'harness', 'cause': 'harness',
                     'detail': repr(e)[:300]}
            res['runs'].append(r)
            print('app_sweep: %-40s %s jit%d %-11s %-12s %5.0fs %s' % (
                app['name'][:40], stack, r['jit'], r['state'], r.get('cause', ''),
                r.get('secs', 0), r.get('detail', '')[:100]), flush=True)
        summarize(res)
        with lock:
            results[app['name']] = res
        save()

    threads = []
    sem = threading.Semaphore(a.j)
    q = list(queue)
    qlock = threading.Lock()

    def worker():
        while True:
            with qlock:
                if not q:
                    return
                app = q.pop(0)
            do_app(app)
    for _ in range(max(1, a.j)):
        t = threading.Thread(target=worker)
        t.start()
        threads.append(t)
        time.sleep(20)  # (two VMs starting at once fight for the disk)
    for t in threads:
        t.join()
    write_reports(results, a, results_path)
    if a.smoke:
        bad = 0
        for n, want in sorted(expect.items()):
            got = results.get(n, {}).get('best', {}).get('state')
            ok = STATE_RANK.get(got, -1) >= STATE_RANK.get(want, 99)
            bad += not ok
            print('app_sweep: smoke %-40s %-6s want %-8s got %s' % (
                n[:40], 'ok' if ok else 'FAIL', want, got))
        sys.exit(1 if bad else 0)


def write_reports(results, a, results_path):
    rows = sorted(results.values(), key=lambda r: (-STATE_RANK.get(r['best'].get('state'), -1),
                                                   r['name'].lower()))
    csv_path = os.path.splitext(results_path)[0] + '.csv'
    with open(csv_path, 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow(['name', 'package', 'version', 'minSdk', 'abis', 'engines', 'shell stack',
                    'best', 'best stack', 'best jit', 'first (shell stack, JIT on)', 'cause',
                    'detail', 'frame', 'gms', 'process', 'png'])
        for r in rows:
            b, f, h = r['best'], r['first'], r.get('hints', {})
            w.writerow([r['name'], r['package'], r['version'], r['minSdk'], ' '.join(r['abis']),
                        ' '.join(h.get('engines', [])), r['shellStack'], b.get('state'),
                        b.get('stack'), b.get('jit'), f.get('state'),
                        b.get('cause') or f.get('cause'), (b.get('detail') or '')[:200],
                        b.get('frame') or '', int(bool(h.get('gms'))), h.get('process', 0),
                        b.get('png') or ''])
    md = os.path.join(os.path.dirname(results_path), 'table.md')
    with open(md, 'w') as fh:
        fh.write('| Приложение | minSdk | ABI | Движок | Стек | Итог (JIT вкл.) | Лучший | Причина | Подробности |\n')
        fh.write('|---|---|---|---|---|---|---|---|---|\n')
        for r in rows:
            b, f, h = r['best'], r['first'], r.get('hints', {})
            abis = ','.join(x.replace('armeabi-v7a', 'v7a').replace('arm64-v8a', 'arm64')
                            for x in r['abis'] if x in ('armeabi-v7a', 'armeabi', 'arm64-v8a'))
            best = '%s %s%s' % (b.get('state'), b.get('stack') or '',
                                '' if b.get('jit') in (1, None) else ' без JIT')
            det = (b.get('frame') or b.get('detail') or '').replace('|', '\\|')[:120]
            fh.write('| %s %s | %s | %s | %s | %s | %s | %s | %s | %s |\n' % (
                r['package'] or r['name'], r['version'], r['minSdk'], abis or '—',
                ' '.join(h.get('engines', [])) or '', r['shellStack'], f.get('state'), best,
                b.get('cause') or f.get('cause') or '', det))
    # counts per stack (first run: the shell's stack with the JIT)
    counts = {}
    for r in rows:
        f = r['first']
        key = f.get('stack') or '-'
        c = counts.setdefault(key, {})
        c[f.get('state')] = c.get(f.get('state'), 0) + 1
    summary = {'apps': len(rows), 'first': counts,
               'best': {s: sum(1 for r in rows if r['best'].get('state') == s) for s in STATE_RANK}}
    with open(os.path.join(os.path.dirname(results_path), 'summary.json'), 'w') as fh:
        json.dump(summary, fh, indent=1)
    print(json.dumps(summary))
    bdir = os.path.join(a.build, 'app-sweep')
    if not a.smoke and os.path.realpath(os.path.dirname(results_path)) != os.path.realpath(bdir):
        os.makedirs(bdir, exist_ok=True)
        for f in (results_path, csv_path, md):
            shutil.copy(f, bdir)


if __name__ == '__main__':
    main()
