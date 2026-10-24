#!/usr/bin/env python3
"""Redirects HTC code's uses of framework members AOSP lacks (topic userspace).

HTC's framework adds members to AOSP classes (ApplicationInfo.allowSkinChange,
TelephonyManager.dualPhoneEnable(), View.buildDrawingCacheAlways(), ...) and
its libraries and apps use them; on our AOSP framework those uses would throw
NoSuchFieldError / NoSuchMethodError. This rewrites each use, in smali, into a
call of a generated static method of arcwasm.htc.Missing that logs the member
once (arcwasm.htc.MissingLog) and answers a default: nothing done, 0, false,
null. As the lenient services do: a feature this system does not have.

  invoke-KIND {regs}, Lc;->m(A)R      ->  invoke-static {regs}, Missing;->mN(Lc;A)R
                                          (the receiver first, unless static)
  iget-T vA, vB, Lc;->f:T             ->  invoke-static {vB}, Missing;->mN(Lc;)T
                                          move-result-T vA
  iput-T vA, vB, Lc;->f:T             ->  invoke-static {vA, vB}, Missing;->mN(TLc;)V
  sget-T vAA, Lc;->f:T                ->  invoke-static {}, ...; move-result-T vAA
  sput-T vAA, Lc;->f:T                ->  invoke-static/range {vAA .. vAA[+1]}, ...

Where arcwasm.htc.Members (framework/java-htc) has our version of a member,
CLASS_NAME(receiver, args), the use calls that instead.

A member is missing when neither its class nor any superclass or interface
(ours, HTC's classes we ship, the app's) declares it, and all of those are
known. The smali must use v registers only (baksmali --parameter-registers
false).

HTC's framework also makes AOSP classes implement HTC interfaces
(ActivityManager implements HtcIfActivityManager, ...); ours do not ("lost"
interfaces). A call through one goes to a stub that makes it when the
receiver implements the interface and answers the default otherwise, and a
check-cast to one is dropped (it would throw for our objects).

Usage: htc_fix_missing.py OUT_DIR --ours DIR... --fix DIR... [--lost FILE]
  --ours  smali trees of our boot jars (read only)
  --fix   smali trees of HTC's classes and apps (rewritten in place)
  --lost  the lost interfaces, one per line
  OUT_DIR gets arcwasm/htc/Missing.smali
"""
import os
import re
import sys

INVOKE = re.compile(r'^(\s*)invoke-(virtual|super|direct|static|interface)(/range)?\s+\{([^}]*)\},\s*'
                    r'(L[^;]+;|\[[^-]+)->([^(\s]+)\(([^)]*)\)(\S+)\s*$')
FIELD = re.compile(r'^(\s*)([is])(get|put)(-wide|-object|-boolean|-byte|-char|-short)?\s+'
                   r'([^,]+?)(?:,\s*([^,]+?))?,\s*(L[^;]+;)->([^:\s]+):(\S+)\s*$')
CHECK_CAST = re.compile(r'^(\s*)check-cast\s+v\d+,\s*(L[^;]+;)\s*$')
TYPE = re.compile(r'\[*(?:[ZBSCIJFDV]|L[^;]+;)')


def index(roots):
    """class -> (super, interfaces, members); members: 'name(args)ret' / 'name:type'."""
    classes = {}
    for root in roots:
        for d, _, files in os.walk(root):
            for f in files:
                if not f.endswith('.smali'):
                    continue
                cls, sup, ifs, mem = None, None, [], set()
                for line in open(os.path.join(d, f), errors='replace'):
                    if line.startswith('.class'):
                        cls = line.split()[-1]
                    elif line.startswith('.super'):
                        sup = line.split()[-1]
                    elif line.startswith('.implements'):
                        ifs.append(line.split()[-1])
                    elif line.startswith('.method'):
                        mem.add(line.split()[-1])
                    elif line.startswith('.field'):
                        mem.add(line.split('=')[0].split()[-1])
                if cls:
                    classes[cls] = (sup, ifs, mem)
    return classes


def resolve(classes, cls, member, depth=0):
    """True: declared; False: missing; None: a class on the way is unknown."""
    if cls.startswith('['):
        return True  # array methods (clone) are Object's
    if depth > 40:
        return None
    entry = classes.get(cls)
    if entry is None:
        return None
    sup, ifs, mem = entry
    if member in mem:
        return True
    unknown = False
    for parent in ([sup] if sup else []) + ifs:
        r = resolve(classes, parent, member, depth + 1)
        if r:
            return True
        if r is None:
            unknown = True
    return None if unknown else False


def params(desc):
    return TYPE.findall(desc)


def size(t):
    return 2 if t in ('J', 'D') else 1


def reg_list(text):
    """'{v1, v2}' contents or 'v3 .. v6' -> register numbers."""
    text = text.strip()
    if not text:
        return []
    if '..' in text:
        a, b = [int(x.strip()[1:]) for x in text.split('..')]
        return list(range(a, b + 1))
    return [int(x.strip()[1:]) for x in text.split(',')]


def suffix(t):
    if t in ('J', 'D'):
        return '-wide'
    if t[0] in 'L[':
        return '-object'
    return ''


class Stubs:
    def __init__(self):
        self.stubs = {}  # (label, desc) -> name

    def get(self, label, args, ret, dispatch=None):
        """dispatch: (interface, name) to call when the receiver has it."""
        key = (label, ''.join(args), ret, dispatch)
        if key not in self.stubs:
            self.stubs[key] = 'm%d' % len(self.stubs)
        return self.stubs[key]

    def smali(self):
        out = ['.class public final Larcwasm/htc/Missing;', '.super Ljava/lang/Object;',
               '.source "htc_fix_missing.py"', '',
               '# Generated by tools/htc_fix_missing.py: HTC framework members AOSP lacks.', '']
        for (label, args, ret, dispatch), name in sorted(self.stubs.items(),
                                                         key=lambda kv: int(kv[1][1:])):
            nparams = sum(size(t) for t in params(args))
            out.append('.method public static %s(%s)%s' % (name, args, ret))
            out.append('    .registers %d' % (2 + nparams))
            if dispatch:
                iface, member = dispatch
                out.append('    instance-of v0, p0, %s' % iface)
                out.append('    if-eqz v0, :missing')
                out.append('    invoke-interface/range {p0 .. p%d}, %s->%s' % (
                    nparams - 1, iface, member))
                if ret != 'V':
                    out.append('    move-result%s v0' % suffix(ret))
                    out.append('    return%s v0' % suffix(ret))
                else:
                    out.append('    return-void')
                out.append('    :missing')
            out.append('    const-string v0, "%s"' % label.replace('"', '\\"'))
            out.append('    invoke-static {v0}, Larcwasm/htc/MissingLog;->once(Ljava/lang/String;)V')
            if ret == 'V':
                out.append('    return-void')
            elif ret in ('J', 'D'):
                out.append('    const-wide/16 v0, 0x0')
                out.append('    return-wide v0')
            elif ret[0] in 'L[':
                out.append('    const/4 v0, 0x0')
                out.append('    return-object v0')
            else:
                out.append('    const/4 v0, 0x0')
                out.append('    return v0')
            out.append('.end method')
            out.append('')
        return '\n'.join(out) + '\n'


def fix_file(path, classes, stubs, counts, members, lost):
    lines = open(path, errors='replace').read().split('\n')
    out = []
    changed = False
    for line in lines:
        m = CHECK_CAST.match(line)
        if m and m.group(2) in lost:
            # Our class does not implement HTC's interface it does there.
            out.append('%s# arc-wasm: check-cast %s dropped (tools/htc_fix_missing.py)' % (
                m.group(1), m.group(2)))
            changed = True
            continue
        m = INVOKE.match(line)
        if m and m.group(2) == 'interface' and m.group(5) in lost:
            ind, kind, rng, regs, cls, name, args, ret = m.groups()
            member = '%s(%s)%s' % (name, args, ret)
            stub_args = [cls] + params(args)
            stub = stubs.get('%s->%s (not implemented by our class)' % (cls, member),
                             ''.join(stub_args), ret, (cls, member))
            out.append('%sinvoke-static%s {%s}, Larcwasm/htc/Missing;->%s(%s)%s' % (
                ind, rng or '', regs, stub, ''.join(stub_args), ret))
            counts[cls + '->' + member + ' (lost)'] = counts.get(cls + '->' + member + ' (lost)', 0) + 1
            changed = True
            continue
        if m:
            ind, kind, rng, regs, cls, name, args, ret = m.groups()
            member = '%s(%s)%s' % (name, args, ret)
            # A constructor stays (an object must be initialized by one).
            if not name.startswith('<') and resolve(classes, cls, member) is False:
                stub_args = ([] if kind == 'static' else [cls]) + params(args)
                ours = '%s_%s' % (cls[1:-1].split('/')[-1].replace('$', '_'), name)
                if '%s(%s)%s' % (ours, ''.join(stub_args), ret) in members:
                    target = 'Larcwasm/htc/Members;->' + ours  # our version of it
                else:
                    target = 'Larcwasm/htc/Missing;->' + stubs.get(
                        '%s->%s' % (cls, member), ''.join(stub_args), ret)
                out.append('%sinvoke-static%s {%s}, %s(%s)%s' % (
                    ind, rng or '', regs, target, ''.join(stub_args), ret))
                counts[cls + '->' + member] = counts.get(cls + '->' + member, 0) + 1
                changed = True
                continue
        m = FIELD.match(line)
        if m:
            ind, sk, op, typ, ra, rb, cls, name, ftype = m.groups()
            member = '%s:%s' % (name, ftype)
            if resolve(classes, cls, member) is False:
                label = '%s->%s' % (cls, member)
                a = int(ra.strip()[1:])
                if sk == 'i':
                    b = int(rb.strip()[1:])
                    if op == 'get':
                        stub = stubs.get(label + ' get', cls, ftype)
                        out.append('%sinvoke-static {v%d}, Larcwasm/htc/Missing;->%s(%s)%s' % (
                            ind, b, stub, cls, ftype))
                        out.append('%smove-result%s v%d' % (ind, suffix(ftype), a))
                    elif a + size(ftype) - 1 > 15:
                        out.append(line)  # no 4-bit register for its pair: left as is
                        continue
                    else:
                        stub = stubs.get(label + ' put', ftype + cls, 'V')
                        src = ['v%d' % a] + (['v%d' % (a + 1)] if size(ftype) == 2 else [])
                        out.append('%sinvoke-static {%s}, Larcwasm/htc/Missing;->%s(%s%s)V' % (
                            ind, ', '.join(src + ['v%d' % b]), stub, ftype, cls))
                else:
                    if op == 'get':
                        stub = stubs.get(label + ' get', '', ftype)
                        out.append('%sinvoke-static {}, Larcwasm/htc/Missing;->%s()%s' % (
                            ind, stub, ftype))
                        out.append('%smove-result%s v%d' % (ind, suffix(ftype), a))
                    else:
                        stub = stubs.get(label + ' put', ftype, 'V')
                        out.append('%sinvoke-static/range {v%d .. v%d}, Larcwasm/htc/Missing;->%s(%s)V' % (
                            ind, a, a + size(ftype) - 1, stub, ftype))
                counts[label] = counts.get(label, 0) + 1
                changed = True
                continue
        out.append(line)
    if changed:
        open(path, 'w').write('\n'.join(out))


def main(argv):
    out_dir = argv[1]
    ours, fix, lost_files, cur = [], [], [], None
    for a in argv[2:]:
        if a in ('--ours', '--fix', '--lost'):
            cur = {'--ours': ours, '--fix': fix, '--lost': lost_files}[a]
        else:
            cur.append(a)
    lost = set()
    for f in lost_files:
        lost |= set(open(f).read().split())
    classes = index(ours)
    classes.update(index(fix))
    members = classes.get('Larcwasm/htc/Members;', (None, [], set()))[2]
    stubs = Stubs()
    counts = {}
    for root in fix:
        for d, _, files in os.walk(root):
            for f in files:
                if f.endswith('.smali'):
                    fix_file(os.path.join(d, f), classes, stubs, counts, members, lost)
    os.makedirs(os.path.join(out_dir, 'arcwasm', 'htc'), exist_ok=True)
    with open(os.path.join(out_dir, 'arcwasm', 'htc', 'Missing.smali'), 'w') as f:
        f.write(stubs.smali())
    print('%d uses of %d missing members, %d stubs' % (
        sum(counts.values()), len(counts), len(stubs.stubs)))
    for m in sorted(counts):
        print('  missing: %s (%d)' % (m, counts[m]))


if __name__ == '__main__':
    main(sys.argv)
