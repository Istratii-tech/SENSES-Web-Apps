#!/usr/bin/env python3
"""Lazy function table: each thread fills only the table slots it calls.

Usage: wasm_lazy_table.py MODULE.wasm [MODULE.js]   (rewrites both in place)

An emscripten pthread is a worker with its own instance of the module, and
every instance fills its own copy of the function table from the module's
element segment. Safari 16 (iOS 16) makes a JS function object for every
element as it does: artvm8.wasm's 30472 elements cost ~12 MB per thread
(the first instance ~50 MB more), 25-35 threads an app -- 300-400 MB of a
tab that iOS reloads at ~1.5 GB (docs/topics/ios-mem.md).

So the element segment becomes passive (the table starts with null slots)
and every `call_indirect (type T)` becomes a call of a small function
  $lazy_T(args..., i): if table[i] is null, table.init it from the segment
                       (one slot); then call_indirect (type T) table[i]
A thread then makes the JS functions of what it calls, not of all 30472.
Slots outside the segment (emscripten's addFunction, the ARM translator's
translations: shim/ndk/cpu/jit.cc) are set by JS and never null when called.

The module exports $arc_table_fill(i) too, and MODULE.js reads the table
through it (wasmTable.get(i) -> arcTableGet(i)): emscripten's
getWasmTableEntry (thread entry points, callbacks) and the JNI bridges
(shim/art*/jni_call_wasm*.cc) take function pointers from JS.

Handles the features emscripten builds here use (MVP, sign-ext, sat
conversions, bulk memory, reference types, multivalue, atomics); no SIMD,
exceptions or tail calls (the tool stops on an unknown opcode).
"""

import struct
import sys


def uleb(d, p):
    r = s = 0
    while True:
        b = d[p]
        p += 1
        r |= (b & 0x7f) << s
        s += 7
        if b < 0x80:
            return r, p


def skip_leb(d, p):
    while d[p] & 0x80:
        p += 1
    return p + 1


def enc_u(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def enc_s(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        if (v == 0 and not b & 0x40) or (v == -1 and b & 0x40):
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def vec(items):
    return enc_u(len(items)) + b''.join(items)


def section(sid, body):
    return bytes([sid]) + enc_u(len(body)) + body


# Opcodes without immediates: control/parametric ones and all numeric ones.
NO_IMM = set([0x00, 0x01, 0x05, 0x0b, 0x0f, 0x1a, 0x1b, 0xd1]) | set(range(0x45, 0xc5))
ONE_U32 = set([0x0c, 0x0d, 0x10, 0x12, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x3f, 0x40, 0xd2])
MEMARG = set(range(0x28, 0x3f))


def scan_body(d, p, end, on_call_indirect):
    """Walks the instructions of a function body d[p:end] (after its locals)
    and calls on_call_indirect(start, after, typeidx, tableidx) for each."""
    while p < end:
        op = d[p]
        start = p
        p += 1
        if op in NO_IMM:
            continue
        if op in ONE_U32:
            p = skip_leb(d, p)
        elif op in MEMARG:
            p = skip_leb(d, skip_leb(d, p))
        elif op in (0x02, 0x03, 0x04):
            bt = d[p]
            if bt == 0x40 or bt in (0x7f, 0x7e, 0x7d, 0x7c, 0x7b, 0x70, 0x6f):
                p += 1
            else:
                p = skip_leb(d, p)  # s33 type index
        elif op == 0x0e:
            n, p = uleb(d, p)
            for _ in range(n + 1):
                p = skip_leb(d, p)
        elif op == 0x11:
            t, p = uleb(d, p)
            tab, p = uleb(d, p)
            on_call_indirect(start, p, t, tab)
        elif op == 0x1c:
            n, p = uleb(d, p)
            p += n
        elif op in (0x41, 0x42):
            p = skip_leb(d, p)
        elif op == 0x43:
            p += 4
        elif op == 0x44:
            p += 8
        elif op == 0xd0:
            p = skip_leb(d, p)
        elif op == 0xfc:
            sub, p = uleb(d, p)
            if sub <= 7:
                pass
            elif sub in (8, 10, 12, 14):
                p = skip_leb(d, skip_leb(d, p))
            elif sub in (9, 11, 13, 15, 16, 17):
                p = skip_leb(d, p)
            else:
                raise ValueError('unknown 0xfc %d at %d' % (sub, start))
        elif op == 0xfe:
            sub, p = uleb(d, p)
            if sub == 0x03:
                p += 1
            elif sub <= 0x02 or 0x10 <= sub <= 0x4e:
                p = skip_leb(d, skip_leb(d, p))
            else:
                raise ValueError('unknown 0xfe %d at %d' % (sub, start))
        else:
            raise ValueError('unknown opcode 0x%02x at %d' % (op, start))
    if p != end:
        raise ValueError('body overrun at %d' % p)


def rewrite(wasm):
    d = wasm
    assert d[:8] == b'\0asm\x01\0\0\0'
    secs = []
    p = 8
    while p < len(d):
        sid = d[p]
        size, q = uleb(d, p + 1)
        secs.append([sid, d[q:q + size]])
        p = q + size
    by_id = {}
    for s in secs:
        if s[0] != 0:
            by_id[s[0]] = s

    # Types: (params, results) as raw value type bytes.
    t = by_id[1][1]
    n, q = uleb(t, 0)
    types = []
    for _ in range(n):
        assert t[q] == 0x60, 'only function types'
        np_, q = uleb(t, q + 1)
        params = t[q:q + np_]
        q += np_
        nr, q = uleb(t, q)
        results = t[q:q + nr]
        q += nr
        types.append((bytes(params), bytes(results)))

    # Imported functions (they come first in the function index space).
    imp = by_id[2][1]
    n, q = uleb(imp, 0)
    nimported = 0
    for _ in range(n):
        l, q = uleb(imp, q)
        q += l
        l, q = uleb(imp, q)
        q += l
        kind = imp[q]
        q += 1
        if kind == 0:
            nimported += 1
            q = skip_leb(imp, q)
        elif kind == 1:
            q += 1
            flags, q = uleb(imp, q)
            q = skip_leb(imp, q)
            if flags & 1:
                q = skip_leb(imp, q)
        elif kind == 2:
            flags, q = uleb(imp, q)
            q = skip_leb(imp, q)
            if flags & 1:
                q = skip_leb(imp, q)
        elif kind == 3:
            q += 2
        else:
            raise ValueError('import kind %d' % kind)

    # A counter of filled slots (arc_kernel.cc's arc_lazy_table_fills: an
    # exported data symbol, i.e. a global holding its address), if linked.
    counter = None
    ex = by_id[7][1]
    nex, q = uleb(ex, 0)
    for _ in range(nex):
        l, q = uleb(ex, q)
        nm = bytes(ex[q:q + l])
        q += l
        kind = ex[q]
        idx, q = uleb(ex, q + 1)
        if kind == 3 and nm == b'arc_lazy_table_fills' and 6 in by_id:
            g = by_id[6][1]
            ng, r = uleb(g, 0)
            for gi in range(ng):
                r += 2
                if g[r] != 0x41:
                    break
                v = s_ = 0
                r += 1
                while True:
                    b = g[r]
                    r += 1
                    v |= (b & 0x7f) << s_
                    s_ += 7
                    if b < 0x80:
                        if b & 0x40:
                            v -= 1 << s_
                        break
                assert g[r] == 0x0b
                r += 1
                if gi == idx:  # (no imported globals in these modules)
                    counter = v
                    break

    fsec = by_id[3][1]
    nfuncs, q = uleb(fsec, 0)
    func_types = []
    for _ in range(nfuncs):
        ti, q = uleb(fsec, q)
        func_types.append(ti)

    # The element segment: one active segment of function indices for table 0.
    e = by_id[9][1]
    n, q = uleb(e, 0)
    if n != 1 or e[q] != 0:
        raise ValueError('expected one active element segment (flags 0)')
    q += 1
    if e[q] != 0x41:
        raise ValueError('element segment offset is not i32.const')
    v = s = 0
    q += 1
    while True:
        b = e[q]
        q += 1
        v |= (b & 0x7f) << s
        s += 7
        if b < 0x80:
            if b & 0x40:
                v -= 1 << s
            break
    base = v
    assert e[q] == 0x0b
    q += 1
    nelems, q = uleb(e, q)
    elems_bytes = e[q:]
    # Passive: flags 1, elemkind 0 (funcref), the same function indices.
    by_id[9][1] = enc_u(1) + b'\x01\x00' + enc_u(nelems) + elems_bytes

    # Code: call_indirect (type T) -> call $lazy_T.
    code = by_id[10][1]
    nbodies, q = uleb(code, 0)
    assert nbodies == nfuncs
    first_new = nimported + nfuncs
    lazy = {}  # type index -> function index of its $lazy_T
    out_bodies = []
    count = [0]
    for _ in range(nbodies):
        size, q = uleb(code, q)
        body_start, body_end = q, q + size
        nl, p = uleb(code, q)
        for _ in range(nl):
            p = skip_leb(code, p)
            p += 1
        pieces = []
        last = [body_start]

        def on_ci(start, after, ti, tab):
            if tab != 0:
                raise ValueError('call_indirect of table %d' % tab)
            if ti not in lazy:
                lazy[ti] = first_new + len(lazy)
            pieces.append(code[last[0]:start])
            pieces.append(b'\x10' + enc_u(lazy[ti]))
            last[0] = after
            count[0] += 1

        scan_body(code, p, body_end, on_ci)
        if pieces:
            pieces.append(code[last[0]:body_end])
            body = b''.join(pieces)
        else:
            body = code[body_start:body_end]
        out_bodies.append(enc_u(len(body)) + body)
        q = body_end

    # The new functions' types: T's params + i32 -> T's results (and the
    # fill function's i32 -> nothing), appended to the type section.
    new_types = []

    def type_index(params, results):
        key = (params, results)
        for i, tt in enumerate(types + new_types):
            if tt == key:
                return i
        new_types.append(key)
        return len(types) + len(new_types) - 1

    # (atomic add 1 to the counter: i32.atomic.rmw.add, then drop)
    count_fill = (b'\x41' + enc_s(counter) + b'\x41\x01\xfe\x1e\x02\x00\x1a'
                  if counter is not None else b'')
    # if (table[i] == null && (unsigned)(i - base) < nelems) table.init(i, i - base, 1)
    fill_body_check = lambda i: (
        b'\x20' + enc_u(i) + b'\x25\x00' + b'\xd1' +
        b'\x20' + enc_u(i) + b'\x41' + enc_s(base) + b'\x6b' + b'\x41' + enc_s(nelems) + b'\x49' +
        b'\x71' + b'\x04\x40' +
        b'\x20' + enc_u(i) + b'\x20' + enc_u(i) + b'\x41' + enc_s(base) + b'\x6b' +
        b'\x41\x01' + b'\xfc\x0c\x00\x00' + count_fill + b'\x0b')
    new_func_types = []
    new_bodies = []
    for ti, fi in sorted(lazy.items(), key=lambda x: x[1]):
        params, results = types[ti]
        new_func_types.append(type_index(params + b'\x7f', results))
        i = len(params)
        b = b'\x00' + fill_body_check(i)
        for k in range(i + 1):
            b += b'\x20' + enc_u(k)
        b += b'\x11' + enc_u(ti) + b'\x00' + b'\x0b'
        new_bodies.append(enc_u(len(b)) + b)
    fill_index = first_new + len(lazy)
    new_func_types.append(type_index(b'\x7f', b''))
    b = b'\x00' + fill_body_check(0) + b'\x0b'
    new_bodies.append(enc_u(len(b)) + b)

    by_id[1][1] = vec([b'\x60' + vec([bytes([x]) for x in pr]) + vec([bytes([x]) for x in rs])
                       for pr, rs in types + new_types])
    by_id[3][1] = vec([enc_u(x) for x in func_types + new_func_types])
    by_id[10][1] = vec(out_bodies + new_bodies)
    ex = by_id[7][1]
    nex, q = uleb(ex, 0)
    name = b'arc_table_fill'
    by_id[7][1] = enc_u(nex + 1) + ex[q:] + enc_u(len(name)) + name + b'\x00' + enc_u(fill_index)
    out = b'\0asm\x01\0\0\0' + b''.join(section(s[0], bytes(s[1])) for s in secs)
    return out, count[0], len(lazy), nelems, counter


JS_GET = 'var arcTableGet = (i) => wasmTable.get(i) || (wasmExports["arc_table_fill"](i), wasmTable.get(i));\n'


def rewrite_js(js):
    if 'arcTableGet' in js:
        return js, 0
    n = js.count('wasmTable.get(')
    js = js.replace('wasmTable.get(', 'arcTableGet(')
    anchor = 'var wasmTableMirror = [];'
    if anchor not in js:
        raise ValueError('no wasmTableMirror in the JS')
    return js.replace(anchor, JS_GET + anchor, 1), n


def main():
    wasm_path = sys.argv[1]
    with open(wasm_path, 'rb') as f:
        d = f.read()
    out, n, ntypes, nelems, counter = rewrite(d)
    with open(wasm_path, 'wb') as f:
        f.write(out)
    msg = '%s: %d elements lazy, %d call_indirect via %d functions%s' % (
        wasm_path, nelems, n, ntypes, ', fills counted at %d' % counter if counter is not None else '')
    if len(sys.argv) > 2:
        with open(sys.argv[2]) as f:
            js = f.read()
        js, nget = rewrite_js(js)
        with open(sys.argv[2], 'w') as f:
            f.write(js)
        msg += ', %d table reads in %s' % (nget, sys.argv[2])
    print(msg)


if __name__ == '__main__':
    main()
