#!/usr/bin/env python3
"""Classify the functions of an ARM ELF .so as ARM or Thumb and histogram
their instructions, using .ARM.exidx for function boundaries.

Usage: arm_functions.py LIB.so

Function starts come from .ARM.exidx (every function with unwind info).
A function is Thumb if its exidx-adjacent symbol is odd, or failing that if
its first halfword looks like a Thumb prologue (push/sub sp) and the first
word is not an ARM one. Instructions are then taken from objdump's ARM or
Thumb disassembly of that range; disassembly stops at the first junk after
an unconditional return/branch (literal pools). Used for topic arm-cpu.
"""
import collections, re, struct, subprocess, sys

def sections(path):
    out = subprocess.run(['objdump', '-h', path], capture_output=True, text=True).stdout
    secs = {}
    for l in out.splitlines():
        p = l.split()
        if len(p) >= 4 and p[0].isdigit() and len(p[1]) > 1:
            secs[p[1]] = (int(p[3], 16), int(p[2], 16))
    return secs

def read_elf_sections(path):
    data = open(path, 'rb').read()
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2e)
    hdrs = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
    strtab = hdrs[shstrndx]
    def name(off):
        s = data[strtab[4] + off:]
        return s[:s.index(b'\0')].decode()
    return data, {name(h[0]): h for h in hdrs}

LINE = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2,8} )+)\s*(\S+)\s*(.*)$')

def disasm(path, triple):
    out = subprocess.run(['objdump', '-d', '--triple=' + triple, '-j', '.text', path],
                         capture_output=True, text=True).stdout
    ins = {}
    for l in out.splitlines():
        m = LINE.match(l)
        if m:
            words = m.group(2).split()
            size = sum(len(w) // 2 for w in words)
            ins[int(m.group(1), 16)] = (size, m.group(3), m.group(4).strip(), words)
    return ins

def main():
    path = sys.argv[1]
    data, shdr = read_elf_sections(path)
    ex = shdr['.ARM.exidx']
    tx = shdr['.text']
    text_addr, text_off, text_size = tx[3], tx[4], tx[5]
    starts = []
    for i in range(0, ex[5], 8):
        w0, = struct.unpack_from('<I', data, ex[4] + i)
        off = w0 & 0x7fffffff
        if off & 0x40000000:
            off -= 0x80000000
        starts.append(ex[3] + i + off)
    starts = sorted(set(s for s in starts if text_addr <= s < text_addr + text_size))
    starts.append(text_addr + text_size)
    arm = disasm(path, 'armv7')
    thm = disasm(path, 'thumbv7')
    def word(a):
        return struct.unpack_from('<I', data, text_off + a - text_addr)[0]
    hist = {'arm': collections.Counter(), 'thumb': collections.Counter()}
    nfun = collections.Counter()
    bytes_ = collections.Counter()
    for f, nxt in zip(starts, starts[1:]):
        w = word(f)
        is_arm = (w >> 28) == 0xe or (w & 0x0e000000) == 0x0a000000
        h = w & 0xffff
        thumbish = (h & 0xfe00) == 0xb400 or (h & 0xff80) == 0xb080 or (h & 0xfff0) == 0xe920 or h == 0xe92d
        kind = 'arm' if is_arm and not ((w >> 28) != 0xe and thumbish) else ('thumb' if thumbish else 'arm')
        nfun[kind] += 1
        bytes_[kind] += nxt - f
        table = arm if kind == 'arm' else thm
        a = f
        while a < nxt:
            e = table.get(a)
            if e is None:
                a += 2
                continue
            size, mn, ops, _ = e
            hist[kind][mn] += 1
            a += size
    print('functions', dict(nfun), 'bytes', dict(bytes_))
    for kind in ('arm', 'thumb'):
        print('==', kind, sum(hist[kind].values()))
        for k, v in hist[kind].most_common():
            print('%8d %s' % (v, k))

main()
