#!/usr/bin/env python3
"""Writes arcwasm/ConstMh.class into CLASSES_DIR: a class whose static methods
return method handle and method type constants (ldc of CONSTANT_MethodHandle
and CONSTANT_MethodType). javac never emits these from Java source; d8
--min-api 28+ turns them into the dex 039 instructions const-method-handle
and const-method-type, which tests/java9/arcwasm/Java9Test.java runs on
ART 10 (topic art10).

  static MethodHandle twice()        REF_invokeStatic   Java9Test.twice(I)I
  static MethodHandle concat()       REF_invokeVirtual  String.concat(String)String
  static MethodHandle newBuilder()   REF_newInvokeSpecial StringBuilder.<init>(String)V
  static MethodHandle getCounter()   REF_getStatic      Java9Test.counter:I
  static MethodHandle putCounter()   REF_putStatic      Java9Test.counter:I
  static MethodHandle getValue()     REF_getField       Java9Test$Box.value:J
  static MethodType   type()         (IJ[Ljava/lang/String;)Ljava/lang/Object;

Usage: gen_const_mh_test.py CLASSES_DIR
"""
import os
import struct
import sys

REF_getField, REF_getStatic, REF_putField, REF_putStatic = 1, 2, 3, 4
REF_invokeVirtual, REF_invokeStatic, REF_invokeSpecial, REF_newInvokeSpecial = 5, 6, 7, 8


class Pool:
    def __init__(self):
        self.entries = []
        self.index = {}

    def add(self, key, data):
        if key not in self.index:
            self.entries.append(data)
            self.index[key] = len(self.entries)
        return self.index[key]

    def utf8(self, s):
        b = s.encode('utf-8')
        return self.add(('utf8', s), struct.pack('>BH', 1, len(b)) + b)

    def cls(self, name):
        return self.add(('class', name), struct.pack('>BH', 7, self.utf8(name)))

    def nat(self, name, desc):
        return self.add(('nat', name, desc), struct.pack('>BHH', 12, self.utf8(name), self.utf8(desc)))

    def member(self, tag, owner, name, desc):
        return self.add((tag, owner, name, desc),
                        struct.pack('>BHH', tag, self.cls(owner), self.nat(name, desc)))

    def method_handle(self, kind, owner, name, desc):
        tag = 9 if kind <= REF_putStatic else 10  # Fieldref / Methodref
        ref = self.member(tag, owner, name, desc)
        return self.add(('mh', kind, ref), struct.pack('>BBH', 15, kind, ref))

    def method_type(self, desc):
        return self.add(('mt', desc), struct.pack('>BH', 16, self.utf8(desc)))

    def bytes(self):
        return struct.pack('>H', len(self.entries) + 1) + b''.join(self.entries)


def main(out_dir):
    pool = Pool()
    this = 'arcwasm/ConstMh'
    test = 'arcwasm/Java9Test'
    mh_desc = '()Ljava/lang/invoke/MethodHandle;'
    methods = [
        ('twice', mh_desc, pool.method_handle(REF_invokeStatic, test, 'twice', '(I)I')),
        ('concat', mh_desc, pool.method_handle(REF_invokeVirtual, 'java/lang/String', 'concat',
                                               '(Ljava/lang/String;)Ljava/lang/String;')),
        ('newBuilder', mh_desc, pool.method_handle(REF_newInvokeSpecial, 'java/lang/StringBuilder',
                                                   '<init>', '(Ljava/lang/String;)V')),
        ('getCounter', mh_desc, pool.method_handle(REF_getStatic, test, 'counter', 'I')),
        ('putCounter', mh_desc, pool.method_handle(REF_putStatic, test, 'counter', 'I')),
        ('getValue', mh_desc, pool.method_handle(REF_getField, test + '$Box', 'value', 'J')),
        ('type', '()Ljava/lang/invoke/MethodType;',
         pool.method_type('(IJ[Ljava/lang/String;)Ljava/lang/Object;')),
    ]
    this_cls = pool.cls(this)
    super_cls = pool.cls('java/lang/Object')
    code_name = pool.utf8('Code')
    body = []
    for name, desc, const in methods:
        # ldc_w #const; areturn (max_stack 1, max_locals 0; no branches, so no
        # StackMapTable)
        code = struct.pack('>BHB', 0x13, const, 0xb0)
        attr = struct.pack('>HHI', 1, 0, len(code)) + code + struct.pack('>HH', 0, 0)
        body.append(struct.pack('>HHHH', 0x0009, pool.utf8(name), pool.utf8(desc), 1) +
                    struct.pack('>HI', code_name, len(attr)) + attr)
    data = (struct.pack('>IHH', 0xCAFEBABE, 0, 52) + pool.bytes() +
            struct.pack('>HHHHH', 0x0031, this_cls, super_cls, 0, 0) +
            struct.pack('>H', len(body)) + b''.join(body) + struct.pack('>H', 0))
    path = os.path.join(out_dir, 'arcwasm', 'ConstMh.class')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


if __name__ == '__main__':
    main(sys.argv[1])
