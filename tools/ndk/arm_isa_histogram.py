#!/usr/bin/env python3
"""Histogram of the ARM/Thumb instructions in an ELF's executable sections.

Usage: arm_isa_histogram.py LIB.so [--thumb] [--top N]

Disassembles .text/.plt with the system objdump (Apple's llvm-objdump has
the ARM target; emsdk's does not) in ARM state (or Thumb with --thumb) and
counts mnemonics. Stripped libraries have no $a/$t/$d mapping symbols, so
literal pools decode as junk: rare conditional forms and <unknown> are
mostly data. Used for topic arm-cpu (docs/topics/arm-cpu.md).
"""
import collections, re, subprocess, sys

def main():
    path = sys.argv[1]
    thumb = '--thumb' in sys.argv
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 400
    triple = 'thumbv7' if thumb else 'armv7'
    out = subprocess.run(['objdump', '-d', '--triple=' + triple, '-j', '.text', '-j', '.plt', path],
                         capture_output=True, text=True).stdout
    raw = collections.Counter()
    total = 0
    line_re = re.compile(r'^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2,8}\s)+\s*(\S+)(.*)$')
    for line in out.splitlines():
        m = line_re.match(line)
        if not m:
            continue
        mnem, ops = m.group(1), m.group(2)
        total += 1
        key = mnem
        if mnem.startswith('v'):
            # Keep VFP/NEON data types; they matter for coverage.
            key = mnem
        raw[key] += 1
    print('total', total)
    for k, v in raw.most_common(top):
        print('%8d %s' % (v, k))

main()
