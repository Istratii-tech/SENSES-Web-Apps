#!/usr/bin/env python3
"""Generates the differential test cases of the AArch64 CPU (topic ndk-arm64).

Writes cases.S (one function per case: the instructions, then ret) and
cases.c (the table of tests/ndk64/cpu/diff/diff.h). Every template below is
instantiated several times with random registers, immediates, conditions,
shift amounts and lane indices; the same files are built natively on the
Mac (reference results) and for the guest.

  gen_a64_cases.py OUT_DIR [--seed N] [--per K]

Template placeholders (registers are distinct within a case, from x0-x17
and x19-x27):
  {Xd} {Xd2} {Wd} {Wd2}      destinations (compared afterwards)
  {Xn} {Xm} {Xa} {Wn} ...    sources
  {Xb}                       a base register pointing into the scratch area
  {Xi} {Wi}                  an index register (small values)
  {Xe} {Xf}                  even/odd register pair (x{e}, x{e+1}) for CASP/LDXP
  {Vd} {Vd2} {Vn} {Vm} {Va}  V register numbers ({Vd+1} etc. for lists)
  other {name}               immediates from GENERATORS
"""
import random
import re
import sys

# --- immediates --------------------------------------------------------------------------------
def bitmask_imms(is64):
    """All valid logical immediates of 32 or 64 bits, as (N, immr, imms, value)."""
    out = []
    for n in ((0, 1) if is64 else (0,)):
        for imms in range(64):
            for immr in range(64):
                combined = (n << 6) | (~imms & 0x3f)
                if combined == 0:
                    continue
                ln = combined.bit_length() - 1
                if ln < 1 or (not is64 and ln > 5):
                    continue
                esize = 1 << ln
                levels = esize - 1
                s, r = imms & levels, immr & levels
                if s == levels:
                    continue
                if immr > levels:
                    continue
                welem = (1 << (s + 1)) - 1
                welem = ((welem >> r) | (welem << (esize - r))) & ((1 << esize) - 1)
                v = 0
                for i in range(0, 64 if is64 else 32, esize):
                    v |= welem << i
                out.append(v)
    return sorted(set(out))

LOGIMM64 = bitmask_imms(True)
LOGIMM32 = bitmask_imms(False)

FIMMS = ["1.0", "-1.0", "0.5", "2.0", "-2.5", "31.0", "0.125", "-0.1875", "10.0", "1.9375", "-15.5", "3.0"]
CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al"]

GENERATORS = {
    "imm12": lambda r: str(r.choice([0, 1, 7, 0xfff, 0x800, r.randrange(4096)])),
    "imm16": lambda r: hex(r.choice([0, 1, 0xffff, 0x8000, r.randrange(65536)])),
    "imm3": lambda r: str(r.randrange(5)),
    "sh64": lambda r: str(r.choice([0, 1, 31, 32, 63, r.randrange(64)])),
    "sh32": lambda r: str(r.choice([0, 1, 16, 31, r.randrange(32)])),
    "r6": lambda r: str(r.randrange(64)),
    "s6": lambda r: str(r.randrange(64)),
    "r5": lambda r: str(r.randrange(32)),
    "s5": lambda r: str(r.randrange(32)),
    "bit6": lambda r: str(r.randrange(64)),
    "bit5": lambda r: str(r.randrange(32)),
    "hw64": lambda r: str(r.choice([0, 16, 32, 48])),
    "hw32": lambda r: str(r.choice([0, 16])),
    "cond": lambda r: r.choice(CONDS),
    "condna": lambda r: r.choice(CONDS[:-1]),
    "nzcv": lambda r: str(r.randrange(16)),
    "imm5": lambda r: str(r.randrange(32)),
    "shift": lambda r: r.choice(["lsl", "lsr", "asr"]),
    "shiftr": lambda r: r.choice(["lsl", "lsr", "asr", "ror"]),
    "ext": lambda r: r.choice(["uxtb", "uxth", "uxtw", "sxtb", "sxth", "sxtw"]),
    "logimm64": lambda r: hex(r.choice(LOGIMM64)),
    "logimm32": lambda r: hex(r.choice(LOGIMM32)),
    "fimm": lambda r: r.choice(FIMMS),
    "fbits32": lambda r: str(r.choice([1, 8, 16, 31, 32, r.randrange(1, 33)])),
    "fbits64": lambda r: str(r.choice([1, 16, 32, 52, 64, r.randrange(1, 65)])),
    "imm8": lambda r: hex(r.randrange(256)),
    "lsl32": lambda r: str(r.choice([0, 8, 16, 24])),
    "lsl16": lambda r: str(r.choice([0, 8])),
    "msl": lambda r: str(r.choice([8, 16])),
    "bytemask": lambda r: hex(sum(0xff << (8 * i) for i in range(8) if r.random() < 0.5)),
    "idxb": lambda r: str(r.randrange(16)), "idxb8": lambda r: str(r.randrange(8)),
    "idxh": lambda r: str(r.randrange(8)), "idxh4": lambda r: str(r.randrange(4)),
    "idxs": lambda r: str(r.randrange(4)), "idxs2": lambda r: str(r.randrange(2)),
    "idxd": lambda r: str(r.randrange(2)),
    "ext8": lambda r: str(r.randrange(8)), "ext16": lambda r: str(r.randrange(16)),
}
# Shift immediates of the vector shift-by-immediate forms, by element size.
for b in (8, 16, 32, 64):
    GENERATORS["rsh%d" % b] = (lambda bb: lambda r: str(r.choice([1, bb, r.randrange(1, bb + 1)])))(b)
    GENERATORS["lsh%d" % b] = (lambda bb: lambda r: str(r.choice([0, bb - 1, r.randrange(bb)])))(b)

# --- templates ---------------------------------------------------------------------------------
# (asm, class, flags, options) -- class: int f32 f64 f16 (+"r" = random FPCR); flags: f nzcv, q QC, m memory.
T = []

def t(asm, cls="int", flags="", per=None, setup=None, wb=False):
    T.append({"asm": asm, "cls": cls, "flags": flags, "per": per, "setup": setup or [], "wb": wb})

# ---- integer data processing ----
for op in ("add", "sub"):
    for s in ("", "s"):
        f = "f" if s else ""
        t("%s%s {Xd}, {Xn}, #{imm12}" % (op, s), flags=f)
        t("%s%s {Xd}, {Xn}, #{imm12}, lsl #12" % (op, s), flags=f)
        t("%s%s {Wd}, {Wn}, #{imm12}" % (op, s), flags=f)
        t("%s%s {Xd}, {Xn}, {Xm}, {shift} #{sh64}" % (op, s), flags=f, per=10)
        t("%s%s {Wd}, {Wn}, {Wm}, {shift} #{sh32}" % (op, s), flags=f, per=10)
        t("%s%s {Xd}, {Xn}, {Wm}, {ext} #{imm3}" % (op, s), flags=f, per=10)
        t("%s%s {Xd}, {Xn}, {Xm}, sxtx #{imm3}" % (op, s), flags=f)
        t("%s%s {Xd}, {Xn}, {Xm}, uxtx #{imm3}" % (op, s), flags=f)
        t("%s%s {Wd}, {Wn}, {Wm}, {ext} #{imm3}" % (op, s), flags=f, per=10)
    for c in ("", "s"):
        t("%sc%s {Xd}, {Xn}, {Xm}" % ("ad" if op == "add" else "sb", c), flags="f" if c else "", per=10)
        t("%sc%s {Wd}, {Wn}, {Wm}" % ("ad" if op == "add" else "sb", c), flags="f" if c else "", per=10)
t("cmp {Xn}, {Xm}", flags="f", per=10)
t("cmn {Wn}, #{imm12}", flags="f")
t("neg {Xd}, {Xm}, lsl #{sh64}")
t("negs {Wd}, {Wm}", flags="f")
t("ngc {Xd}, {Xm}")
t("ngcs {Wd}, {Wm}", flags="f")
for op in ("and", "orr", "eor", "ands"):
    t("%s {Xd}, {Xn}, #{logimm64}" % op, flags="f" if op == "ands" else "", per=10)
    t("%s {Wd}, {Wn}, #{logimm32}" % op, flags="f" if op == "ands" else "", per=10)
t("tst {Xn}, #{logimm64}", flags="f")
t("mov {Xd}, #{logimm64}")
for op in ("and", "bic", "orr", "orn", "eor", "eon", "ands", "bics"):
    fl = "f" if op in ("ands", "bics") else ""
    t("%s {Xd}, {Xn}, {Xm}, {shiftr} #{sh64}" % op, flags=fl, per=8)
    t("%s {Wd}, {Wn}, {Wm}, {shiftr} #{sh32}" % op, flags=fl, per=8)
t("mov {Xd}, {Xm}")
t("mvn {Wd}, {Wm}")
for op in ("movz", "movn"):
    t("%s {Xd}, #{imm16}, lsl #{hw64}" % op)
    t("%s {Wd}, #{imm16}, lsl #{hw32}" % op)
t("movk {Xd}, #{imm16}, lsl #{hw64}", per=8)
t("movk {Wd}, #{imm16}, lsl #{hw32}", per=8)
for op in ("sbfm", "bfm", "ubfm"):
    t("%s {Xd}, {Xn}, #{r6}, #{s6}" % op, per=20)
    t("%s {Wd}, {Wn}, #{r5}, #{s5}" % op, per=20)
t("lsl {Xd}, {Xn}, #{sh64}"); t("lsr {Wd}, {Wn}, #{sh32}"); t("asr {Xd}, {Xn}, #{sh64}")
for op in ("sxtb", "sxth", "uxtb", "uxth"):
    t("%s {Wd}, {Wn}" % op)
t("sxtb {Xd}, {Wn}"); t("sxth {Xd}, {Wn}"); t("sxtw {Xd}, {Wn}")
t("extr {Xd}, {Xn}, {Xm}, #{sh64}", per=10)
t("extr {Wd}, {Wn}, {Wm}, #{sh32}", per=10)
t("ror {Xd}, {Xn}, #{sh64}")
for op in ("lslv", "lsrv", "asrv", "rorv", "udiv", "sdiv"):
    t("%s {Xd}, {Xn}, {Xm}" % op, per=12)
    t("%s {Wd}, {Wn}, {Wm}" % op, per=12)
for op in ("rbit", "rev16", "rev", "clz", "cls"):
    t("%s {Xd}, {Xn}" % op, per=8)
    t("%s {Wd}, {Wn}" % op, per=8)
t("rev32 {Xd}, {Xn}")
for op in ("madd", "msub"):
    t("%s {Xd}, {Xn}, {Xm}, {Xa}" % op, per=8)
    t("%s {Wd}, {Wn}, {Wm}, {Wa}" % op, per=8)
for op in ("smaddl", "smsubl", "umaddl", "umsubl"):
    t("%s {Xd}, {Wn}, {Wm}, {Xa}" % op, per=8)
t("smulh {Xd}, {Xn}, {Xm}", per=12); t("umulh {Xd}, {Xn}, {Xm}", per=12)
t("mul {Wd}, {Wn}, {Wm}"); t("mneg {Xd}, {Xn}, {Xm}"); t("smull {Xd}, {Wn}, {Wm}"); t("umnegl {Xd}, {Wn}, {Wm}")
for op in ("crc32b", "crc32h", "crc32w", "crc32cb", "crc32ch", "crc32cw"):
    t("%s {Wd}, {Wn}, {Wm}" % op)
t("crc32x {Wd}, {Wn}, {Xm}"); t("crc32cx {Wd}, {Wn}, {Xm}")
for op in ("csel", "csinc", "csinv", "csneg"):
    t("%s {Xd}, {Xn}, {Xm}, {cond}" % op, per=10)
    t("%s {Wd}, {Wn}, {Wm}, {cond}" % op, per=10)
t("cset {Xd}, {condna}", per=10); t("csetm {Wd}, {condna}", per=8); t("cinc {Xd}, {Xn}, {condna}")
t("cneg {Wd}, {Wn}, {condna}")
for op in ("ccmp", "ccmn"):
    t("%s {Xn}, {Xm}, #{nzcv}, {cond}" % op, flags="f", per=12)
    t("%s {Wn}, #{imm5}, #{nzcv}, {cond}" % op, flags="f", per=12)
# branches
t("b.{cond} 1f; mov {Xd}, #1; ret; 1: mov {Xd}, #2", per=16)
t("cbz {Xn}, 1f; mov {Xd}, #1; ret; 1: mov {Xd}, #2", per=6)
t("cbnz {Wn}, 1f; mov {Xd}, #1; ret; 1: mov {Xd}, #2", per=6)
t("tbz {Xn}, #{bit6}, 1f; mov {Xd}, #1; ret; 1: mov {Xd}, #2", per=8)
t("tbnz {Wn}, #{bit5}, 1f; mov {Xd}, #1; ret; 1: mov {Xd}, #2", per=8)
t("adr {Xd}, 1f; adr {Xa}, 2f; sub {Xd}, {Xa}, {Xd}; mov {Xa}, #0; ret; 1: nop; nop; 2: nop")
t("mrs {Xd}, nzcv", per=4)
t("msr nzcv, {Xn}; cset {Xd}, {condna}", per=8)
t("and {Xn}, {Xn}, #0x3c00000; msr fpcr, {Xn}; mrs {Xd}, fpcr", per=4)

# ---- loads and stores ----
def ldst(asm, wb=False, flags="m", per=6, off=0):
    t(asm, setup=[("Xb", "addr", off)], flags=flags, per=per, wb=wb)

for op, sz in (("ldr {Xd}", 8), ("ldr {Wd}", 4), ("ldrh {Wd}", 2), ("ldrb {Wd}", 1), ("ldrsb {Wd}", 1),
               ("ldrsb {Xd}", 1), ("ldrsh {Wd}", 2), ("ldrsh {Xd}", 2), ("ldrsw {Xd}", 4)):
    ldst("%s, [{Xb}, #%d]" % (op, sz * 3), flags="")
    ldst("%s, [{Xb}, #-7]" % op.replace("ldr", "ldur"), flags="", off=64)
    ldst("%s, [{Xb}, #%d]!" % (op, sz * 2), wb=True, flags="", off=32)
    ldst("%s, [{Xb}], #-%d" % (op, sz), wb=True, flags="", off=32)
    t("%s, [{Xb}, {Xi}, lsl #%d]" % (op, {1: 0, 2: 1, 4: 2, 8: 3}[sz]),
      setup=[("Xb", "addr", 0), ("Xi", "mask", 0xf)])
    t("%s, [{Xb}, {Wi}, sxtw]" % op, setup=[("Xb", "addr", 64), ("Wi", "imm", -5)])
    t("%s, [{Xb}, {Wi}, uxtw]" % op, setup=[("Xb", "addr", 64), ("Wi", "mask", 0x1f)])
for op, sz in (("str {Xn}", 8), ("str {Wn}", 4), ("strh {Wn}", 2), ("strb {Wn}", 1)):
    ldst("%s, [{Xb}, #%d]" % (op, sz * 5))
    ldst("%s, [{Xb}, #13]" % op.replace("str", "stur"), off=64)
    ldst("%s, [{Xb}, #-%d]!" % (op, sz * 2), wb=True, off=64)
    ldst("%s, [{Xb}], #%d" % (op, sz * 3), wb=True, off=64)
    t("%s, [{Xb}, {Xi}]" % op, setup=[("Xb", "addr", 0), ("Xi", "mask", 0x7f)], flags="m")
ldst("ldr {Xd}, [{Xb}, #0x1000]", flags="", off=-4096 + 16)
ldst("ldp {Xd}, {Xd2}, [{Xb}, #16]", flags="")
ldst("ldp {Wd}, {Wd2}, [{Xb}, #-8]", flags="", off=64)
ldst("ldp {Xd}, {Xd2}, [{Xb}, #-16]!", wb=True, flags="", off=64)
ldst("ldp {Xd}, {Xd2}, [{Xb}], #32", wb=True, flags="", off=64)
ldst("ldpsw {Xd}, {Xd2}, [{Xb}, #8]", flags="")
ldst("ldnp {Xd}, {Xd2}, [{Xb}, #32]", flags="")
ldst("stp {Xn}, {Xm}, [{Xb}, #24]")
ldst("stp {Wn}, {Wm}, [{Xb}, #-4]!", wb=True, off=64)
ldst("stp {Xn}, {Xm}, [{Xb}], #-48", wb=True, off=128)
ldst("stnp {Wn}, {Wm}, [{Xb}, #12]")
ldst("stp xzr, {Xn}, [{Xb}, #8]")
for r, sz in (("b", 1), ("h", 2), ("s", 4), ("d", 8), ("q", 16)):
    ldst("ldr %s{Vd}, [{Xb}, #%d]" % (r, sz * 2), flags="")
    ldst("ldur %s{Vd}, [{Xb}, #-3]" % r, flags="", off=64)
    ldst("ldr %s{Vd}, [{Xb}, #%d]!" % (r, sz), wb=True, flags="", off=32)
    ldst("ldr %s{Vd}, [{Xb}], #%d" % (r, sz), wb=True, flags="", off=32)
    ldst("str %s{Vn}, [{Xb}, #%d]" % (r, sz * 3))
    ldst("str %s{Vn}, [{Xb}, #-%d]!" % (r, sz), wb=True, off=64)
    t("ldr %s{Vd}, [{Xb}, {Xi}, lsl #%d]" % (r, {1: 0, 2: 1, 4: 2, 8: 3, 16: 4}[sz]),
      setup=[("Xb", "addr", 0), ("Xi", "mask", 0x7)])
for r, sz in (("s", 4), ("d", 8), ("q", 16)):
    ldst("ldp %s{Vd}, %s{Vd2}, [{Xb}, #%d]" % (r, r, sz * 2), flags="")
    ldst("ldp %s{Vd}, %s{Vd2}, [{Xb}, #-%d]!" % (r, r, sz * 2), wb=True, flags="", off=96)
    ldst("stp %s{Vn}, %s{Vm}, [{Xb}], #%d" % (r, r, sz), wb=True, off=32)
# structures
for arr, n in (("16b", 16), ("8b", 8), ("8h", 16), ("4h", 8), ("4s", 16), ("2s", 8), ("2d", 16), ("1d", 8)):
    ldst("ld1 {{v{Vd}.%s}}, [{Xb}]" % arr, flags="")
    ldst("ld1 {{v{Vd}.%s, v{Vd+1}.%s}}, [{Xb}], #%d" % (arr, arr, 2 * n), wb=True, flags="")
    ldst("ld1 {{v{Vd}.%s, v{Vd+1}.%s, v{Vd+2}.%s}}, [{Xb}]" % (arr, arr, arr), flags="")
    ldst("ld1 {{v{Vd}.%s, v{Vd+1}.%s, v{Vd+2}.%s, v{Vd+3}.%s}}, [{Xb}], {Xi}" % (arr, arr, arr, arr), wb=True, flags="",
         per=4)
    ldst("st1 {{v{Vn}.%s, v{Vn+1}.%s}}, [{Xb}]" % (arr, arr))
    if arr != "1d":
        ldst("ld2 {{v{Vd}.%s, v{Vd+1}.%s}}, [{Xb}]" % (arr, arr), flags="")
        ldst("ld3 {{v{Vd}.%s, v{Vd+1}.%s, v{Vd+2}.%s}}, [{Xb}], #%d" % (arr, arr, arr, 3 * n), wb=True, flags="")
        ldst("ld4 {{v{Vd}.%s, v{Vd+1}.%s, v{Vd+2}.%s, v{Vd+3}.%s}}, [{Xb}]" % (arr, arr, arr, arr), flags="")
        ldst("st2 {{v{Vn}.%s, v{Vn+1}.%s}}, [{Xb}], #%d" % (arr, arr, 2 * n), wb=True)
        ldst("st3 {{v{Vn}.%s, v{Vn+1}.%s, v{Vn+2}.%s}}, [{Xb}]" % (arr, arr, arr))
        ldst("st4 {{v{Vn}.%s, v{Vn+1}.%s, v{Vn+2}.%s, v{Vn+3}.%s}}, [{Xb}]" % (arr, arr, arr, arr))
    ldst("ld1r {{v{Vd}.%s}}, [{Xb}]" % arr, flags="")
    ldst("ld2r {{v{Vd}.%s, v{Vd+1}.%s}}, [{Xb}], #%d" % (arr, arr, 2 * {"16b": 1, "8b": 1, "8h": 2, "4h": 2, "4s": 4,
                                                                       "2s": 4, "2d": 8, "1d": 8}[arr]), wb=True, flags="")
    ldst("ld4r {{v{Vd}.%s, v{Vd+1}.%s, v{Vd+2}.%s, v{Vd+3}.%s}}, [{Xb}]" % (arr, arr, arr, arr), flags="")
for e, idx, sz in (("b", "idxb", 1), ("h", "idxh", 2), ("s", "idxs", 4), ("d", "idxd", 8)):
    ldst("ld1 {{v{Vd}.%s}}[{%s}], [{Xb}]" % (e, idx), flags="")
    ldst("ld2 {{v{Vd}.%s, v{Vd+1}.%s}}[{%s}], [{Xb}], #%d" % (e, e, idx, 2 * sz), wb=True, flags="")
    ldst("ld3 {{v{Vd}.%s, v{Vd+1}.%s, v{Vd+2}.%s}}[{%s}], [{Xb}]" % (e, e, e, idx), flags="")
    ldst("ld4 {{v{Vd}.%s, v{Vd+1}.%s, v{Vd+2}.%s, v{Vd+3}.%s}}[{%s}], [{Xb}], {Xi}" % (e, e, e, e, idx), wb=True,
         flags="", per=3)
    ldst("st1 {{v{Vn}.%s}}[{%s}], [{Xb}]" % (e, idx))
    ldst("st2 {{v{Vn}.%s, v{Vn+1}.%s}}[{%s}], [{Xb}]" % (e, e, idx))
    ldst("st4 {{v{Vn}.%s, v{Vn+1}.%s, v{Vn+2}.%s, v{Vn+3}.%s}}[{%s}], [{Xb}], #%d" % (e, e, e, e, idx, 4 * sz),
         wb=True)
# exclusives, ordered, atomics (aligned addresses)
for sfx, reg, sz in (("b", "W", 1), ("h", "W", 2), ("", "W", 4), ("", "X", 8)):
    ldst("ldxr%s {%sd}, [{Xb}]" % (sfx, reg), flags="", off=8 * 3)
    ldst("ldaxr%s {%sd}, [{Xb}]" % (sfx, reg), flags="", off=8 * 4)
    ldst("ldar%s {%sd}, [{Xb}]" % (sfx, reg), flags="", off=8 * 5)
    ldst("ldapr%s {%sd}, [{Xb}]" % (sfx, reg), flags="", off=8 * 5)
    ldst("stlr%s {%sn}, [{Xb}]" % (sfx, reg), off=8 * 6)
    ldst("ldxr%s {%sd}, [{Xb}]; stxr%s {Wd2}, {%sn}, [{Xb}]" % (sfx, reg, sfx, reg), off=8 * 7)
    ldst("ldaxr%s {%sd}, [{Xb}]; stlxr%s {Wd2}, {%sn}, [{Xb}]" % (sfx, reg, sfx, reg), off=8 * 8)
    for a in ("ldadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldsmin", "ldumax", "ldumin", "swp"):
        for order in ("", "al"):
            ldst("%s%s%s {%sn}, {%sd}, [{Xb}]" % (a, order, sfx, reg, reg), off=8 * 9)
    ldst("cas%s {%sm}, {%sn}, [{Xb}]; mov {Xd}, {Xm}" % (sfx, reg, reg), off=8 * 10)
    ldst("ldr%s {%sm}, [{Xb}]; casal%s {%sm}, {%sn}, [{Xb}]; mov {Xd}, {Xm}" %
         ("b" if sz == 1 else "h" if sz == 2 else "", reg, sfx, reg, reg), off=8 * 10)
ldst("ldxp {Xd}, {Xd2}, [{Xb}]", flags="", off=16)
ldst("ldaxp {Wd}, {Wd2}, [{Xb}]", flags="", off=16)
ldst("ldxp {Xd}, {Xd2}, [{Xb}]; stxp {Wa}, {Xn}, {Xm}, [{Xb}]; mov {Xd}, {Xa}", off=32)
ldst("ldaxp {Wd}, {Wd2}, [{Xb}]; stlxp {Wa}, {Wn}, {Wm}, [{Xb}]; mov {Xd}, {Xa}", off=48)
ldst("casp {Xe}, {Xe+1}, {Xf}, {Xf+1}, [{Xb}]", off=64)
ldst("ldp {Xe}, {Xe+1}, [{Xb}]; caspal {Xe}, {Xe+1}, {Xf}, {Xf+1}, [{Xb}]", off=80)
ldst("casp {We}, {We+1}, {Wf}, {Wf+1}, [{Xb}]", off=96)
ldst("dc zva, {Xb}", off=64)
ldst("prfm pldl1keep, [{Xb}, #64]", flags="")
# ---- scalar floating point ----
for p, cls in (("s", "f32"), ("d", "f64")):
    P = p.upper()
    for op in ("fadd", "fsub", "fmul", "fdiv", "fmax", "fmin", "fmaxnm", "fminnm", "fnmul"):
        t("%s %s{Vd}, %s{Vn}, %s{Vm}" % (op, p, p, p), cls=cls, per=14)
    for op in ("fmadd", "fmsub", "fnmadd", "fnmsub"):
        t("%s %s{Vd}, %s{Vn}, %s{Vm}, %s{Va}" % (op, p, p, p, p), cls=cls, per=14)
    for op in ("fabs", "fneg", "fsqrt", "fmov", "frintn", "frintp", "frintm", "frintz", "frinta"):
        t("%s %s{Vd}, %s{Vn}" % (op, p, p), cls=cls, per=10)
    for op in ("frintx", "frinti"):
        t("%s %s{Vd}, %s{Vn}" % (op, p, p), cls=cls + "r", per=12)
    t("fcmp %s{Vn}, %s{Vm}" % (p, p), cls=cls, flags="f", per=14)
    t("fcmpe %s{Vn}, #0.0" % p, cls=cls, flags="f", per=8)
    t("fccmp %s{Vn}, %s{Vm}, #{nzcv}, {cond}" % (p, p), cls=cls, flags="f", per=12)
    t("fccmpe %s{Vn}, %s{Vm}, #{nzcv}, {cond}" % (p, p), cls=cls, flags="f", per=6)
    t("fcsel %s{Vd}, %s{Vn}, %s{Vm}, {cond}" % (p, p, p), cls=cls, per=8)
    t("fmov %s{Vd}, #{fimm}" % p, cls=cls, per=8)
    for r in ("W", "X"):
        for op in ("scvtf", "ucvtf"):
            t("%s %s{Vd}, {%sn}" % (op, p, r), cls=cls, per=10)
            t("%s %s{Vd}, {%sn}, #{fbits%s}" % (op, p, r, "32" if r == "W" else "64"), cls=cls, per=8)
        for op in ("fcvtzs", "fcvtzu", "fcvtns", "fcvtnu", "fcvtms", "fcvtmu", "fcvtps", "fcvtpu", "fcvtas", "fcvtau"):
            t("%s {%sd}, %s{Vn}" % (op, r, p), cls=cls, per=10)
        for op in ("fcvtzs", "fcvtzu"):
            t("%s {%sd}, %s{Vn}, #{fbits%s}" % (op, r, p, "32" if r == "W" else "64"), cls=cls, per=8)
t("fcvt d{Vd}, s{Vn}", cls="f32", per=12)
t("fcvt s{Vd}, d{Vn}", cls="f64", per=12)
t("fcvt h{Vd}, s{Vn}", cls="f32", per=12)
t("fcvt h{Vd}, d{Vn}", cls="f64", per=12)
t("fcvt s{Vd}, h{Vn}", cls="f16", per=10)
t("fcvt d{Vd}, h{Vn}", cls="f16", per=10)
t("fmov {Wd}, s{Vn}", cls="f32"); t("fmov {Xd}, d{Vn}", cls="f64"); t("fmov s{Vd}, {Wn}")
t("fmov d{Vd}, {Xn}"); t("fmov {Xd}, v{Vn}.d[1]", cls="f64"); t("fmov v{Vd}.d[1], {Xn}", cls="f64")
t("fmov d{Vd}, d{Vn}", cls="f64")

# ---- Advanced SIMD ----
INT_ARR = ["8b", "16b", "4h", "8h", "2s", "4s"]
INT_ARR_D = INT_ARR + ["2d"]
def vbin(op, arrs, cls="int", flags="", per=4):
    for a in arrs:
        t("%s v{Vd}.%s, v{Vn}.%s, v{Vm}.%s" % (op, a, a, a), cls=cls, flags=flags, per=per)

for op in ("add", "sub", "cmgt", "cmhi", "cmge", "cmhs", "cmeq", "cmtst", "sshl", "ushl", "srshl", "urshl"):
    vbin(op, INT_ARR_D)
for op in ("sqadd", "uqadd", "sqsub", "uqsub", "sqshl", "uqshl", "sqrshl", "uqrshl"):
    vbin(op, INT_ARR_D, flags="q")
for op in ("mul", "mla", "mls", "shadd", "uhadd", "srhadd", "urhadd", "shsub", "uhsub", "smax", "umax", "smin",
           "umin", "sabd", "uabd", "saba", "uaba", "addp", "smaxp", "umaxp", "sminp", "uminp"):
    vbin(op, INT_ARR)
vbin("addp", ["2d"])
vbin("pmul", ["8b", "16b"])
vbin("sqdmulh", ["4h", "8h", "2s", "4s"], flags="q")
vbin("sqrdmulh", ["4h", "8h", "2s", "4s"], flags="q")
for op in ("and", "bic", "orr", "orn", "eor", "bsl", "bit", "bif"):
    vbin(op, ["8b", "16b"], per=3)
FP_ARR = [("2s", "f32"), ("4s", "f32"), ("2d", "f64")]
for op in ("fadd", "fsub", "fmul", "fdiv", "fmax", "fmin", "fmaxnm", "fminnm", "fmla", "fmls", "fmulx", "fabd",
           "fcmeq", "fcmge", "fcmgt", "facge", "facgt", "frecps", "frsqrts", "faddp", "fmaxp", "fminp", "fmaxnmp",
           "fminnmp"):
    for a, cls in FP_ARR:
        t("%s v{Vd}.%s, v{Vn}.%s, v{Vm}.%s" % (op, a, a, a), cls=cls, per=5)
# three different
LONG = [("8h", "8b", "16b"), ("4s", "4h", "8h"), ("2d", "2s", "4s")]
for op in ("saddl", "uaddl", "ssubl", "usubl", "sabal", "uabal", "sabdl", "uabdl", "smlal", "umlal", "smlsl", "umlsl",
           "smull", "umull"):
    for w, n, n2 in LONG:
        t("%s v{Vd}.%s, v{Vn}.%s, v{Vm}.%s" % (op, w, n, n), per=3)
        t("%s2 v{Vd}.%s, v{Vn}.%s, v{Vm}.%s" % (op, w, n2, n2), per=3)
for op in ("saddw", "uaddw", "ssubw", "usubw"):
    for w, n, n2 in LONG:
        t("%s v{Vd}.%s, v{Vn}.%s, v{Vm}.%s" % (op, w, w, n), per=3)
        t("%s2 v{Vd}.%s, v{Vn}.%s, v{Vm}.%s" % (op, w, w, n2), per=3)
for op in ("addhn", "raddhn", "subhn", "rsubhn"):
    for w, n, n2 in LONG:
        t("%s v{Vd}.%s, v{Vn}.%s, v{Vm}.%s" % (op, n, w, w), per=3)
        t("%s2 v{Vd}.%s, v{Vn}.%s, v{Vm}.%s" % (op, n2, w, w), per=3)
for op in ("sqdmull", "sqdmlal", "sqdmlsl"):
    for w, n, n2 in LONG[1:]:
        t("%s v{Vd}.%s, v{Vn}.%s, v{Vm}.%s" % (op, w, n, n), flags="q", per=4)
        t("%s2 v{Vd}.%s, v{Vn}.%s, v{Vm}.%s" % (op, w, n2, n2), flags="q", per=4)
t("pmull v{Vd}.8h, v{Vn}.8b, v{Vm}.8b"); t("pmull2 v{Vd}.8h, v{Vn}.16b, v{Vm}.16b")
# two-register misc
def vun(op, arrs, cls="int", flags="", per=4, src=None):
    for a in arrs:
        t("%s v{Vd}.%s, v{Vn}.%s" % (op, a, src or a), cls=cls, flags=flags, per=per)
vun("rev64", ["8b", "16b", "4h", "8h", "2s", "4s"])
vun("rev32", ["8b", "16b", "4h", "8h"])
vun("rev16", ["8b", "16b"])
for op in ("cls", "clz"):
    vun(op, INT_ARR)
vun("cnt", ["8b", "16b"]); vun("not", ["8b", "16b"]); vun("rbit", ["8b", "16b"])
for op in ("abs", "neg", "cmgt", "cmge", "cmeq", "cmle", "cmlt"):
    for a in INT_ARR_D:
        if op.startswith("cm"):
            t("%s v{Vd}.%s, v{Vn}.%s, #0" % (op, a, a), per=3)
        else:
            t("%s v{Vd}.%s, v{Vn}.%s" % (op, a, a), per=3)
for op in ("sqabs", "sqneg", "suqadd", "usqadd"):
    vun(op, INT_ARR_D, flags="q")
for op in ("saddlp", "uaddlp", "sadalp", "uadalp"):
    for w, n in (("4h", "8b"), ("8h", "16b"), ("2s", "4h"), ("4s", "8h"), ("1d", "2s"), ("2d", "4s")):
        t("%s v{Vd}.%s, v{Vn}.%s" % (op, w, n), per=3)
for op, fl in (("xtn", ""), ("sqxtn", "q"), ("uqxtn", "q"), ("sqxtun", "q")):
    for w, n, n2 in LONG:
        t("%s v{Vd}.%s, v{Vn}.%s" % (op, n, w), flags=fl, per=4)
        t("%s2 v{Vd}.%s, v{Vn}.%s" % (op, n2, w), flags=fl, per=3)
for w, n, n2 in LONG:
    t("shll v{Vd}.%s, v{Vn}.%s, #%d" % (w, n, {"8b": 8, "4h": 16, "2s": 32}[n]))
    t("shll2 v{Vd}.%s, v{Vn}.%s, #%d" % (w, n2, {"8b": 8, "4h": 16, "2s": 32}[n]))
t("fcvtn v{Vd}.2s, v{Vn}.2d", cls="f64", per=8); t("fcvtn2 v{Vd}.4s, v{Vn}.2d", cls="f64", per=4)
t("fcvtn v{Vd}.4h, v{Vn}.4s", cls="f32", per=8); t("fcvtn2 v{Vd}.8h, v{Vn}.4s", cls="f32", per=4)
t("fcvtxn v{Vd}.2s, v{Vn}.2d", cls="f64", per=8); t("fcvtxn s{Vd}, d{Vn}", cls="f64", per=6)
t("fcvtl v{Vd}.2d, v{Vn}.2s", cls="f32", per=8); t("fcvtl2 v{Vd}.2d, v{Vn}.4s", cls="f32", per=4)
t("fcvtl v{Vd}.4s, v{Vn}.4h", cls="f16", per=8); t("fcvtl2 v{Vd}.4s, v{Vn}.8h", cls="f16", per=4)
for op in ("frintn", "frintp", "frintm", "frintz", "frinta", "fcvtns", "fcvtnu", "fcvtms", "fcvtmu", "fcvtas",
           "fcvtau", "fcvtps", "fcvtpu", "fcvtzs", "fcvtzu", "scvtf", "ucvtf", "fabs", "fneg", "fsqrt", "frecpe",
           "frsqrte"):
    for a, cls in FP_ARR:
        t("%s v{Vd}.%s, v{Vn}.%s" % (op, a, a), cls=cls if not op.endswith("cvtf") else "int", per=4)
for op in ("frintx", "frinti"):
    for a, cls in FP_ARR:
        t("%s v{Vd}.%s, v{Vn}.%s" % (op, a, a), cls=cls + "r", per=4)
for op in ("fcmgt", "fcmge", "fcmeq", "fcmle", "fcmlt"):
    for a, cls in FP_ARR:
        t("%s v{Vd}.%s, v{Vn}.%s, #0.0" % (op, a, a), cls=cls, per=3)
t("urecpe v{Vd}.4s, v{Vn}.4s", per=6); t("ursqrte v{Vd}.2s, v{Vn}.2s", per=6)
# across lanes
for op in ("addv", "smaxv", "umaxv", "sminv", "uminv"):
    for r, a in (("b", "8b"), ("b", "16b"), ("h", "4h"), ("h", "8h"), ("s", "4s")):
        t("%s %s{Vd}, v{Vn}.%s" % (op, r, a), per=3)
for op in ("saddlv", "uaddlv"):
    for r, a in (("h", "8b"), ("h", "16b"), ("s", "4h"), ("s", "8h"), ("d", "4s")):
        t("%s %s{Vd}, v{Vn}.%s" % (op, r, a), per=3)
for op in ("fmaxv", "fminv", "fmaxnmv", "fminnmv"):
    t("%s s{Vd}, v{Vn}.4s" % op, cls="f32", per=8)
# copy
for e, idx, arrs in (("b", "idxb", ("8b", "16b")), ("h", "idxh", ("4h", "8h")), ("s", "idxs", ("2s", "4s")),
                     ("d", "idxd", ("2d",))):
    for a in arrs:
        t("dup v{Vd}.%s, v{Vn}.%s[{%s}]" % (a, e, idx), per=3)
        t("dup v{Vd}.%s, {%sn}" % (a, "X" if e == "d" else "W"), per=3)
    t("ins v{Vd}.%s[{%s}], {%sn}" % (e, idx, "X" if e == "d" else "W"), per=3)
    t("ins v{Vd}.%s[{%s}], v{Vn}.%s[{%s}]" % (e, idx, e, idx), per=3)
    t("dup %s{Vd}, v{Vn}.%s[{%s}]" % (e, e, idx), per=3)
    t("umov {%sd}, v{Vn}.%s[{%s}]" % ("X" if e == "d" else "W", e, idx), per=3)
    if e != "d":
        t("smov {Xd}, v{Vn}.%s[{%s}]" % (e, idx), per=3)
        if e != "s":
            t("smov {Wd}, v{Vn}.%s[{%s}]" % (e, idx), per=3)
# modified immediate
t("movi v{Vd}.16b, #{imm8}"); t("movi v{Vd}.8b, #{imm8}")
t("movi v{Vd}.4s, #{imm8}, lsl #{lsl32}", per=6); t("movi v{Vd}.4h, #{imm8}, lsl #{lsl16}", per=4)
t("mvni v{Vd}.2s, #{imm8}, lsl #{lsl32}", per=6); t("mvni v{Vd}.8h, #{imm8}, lsl #{lsl16}", per=4)
t("movi v{Vd}.4s, #{imm8}, msl #{msl}", per=4); t("mvni v{Vd}.2s, #{imm8}, msl #{msl}", per=4)
t("orr v{Vd}.4s, #{imm8}, lsl #{lsl32}", per=6); t("bic v{Vd}.4h, #{imm8}, lsl #{lsl16}", per=4)
t("bic v{Vd}.2s, #{imm8}, lsl #{lsl32}", per=4); t("orr v{Vd}.8h, #{imm8}, lsl #{lsl16}", per=4)
t("movi v{Vd}.2d, #{bytemask}", per=4); t("movi d{Vd}, #{bytemask}", per=4)
t("fmov v{Vd}.4s, #{fimm}", per=4); t("fmov v{Vd}.2s, #{fimm}", per=3); t("fmov v{Vd}.2d, #{fimm}", per=4)
# shift by immediate
SH_ARR = [("8b", 8), ("16b", 8), ("4h", 16), ("8h", 16), ("2s", 32), ("4s", 32), ("2d", 64)]
for op in ("sshr", "ushr", "ssra", "usra", "srshr", "urshr", "srsra", "ursra", "sri"):
    for a, b in SH_ARR:
        t("%s v{Vd}.%s, v{Vn}.%s, #{rsh%d}" % (op, a, a, b), per=2)
for op, fl in (("shl", ""), ("sli", ""), ("sqshl", "q"), ("uqshl", "q"), ("sqshlu", "q")):
    for a, b in SH_ARR:
        t("%s v{Vd}.%s, v{Vn}.%s, #{lsh%d}" % (op, a, a, b), flags=fl, per=2)
for op, fl in (("shrn", ""), ("rshrn", ""), ("sqshrn", "q"), ("uqshrn", "q"), ("sqrshrn", "q"), ("uqrshrn", "q"),
               ("sqshrun", "q"), ("sqrshrun", "q")):
    for (w, n, n2), b in zip(LONG, (8, 16, 32)):
        t("%s v{Vd}.%s, v{Vn}.%s, #{rsh%d}" % (op, n, w, b), flags=fl, per=2)
        t("%s2 v{Vd}.%s, v{Vn}.%s, #{rsh%d}" % (op, n2, w, b), flags=fl, per=2)
for op in ("sshll", "ushll"):
    for (w, n, n2), b in zip(LONG, (8, 16, 32)):
        t("%s v{Vd}.%s, v{Vn}.%s, #{lsh%d}" % (op, w, n, b), per=2)
        t("%s2 v{Vd}.%s, v{Vn}.%s, #{lsh%d}" % (op, w, n2, b), per=2)
for op in ("scvtf", "ucvtf"):
    t("%s v{Vd}.4s, v{Vn}.4s, #{fbits32}" % op, per=4); t("%s v{Vd}.2d, v{Vn}.2d, #{fbits64}" % op, per=4)
for op in ("fcvtzs", "fcvtzu"):
    t("%s v{Vd}.4s, v{Vn}.4s, #{fbits32}" % op, cls="f32", per=4)
    t("%s v{Vd}.2d, v{Vn}.2d, #{fbits64}" % op, cls="f64", per=4)
# by element
for op in ("mul", "mla", "mls"):
    t("%s v{Vd}.4h, v{Vn}.4h, v{Vml}.h[{idxh}]" % op, per=3); t("%s v{Vd}.8h, v{Vn}.8h, v{Vml}.h[{idxh}]" % op, per=3)
    t("%s v{Vd}.2s, v{Vn}.2s, v{Vm}.s[{idxs}]" % op, per=3); t("%s v{Vd}.4s, v{Vn}.4s, v{Vm}.s[{idxs}]" % op, per=3)
for op, fl in (("smull", ""), ("umull", ""), ("smlal", ""), ("umlal", ""), ("smlsl", ""), ("umlsl", ""),
               ("sqdmull", "q"), ("sqdmlal", "q"), ("sqdmlsl", "q")):
    t("%s v{Vd}.4s, v{Vn}.4h, v{Vml}.h[{idxh}]" % op, flags=fl, per=3)
    t("%s2 v{Vd}.4s, v{Vn}.8h, v{Vml}.h[{idxh}]" % op, flags=fl, per=3)
    t("%s v{Vd}.2d, v{Vn}.2s, v{Vm}.s[{idxs}]" % op, flags=fl, per=3)
    t("%s2 v{Vd}.2d, v{Vn}.4s, v{Vm}.s[{idxs}]" % op, flags=fl, per=3)
for op in ("sqdmulh", "sqrdmulh"):
    t("%s v{Vd}.8h, v{Vn}.8h, v{Vml}.h[{idxh}]" % op, flags="q", per=3)
    t("%s v{Vd}.2s, v{Vn}.2s, v{Vm}.s[{idxs}]" % op, flags="q", per=3)
    t("%s h{Vd}, h{Vn}, v{Vml}.h[{idxh}]" % op, flags="q", per=3)
    t("%s s{Vd}, s{Vn}, v{Vm}.s[{idxs}]" % op, flags="q", per=3)
for op in ("fmul", "fmla", "fmls", "fmulx"):
    t("%s v{Vd}.4s, v{Vn}.4s, v{Vm}.s[{idxs}]" % op, cls="f32", per=4)
    t("%s v{Vd}.2s, v{Vn}.2s, v{Vm}.s[{idxs}]" % op, cls="f32", per=3)
    t("%s v{Vd}.2d, v{Vn}.2d, v{Vm}.d[{idxd}]" % op, cls="f64", per=4)
    t("%s s{Vd}, s{Vn}, v{Vm}.s[{idxs}]" % op, cls="f32", per=3)
    t("%s d{Vd}, d{Vn}, v{Vm}.d[{idxd}]" % op, cls="f64", per=3)
for op in ("sqdmull", "sqdmlal", "sqdmlsl"):
    t("%s s{Vd}, h{Vn}, v{Vml}.h[{idxh}]" % op, flags="q", per=3)
    t("%s d{Vd}, s{Vn}, v{Vm}.s[{idxs}]" % op, flags="q", per=3)
# tables, permutations
for a in ("8b", "16b"):
    t("tbl v{Vd}.%s, {{v{Vn}.16b}}, v{Vm}.%s" % (a, a), setup=[("Vm", "vmask", 0x1f)], per=4)
    t("tbx v{Vd}.%s, {{v{Vn}.16b, v{Vn+1}.16b}}, v{Vm}.%s" % (a, a), setup=[("Vm", "vmask", 0x3f)], per=4)
    t("tbl v{Vd}.%s, {{v{Vn}.16b, v{Vn+1}.16b, v{Vn+2}.16b}}, v{Vm}.%s" % (a, a), setup=[("Vm", "vmask", 0x3f)],
      per=4)
    t("tbx v{Vd}.%s, {{v{Vn}.16b, v{Vn+1}.16b, v{Vn+2}.16b, v{Vn+3}.16b}}, v{Vm}.%s" % (a, a), per=4)
for op in ("uzp1", "uzp2", "trn1", "trn2", "zip1", "zip2"):
    vbin(op, INT_ARR_D, per=2)
t("ext v{Vd}.16b, v{Vn}.16b, v{Vm}.16b, #{ext16}", per=6)
t("ext v{Vd}.8b, v{Vn}.8b, v{Vm}.8b, #{ext8}", per=4)
# scalar SIMD
for op in ("add", "sub", "cmgt", "cmhi", "cmge", "cmhs", "cmeq", "cmtst", "sshl", "ushl", "srshl", "urshl"):
    t("%s d{Vd}, d{Vn}, d{Vm}" % op, per=3)
for op in ("sqadd", "uqadd", "sqsub", "uqsub", "sqshl", "uqshl", "sqrshl", "uqrshl"):
    for r in "bhsd":
        t("%s %s{Vd}, %s{Vn}, %s{Vm}" % (op, r, r, r), flags="q", per=2)
for r in "hs":
    t("sqdmulh %s{Vd}, %s{Vn}, %s{Vm}" % (r, r, r), flags="q", per=3)
    t("sqrdmulh %s{Vd}, %s{Vn}, %s{Vm}" % (r, r, r), flags="q", per=3)
for op in ("abs", "neg"):
    t("%s d{Vd}, d{Vn}" % op, per=3)
for op in ("cmgt", "cmge", "cmeq", "cmle", "cmlt"):
    t("%s d{Vd}, d{Vn}, #0" % op, per=2)
for op in ("sqabs", "sqneg", "suqadd", "usqadd"):
    for r in "bhsd":
        t("%s %s{Vd}, %s{Vn}" % (op, r, r), flags="q", per=2)
for op in ("sqxtn", "uqxtn", "sqxtun"):
    for n, w in (("b", "h"), ("h", "s"), ("s", "d")):
        t("%s %s{Vd}, %s{Vn}" % (op, n, w), flags="q", per=3)
for op in ("sshr", "ushr", "ssra", "usra", "srshr", "urshr", "srsra", "ursra", "sri"):
    t("%s d{Vd}, d{Vn}, #{rsh64}" % op, per=3)
for op, fl in (("shl", ""), ("sli", ""), ("sqshl", "q"), ("uqshl", "q"), ("sqshlu", "q")):
    t("%s d{Vd}, d{Vn}, #{lsh64}" % op, flags=fl, per=3)
for op in ("sqshl", "uqshl", "sqshlu"):
    t("%s b{Vd}, b{Vn}, #{lsh8}" % op, flags="q", per=2)
    t("%s s{Vd}, s{Vn}, #{lsh32}" % op, flags="q", per=2)
for op in ("sqshrn", "uqshrn", "sqrshrn", "uqrshrn", "sqshrun", "sqrshrun"):
    t("%s b{Vd}, h{Vn}, #{rsh8}" % op, flags="q", per=2)
    t("%s h{Vd}, s{Vn}, #{rsh16}" % op, flags="q", per=2)
    t("%s s{Vd}, d{Vn}, #{rsh32}" % op, flags="q", per=2)
t("addp d{Vd}, v{Vn}.2d", per=3)
for op in ("faddp", "fmaxp", "fminp", "fmaxnmp", "fminnmp"):
    t("%s s{Vd}, v{Vn}.2s" % op, cls="f32", per=4)
    t("%s d{Vd}, v{Vn}.2d" % op, cls="f64", per=4)
for op in ("fmulx", "fcmeq", "fcmge", "fcmgt", "facge", "facgt", "fabd", "frecps", "frsqrts"):
    t("%s s{Vd}, s{Vn}, s{Vm}" % op, cls="f32", per=4)
    t("%s d{Vd}, d{Vn}, d{Vm}" % op, cls="f64", per=4)
for op in ("fcmgt", "fcmge", "fcmeq", "fcmle", "fcmlt"):
    t("%s s{Vd}, s{Vn}, #0.0" % op, cls="f32", per=2)
for op in ("frecpe", "frsqrte", "frecpx", "fcvtzs", "fcvtzu", "fcvtns", "fcvtnu", "fcvtms", "fcvtmu", "fcvtps",
           "fcvtpu", "fcvtas", "fcvtau"):
    t("%s s{Vd}, s{Vn}" % op, cls="f32", per=4)
    t("%s d{Vd}, d{Vn}" % op, cls="f64", per=4)
for op in ("scvtf", "ucvtf"):
    t("%s s{Vd}, s{Vn}" % op, per=3); t("%s d{Vd}, d{Vn}" % op, per=3)
    t("%s s{Vd}, s{Vn}, #{fbits32}" % op, per=3); t("%s d{Vd}, d{Vn}, #{fbits64}" % op, per=3)
for op in ("fcvtzs", "fcvtzu"):
    t("%s s{Vd}, s{Vn}, #{fbits32}" % op, cls="f32", per=3); t("%s d{Vd}, d{Vn}, #{fbits64}" % op, cls="f64", per=3)
for op in ("sqdmull", "sqdmlal", "sqdmlsl"):
    t("%s s{Vd}, h{Vn}, h{Vm}" % op, flags="q", per=3)
    t("%s d{Vd}, s{Vn}, s{Vm}" % op, flags="q", per=3)
# a few sequences with dependencies between instructions
t("adds {Xd}, {Xn}, {Xm}; adcs {Xd2}, {Xa}, xzr; cset {Xa}, cs", flags="f", per=10)
t("subs {Wd}, {Wn}, {Wm}; csel {Xd2}, {Xn}, {Xm}, lt; csinc {Wa}, {Wn}, {Wm}, hi", flags="f", per=10)
t("fmov s{Vd}, {Wn}; fcvtzs {Xd}, s{Vd}", per=6)

XREGS = [r for r in range(28) if r != 18]


def instantiate(tpl, rnd, idx):
    asm = tpl["asm"]
    regs = list(XREGS)
    rnd.shuffle(regs)
    vregs = list(range(32))
    rnd.shuffle(vregs)
    xmap, vmap = {}, {}
    out_x, out_v = set(), set()
    names = re.findall(r"\{([A-Za-z0-9_]+)(?:\+(\d))?\}", asm)
    # Even/odd pairs first (CASP).
    for nm, _ in names:
        if nm in ("Xe", "Xf", "We", "Wf") and nm[1] not in xmap:
            evens = [r for r in regs if r % 2 == 0 and r + 1 in regs and r + 1 != 18]
            e = rnd.choice(evens)
            regs.remove(e)
            regs.remove(e + 1)
            xmap[nm[1]] = e
    for nm, _ in names:
        if nm[0] in "XW" and len(nm) >= 2 and nm[1:] in ("d", "d2", "n", "m", "a", "b", "i"):
            key = nm[1:]
            if key not in xmap:
                xmap[key] = regs.pop()
        elif nm[0] == "V" and nm[1:] in ("d", "d2", "n", "m", "a", "ml"):
            key = nm[1:]
            if key not in vmap:
                if key == "ml":   # by-element 16-bit: only v0-v15
                    cand = [v for v in vregs if v < 16]
                    v = cand[0]
                else:
                    # room for lists of up to four registers
                    cand = [v for v in vregs if v <= 28 and all(v + k not in vmap.values() for k in range(4))]
                    v = cand[0]
                vregs.remove(v)
                for k in range(1, 4):
                    if v + k in vregs:
                        vregs.remove(v + k)
                vmap[key] = v
    def sub(m):
        nm, plus = m.group(1), int(m.group(2) or 0)
        if nm[0] in "XW" and nm[1:] in xmap:
            r = xmap[nm[1:]] + plus
            if nm[1:] in ("d", "d2"):
                out_x.add(r)
            return ("x%d" if nm[0] == "X" else "w%d") % r
        if nm[0] == "V" and nm[1:] in vmap:
            v = vmap[nm[1:]] + plus
            if nm[1:] in ("d", "d2"):
                out_v.add(v)
            return str(v)
        if nm in GENERATORS:
            return GENERATORS[nm](rnd)
        raise ValueError("unknown placeholder %s in %s" % (nm, asm))
    text = re.sub(r"\{([A-Za-z0-9_]+)(?:\+(\d))?\}", sub, asm).replace("{{", "{").replace("}}", "}")
    # Destination lists of vector loads: all registers of the list.
    if re.match(r"\s*ld[1-4]", text):
        for m in re.finditer(r"v(\d+)\.", text.split("[")[0] if "{" in text else text):
            out_v.add(int(m.group(1)))
    # CASP/LDXP pairs and the compare registers of CAS are outputs too.
    if "e" in xmap:
        out_x.update({xmap["e"], xmap["e"] + 1})
    setup = []
    for reg, kind, val in tpl["setup"]:
        if reg.startswith("V"):
            setup.append((vmap[reg[1:]], 4, val))
        else:
            r = xmap[reg[1:]]
            setup.append((r, {"addr": 1, "imm": 2, "mask": 3}[kind], val))
    if "b" in xmap and not any(s[1] == 1 for s in setup):
        setup.insert(0, (xmap["b"], 1, 0))
    if "i" in xmap and not any(s[0] == xmap["i"] for s in setup):
        setup.append((xmap["i"], 3, 0x3f))   # post-index register: small, any alignment
    if tpl["wb"]:
        out_x.add(xmap["b"])
    flags = 0
    if "f" in tpl["flags"]:
        flags |= 1
    if "q" in tpl["flags"]:
        flags |= 2
    if "m" in tpl["flags"]:
        flags |= 4
    cls = tpl["cls"]
    clsv = {"int": 0, "f32": 1, "f64": 2, "f16": 3}[cls.rstrip("r")] | (0x10 if cls.endswith("r") else 0)
    return text, out_x, out_v, flags, clsv, setup


def main():
    out_dir = sys.argv[1]
    seed = 1
    per_default = 5
    args = sys.argv[2:]
    while args:
        a = args.pop(0)
        if a == "--seed":
            seed = int(args.pop(0))
        elif a == "--per":
            per_default = int(args.pop(0))
    rnd = random.Random(seed)
    cases = []
    for tpl in T:
        n = tpl["per"] or per_default
        for _ in range(n):
            cases.append(instantiate(tpl, rnd, len(cases)))
    with open(out_dir + "/cases.S", "w") as f:
        f.write("// Generated by tools/ndk64/gen_a64_cases.py; do not edit.\n")
        f.write("#ifdef __APPLE__\n#define N(x) _##x\n#else\n#define N(x) x\n#endif\n")
        f.write("        .text\n")
        for i, (text, *_rest) in enumerate(cases):
            f.write(".globl N(a64_case_%d)\n.p2align 2\nN(a64_case_%d):\n" % (i, i))
            for ins in text.split(";"):
                ins = ins.strip()
                if re.match(r"^\d+:", ins):
                    lab, rest = ins.split(":", 1)
                    f.write("%s:\n" % lab)
                    ins = rest.strip()
                if ins:
                    f.write("        %s\n" % ins)
            f.write("        ret\n")
    with open(out_dir + "/cases.c", "w") as f:
        f.write("// Generated by tools/ndk64/gen_a64_cases.py; do not edit.\n")
        f.write('#include "diff.h"\n')
        for i in range(len(cases)):
            f.write("void a64_case_%d(void);\n" % i)
        f.write("const struct diff_case diff_cases[] = {\n")
        for i, (text, out_x, out_v, flags, clsv, setup) in enumerate(cases):
            mx = sum(1 << r for r in out_x)
            mv = sum(1 << v for v in out_v)
            su = ", ".join("{%d, %d, %d}" % s for s in setup)
            name = text.replace("\\", "\\\\").replace('"', '\\"')
            f.write('  {a64_case_%d, "%s", 0x%x, 0x%x, %d, %d, %d, 0, {%s}},\n' %
                    (i, name, mx, mv, flags, clsv, len(setup), su))
        f.write("};\nconst int diff_case_count = %d;\n" % len(cases))
    print("%d cases from %d templates" % (len(cases), len(T)))


if __name__ == "__main__":
    main()
