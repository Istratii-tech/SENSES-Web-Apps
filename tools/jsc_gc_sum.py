#!/usr/bin/env python3
"""jsc --logGC=1 output on stdin (shim/gles/remote_webgl_bench.js): what the VM allocated
between the lines "BENCH START" and "BENCH END ...", from the collections' sizes (each
collection's heap before it, less the heap after the one before), per frame."""
import re, sys

inside, after, total, gcs, frames, end = False, None, 0.0, 0, 0, ''
for line in sys.stdin:
    if line.startswith('BENCH START'):
        inside, total, gcs = True, 0.0, 0
        continue
    if line.startswith('BENCH END'):
        end = line.strip()
        m = re.search(r'frames (\d+)', line)
        frames = int(m.group(1)) if m else 0
        inside = 'last'
        continue
    m = re.search(r'START \S+ ([\d.]+)kb => .* => ([\d.]+)kb', line)
    if not m:
        if inside is not True and not line.startswith('[GC') and 'Requesting GC' not in line and line.strip() != 'GC END!':
            print(line.rstrip())
        continue
    before, aft = float(m.group(1)), float(m.group(2))
    if inside:
        if after is not None:
            total += before - after
        gcs += 1
        if inside == 'last':
            inside = False
    after = aft
print(end)
if frames:
    print('allocated %.1f MB in %d collections: %.1f KB a frame' % (total / 1024, gcs, total / frames))
