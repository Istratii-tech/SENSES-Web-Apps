#!/usr/bin/env python3
"""Summarizes a monkey run's log (topic userspace, arcwasm.server.Monkey):
the methods lean services answered with a default (lenient mode), with the
first app frame that called each, process deaths and hangs.

Usage: monkey_walls.py LOG
"""
import re, sys
lines = open(sys.argv[1], errors='replace').read().splitlines()
seen = {}
for i, l in enumerate(lines):
    m = re.search(r'(\S+\.\S+) is not implemented: default answer', l)
    if m and m.group(1) not in seen:
        caller = ''
        for k in range(i + 1, min(i + 40, len(lines))):
            t = lines[k]
            if 'at ' in t and 'arcwasm.server' not in t and 'android.os.Binder' not in t \
               and 'Proxy' not in t and 'Stub' not in t and 'Parcel' not in t:
                caller = t.strip()
                break
        seen[m.group(1)] = caller
print('== default answers (%d)' % len(seen))
for k, v in sorted(seen.items()):
    print('  %-60s %s' % (k, v[:110]))
print('== deaths / crashes')
for i, l in enumerate(lines):
    if re.search(r'process \S+ died|FATAL|Abort|RuntimeError', l):
        print('  ' + l[:160])
        for k in range(i + 1, min(i + 4, len(lines))):
            print('    ' + lines[k][:160])
print('== hangs')
for l in lines:
    if 'main looper blocked' in l:
        print('  ' + l[:160])
print('== monkey actions: %d' % sum(1 for l in lines if re.search(r'Monkey: \d+ ', l)))
