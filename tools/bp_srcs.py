#!/usr/bin/env python3
"""Prints a list property of a module in an Android.bp (Soong blueprint),
e.g. its source files, for the Makefiles of the Android 8.1 port (AOSP 8.x
builds with Soong; tools/mk_srcs.py reads Android.mk files).

Usage: bp_srcs.py ANDROID_BP MODULE [options]
  --prop NAME      the property (default: srcs); dotted for nested maps,
                   e.g. --prop export_include_dirs
  --arch ARCH      also arch: { ARCH: { ... } } (repeatable)
  --target T       also target: { T: { ... } } (repeatable: android, host,
                   linux, linux_glibc, not_windows ...)
  --multilib M     also multilib: { M: { ... } } (lib32, lib64)
  --static/--shared  also static: { ... } / shared: { ... }
  --prefix P       prepended to every item (e.g. the directory of the file)
  --exclude-prop   also remove the items of exclude_<prop> of those sections

The module's "defaults" are followed (modules of the same file only; others
are reported on stderr). Variables (NAME = [...], NAME += [...]) and "+"
concatenation are handled; globs are printed as they are.
"""
import re
import sys


class Parser:
    TOKEN = re.compile(r'''\s*(?:
        (?P<comment>//[^\n]*|/\*.*?\*/) |
        (?P<string>"(?:[^"\\]|\\.)*") |
        (?P<int>-?\d+) |
        (?P<ident>[A-Za-z_][A-Za-z0-9_.]*) |
        (?P<op>\+=|[{}\[\]:,=+()])
    )''', re.X | re.S)

    def __init__(self, text):
        self.toks = []
        pos = 0
        while True:
            m = self.TOKEN.match(text, pos)
            if not m or m.end() == pos:
                if text[pos:].strip():
                    raise SyntaxError('bad blueprint near: %r' % text[pos:pos + 40])
                break
            pos = m.end()
            kind = m.lastgroup
            if kind == 'comment':
                continue
            self.toks.append((kind, m.group(kind)))
        self.i = 0
        self.vars = {}
        self.modules = []

    def peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else (None, None)

    def take(self, value=None):
        tok = self.toks[self.i]
        if value is not None and tok[1] != value:
            raise SyntaxError('expected %r, got %r' % (value, tok[1]))
        self.i += 1
        return tok

    def parse(self):
        while self.i < len(self.toks):
            kind, name = self.take()
            op = self.peek()[1]
            if op in ('=', '+='):
                self.take()
                value = self.value()
                if op == '+=':
                    value = self.vars[name] + value
                self.vars[name] = value
            elif op in ('{', '('):
                props = self.value()
                props['__type__'] = name
                self.modules.append(props)
            else:
                raise SyntaxError('unexpected %r after %r' % (op, name))
        return self

    def value(self):
        v = self.single()
        while self.peek()[1] == '+':
            self.take()
            w = self.single()
            if isinstance(v, dict):
                v = dict(v, **w)
            else:
                v = v + w
        return v

    def single(self):
        kind, tok = self.take()
        if kind == 'string':
            return bytes(tok[1:-1], 'utf-8').decode('unicode_escape')
        if kind == 'int':
            return int(tok)
        if kind == 'ident':
            if tok in ('true', 'false'):
                return tok == 'true'
            return self.vars[tok]
        if tok == '[':
            items = []
            while self.peek()[1] != ']':
                items.append(self.value())
                if self.peek()[1] == ',':
                    self.take()
            self.take(']')
            return items
        if tok in ('{', '('):
            close = '}' if tok == '{' else ')'
            props = {}
            while self.peek()[1] != close:
                _, key = self.take()
                sep = self.take()[1]
                if sep not in (':', '='):
                    raise SyntaxError('expected ":" after %r' % key)
                props[key] = self.value()
                if self.peek()[1] == ',':
                    self.take()
            self.take(close)
            return props
        raise SyntaxError('unexpected %r' % tok)


def collect(module, by_name, prop, sections, exclude, seen=None):
    seen = seen or set()
    items = []
    for d in module.get('defaults', []):
        if d in seen:
            continue
        seen.add(d)
        if d in by_name:
            items += collect(by_name[d], by_name, prop, sections, exclude, seen)
        else:
            print('bp_srcs.py: defaults %r is not in this file' % d, file=sys.stderr)
    items += get(module, prop)
    excluded = []
    for path in sections:
        node = module
        for key in path:
            node = node.get(key, {}) if isinstance(node, dict) else {}
        items += get(node, prop)
        if exclude:
            excluded += get(node, 'exclude_' + prop)
    return [x for x in items if x not in excluded]


def get(node, prop):
    for key in prop.split('.')[:-1]:
        node = node.get(key, {})
    v = node.get(prop.split('.')[-1], [])
    return v if isinstance(v, list) else [v]


def main(argv):
    path, name = argv[0], argv[1]
    prop, prefix, sections, exclude = 'srcs', '', [], False
    args = argv[2:]
    while args:
        a = args.pop(0)
        if a == '--prop':
            prop = args.pop(0)
        elif a == '--prefix':
            prefix = args.pop(0)
        elif a in ('--arch', '--target', '--multilib'):
            sections.append((a[2:], args.pop(0)))
        elif a in ('--static', '--shared'):
            sections.append((a[2:],))
        elif a == '--exclude-prop':
            exclude = True
        else:
            raise SystemExit('bp_srcs.py: unknown option ' + a)
    p = Parser(open(path).read()).parse()
    by_name = {m.get('name'): m for m in p.modules}
    if name not in by_name:
        raise SystemExit('bp_srcs.py: no module %r in %s' % (name, path))
    items = collect(by_name[name], by_name, prop, sections, exclude)
    print('\n'.join(prefix + x for x in items))


if __name__ == '__main__':
    main(sys.argv[1:])
