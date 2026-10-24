#!/usr/bin/env python3
"""Prints the sizes of the function bodies of a .wasm file (bytes of wasm
code), largest first, with their indexes and names (the "name" section,
which --profiling-funcs keeps); only those whose names contain SUBSTRING if
given.

Usage: wasm_func_sizes.py FILE.wasm [SUBSTRING]

Safari's JavaScriptCore leaves very large functions out of its optimizing
tier: ART 8.1's interpreter loop ran 1.5 times slower in Safari at 69 KB than
at 58 KB (docs/topics/art8.md). Indexes as tools/wasm_func_index.py prints.
"""

import sys


def leb(b, i):
    result = shift = 0
    while True:
        x = b[i]
        i += 1
        result |= (x & 0x7f) << shift
        shift += 7
        if x < 0x80:
            return result, i


def limits(b, i):
    flags = b[i]
    i += 1
    _, i = leb(b, i)
    if flags & 1:
        _, i = leb(b, i)
    return i


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    b = open(sys.argv[1], 'rb').read()
    pattern = sys.argv[2] if len(sys.argv) == 3 else ''
    i = 8
    imported_funcs = 0
    code = None
    names = {}
    while i < len(b):
        section = b[i]
        size, i = leb(b, i + 1)
        start = i
        if section == 2:  # imports: function indexes start after them
            n, j = leb(b, i)
            for _ in range(n):
                for _ in range(2):  # module, field
                    length, j = leb(b, j)
                    j += length
                kind = b[j]
                j += 1
                if kind == 0:
                    _, j = leb(b, j)
                    imported_funcs += 1
                elif kind == 1:
                    j = limits(b, j + 1)
                elif kind == 2:
                    j = limits(b, j)
                elif kind == 3:
                    j += 2
                elif kind == 4:
                    _, j = leb(b, j + 1)
        elif section == 10:
            code = i
        elif section == 0:
            length, j = leb(b, i)
            if b[j:j + length] == b'name':
                j += length
                while j < start + size:
                    sub = b[j]
                    sub_size, j = leb(b, j + 1)
                    end = j + sub_size
                    if sub == 1:
                        n, j = leb(b, j)
                        for _ in range(n):
                            idx, j = leb(b, j)
                            length, j = leb(b, j)
                            names[idx] = b[j:j + length].decode('utf-8', 'replace')
                            j += length
                    j = end
        i = start + size
    n, j = leb(b, code)
    sizes = []
    for k in range(n):
        body, j2 = leb(b, j)
        sizes.append((body, imported_funcs + k))
        j = j2 + body
    for body, idx in sorted(sizes, reverse=True):
        name = names.get(idx, '?')
        if pattern in name:
            print(body, idx, name)


if __name__ == '__main__':
    main()
