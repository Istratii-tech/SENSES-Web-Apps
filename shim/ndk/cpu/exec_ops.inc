// The non-control operations of the interpreter's switch (exec.cc): data
// processing, loads/stores, multiplies, media, synchronization, VFP, NEON.
// Included twice: by Run() and by ndk_jit_exec_one(), which translated
// code (jit.cc) calls for operations it does not compile itself. Uses c, R
// and d; every case ends with break.
        DP_CASES(kAND) DP_CASES(kEOR) DP_CASES(kSUB) DP_CASES(kRSB)
        DP_CASES(kADD) DP_CASES(kADC) DP_CASES(kSBC) DP_CASES(kRSC)
        DP_CASES(kTST) DP_CASES(kTEQ) DP_CASES(kCMP) DP_CASES(kCMN)
        DP_CASES(kORR) DP_CASES(kMOV) DP_CASES(kBIC) DP_CASES(kMVN)
        DP_CASES(kORN)
        MEM_CASES(kLDR) MEM_CASES(kLDRB) MEM_CASES(kLDRH) MEM_CASES(kLDRSB) MEM_CASES(kLDRSH)
        MEM_CASES(kSTR) MEM_CASES(kSTRB) MEM_CASES(kSTRH) MEM_CASES(kLDRD) MEM_CASES(kSTRD)

        case OP_NOP:
          break;
        case OP_SETPC:
          R[15] = d->imm;
          break;
        case OP_MUL: {
          uint32_t r = R[d->rn] * R[d->rm];
          R[d->rd] = r;
          if (d->flags & 1) { c->fn = r; c->fz = r; }
          break;
        }
        case OP_MLA: {
          uint32_t r = R[d->rn] * R[d->rm] + R[d->ra];
          R[d->rd] = r;
          if (d->flags & 1) { c->fn = r; c->fz = r; }
          break;
        }
        case OP_MLS:
          R[d->rd] = R[d->ra] - R[d->rn] * R[d->rm];
          break;
        case OP_UMULL: case OP_UMLAL: case OP_SMULL: case OP_SMLAL: case OP_UMAAL: {
          uint64_t r;
          if (d->op == OP_SMULL || d->op == OP_SMLAL) r = (uint64_t)((int64_t)(int32_t)R[d->rn] * (int32_t)R[d->rm]);
          else r = (uint64_t)R[d->rn] * R[d->rm];
          if (d->op == OP_UMLAL || d->op == OP_SMLAL) r += ((uint64_t)R[d->ra] << 32) | R[d->rd];
          else if (d->op == OP_UMAAL) r += (uint64_t)R[d->ra] + R[d->rd];
          R[d->rd] = (uint32_t)r;
          R[d->ra] = (uint32_t)(r >> 32);
          if (d->flags & 1) { c->fn = (uint32_t)(r >> 32); c->fz = (uint32_t)r | (uint32_t)(r >> 32); }
          break;
        }
        case OP_SMULXY: {
          int32_t p = Half(R[d->rn], d->imm & 1) * Half(R[d->rm], (d->imm >> 1) & 1);
          if (d->imm & 4) {
            int64_t s = (int64_t)p + (int32_t)R[d->ra];
            if (s != (int32_t)s) c->q = 1;
            p = (int32_t)s;
          }
          R[d->rd] = p;
          break;
        }
        case OP_SMULWY: {
          int32_t p = (int32_t)(((int64_t)(int32_t)R[d->rn] * Half(R[d->rm], d->imm & 1)) >> 16);
          if (d->imm & 4) {
            int64_t s = (int64_t)p + (int32_t)R[d->ra];
            if (s != (int32_t)s) c->q = 1;
            p = (int32_t)s;
          }
          R[d->rd] = p;
          break;
        }
        case OP_SMLALXY: {
          int64_t acc = (int64_t)(((uint64_t)R[d->ra] << 32) | R[d->rd]);
          acc += (int64_t)Half(R[d->rn], d->imm & 1) * Half(R[d->rm], (d->imm >> 1) & 1);
          R[d->rd] = (uint32_t)acc;
          R[d->ra] = (uint32_t)((uint64_t)acc >> 32);
          break;
        }
        case OP_SMUAD: {
          uint32_t m = (d->imm & 1) ? Ror(R[d->rm], 16) : R[d->rm];
          int64_t p1 = (int64_t)Half(R[d->rn], 0) * Half(m, 0);
          int64_t p2 = (int64_t)Half(R[d->rn], 1) * Half(m, 1);
          int64_t s = (d->imm & 2) ? p1 - p2 : p1 + p2;
          if (d->imm & 8) {
            int64_t acc = (int64_t)(((uint64_t)R[d->ra] << 32) | R[d->rd]) + s;
            R[d->rd] = (uint32_t)acc;
            R[d->ra] = (uint32_t)((uint64_t)acc >> 32);
          } else {
            if (d->imm & 4) s += (int32_t)R[d->ra];
            if (s != (int32_t)s) c->q = 1;
            R[d->rd] = (uint32_t)s;
          }
          break;
        }
        case OP_SMMUL: {
          int64_t p = (int64_t)(int32_t)R[d->rn] * (int32_t)R[d->rm];
          int64_t v;
          if (d->imm & 2) {
            int64_t acc = (int64_t)((uint64_t)R[d->ra] << 32);
            v = (d->imm & 4) ? acc - p : acc + p;
          } else {
            v = p;
          }
          if (d->imm & 1) v += 0x80000000LL;
          R[d->rd] = (uint32_t)((uint64_t)v >> 32);
          break;
        }
        case OP_SDIV: {
          int32_t n = (int32_t)R[d->rn], m = (int32_t)R[d->rm];
          R[d->rd] = m == 0 ? 0 : (n == INT32_MIN && m == -1) ? (uint32_t)n : (uint32_t)(n / m);
          break;
        }
        case OP_UDIV:
          R[d->rd] = R[d->rm] == 0 ? 0 : R[d->rn] / R[d->rm];
          break;
        case OP_CLZ:
          R[d->rd] = R[d->rm] ? __builtin_clz(R[d->rm]) : 32;
          break;
        case OP_REV:
          R[d->rd] = __builtin_bswap32(R[d->rm]);
          break;
        case OP_REV16: {
          uint32_t v = R[d->rm];
          R[d->rd] = ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
          break;
        }
        case OP_REVSH: {
          uint32_t v = R[d->rm];
          R[d->rd] = (uint32_t)(int32_t)(int16_t)(((v & 0xff) << 8) | ((v >> 8) & 0xff));
          break;
        }
        case OP_RBIT: {
          uint32_t v = R[d->rm];
          v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
          v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
          v = ((v >> 4) & 0x0f0f0f0fu) | ((v & 0x0f0f0f0fu) << 4);
          R[d->rd] = __builtin_bswap32(v);
          break;
        }
        case OP_EXTEND: {
          uint32_t v = Ror(R[d->rm], d->imm), r;
          switch (d->imm2) {
            case 0: r = (uint32_t)(int32_t)(int8_t)v; break;
            case 1: r = (uint32_t)(int32_t)(int16_t)v; break;
            case 2: r = v & 0xff; break;
            case 3: r = v & 0xffff; break;
            case 4: r = ((uint32_t)(int32_t)(int8_t)v & 0xffff) | ((uint32_t)(int32_t)(int8_t)(v >> 16) << 16); break;
            default: r = v & 0x00ff00ffu; break;
          }
          if (d->rn != 15) {
            uint32_t n = R[d->rn];
            if (d->imm2 >= 4) r = ((n + r) & 0xffff) | (((n >> 16) + (r >> 16)) << 16);
            else r += n;
          }
          R[d->rd] = r;
          break;
        }
        case OP_UBFX: {
          uint32_t v = R[d->rn] >> d->imm;
          R[d->rd] = d->imm2 >= 32 ? v : v & ((1u << d->imm2) - 1);
          break;
        }
        case OP_SBFX: {
          uint32_t top = 32 - d->imm - d->imm2;
          R[d->rd] = (uint32_t)((int32_t)(R[d->rn] << top) >> (32 - d->imm2));
          break;
        }
        case OP_BFI: {
          uint32_t width_mask = d->imm2 >= 32 ? 0xffffffffu : (1u << d->imm2) - 1;
          uint32_t mask = width_mask << d->imm;
          uint32_t src = d->rn == 15 ? 0 : R[d->rn];
          R[d->rd] = (R[d->rd] & ~mask) | ((src << d->imm) & mask);
          break;
        }
        case OP_MOVW:
          R[d->rd] = d->imm;
          break;
        case OP_MOVT:
          R[d->rd] = (R[d->rd] & 0xffff) | (d->imm << 16);
          break;
        case OP_QADDSUB: {
          int64_t n = (int32_t)R[d->rn];
          if (d->imm & 2) n = SignedSat(2 * n, 32, &c->q);
          int64_t r = (d->imm & 1) ? (int64_t)(int32_t)R[d->rm] - n : (int64_t)(int32_t)R[d->rm] + n;
          R[d->rd] = (uint32_t)SignedSat(r, 32, &c->q);
          break;
        }
        case OP_SSAT: case OP_USAT: {
          uint32_t v = R[d->rn];
          int64_t x = d->sh == kASR ? (int64_t)((int32_t)v >> (d->imm2 >= 32 ? 31 : d->imm2))
                                    : (int64_t)(int32_t)(v << d->imm2);
          R[d->rd] = d->op == OP_SSAT ? (uint32_t)SignedSat(x, d->imm, &c->q) : UnsignedSat(x, d->imm, &c->q);
          break;
        }
        case OP_SSAT16: case OP_USAT16: {
          uint32_t v = R[d->rn], r = 0;
          for (int i = 0; i < 2; i++) {
            int64_t x = Half(v, i);
            uint32_t h = d->op == OP_SSAT16 ? (uint32_t)SignedSat(x, d->imm, &c->q) : UnsignedSat(x, d->imm, &c->q);
            r |= (h & 0xffff) << (16 * i);
          }
          R[d->rd] = r;
          break;
        }
        case OP_PARALLEL:
          R[d->rd] = Parallel(c, R[d->rn], R[d->rm], d->imm);
          break;
        case OP_SEL: {
          uint32_t r = 0;
          for (int i = 0; i < 4; i++) {
            uint32_t m = 0xffu << (8 * i);
            r |= ((c->ge >> i) & 1) ? (R[d->rn] & m) : (R[d->rm] & m);
          }
          R[d->rd] = r;
          break;
        }
        case OP_USAD8: case OP_USADA8: {
          uint32_t a = R[d->rn], bb = R[d->rm], s = 0;
          for (int i = 0; i < 4; i++) {
            int x = (a >> (8 * i)) & 0xff, y = (bb >> (8 * i)) & 0xff;
            s += x > y ? x - y : y - x;
          }
          if (d->op == OP_USADA8) s += R[d->ra];
          R[d->rd] = s;
          break;
        }
        case OP_PKH:
          if (d->imm) {
            uint32_t m = (uint32_t)((int32_t)R[d->rm] >> (d->imm2 == 0 ? 31 : d->imm2));
            R[d->rd] = (R[d->rn] & 0xffff0000u) | (m & 0xffff);
          } else {
            R[d->rd] = (R[d->rn] & 0xffff) | ((R[d->rm] << d->imm2) & 0xffff0000u);
          }
          break;
        case OP_MRS:
          R[d->rd] = PackCpsr(c) & 0xf80f0000u;
          break;
        case OP_MSR: {
          uint32_t v = (d->flags & 1) ? d->imm : R[d->rm];
          if (d->imm2 & 8) {
            c->fn = v & 0x80000000u;
            c->fz = (v & 0x40000000u) ? 0 : 1;
            c->fc = (v >> 29) & 1;
            c->fv = (v & 0x10000000u) ? 0x80000000u : 0;
            c->q = (v >> 27) & 1;
          }
          if (d->imm2 & 4) c->ge = (v >> 16) & 15;
          break;
        }
        case OP_SWP:
          R[d->rd] = __atomic_exchange_n(static_cast<uint32_t*>(HostPtr(R[d->rn])), R[d->rm], __ATOMIC_SEQ_CST);
          break;
        case OP_SWPB:
          R[d->rd] = __atomic_exchange_n(static_cast<uint8_t*>(HostPtr(R[d->rn])), (uint8_t)R[d->rm], __ATOMIC_SEQ_CST);
          break;
        case OP_LDREX: {
          uint32_t addr = R[d->rn] + d->imm2;
          uint64_t v;
          switch (d->imm) {
            case 1: v = __atomic_load_n(static_cast<uint8_t*>(HostPtr(addr)), __ATOMIC_SEQ_CST); R[d->rd] = (uint32_t)v; break;
            case 2: v = __atomic_load_n(static_cast<uint16_t*>(HostPtr(addr)), __ATOMIC_SEQ_CST); R[d->rd] = (uint32_t)v; break;
            case 4: v = __atomic_load_n(static_cast<uint32_t*>(HostPtr(addr)), __ATOMIC_SEQ_CST); R[d->rd] = (uint32_t)v; break;
            default:
              v = __atomic_load_n(static_cast<uint64_t*>(HostPtr(addr)), __ATOMIC_SEQ_CST);
              R[d->rd] = (uint32_t)v; R[d->ra] = (uint32_t)(v >> 32);
              break;
          }
          c->ex_valid = true; c->ex_addr = addr; c->ex_val = v;
          break;
        }
        case OP_STREX: {
          uint32_t addr = R[d->rn] + d->imm2;
          bool ok = false;
          if (c->ex_valid && c->ex_addr == addr) {
            switch (d->imm) {
              case 1: { uint8_t e = (uint8_t)c->ex_val; ok = __atomic_compare_exchange_n(static_cast<uint8_t*>(HostPtr(addr)), &e, (uint8_t)R[d->rm], false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); break; }
              case 2: { uint16_t e = (uint16_t)c->ex_val; ok = __atomic_compare_exchange_n(static_cast<uint16_t*>(HostPtr(addr)), &e, (uint16_t)R[d->rm], false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); break; }
              case 4: { uint32_t e = (uint32_t)c->ex_val; ok = __atomic_compare_exchange_n(static_cast<uint32_t*>(HostPtr(addr)), &e, R[d->rm], false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); break; }
              default: {
                uint64_t e = c->ex_val;
                uint64_t nv = R[d->rm] | ((uint64_t)R[d->ra] << 32);
                ok = __atomic_compare_exchange_n(static_cast<uint64_t*>(HostPtr(addr)), &e, nv, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                break;
              }
            }
          }
          c->ex_valid = false;
          R[d->rd] = ok ? 0 : 1;
          break;
        }
        case OP_CLREX:
          c->ex_valid = false;
          break;
        case OP_DMB:
          __atomic_thread_fence(__ATOMIC_SEQ_CST);
          break;
        case OP_SVC:
          Syscall(c, d->imm);
          break;
        case OP_MRC_TLS:
          R[d->rd] = c->tls;
          break;
        // --- VFP ---
        case OP_VFP_BIN:
          if (d->flags & 1) SetD(c, d->rd, VfpBin<double>(d->imm, GetD(c, d->rn), GetD(c, d->rm), GetD(c, d->rd)));
          else SetS(c, d->rd, VfpBin<float>(d->imm, GetS(c, d->rn), GetS(c, d->rm), GetS(c, d->rd)));
          break;
        case OP_VFP_UN:
          if (d->flags & 1) {
            uint64_t v = c->d[d->rm];
            switch (d->imm) {
              case kVMOV: break;
              case kVABS: v &= ~(1ull << 63); break;
              case kVNEG: v ^= 1ull << 63; break;
              default: { double x; memcpy(&x, &v, 8); x = sqrt(x); memcpy(&v, &x, 8); }
            }
            c->d[d->rd] = v;
          } else {
            uint32_t v = SWords(c)[d->rm];
            switch (d->imm) {
              case kVMOV: break;
              case kVABS: v &= ~(1u << 31); break;
              case kVNEG: v ^= 1u << 31; break;
              default: { float x; memcpy(&x, &v, 4); x = sqrtf(x); memcpy(&v, &x, 4); }
            }
            SWords(c)[d->rd] = v;
          }
          break;
        case OP_VFP_CMP: {
          double a, bb;
          if (d->flags & 1) { a = GetD(c, d->rd); bb = d->imm2 ? 0.0 : GetD(c, d->rm); }
          else { a = GetS(c, d->rd); bb = d->imm2 ? 0.0 : GetS(c, d->rm); }
          c->fpscr = (c->fpscr & 0x0fffffffu) | (VfpCompare(a, bb) << 28);
          break;
        }
        case OP_VFP_CVT:
          VfpCvt(c, d);
          break;
        case OP_VFP_MOVI:
          if (d->flags & 1) c->d[d->rd] = d->imm | ((uint64_t)d->imm2 << 32);
          else SWords(c)[d->rd] = d->imm;
          break;
        case OP_VFP_LDST: {
          uint32_t base = d->rn == 15 ? (R[15] & ~3u) : R[d->rn];
          uint32_t addr = base + d->imm;
          void* reg = SWords(c) + d->rd;
          int size = (d->flags & 1) ? 8 : 4;
          if (d->flags & 2) memcpy(HostPtr(addr), reg, size);
          else memcpy(reg, HostPtr(addr), size);
          break;
        }
        case OP_VFP_LDM: {
          uint32_t base = R[d->rn];
          uint32_t addr = (d->flags & kLdmInc) ? base : base - d->imm2;
          void* reg = SWords(c) + d->rd;
          if (d->flags & 2) memcpy(HostPtr(addr), reg, d->imm * 4);
          else memcpy(reg, HostPtr(addr), d->imm * 4);
          if (d->flags & kLdmWb) R[d->rn] = (d->flags & kLdmInc) ? base + d->imm2 : base - d->imm2;
          break;
        }
        case OP_VMOV_CORE_S:
          if (d->flags & 1) R[d->rd] = SWords(c)[d->rn];
          else SWords(c)[d->rn] = R[d->rd];
          break;
        case OP_VMOV_CORE_D: {
          uint32_t* w = (d->flags & 2) ? SWords(c) + d->rn : SWords(c) + d->rn * 2;
          if (d->flags & 1) { uint32_t lo = w[0], hi = w[1]; R[d->rd] = lo; R[d->ra] = hi; }
          else { w[0] = R[d->rd]; w[1] = R[d->ra]; }
          break;
        }
        case OP_VMOV_SCALAR: {
          uint8_t* p = reinterpret_cast<uint8_t*>(&c->d[d->rn]) + d->imm;
          if (d->flags & 1) {
            uint32_t v = 0;
            memcpy(&v, p, d->imm2);
            if (!(d->flags & 4)) {
              if (d->imm2 == 1) v = (uint32_t)(int32_t)(int8_t)v;
              else if (d->imm2 == 2) v = (uint32_t)(int32_t)(int16_t)v;
            }
            R[d->rd] = v;
          } else {
            uint32_t v = R[d->rd];
            memcpy(p, &v, d->imm2);
          }
          break;
        }
        case OP_VMRS:
          if (d->rd == 15) {
            uint32_t f = c->fpscr;
            c->fn = f & 0x80000000u;
            c->fz = (f & 0x40000000u) ? 0 : 1;
            c->fc = (f >> 29) & 1;
            c->fv = (f & 0x10000000u) ? 0x80000000u : 0;
          } else {
            R[d->rd] = VmrsValue(c, d->imm);
          }
          break;
        case OP_VMSR:
          c->fpscr = R[d->rd];
          break;
        case OP_VDUP_CORE: {
          uint64_t v = R[d->rd];
          if (d->imm == 1) v = (v & 0xff) * 0x0101010101010101ull;
          else if (d->imm == 2) v = (v & 0xffff) * 0x0001000100010001ull;
          else v = v | (v << 32);
          for (uint32_t i = 0; i < d->imm2; i++) c->d[(d->rn + i) & 31] = v;
          break;
        }
        case OP_NEON:
          NeonExec(c, d->imm);
          break;
        case OP_NEON_LDST:
          NeonLdSt(c, d->imm);
          break;
