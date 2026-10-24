#!/usr/bin/env python3
"""Prints the files assigned to a variable in an Android.mk (handles
':=', '+=', line continuations and comments; ignores conditionals).

Usage: mk_srcs.py ANDROID_MK VARIABLE [PREFIX]
"""
import re
import sys


def main(path, var, prefix=''):
    text = open(path).read().replace('\\\n', ' ')
    files = []
    for line in text.splitlines():
        line = line.split('#', 1)[0]
        m = re.match(r'\s*' + re.escape(var) + r'\s*(:=|\+=|=)(.*)', line)
        if not m:
            continue
        if m.group(1) != '+=':
            files = []
        files += m.group(2).split()
    print('\n'.join(prefix + f for f in files))


if __name__ == '__main__':
    main(*sys.argv[1:])
