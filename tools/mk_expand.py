#!/usr/bin/env python3
"""Prints the words of a variable of one or more Android.mk fragments with
$(VAR) references to other variables of those files expanded (handles
':=', '+=', '=', line continuations and comments; ignores conditionals and
functions). tools/mk_srcs.py is the same without expansion.

Usage: mk_expand.py VARIABLE [PREFIX] -- MK_FILE...
"""
import re
import sys


def read_vars(paths):
    vals = {}
    for path in paths:
        text = open(path).read().replace('\\\n', ' ')
        for line in text.splitlines():
            line = line.split('#', 1)[0]
            m = re.match(r'\s*([A-Za-z0-9_.-]+)\s*(:=|\+=|=)(.*)', line)
            if not m:
                continue
            name, op, words = m.group(1), m.group(2), m.group(3).split()
            vals[name] = (vals.get(name, []) if op == '+=' else []) + words
    return vals


def expand(vals, name, seen=()):
    out = []
    for w in vals.get(name, []):
        m = re.fullmatch(r'\$\(([A-Za-z0-9_.-]+)\)', w)
        if m and m.group(1) not in seen:
            out += expand(vals, m.group(1), seen + (name,))
        else:
            out.append(w)
    return out


def main(argv):
    sep = argv.index('--')
    var, prefix = argv[0], (argv[1] if sep > 1 else '')
    vals = read_vars(argv[sep + 1:])
    print('\n'.join(prefix + w for w in expand(vals, var)))


if __name__ == '__main__':
    main(sys.argv[1:])
