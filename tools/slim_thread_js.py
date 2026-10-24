#!/usr/bin/env python3
"""Less JavaScript in every thread: the VM's JS minified and in one function.

Usage: slim_thread_js.py MODULE.js   (rewrites it in place)

An emscripten pthread is a worker that loads the same MODULE.js as the page
and runs its top level. On an iPhone (Safari 16, JavaScriptCore) that cost
~7 MB a worker before its thread ran anything: 50-90 threads an app (Subway
Surfers, Opera GX) -- most of the 1.5 GB at which iOS reloads the tab
(docs/topics/ios-threads.md). Measured per worker (memprobe.html ?what=pjs):

  as linked (675 KB; 56 non-Latin-1 chars: JSC keeps it all as UTF-16)  7.1 MB
  minified, ASCII only (408 KB)                                         4.8 MB
  minified, ASCII only, all of it in one function                       2.6 MB

The last step is the largest: JSC compiles a script's top level as one code
block (every statement of the 800 at the top, the libraries' object literals
included) and keeps it with the script; a function's code block that ran
once is freed by the next GC. So the script becomes

  (function () { <the script> })();

Top-level declarations are then the function's, not the global object's:
- `var Module = typeof Module != "undefined" ? Module : {}` reads the page's
  Module from globalThis (inside the function the name is the local one);
- what pages use from outside (ENV in preRun, PThread, wasmExports,
  wasmMemory: demo/apps/run.html, demo/senses-apps-vm/run.html) is defined on
  globalThis as getters of the function's variables (EXPORTS).
Minified with emscripten's terser: no comments, \\u escapes for non-ASCII
(whitespace only: no renaming, no compression). Stack traces of JS lose
their lines; wasm frames keep their names (--profiling-funcs).
"""

import os
import subprocess
import sys

EXPORTS = ['ENV', 'PThread', 'wasmExports', 'wasmMemory', 'wasmTable', 'FS', 'GL']
MODULE_DECL = 'var Module=typeof Module!="undefined"?Module:{};'
MODULE_GLOBAL = 'var Module=typeof globalThis.Module!="undefined"?globalThis.Module:{};'
MARK = '/* slim_thread_js */'


def main():
    path = sys.argv[1]
    with open(path, encoding='utf-8') as f:
        src = f.read()
    if src.startswith(MARK):
        print('%s: already slim' % path)
        return
    emsdk = os.environ.get('EMSDK') or os.path.expanduser('~/emsdk')
    terser = os.path.join(emsdk, 'upstream/emscripten/node_modules/terser/bin/terser')
    node = os.environ.get('NODE') or 'node'
    out = subprocess.run([node, terser, '--format', 'ascii_only=true,comments=false'],
                         input=src.encode('utf-8'), stdout=subprocess.PIPE, check=True).stdout.decode('ascii')
    n = out.count(MODULE_DECL)
    if not n:
        raise SystemExit('%s: no "%s" after minifying' % (path, MODULE_DECL))
    out = out.replace(MODULE_DECL, MODULE_GLOBAL)
    getters = ''.join('Object.defineProperty(globalThis,"%s",{get:()=>%s,configurable:true});' % (e, e)
                      for e in EXPORTS)
    # (getters first: preRun, called from inside, may read ENV through them;
    # the names are hoisted, a getter of one never assigned returns undefined)
    out = MARK + '\n(function(){\n' + getters + '\n' + out + '\n})();\n'
    with open(path, 'w', encoding='ascii') as f:
        f.write(out)
    print('%s: %d -> %d bytes, ASCII, in one function (%d Module declarations)' % (
        path, len(src.encode('utf-8')), len(out), n))


if __name__ == '__main__':
    main()
