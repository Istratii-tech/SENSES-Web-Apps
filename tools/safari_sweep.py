#!/usr/bin/env python3
"""Run the apps of an app_sweep results.json in Safari on another Mac, one
tab each: the VM test page (build/senses-apps/vm) installs the APK and runs
it; the run ends at "SENSES: app shown|crash|exit|missing" (shown: AFTER s
more for a crash right after) or a VM failure, at most 150 s; the tab is closed
and the next URL removes the previous app.

  python3 tools/serve.py PORT build/senses-apps LOG
  ssh -N -R PORT:localhost:PORT -p 2222 user@host     # its localhost = here
  python3 -I tools/safari_sweep.py RESULTS.json LOG build/senses-apps/__sweep OUT.json [PORT [STACK]]

SAFARI_SSH="-p 2222 user@host" picks the Mac (default: .63, Safari 26).
SAFARI_ONLY=FILE|NAME,... runs only those apps (results.json names, one per
line in FILE), in that order. SAFARI_AFTER=S keeps a shown app S s (default
8): "shown-crash"/"shown-exit" for a crash or exit, "shown-reload" when the VM
starts again in the tab (WebKit reloaded it), "shown-vm-failure" for a VM
abort, "shown-gone" when the tab's WebContent process goes. Each run also records
the peak RSS of the WebContent processes on that Mac (ps, every 5 s).
docs/HANDOFF.md: Safari 26 run of 2026-10-21; docs/topics/recheck-oct21.md."""
import json, os, re, subprocess, sys, threading, time, urllib.parse

RESULTS, LOG, SWEEP_DIR, OUT = sys.argv[1:5]
PORT = int(sys.argv[5]) if len(sys.argv) > 5 else 8193
STACK = sys.argv[6] if len(sys.argv) > 6 else 'a10'
# SAFARI_SSH: how to reach the Mac with Safari, e.g. "-p 2222 USER@HOST".
SSH = ['ssh', '-o', 'BatchMode=yes'] + os.environ['SAFARI_SSH'].split()
LIMIT = 150
AFTER = int(os.environ.get('SAFARI_AFTER', '8'))

d = json.load(open(RESULTS))
apps = d['apps'] if isinstance(d, dict) and 'apps' in d else d
apps = list(apps.values()) if isinstance(apps, dict) else apps
only = os.environ.get('SAFARI_ONLY')
if only:
    names = [l.split('#')[0].strip() for l in open(only)] if os.path.exists(only) else only.split(',')
    names = [n for n in names if n]
    byname = {a['name']: a for a in apps}
    missing = [n for n in names if n not in byname]
    if missing:
        sys.exit('not in %s: %s' % (RESULTS, ', '.join(missing)))
    apps = [byname[n] for n in names]
done = {}
if os.path.exists(OUT):
    done = json.load(open(OUT))

def close_tabs():
    subprocess.run(SSH + ["osascript -e 'tell application \"Safari\" to close (every tab of every window "
                          "whose URL contains \"localhost:%d\")'" % PORT], capture_output=True)

def webcontent():
    """{pid: RSS MB} of the WebContent processes on that Mac"""
    r = subprocess.run(SSH + ['ps -axo pid=,rss=,comm= | grep WebContent'], capture_output=True, text=True)
    out = {}
    for line in r.stdout.splitlines():
        p = line.split(None, 2)
        if len(p) == 3 and p[0].isdigit() and 'WebContent' in p[2]:
            out[int(p[0])] = int(p[1]) // 1024
    return out

class Sampler(threading.Thread):
    """WebContent RSS every 5 s: the peak, and the process at the peak"""
    def __init__(self, before):
        super().__init__(daemon=True)
        self.before, self.peak, self.pid, self.alive, self.stop = set(before), 0, None, {}, False
    def run(self):
        while not self.stop:
            wc = webcontent()
            self.alive = wc
            for pid, mb in wc.items():
                if pid not in self.before and mb > self.peak:
                    self.peak, self.pid = mb, pid
            time.sleep(5)

def read_from(start):
    with open(LOG, errors='replace') as f:
        f.seek(start)
        return f.read()

prev = None
for app in apps:
    name, pkg = app['name'], app.get('package')
    if not pkg or app.get('kind') != 'apk' or name in done:
        continue
    link = os.path.join(SWEEP_DIR, name + '.apk')
    if os.path.lexists(link):
        os.remove(link)
    os.symlink(app['file'], link)
    q = 'stack=%s&add=/__sweep/%s.apk&run=%s' % (STACK, urllib.parse.quote(name), pkg)
    if prev:
        q = 'remove=%s&' % prev + q
    url = 'http://localhost:%d/vm/index.html?%s' % (PORT, q)
    start = os.path.getsize(LOG) if os.path.exists(LOG) else 0
    sampler = Sampler(webcontent())
    sampler.start()
    t0 = time.time()
    subprocess.run(SSH + ["open -a Safari '%s'" % url], capture_output=True)
    state, detail, shown_at, lived = 'timeout', '', None, None
    while time.time() - t0 < LIMIT:
        time.sleep(2)
        text = read_from(start)
        m = re.search(r'SENSES: app (shown|crash|exit|missing) ' + re.escape(pkg) + r'(.*)', text)
        if m:
            state, detail = m.group(1), m.group(2).strip()[:160]
            if state == 'shown' and shown_at is None:
                shown_at = time.time() - t0
                # AFTER s more: a crash, an exit or the tab going after the first frame
                vm_starts = len(re.findall(r'SensesVm: \w+ arcwasm', text))
                mark = len(text)
                while time.time() - t0 < shown_at + AFTER:
                    time.sleep(2)
                    text = read_from(start)
                    c = re.search(r'SENSES: app (crash|exit) ' + re.escape(pkg) + r'(.*)', text[mark:])
                    if c:
                        state, detail = 'shown-' + c.group(1), c.group(2).strip()[:160]
                        break
                    if len(re.findall(r'SensesVm: \w+ arcwasm', text)) > vm_starts:
                        state, detail = 'shown-reload', 'the VM started again in the tab'
                        break
                    v = re.search(r'(сбой VM: .*|ABORT.*)', text[mark:])
                    if v:
                        state, detail = 'shown-vm-failure', v.group(1)[:160]
                        break
                    if sampler.pid and sampler.alive and sampler.pid not in sampler.alive:
                        state, detail = 'shown-gone', 'WebContent %d at %d MB went' % (sampler.pid, sampler.peak)
                        break
                lived = round(time.time() - t0 - shown_at)
            break
        v = re.search(r'(сбой VM: .*|ABORT.*|Хранилище: .*)', text)
        if v:
            state, detail = 'vm-failure', v.group(1)[:160]
            break
    sampler.stop = True
    close_tabs()
    done[name] = {'package': pkg, 'state': state, 'detail': detail,
                  'secs': round(shown_at or (time.time() - t0)), 'node_a10': app.get('best', {}).get('state'),
                  'peak_rss_mb': sampler.peak}
    if lived is not None:
        done[name]['lived'] = lived
    json.dump(done, open(OUT, 'w'), indent=1, ensure_ascii=False)
    print('%-45s %-12s %4ds %5d MB %s' % (name, state, done[name]['secs'], sampler.peak, detail), flush=True)
    prev = pkg
    time.sleep(3)
print('DONE', len(done))
