#!/usr/bin/env python3
"""Checks that JNI registrations match the C functions they point to.

WebAssembly calls a function only through its exact type, so a Java
signature that disagrees with the C prototype (long vs int, a missing
argument, a different return type) breaks at run time in wasm even where
32-bit ARM tolerated it. Handles the NATIVE_METHOD(Class, name, "sig")
registration style of libcore and conscrypt.

Usage: check_jni_sigs.py SOURCE.cpp...   (exit status: number of mismatches)
"""
import re
import sys

C_TYPES = {'jlong': 'J', 'jint': 'I', 'jboolean': 'Z', 'jbyte': 'B', 'jchar': 'C',
           'jshort': 'S', 'jfloat': 'F', 'jdouble': 'D', 'void': 'V',
           'float': 'F', 'double': 'D', 'bool': 'Z', 'char': 'B', 'short': 'S'}


def java_types(sig):
    m = re.match(r'\((.*)\)(.*)', sig)
    args, ret = m.group(1), m.group(2)
    out, i = [], 0
    while i < len(args):
        c = args[i]
        if c == 'L':
            i = args.index(';', i) + 1
            out.append('L')
        elif c == '[':
            while args[i] == '[':
                i += 1
            i = args.index(';', i) + 1 if args[i] == 'L' else i + 1
            out.append('L')
        else:
            out.append(c)
            i += 1
    return out, (ret[0] if ret[0] in 'VZBCSIJFD' else 'L')


def c_type(decl):
    words = decl.replace('const', ' ').replace('*', ' * ').split()
    if not words:
        return None
    if '*' in words:
        return 'P'  # a pointer: 32-bit, like I and L
    words = [w for w in words if w not in ('unsigned', 'signed')]
    if words[:2] == ['long', 'long'] or words[0] in ('int64_t', 'uint64_t'):
        return 'J'
    if words[0] in ('int', 'long', 'int32_t', 'uint32_t', 'size_t'):
        return 'I'
    return C_TYPES.get(words[0], 'L')


def check(path):
    src = open(path).read()
    # String macros used inside signatures (e.g. conscrypt's FILE_DESCRIPTOR,
    # SSL_CALLBACKS): any #define whose value starts with "L or "[ is an
    # object type (the value may start on the next line, after a backslash).
    obj_macros = set(re.findall(r'#define\s+(\w+)\s+(?:\\\n\s*)?"[L\[]', src))
    defs = {}
    for m in re.finditer(r'static\s+([\w\s\*]+?)\s*\b(\w+?)_(\w+)\s*\(([^)]*)\)\s*\{', src):
        defs[(m.group(2), m.group(3))] = (m.group(1), m.group(4))
    # Any function definition by name, for the {"name", "sig", (void*) fn}
    # style of frameworks/base/core/jni.
    fns = {}
    for m in re.finditer(r'(?:^|\n)(?:static\s+)?([\w:\s\*]+?)\s*\b(\w+)\s*\(\s*(JNIEnv\s*\*[^)]*)\)\s*\{', src):
        fns[m.group(2)] = (m.group(1).split()[-1] if m.group(1).split() else '', m.group(3))
    bad = 0
    for m in re.finditer(r'\{\s*"(\w+)"\s*,\s*((?:"[^"]*"\s*|\w+\s*)+),\s*\(void\s*\*\)\s*&?\s*(\w+)\s*\}', src):
        name, sig_expr, fn = m.groups()
        for name_ in obj_macros:
            sig_expr = re.sub(r'\b%s\b' % name_, '"Lx;"', sig_expr)
        sig = ''.join(re.findall(r'"([^"]*)"', sig_expr))
        sig = re.sub(r'L[^;]*;', 'Lx;', sig)
        if fn not in fns or not sig.startswith('('):
            continue
        jargs, jret = java_types(sig)
        ret, params = fns[fn]
        cargs = [c_type(p) for p in params.split(',')[2:]]
        cret = c_type(ret)
        norm = lambda t: 'I' if t in ('I', 'L', 'P', 'Z', 'B', 'C', 'S') else t
        if [norm(t) for t in cargs] != [norm(t) for t in jargs] or norm(cret) != norm(jret):
            bad += 1
            print('%s: %s %s -> C %s %s(%s)' % (path, name, sig, ret, fn,
                                                ' '.join(params.split())))
    # NATIVE_METHOD(Class, name, "sig") of libcore and conscrypt 8.1, and
    # CONSCRYPT_NATIVE_METHOD(name, "sig") of conscrypt 10 (NativeCrypto_name).
    regs = [m.groups() for m in re.finditer(
        r'(?<!CONSCRYPT_)NATIVE_METHOD\(\s*(\w+)\s*,\s*(\w+)\s*,\s*((?:"[^"]*"|[\s\w()]+?)+)\)\s*,', src)]
    regs += [('NativeCrypto',) + m.groups() for m in re.finditer(
        r'CONSCRYPT_NATIVE_METHOD\(\s*(\w+)\s*,\s*((?:"[^"]*"|[^"(),]|\(\w*\))+?)\)\s*,', src)]
    for cls, name, sig_expr in regs:
        for name_ in obj_macros:
            sig_expr = re.sub(r'\b%s\b' % name_, '"Lx;"', sig_expr)
        sig = ''.join(re.findall(r'"([^"]*)"', sig_expr))
        sig = re.sub(r'L[^;]*;', 'Lx;', sig)
        if (cls, name) not in defs or not sig.startswith('('):
            continue
        jargs, jret = java_types(sig)
        ret, params = defs[(cls, name)]
        cargs = [c_type(p) for p in params.split(',')[2:]]
        cret = c_type(ret)
        norm = lambda t: 'I' if t in ('I', 'L', 'P', 'Z', 'B', 'C', 'S') else t
        if [norm(t) for t in cargs] != [norm(t) for t in jargs] or norm(cret) != norm(jret):
            bad += 1
            print('%s: %s.%s Java %s -> %s, C %s(%s)' % (path, cls, name, sig, jret, ret.strip(),
                                                     params.strip()))
    return bad


if __name__ == '__main__':
    total = sum(check(p) for p in sys.argv[1:])
    print('%d mismatch(es)' % total)
    sys.exit(min(total, 125))
