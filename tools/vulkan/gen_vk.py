#!/usr/bin/env python3
"""Generates the guest-facing half of arc-wasm's libvulkan.so (topic vulkan,
docs/topics/vulkan.md) from the Khronos registry (vk.xml) and the list of
versions and extensions shim/vulkan implements (shim/vulkan/vk_extensions.txt).

  gen_vk.py VK_XML EXTENSIONS_TXT OUT_DIR

writes

  OUT_DIR/vk_api_gen.h      the ICD's functions (namespace arcvk, the C
                            prototypes of vk.xml), ARCVK_COMMANDS (every
                            entry point, aliases included, with its level),
                            ARCVK_INSTANCE_EXTENSIONS / ARCVK_DEVICE_EXTENSIONS
  OUT_DIR/vk_thunks_gen.inc the host symbols of libvulkan.so for both guest
                            ABIs (included by shim/vulkan/vk_thunks.cc):

    armeabi-v7a  The guest's structures are laid out as the host's (both
                 ILP32, 64-bit types 8-aligned, 4-byte enums), so a call
                 passes its pointers as they are (ndk::Thunk). Vulkan on
                 32-bit ARM Android is aapcs-vfp (VKAPI_ATTR): the few
                 float parameters arrive in s0-s15, read by raw thunks.
    arm64-v8a    Pointers, size_t, dispatchable handles and function
                 pointers are 8 bytes, so every structure with one of them
                 differs. Raw thunks copy the guest's input structures
                 (pNext chains too) into host ones in a per-thread arena,
                 call the ICD, and copy output structures and handles back.
                 Structures laid out alike in both ABIs (VkBufferCopy,
                 VkViewport...) pass through. Unknown pNext structures are
                 dropped on the way in and left alone on the way out.

The generated C++ checks the ILP32 layouts this script computes against the
compiler's (static_assert), which is what makes passing 32-bit guest
structures through safe.
"""

import re
import sys
import xml.etree.ElementTree as ET

# Commands vk_thunks.cc implements by hand for both ABIs.
CUSTOM = {'vkGetInstanceProcAddr', 'vkGetDeviceProcAddr'}
GLOBAL = {'vkCreateInstance', 'vkEnumerateInstanceExtensionProperties',
          'vkEnumerateInstanceLayerProperties', 'vkEnumerateInstanceVersion',
          'vkGetInstanceProcAddr'}

PRIM = {
    'char': (1, 1), 'int8_t': (1, 1), 'uint8_t': (1, 1),
    'int16_t': (2, 2), 'uint16_t': (2, 2),
    'int32_t': (4, 4), 'uint32_t': (4, 4), 'int': (4, 4), 'float': (4, 4),
    'int64_t': (8, 8), 'uint64_t': (8, 8), 'double': (8, 8),
}


def die(msg):
    sys.stderr.write('gen_vk.py: %s\n' % msg)
    sys.exit(1)


class Member:
    def __init__(self, el, consts):
        self.el = el
        self.type = el.find('type').text
        self.name = el.find('name').text
        parts = []
        for node in el.iter():
            if node.tag == 'comment':
                continue
            if node is not el and node.text and node.tag not in ('comment',):
                pass
        # The text around <type> and <name>.
        before = (el.text or '')
        mid = el.find('type').tail or ''
        after = el.find('name').tail or ''
        for ch in el:
            if ch.tag == 'enum':
                after += ch.text + (ch.tail or '')
        self.is_const = 'const' in before
        self.ptr = mid.count('*')
        self.inner_const = 'const' in mid  # const char* const*
        self.dims = []
        for d in re.findall(r'\[([A-Za-z0-9_]+)\]', after):
            self.dims.append(int(d) if d.isdigit() else consts[d])
        self.bitfield = ':' in after
        self.len = el.get('len')
        if self.len:
            self.len = self.len.split(',')[0]
        self.altlen = el.get('altlen')
        self.optional = el.get('optional')
        self.values = el.get('values')
        self.api = el.get('api')


class Registry:
    def __init__(self, path):
        self.root = ET.parse(path).getroot()
        self.consts = {}
        for enums in self.root.findall('enums'):
            if enums.get('name') == 'API Constants':
                for e in enums.findall('enum'):
                    v = e.get('value')
                    if v is not None and re.fullmatch(r'[0-9]+U?', v):
                        self.consts[e.get('name')] = int(v.rstrip('U'))
                    elif e.get('alias'):
                        self.consts[e.get('name')] = e.get('alias')
        for k, v in list(self.consts.items()):
            if isinstance(v, str):
                if v in self.consts and not isinstance(self.consts[v], str):
                    self.consts[k] = self.consts[v]
                else:
                    del self.consts[k]
        self.enum_width = {}
        for enums in self.root.findall('enums'):
            self.enum_width[enums.get('name')] = int(enums.get('bitwidth', '32'))
        self.types = {}      # name -> element
        self.alias = {}      # type alias -> target
        for t in self.root.find('types').findall('type'):
            if t.get('api') == 'vulkansc':
                continue
            name = t.get('name') or (t.find('name').text if t.find('name') is not None else None)
            if not name:
                continue
            if t.get('alias'):
                self.alias[name] = t.get('alias')
                continue
            self.types[name] = t
        self.members = {}
        for name, t in self.types.items():
            if t.get('category') in ('struct', 'union'):
                ms = [Member(m, self.consts) for m in t.findall('member')]
                self.members[name] = [m for m in ms if m.api != 'vulkansc']
        self.commands = {}
        self.cmd_alias = {}
        for c in self.root.find('commands').findall('command'):
            if c.get('api') == 'vulkansc':
                continue
            if c.get('alias'):
                self.cmd_alias[c.get('name')] = c.get('alias')
                continue
            proto = c.find('proto')
            name = proto.find('name').text
            params = [Member(p, self.consts) for p in c.findall('param') if p.get('api') != 'vulkansc']
            self.commands[name] = (proto, params)
        # sType value -> struct
        self.stype_of = {}
        for name, ms in self.members.items():
            for m in ms:
                if m.name == 'sType' and m.values:
                    self.stype_of[name] = m.values

    def resolve(self, name):
        while name in self.alias:
            name = self.alias[name]
        return name

    def category(self, name):
        name = self.resolve(name)
        if name in PRIM or name in ('size_t', 'void'):
            return 'prim'
        t = self.types.get(name)
        if t is None:
            return 'external'
        return t.get('category') or 'external'

    def dispatchable(self, name):
        t = self.types.get(self.resolve(name))
        return t is not None and t.get('category') == 'handle' and \
            t.find('type') is not None and t.find('type').text == 'VK_DEFINE_HANDLE'

    def scalar_size(self, name, lp64):
        """(size, align) of a non-pointer, non-array value of type |name|."""
        name = self.resolve(name)
        if name in PRIM:
            return PRIM[name]
        if name == 'size_t':
            return (8, 8) if lp64 else (4, 4)
        cat = self.category(name)
        t = self.types.get(name)
        if cat == 'basetype':
            inner = t.find('type')
            if inner is None:
                die('opaque base type by value: ' + name)
            if '*' in (inner.tail or ''):
                return (8, 8) if lp64 else (4, 4)
            return self.scalar_size(inner.text, lp64)
        if cat == 'bitmask':
            inner = t.find('type').text
            return self.scalar_size(inner, lp64)
        if cat == 'enum':
            w = self.enum_width.get(name, 32)
            return (8, 8) if w == 64 else (4, 4)
        if cat == 'handle':
            if self.dispatchable(name):
                return (8, 8) if lp64 else (4, 4)
            return (8, 8)
        if cat == 'funcpointer':
            return (8, 8) if lp64 else (4, 4)
        if cat in ('struct', 'union'):
            return self.layout(name, lp64)[0:2]
        die('size of %s (%s)' % (name, cat))

    def member_size(self, m, lp64):
        if m.ptr:
            s = (8, 8) if lp64 else (4, 4)
        else:
            s = self.scalar_size(m.type, lp64)
        n = 1
        for d in m.dims:
            n *= d
        return (s[0] * n, s[1])

    def layout(self, name, lp64, _cache={}):
        key = (name, lp64)
        if key in _cache:
            return _cache[key]
        union = self.types[name].get('category') == 'union'
        off = 0
        align = 1
        offs = []
        for m in self.members[name]:
            if m.bitfield:
                _cache[key] = None
                return None
            sz, al = self.member_size(m, lp64)
            align = max(align, al)
            if union:
                offs.append(0)
                off = max(off, sz)
            else:
                off = (off + al - 1) & ~(al - 1)
                offs.append(off)
                off += sz
        size = (off + align - 1) & ~(align - 1)
        _cache[key] = (size, align, offs)
        return _cache[key]

    def same_layout(self, name):
        a = self.layout(name, False)
        b = self.layout(name, True)
        return a is not None and b is not None and a == b


def depends_ok(expr, have):
    """Evaluates a vk.xml 'depends' expression (',' or, '+' and, parentheses)."""
    if not expr:
        return True
    tokens = re.findall(r'[A-Za-z0-9_]+|[,+()]', expr)
    pos = [0]

    def parse_or():
        v = parse_and()
        while pos[0] < len(tokens) and tokens[pos[0]] == ',':
            pos[0] += 1
            v = parse_and() or v
        return v

    def parse_and():
        v = parse_atom()
        while pos[0] < len(tokens) and tokens[pos[0]] == '+':
            pos[0] += 1
            v = parse_atom() and v
        return v

    def parse_atom():
        t = tokens[pos[0]]
        pos[0] += 1
        if t == '(':
            v = parse_or()
            pos[0] += 1
            return v
        return t in have

    return parse_or()


def main():
    if len(sys.argv) != 4:
        die('usage: gen_vk.py VK_XML EXTENSIONS_TXT OUT_DIR')
    reg = Registry(sys.argv[1])
    versions, inst_exts, dev_exts = [], [], []
    for line in open(sys.argv[2]):
        line = line.split('#')[0].split()
        if not line:
            continue
        kind, name = line
        {'version': versions, 'instance': inst_exts, 'device': dev_exts}[kind].append(name)
    have = set(versions) | set(inst_exts) | set(dev_exts)

    # Commands and types the supported API requires.
    cmd_names = []   # in registry order, aliases included
    req_types = set()
    ext_version = {}

    def take(block):
        for req in block.findall('require'):
            if req.get('api') == 'vulkansc' or not depends_ok(req.get('depends'), have):
                continue
            for c in req.findall('command'):
                if c.get('name') not in cmd_names:
                    cmd_names.append(c.get('name'))
            for t in req.findall('type'):
                req_types.add(t.get('name'))

    for f in reg.root.findall('feature'):
        if f.get('name') in versions and 'vulkan' in f.get('api', 'vulkan').split(','):
            take(f)
    found = set()
    for e in reg.root.find('extensions').findall('extension'):
        n = e.get('name')
        if n in inst_exts or n in dev_exts:
            found.add(n)
            take(e)
            for en in e.iter('enum'):
                if en.get('name', '').endswith('_SPEC_VERSION') and en.get('value'):
                    ext_version[n] = int(en.get('value'))
            kind = e.get('type')
            if (kind == 'instance') != (n in inst_exts):
                die('%s is a %s extension' % (n, kind))
    missing = (set(inst_exts) | set(dev_exts)) - found
    if missing:
        die('unknown extensions: ' + ', '.join(sorted(missing)))
    req_types = {reg.resolve(t) for t in req_types}

    def target(c):
        return reg.cmd_alias.get(c, c)

    impl = []  # implemented (non-alias) commands, in order
    for c in cmd_names:
        t = target(c)
        if t not in reg.commands:
            die('no command ' + t)
        if t not in impl:
            impl.append(t)

    def level(c):
        if c in GLOBAL:
            return 'global'
        params = reg.commands[target(c)][1]
        first = reg.resolve(params[0].type)
        if first in ('VkDevice', 'VkQueue', 'VkCommandBuffer'):
            return 'device'
        return 'instance'

    def ctype(m, name=None, host=True):
        """C declaration of |m| as the host sees it."""
        txt = ''.join(m.el.itertext())
        for ch in m.el:
            if ch.tag == 'comment':
                txt = txt.replace(ch.text, '')
        txt = ' '.join(txt.split())
        if name is not None:
            txt = re.sub(r'\b%s\b' % re.escape(m.name), name, txt)
        return txt

    # ---- vk_api_gen.h -----------------------------------------------------
    out = []
    w = out.append
    w('// Generated by tools/vulkan/gen_vk.py from vk.xml and shim/vulkan/vk_extensions.txt.')
    w('// The functions of arc-wasm\'s Vulkan ICD (shim/vulkan) and the API it reports.')
    w('#pragma once')
    w('#include <vulkan/vulkan.h>')
    w('')
    w('namespace arcvk {')
    for c in impl:
        proto, params = reg.commands[c]
        ret = proto.find('type').text
        w('%s %s(%s);' % (ret, c, ', '.join(ctype(p) for p in params)))
    w('}  // namespace arcvk')
    w('')
    w('// X(name, implementing function, level: global, instance or device)')
    w('#define ARCVK_COMMANDS(X) \\')
    for c in cmd_names:
        w('  X(%s, %s, %s) \\' % (c, target(c), level(c)))
    w('')
    w('// X(name, spec version)')
    w('#define ARCVK_INSTANCE_EXTENSIONS(X) \\')
    for e in inst_exts:
        w('  X("%s", %d) \\' % (e, ext_version[e]))
    w('')
    w('#define ARCVK_DEVICE_EXTENSIONS(X) \\')
    for e in dev_exts:
        w('  X("%s", %d) \\' % (e, ext_version[e]))
    w('')
    open(sys.argv[3] + '/vk_api_gen.h', 'w').write('\n'.join(out) + '\n')

    # ---- struct closure for the arm64 conversions --------------------------
    structs = []
    seen = set()

    def add_struct(n):
        n = reg.resolve(n)
        if n in seen or reg.category(n) not in ('struct', 'union'):
            return
        seen.add(n)
        for m in reg.members[n]:
            add_struct(m.type)
        structs.append(n)
        # Structures that may extend it through pNext.
        if n in reg.stype_of:
            for other in sorted(req_types):
                t = reg.types.get(other)
                if t is None or t.get('category') != 'struct':
                    continue
                ext = t.get('structextends')
                if ext and n in [reg.resolve(x) for x in ext.split(',')]:
                    add_struct(other)

    for c in impl:
        for p in reg.commands[c][1]:
            add_struct(p.type)
    for n in list(structs):
        if reg.layout(n, False) is None:
            die('bitfield struct reachable: ' + n)

    chain_structs = [n for n in structs if n in reg.stype_of and n in req_types | set(structs)]

    def is_input_struct(n):
        return reg.types[n].get('returnedonly') != 'true'

    o = []
    w = o.append
    w('// Generated by tools/vulkan/gen_vk.py from vk.xml and shim/vulkan/vk_extensions.txt.')
    w('// Included by shim/vulkan/vk_thunks.cc (namespace ndk::vkthunk), which defines Arena,')
    w('// ArenaScope, GuestPtr, NoteUnknownSType, F32 (an aapcs-vfp float argument) and')
    w('// the hand-written entry points.')
    w('')
    # Layout checks of the ILP32 host.
    w('// ILP32 layouts as gen_vk.py computes them (the armeabi-v7a guest\'s, the host\'s).')
    for n in structs:
        size, align, offs = reg.layout(n, False)
        w('static_assert(sizeof(%s) == %d && alignof(%s) == %d, "%s");' % (n, size, n, align, n))
        for m, off in zip(reg.members[n], offs):
            if reg.types[n].get('category') == 'struct':
                w('static_assert(offsetof(%s, %s) == %d, "%s.%s");' % (n, m.name, off, n, m.name))
    w('')

    # Forward declarations.
    for n in structs:
        if reg.same_layout(n):
            continue
        w('[[maybe_unused]] static void InBody_%s(uint32_t g, %s* h, Arena& a);' % (n, n))
        w('[[maybe_unused]] static void OutBody_%s(const %s* h, uint32_t g);' % (n, n))
    w('static void* InChain(uint64_t g, Arena& a);')
    w('static void* PrepChain(uint64_t g, Arena& a);')
    w('static void OutChain(const void* h, uint64_t g);')
    w('')

    def lp64size(n):
        return reg.layout(reg.resolve(n), True)[0]

    def hostptr(expr):
        return '(uintptr_t)GuestAddr64(%s)' % expr

    def len_expr(m, host_prefix):
        """Element count of pointer member |m|, from the host copy's fields."""
        if not m.len or m.len == 'null-terminated':
            return '1'
        if m.len.startswith('latexmath') or not re.fullmatch(r'[A-Za-z0-9_>\-]+', m.len):
            return None
        return host_prefix + m.len.replace('->', '->')

    for n in structs:
        if reg.same_layout(n):
            continue
        size, align, offs = reg.layout(n, True)
        is_union = reg.types[n].get('category') == 'union'
        # InBody: guest (LP64) -> host (ILP32), sType and pNext excluded.
        w('static void InBody_%s(uint32_t g, %s* h, Arena& a) {' % (n, n))
        w('  (void)a;')
        if is_union:
            w('  memcpy(h, GuestPtr(g), sizeof *h < %d ? sizeof *h : %d);' % (size, size))
            w('}')
        else:
            later = []
            for m, off in zip(reg.members[n], offs):
                if m.name in ('sType', 'pNext'):
                    continue
                t = reg.resolve(m.type)
                cat = reg.category(t)
                fld = 'h->' + m.name
                if m.ptr:
                    later.append((m, off))
                    continue
                if m.dims:
                    count = 1
                    for d in m.dims:
                        count *= d
                    if cat in ('struct', 'union') and not reg.same_layout(t):
                        es = lp64size(t)
                        w('  for (int i = 0; i < %d; i++) InBody_%s(g + %d + i * %d, &(&%s[0])[i], a);'
                          % (count, t, off, es, fld))
                        if t in reg.stype_of:
                            die('array of chained structs ' + n)
                    elif t == 'size_t' or reg.dispatchable(t) or cat == 'funcpointer':
                        w('  for (int i = 0; i < %d; i++) (&%s[0])[i] = (decltype(+%s[0]))%s;'
                          % (count, fld, fld, hostptr('Load64(g + %d + 8 * i)' % off)))
                    else:
                        w('  memcpy(&%s, GuestPtr(g + %d), sizeof %s);' % (fld, off, fld))
                    continue
                if cat in ('struct', 'union'):
                    if reg.same_layout(t):
                        w('  memcpy(&%s, GuestPtr(g + %d), sizeof %s);' % (fld, off, fld))
                    else:
                        if t in reg.stype_of:
                            w('  %s.sType = (VkStructureType)Load32(g + %d);' % (fld, off))
                            w('  %s.pNext = InChain(Load64(g + %d + 8), a);' % (fld, off))
                        w('  InBody_%s(g + %d, &%s, a);' % (t, off, fld))
                elif t == 'size_t':
                    w('  %s = (size_t)Load64(g + %d);' % (fld, off))
                elif reg.dispatchable(t) or cat == 'funcpointer':
                    w('  %s = (decltype(%s))%s;' % (fld, fld, hostptr('Load64(g + %d)' % off)))
                else:
                    w('  memcpy(&%s, GuestPtr(g + %d), sizeof %s);' % (fld, off, fld))
            for m, off in later:
                t = reg.resolve(m.type)
                cat = reg.category(t)
                fld = 'h->' + m.name
                gp = 'Load64(g + %d)' % off
                if m.ptr == 2 or (m.ptr == 1 and (reg.dispatchable(t) or cat == 'funcpointer')):
                    # An array of pointers (const char* const*) or of dispatchable handles.
                    cnt = len_expr(m, 'h->')
                    if cnt is None:
                        die('count of %s.%s' % (n, m.name))
                    w('  {')
                    w('    uint32_t p = GuestAddr64(%s);' % gp)
                    w('    if (!p) { %s = nullptr; } else {' % fld)
                    w('      uint32_t count = %s;' % cnt)
                    w('      auto* v = a.NewArray<uint32_t>(count);')
                    w('      for (uint32_t i = 0; i < count; i++) v[i] = GuestAddr64(Load64(p + 8 * i));')
                    w('      %s = (decltype(%s))v;' % (fld, fld))
                    w('    }')
                    w('  }')
                elif m.ptr == 1 and cat in ('struct', 'union') and not reg.same_layout(t):
                    cnt = len_expr(m, 'h->')
                    if cnt is None:
                        die('count of %s.%s' % (n, m.name))
                    es = lp64size(t)
                    w('  {')
                    w('    uint32_t p = GuestAddr64(%s);' % gp)
                    w('    if (!p) { %s = nullptr; } else {' % fld)
                    w('      uint32_t count = %s;' % cnt)
                    w('      auto* v = a.NewArray<%s>(count);' % t)
                    w('      for (uint32_t i = 0; i < count; i++) {')
                    if t in reg.stype_of:
                        w('        v[i].sType = (VkStructureType)Load32(p + i * %d);' % es)
                        w('        v[i].pNext = InChain(Load64(p + i * %d + 8), a);' % es)
                    w('        InBody_%s(p + i * %d, &v[i], a);' % (t, es))
                    w('      }')
                    w('      %s = v;' % fld)
                    w('    }')
                    w('  }')
                else:
                    # Data, strings, scalars, non-dispatchable handles, same-layout structures.
                    w('  %s = (decltype(%s))%s;' % (fld, fld, hostptr(gp)))
            w('}')
        # OutBody: host -> guest, sType and pNext excluded.
        w('static void OutBody_%s(const %s* h, uint32_t g) {' % (n, n))
        if is_union:
            w('  memcpy(GuestPtr(g), h, sizeof *h < %d ? sizeof *h : %d);' % (size, size))
            w('}')
            continue
        for m, off in zip(reg.members[n], offs):
            if m.name in ('sType', 'pNext'):
                continue
            t = reg.resolve(m.type)
            cat = reg.category(t)
            fld = 'h->' + m.name
            if m.ptr:
                continue  # pointers stay the guest's
            if m.dims:
                count = 1
                for d in m.dims:
                    count *= d
                if cat in ('struct', 'union') and not reg.same_layout(t):
                    es = lp64size(t)
                    w('  for (int i = 0; i < %d; i++) OutBody_%s(&(&%s[0])[i], g + %d + i * %d);'
                      % (count, t, fld, off, es))
                elif t == 'size_t' or reg.dispatchable(t) or cat == 'funcpointer':
                    w('  for (int i = 0; i < %d; i++) Store64(g + %d + 8 * i, GuestPtr64((const void*)(&%s[0])[i]));'
                      % (count, off, fld))
                else:
                    w('  memcpy(GuestPtr(g + %d), &%s, sizeof %s);' % (off, fld, fld))
                continue
            if cat in ('struct', 'union'):
                if reg.same_layout(t):
                    w('  memcpy(GuestPtr(g + %d), &%s, sizeof %s);' % (off, fld, fld))
                else:
                    w('  OutBody_%s(&%s, g + %d);' % (t, fld, off))
                    if t in reg.stype_of:
                        w('  OutChain(%s.pNext, Load64(g + %d + 8));' % (fld, off))
            elif t == 'size_t':
                w('  Store64(g + %d, (uint64_t)%s);' % (off, fld))
            elif reg.dispatchable(t) or cat == 'funcpointer':
                w('  Store64(g + %d, GuestPtr64((const void*)%s));' % (off, fld))
            else:
                w('  memcpy(GuestPtr(g + %d), &%s, sizeof %s);' % (off, fld, fld))
        w('}')
    w('')

    # Chains.
    w('static void* NewChained(VkStructureType st, uint32_t g, Arena& a, bool body) {')
    w('  switch (st) {')
    for n in chain_structs:
        w('    case %s: {' % reg.stype_of[n])
        w('      auto* h = a.New<%s>();' % n)
        w('      h->sType = st;')
        w('      if (body) InBody_%s(g, h, a);' % n)
        w('      return h;')
        w('    }')
    w('    default:')
    w('      NoteUnknownSType(st);')
    w('      return nullptr;')
    w('  }')
    w('}')
    w('static void* ChainCommon(uint64_t gp, Arena& a, bool body) {')
    w('  VkBaseOutStructure* head = nullptr;')
    w('  VkBaseOutStructure** tail = &head;')
    w('  for (int guard = 0; gp && guard < 64; guard++) {')
    w('    uint32_t g = GuestAddr64(gp);')
    w('    auto* h = static_cast<VkBaseOutStructure*>(NewChained((VkStructureType)Load32(g), g, a, body));')
    w('    if (h) {')
    w('      h->pNext = nullptr;')
    w('      *tail = h;')
    w('      tail = &h->pNext;')
    w('    }')
    w('    gp = Load64(g + 8);')
    w('  }')
    w('  return head;')
    w('}')
    w('static void* InChain(uint64_t g, Arena& a) { return ChainCommon(g, a, true); }')
    w('static void* PrepChain(uint64_t g, Arena& a) { return ChainCommon(g, a, false); }')
    w('static void OutChain(const void* hp, uint64_t gp) {')
    w('  for (int guard = 0; gp && guard < 64; guard++) {')
    w('    uint32_t g = GuestAddr64(gp);')
    w('    VkStructureType st = (VkStructureType)Load32(g);')
    w('    const VkBaseInStructure* h = static_cast<const VkBaseInStructure*>(hp);')
    w('    while (h && h->sType != st) h = h->pNext;')
    w('    if (h) {')
    w('      switch (st) {')
    for n in chain_structs:
        w('        case %s:' % reg.stype_of[n])
        w('          OutBody_%s(reinterpret_cast<const %s*>(h), g);' % (n, n))
        w('          break;')
    w('        default:')
    w('          break;')
    w('      }')
    w('    }')
    w('    gp = Load64(g + 8);')
    w('  }')
    w('}')
    w('')
    # ---- command thunks --------------------------------------------------
    syms = [(c, target(c)) for c in cmd_names if target(c) not in CUSTOM]

    # Parameters declared as arrays (const float blendConstants[4]) are pointers.
    for c in impl:
        for p in reg.commands[c][1]:
            if p.dims:
                p.ptr += 1
                p.dims = []

    def hosttype(p):
        base = ('const ' if p.is_const else '') + p.type
        if p.ptr == 1:
            return base + '*'
        if p.ptr == 2:
            return base + ('* const*' if p.inner_const else '**')
        return base

    def has_float(params):
        return any(reg.resolve(p.type) == 'float' and not p.ptr for p in params)

    # armeabi-v7a: raw thunks only where floats are passed (aapcs-vfp: s0-s15).
    for c in impl:
        if c in CUSTOM:
            continue
        proto, params = reg.commands[c]
        ret = proto.find('type').text
        if not has_float(params):
            continue
        w('static void V32_%s(NdkCpuState* s, void*) {' % c)
        w('  RegArgs r(s);')
        nf = 0
        for p in params:
            if reg.resolve(p.type) == 'float' and not p.ptr:
                w('  float %s = F32(s, %d);' % (p.name, nf))
                nf += 1
            else:
                w('  %s %s = GetArg<%s>(r);' % (hosttype(p), p.name, hosttype(p)))
        call = 'arcvk::%s(%s)' % (c, ', '.join(p.name for p in params))
        w(('  %s;' % call) if ret == 'void' else ('  SetResult(s, %s);' % call))
        w('}')

    # arm64-v8a.
    for c in impl:
        if c in CUSTOM:
            continue
        proto, params = reg.commands[c]
        ret = proto.find('type').text
        w('static void V64_%s(NdkCpu64State* s, void*) {' % c)
        w('  ArenaScope scope;')
        w('  Arena& a = scope.arena();')
        w('  (void)a;')
        w('  RegArgs64 r(s);')
        post = []
        args = []
        pnames = {p.name: p for p in params}
        # Every register, in order (integer and float registers count separately).
        for p in params:
            t = reg.resolve(p.type)
            if p.ptr:
                w('  uint32_t g_%s = GuestAddr64(r.X());' % p.name)
            elif t == 'float':
                w('  float %s = BitCast<float>((uint32_t)r.V());' % p.name)
            elif reg.dispatchable(t):
                w('  %s %s = (%s)(uintptr_t)(uint32_t)r.X();' % (p.type, p.name, p.type))
            else:
                w('  %s %s = (%s)r.X();' % (p.type, p.name, p.type))
        for p in params:
            t = reg.resolve(p.type)
            cat = reg.category(t)
            if not p.ptr:
                args.append(p.name)
                continue
            g = 'g_' + p.name
            if t == 'VkAllocationCallbacks':
                w('  (void)%s;' % g)
                args.append('nullptr')  # the ICD allocates on its own
                continue
            cnt = None
            if p.len and p.len != 'null-terminated':
                if '->' in p.len:
                    cnt = p.len
                elif p.len in pnames:
                    cnt = ('*' + p.len) if pnames[p.len].ptr else p.len
            n_expr = cnt or '1'
            after_n = cnt if (cnt and cnt.startswith('*')) else None
            if p.ptr == 1 and cat in ('struct', 'union') and not reg.same_layout(t):
                es = lp64size(t)
                w('  %s* %s = nullptr;' % (t, p.name))
                w('  uint32_t n_%s = 0;' % p.name)
                w('  if (%s) {' % g)
                w('    n_%s = %s;' % (p.name, n_expr))
                w('    %s = a.NewArray<%s>(n_%s);' % (p.name, t, p.name))
                w('    for (uint32_t i = 0; i < n_%s; i++) {' % p.name)
                if t in reg.stype_of:
                    w('      %s[i].sType = (VkStructureType)Load32(%s + i * %d);' % (p.name, g, es))
                    w('      %s[i].pNext = %s(Load64(%s + i * %d + 8), a);'
                      % (p.name, 'InChain' if p.is_const else 'PrepChain', g, es))
                if p.is_const:
                    w('      InBody_%s(%s + i * %d, &%s[i], a);' % (t, g, es, p.name))
                w('    }')
                w('  }')
                if not p.is_const:
                    post.append('  if (%s) {' % g)
                    post.append('    uint32_t count = %s;' % (after_n or 'n_' + p.name))
                    post.append('    if (count > n_%s) count = n_%s;' % (p.name, p.name))
                    post.append('    for (uint32_t i = 0; i < count; i++) {')
                    post.append('      OutBody_%s(&%s[i], %s + i * %d);' % (t, p.name, g, es))
                    if t in reg.stype_of:
                        post.append('      OutChain(%s[i].pNext, Load64(%s + i * %d + 8));' % (p.name, g, es))
                    post.append('    }')
                    post.append('  }')
                args.append(p.name)
            elif p.ptr == 2 and t == 'void':
                # void** (vkMapMemory): one pointer out.
                w('  void* v_%s = nullptr;' % p.name)
                args.append('%s ? &v_%s : nullptr' % (g, p.name))
                post.append('  if (%s) Store64(%s, GuestPtr64(v_%s));' % (g, g, p.name))
            elif p.ptr == 2 or (p.ptr == 1 and (reg.dispatchable(t) or cat == 'funcpointer')):
                # Arrays of 8-byte guest pointers or dispatchable handles.
                w('  uint32_t* v_%s = nullptr;' % p.name)
                w('  uint32_t n_%s = 0;' % p.name)
                w('  if (%s) {' % g)
                w('    n_%s = %s;' % (p.name, n_expr))
                w('    v_%s = a.NewArray<uint32_t>(n_%s);' % (p.name, p.name))
                if p.is_const or p.inner_const:
                    w('    for (uint32_t i = 0; i < n_%s; i++) v_%s[i] = GuestAddr64(Load64(%s + 8 * i));'
                      % (p.name, p.name, g))
                w('  }')
                args.append('(%s)v_%s' % (hosttype(p), p.name))
                if not (p.is_const or p.inner_const):
                    post.append('  if (%s) {' % g)
                    post.append('    uint32_t count = %s;' % (after_n or 'n_' + p.name))
                    post.append('    if (count > n_%s) count = n_%s;' % (p.name, p.name))
                    post.append('    for (uint32_t i = 0; i < count; i++) Store64(%s + 8 * i, v_%s[i]);'
                                % (g, p.name))
                    post.append('  }')
            elif t == 'size_t':
                # size_t* in and out (vkGetPipelineCacheData).
                w('  size_t v_%s = %s ? (size_t)Load64(%s) : 0;' % (p.name, g, g))
                w('  size_t* %s = %s ? &v_%s : nullptr;' % (p.name, g, p.name))
                args.append(p.name)
                post.append('  if (%s) Store64(%s, (uint64_t)v_%s);' % (g, g, p.name))
            else:
                # Data, strings, scalars, non-dispatchable handles, same-layout structures.
                w('  %s %s = (%s)(uintptr_t)%s;' % (hosttype(p), p.name, hosttype(p), g))
                args.append(p.name)
        call = 'arcvk::%s(%s)' % (c, ', '.join(args))
        if ret == 'void':
            w('  %s;' % call)
            o.extend(post)
        elif ret == 'VkResult':
            w('  VkResult res = %s;' % call)
            if post:
                w('  if (res >= 0) {')
                o.extend(['  ' + x for x in post])
                w('  }')
            w('  SetResult64(s, (int32_t)res);')
        else:
            w('  auto res = %s;' % call)
            o.extend(post)
            w('  SetResult64(s, res);')
        w('}')
    w('')
    w('static const ::ndk::HostSymbol kVulkanSymbols[] = {')
    for c, t in syms:
        params = reg.commands[t][1]
        f32 = ('&V32_%s' % t) if has_float(params) else ('&::ndk::Thunk<arcvk::%s>::Call' % t)
        w('    {"%s", %s, nullptr, 0, &V64_%s, 0},' % (c, f32, t))
    w('};')
    open(sys.argv[3] + '/vk_thunks_gen.inc', 'w').write('\n'.join(o) + '\n')


if __name__ == '__main__':
    main()
