#!/usr/bin/env python3
"""Prints the indexes of the functions of a .wasm file whose names contain a
string (from the "name" section, which --profiling-funcs keeps).

Usage: wasm_func_index.py FILE.wasm SUBSTRING

With an index, V8 prints the machine code it generates for that function:
  node --no-liftoff --print-wasm-code-function-index=N artvm.js ...
(see docs/topics/art-speed.md, "Как смотреть код V8").
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


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    b = open(sys.argv[1], 'rb').read()
    pattern = sys.argv[2]
    i = 8
    while i < len(b):
        section_id = b[i]
        size, i = leb(b, i + 1)
        end = i + size
        if section_id == 0:
            name_len, j = leb(b, i)
            if b[j:j + name_len] == b'name':
                j += name_len
                while j < end:
                    sub_id = b[j]
                    sub_size, j = leb(b, j + 1)
                    sub_end = j + sub_size
                    if sub_id == 1:  # function names
                        count, k = leb(b, j)
                        for _ in range(count):
                            index, k = leb(b, k)
                            length, k = leb(b, k)
                            name = b[k:k + length].decode('utf-8', 'replace')
                            k += length
                            if pattern in name:
                                print(index, name)
                    j = sub_end
        i = end


if __name__ == '__main__':
    main()
