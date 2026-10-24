#!/usr/bin/env python3
"""Describes SENSES Web Apps' VM builds for the shell (topic install8).

    python3 tools/senses_apps_vm_json.py build/senses-apps/vm > vm.json

One entry per stack (a44, a60, a81, a10): the script to load, its files with
sizes and content hashes (for the shell's offline cache), the program
arguments (Module.arguments; PACKAGE is replaced by the app's package) and
the environment (Module.preRun: ENV) -- docs/topics/install8.md, "Contract
with the shell".
"""

import hashlib
import json
import os
import sys

STACKS = {
    'a44': {
        'name': 'Android 4.4', 'vm': 'dalvik', 'sdk': 19, 'script': 'a44/dalvikvm.js',
        'files': ['a44/dalvikvm.js', 'a44/dalvikvm.wasm', 'a44/dalvikvm.data'],
        # Dalvik's heap is fixed at start (16 MB by default): the shell puts
        # -Xmx first, by the wasm memory the device gave (heaps below).
        'arguments': ['arcwasm.server.SystemServer', '--display', 'WIDTHxHEIGHT@DPI',
                      '--senses', 'PACKAGE'],
        'heap': [[1536, 256], [1024, 192], [768, 128], [0, 64]],
    },
    'a60': {
        'name': 'Android 6.0.1', 'vm': 'art', 'sdk': 23, 'script': 'a60/artvm.js',
        'files': ['a60/artvm.js', 'a60/artvm.wasm', 'a60/artvm.data'],
        'arguments': ['arcwasm.server.SystemServer', '--senses', 'PACKAGE'],
    },
    'a81': {
        'name': 'Android 8.1', 'vm': 'art8', 'sdk': 27, 'script': 'a81/artvm8.js',
        'files': ['a81/artvm8.js', 'a81/artvm8.wasm', 'a81/artvm8.data'],
        'arguments': ['arcwasm.server.SystemServer', '--senses', 'PACKAGE'],
        'default': True,
    },
    # Apps for Android 9-10 (minSdk 28-29) and arm64-only ones (topic senses-a10).
    'a10': {
        'name': 'Android 10', 'vm': 'art10', 'sdk': 29, 'script': 'a10/artvm10.js',
        'files': ['a10/artvm10.js', 'a10/artvm10.wasm', 'a10/artvm10.data'],
        'arguments': ['arcwasm.server.SystemServer', '--senses', 'PACKAGE'],
    },
}


def describe(root):
    out = {'contract': 1, 'library': 'senses/apps', 'stacks': {}}
    for key, s in STACKS.items():
        if not os.path.exists(os.path.join(root, s['script'])):
            continue  # (a build without that stack)
        entry = dict(s)
        entry['env'] = {'ARC_DATA_DIR': 'senses/data/' + key, 'ARC_APPS_DIR': 'senses/apps'}
        files = []
        for f in s['files']:
            path = os.path.join(root, f)
            h = hashlib.sha256()
            with open(path, 'rb') as fh:
                for block in iter(lambda: fh.read(1 << 20), b''):
                    h.update(block)
            files.append({'path': f, 'size': os.path.getsize(path), 'sha256': h.hexdigest()[:16]})
        entry['files'] = files
        out['stacks'][key] = entry
    return out


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    json.dump(describe(sys.argv[1]), sys.stdout, indent=2, ensure_ascii=False)
    sys.stdout.write('\n')
