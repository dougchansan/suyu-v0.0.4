// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Header-only AArch64 -> portable C static recompiler engine, shared by the standalone
// tools/static_recompiler CLI and the in-app game export feature.
//
// It decodes a subset of user-mode AArch64 and emits C against a GuestContext (N64Recomp-style).
// Every instruction either translates to native C or emits a runtime fallback, so the generated
// project ALWAYS builds into a native binary (Windows .exe / Linux+BSD ELF / macOS Mach-O) or can
// be emitted as plain C source. A full game additionally needs suyu's HLE/GPU runtime, wired in via
// the generated runtime's recomp_svc()/MMIO hooks.

#pragma once

#include <algorithm>
#include <iomanip>
#include <utility>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace suyu::recomp {

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;
using s64 = int64_t;

// Output paths reach this engine as UTF-8 (the frontend hands over
// QString::toStdString()), but the narrow char overloads of _mkdir/mkdir and
// std::ofstream interpret their argument in the process code page - CP-1252 on
// a stock Windows install. A title whose name carries a non-ASCII character
// ("Pokemon Sword" reads as "Pokémon Sword" once the name comes from the
// ROM's own NACP) therefore had every directory creation and every file open
// silently fail, leaving empty module directories and a build that could not
// configure. Route both through std::filesystem::path, which is constructed
// from the UTF-8 bytes explicitly and uses the wide OS entry points.
inline std::filesystem::path Utf8Path(const std::string& s) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

struct Block {
    u64 vaddr;
    u32 size;
    u32 count;
    bool is_entry;
};

inline bool IsTerminator(u32 i) {
    if ((i & 0xFC000000) == 0x14000000) return true; // B
    if ((i & 0xFC000000) == 0x94000000) return true; // BL
    if ((i & 0xFFFFFC1F) == 0xD61F0000) return true; // BR
    if ((i & 0xFFFFFC1F) == 0xD63F0000) return true; // BLR
    if ((i & 0xFFFFFC1F) == 0xD65F0000) return true; // RET
    if ((i & 0x7F000000) == 0x34000000) return true; // CBZ
    if ((i & 0x7F000000) == 0x35000000) return true; // CBNZ
    if ((i & 0x7F000000) == 0x36000000) return true; // TBZ
    if ((i & 0x7F000000) == 0x37000000) return true; // TBNZ
    if ((i & 0xFF000010) == 0x54000000) return true; // B.cond
    if ((i & 0xFFE0001F) == 0xD4000001) return true; // SVC
    if ((i & 0xFFE0001F) == 0xD4200000) return true; // BRK
    if ((i & 0xFFFFF0FF) == 0xD50330DF) return true; // ISB: new fetch boundary
    if ((i & 0xFFFFFFE0) == 0xD50B7520) return true; // IC IVAU host event
    return false;
}

// Statically known target of a direct branch, if the instruction has one.
//
// This has to cover every form whose target the translator bakes in, not just
// the unconditional imm26 pair: a conditional branch's target is equally
// static, and it is overwhelmingly the common case for loop back-edges. Leaving
// B.cond/CBZ/CBNZ/TBZ/TBNZ out here meant their targets never began a block, so
// the generated code would set pc to an address that recomp_lookup - which
// matches block starts exactly - could not resolve, and execution died with
// "No recompiled block at PC" at the top of the first loop it entered.
inline bool DirectBranchTarget(u32 i, u64 pc, u64& out) {
    if ((i & 0xFC000000) == 0x14000000 || (i & 0xFC000000) == 0x94000000) { // B / BL
        s32 imm26 = (s32)(i << 6) >> 6;
        out = pc + (s64)imm26 * 4;
        return true;
    }
    if ((i & 0xFF000010) == 0x54000000 ||    // B.cond
        (i & 0x7E000000) == 0x34000000) {    // CBZ / CBNZ
        s64 imm19 = ((s32)(((i >> 5) & 0x7FFFF) << 13) >> 13);
        out = pc + imm19 * 4;
        return true;
    }
    if ((i & 0x7E000000) == 0x36000000) {    // TBZ / TBNZ
        s64 imm14 = ((s32)(((i >> 5) & 0x3FFF) << 18) >> 18);
        out = pc + imm14 * 4;
        return true;
    }
    return false;
}

// Function addresses the code builds for itself with ADRP+ADD.
//
// A callback handed to the OS - a thread entry above all - is never branched to
// from inside .text and never appears in a relocation either: the compiler just
// materialises its address into a register (adrp xN, page; add xN, xN, #lo12)
// and passes it to svcCreateThread. Without seeding those, every thread the
// game starts begins at an address no block covers and drops straight to the
// interpreter for its whole life, which is most of the "No recompiled block at
// PC" traffic in a real run.
//
// Deliberately loose: any ADRP+ADD landing in .text becomes a root. A pair that
// was really computing a data address only costs one extra block boundary.
inline void CollectAdrpAddTargets(const u8* text, size_t n_bytes, u64 base,
                                  std::vector<u64>& out) {
    const u32 n = static_cast<u32>(n_bytes / 4);
    const u32* p = reinterpret_cast<const u32*>(text);
    // Last ADRP seen per destination register, as a page address; ~0 means the
    // register no longer holds one.
    std::array<u64, 32> page{};
    page.fill(~0ULL);
    for (u32 i = 0; i < n; ++i) {
        const u32 insn = p[i];
        if ((insn & 0x9F000000) == 0x90000000) { // ADRP
            const u32 rd = insn & 31;
            s64 immhi = static_cast<s32>((((insn >> 5) & 0x7FFFF) << 13)) >> 13;
            const u32 immlo = (insn >> 29) & 3;
            page[rd] = ((base + static_cast<u64>(i) * 4) & ~0xFFFULL) +
                       static_cast<u64>(((immhi << 2) | immlo) << 12);
        } else if ((insn & 0xFFC00000) == 0x91000000) { // ADD (immediate, 64-bit, LSL #0)
            const u32 rd = insn & 31, rn = (insn >> 5) & 31;
            const u32 imm12 = (insn >> 10) & 0xFFF;
            if (page[rn] != ~0ULL) {
                const u64 target = page[rn] + imm12;
                if (target >= base && (target - base) / 4 < n && (target & 3) == 0) {
                    out.push_back(target);
                }
            }
            if (rd != rn) {
                page[rd] = ~0ULL;
            }
        } else {
            // Any other write to a register invalidates the page it held. Only
            // the common destination encodings are decoded here; missing one
            // can add a stale root, never remove a real one.
            const u32 rd = insn & 31;
            page[rd] = ~0ULL;
        }
    }
}

inline std::vector<Block> DiscoverBlocks(const u8* text, size_t n_bytes, u64 base,
                                         u64 entry_pc = 0,
                                         const std::vector<u64>* extra_roots = nullptr) {
    const u32 n = (u32)(n_bytes / 4);
    if (n == 0) return {};
    std::vector<bool> start(n, false);
    start[0] = true;
    // The module entry has to begin a block in its own right. Nothing branches
    // to it from inside the image, and for a real NSO it isn't offset 0
    // either - .text opens with a MOD0 header - so without this the entry
    // address falls in the middle of some other block and lookup fails.
    if (entry_pc > base && (entry_pc - base) / 4 < n) {
        start[(u32)((entry_pc - base) / 4)] = true;
    }
    // Exported dynsym addresses (functions like nn::init::Start, only ever
    // reached indirectly via another module's resolved GOT/PLT entry, never
    // by a direct branch inside this module's own .text). Without seeding
    // these as roots too, a function preceded by alignment padding rather
    // than a terminator falls mid-block and the runtime dispatcher can never
    // resolve a call landing exactly on its real entry address.
    if (extra_roots) {
        for (u64 addr : *extra_roots) {
            if (addr >= base && (addr - base) / 4 < n) {
                start[(u32)((addr - base) / 4)] = true;
            }
        }
    }
    {
        std::vector<u64> computed;
        CollectAdrpAddTargets(text, n_bytes, base, computed);
        for (u64 addr : computed) {
            start[(u32)((addr - base) / 4)] = true;
        }
    }
    // Preserve roots independently of the mechanical boundary after a terminator.
    // Only the latter can be omitted when an unconditional transfer precedes zeros.
    std::vector<bool> required = start;
    const u32* p = reinterpret_cast<const u32*>(text);
    for (u32 i = 0; i < n; ++i) {
        const u32 insn = p[i];
        const u64 pc = base + (u64)i * 4;
        if (IsTerminator(insn)) {
            if (i + 1 < n) start[i + 1] = true;
            u64 t = 0;
            if (DirectBranchTarget(insn, pc, t) && t >= base && (t - base) / 4 < n) {
                const u32 target = (u32)((t - base) / 4);
                start[target] = required[target] = true;
            }
        }
    }
    std::vector<Block> blocks;
    for (u32 s = 0; s < n;) {
        u32 end = s + 1;
        if (p[s] != 0) {
            while (end < n && !start[end] && p[end] != 0) ++end;
        }
        // A zero immediately after B/BR/RET has no architectural fallthrough.
        // Omit only that mechanical boundary, retaining every independently
        // rooted address. Calls, conditional branches and SVC can return/fall
        // through, so their first zero remains an undefined-instruction trap.
        const u32 previous = s > 0 ? p[s - 1] : 0;
        const bool no_fallthrough = (previous & 0xFC000000) == 0x14000000 || // B, not BL
                                    (previous & 0xFFFFFC1F) == 0xD61F0000 || // BR
                                    (previous & 0xFFFFFC1F) == 0xD65F0000;   // RET
        if (p[s] != 0 || required[s] || !no_fallthrough) {
            blocks.push_back(Block{base + (u64)s * 4, (end - s) * 4, end - s, s == 0});
        }
        // Further unrooted zeros cannot execute past the retained trap (or
        // preceding transfer). Resume conservatively at the next nonzero word.
        if (p[s] == 0) {
            while (end < n && p[end] == 0 && !start[end]) ++end;
        }
        s = end;
    }
    return blocks;
}

// ARM's logical-immediate encoding: N:immr:imms describe a repeating bit
// pattern rather than a literal value. This is the standard DecodeBitMasks
// from the architecture reference, restricted to the immediate (non-tested)
// result. Returns false for the reserved encodings.
inline bool DecodeBitMasks(u32 N, u32 imms, u32 immr, bool is64, u64& out) {
    if (!is64 && N) return false;
    // len = index of the highest set bit of (N:~imms), computed portably.
    const u32 bits = (N << 6) | ((~imms) & 0x3F);
    if (bits == 0) return false;
    u32 len = 0;
    for (u32 t = bits; t > 1; t >>= 1) ++len;
    if (len < 1) return false;
    const u32 esize = 1u << len;
    if (esize > (is64 ? 64u : 32u)) return false;
    const u32 levels = esize - 1;
    const u32 s = imms & levels;
    const u32 r = immr & levels;
    if (s == levels) return false;   // reserved
    u64 welem = (s + 1 >= 64) ? ~0ULL : ((1ULL << (s + 1)) - 1);
    // Rotate right within the element, then replicate to the register width.
    // The rotate applies at every element size, 64 included: skipping it there
    // (to dodge the undefined `welem << 64` when r is 0) silently turned every
    // 64-bit rotated mask into its unrotated form - so "and x8, x8, #~0xF",
    // the standard align-down, decoded as `& 0x0FFFFFFFFFFFFFFF` and left the
    // pointer unaligned instead. Guard r == 0 explicitly instead.
    u64 elem;
    if (r == 0) {
        elem = welem;
    } else if (esize >= 64) {
        elem = (welem >> r) | (welem << (64 - r));
    } else {
        elem = ((welem >> r) | (welem << (esize - r))) & ((1ULL << esize) - 1);
    }
    u64 result = 0;
    for (u32 i = 0; i < (is64 ? 64u : 32u); i += esize) result |= elem << i;
    if (!is64) result &= 0xFFFFFFFFULL;
    out = result;
    return true;
}

inline std::string FuncName(const std::string& mod, u64 v) {
    char b[64]; snprintf(b, sizeof b, "blk_%s_%016llx", mod.c_str(), (unsigned long long)v); return b;
}

// Same name, written into a caller-owned buffer. The emit loops call this once
// per block for the body and twice more per block for the dispatch table, so on
// a multi-million-block title the returned-std::string form above is millions of
// heap allocations for a name that is always well under 64 bytes.
inline size_t FuncNameTo(char (&b)[64], const char* mod, u64 v) {
    return (size_t)snprintf(b, sizeof b, "blk_%s_%016llx", mod, (unsigned long long)v);
}

// Set by the emit loop so a direct branch whose target is a known block start
// can call that block instead of returning to the dispatcher. Null while the
// decode tests run, which is what keeps their expected output stable.
// Emits "carry on at t": a direct call when t is a block we emitted, otherwise
// the original park-the-PC-and-return. Loop back edges are conditional, so the
// conditional forms need this as much as the unconditional ones do.
inline std::string ChainTo(u64 t);

// With no JIT behind it there is no cheaper alternative to translating an
// instruction, so the forms that lose to the JIT are turned on here.
inline bool g_translate_all = false;

inline const std::unordered_set<u64>* g_chain_blocks = nullptr;
inline const char* g_chain_mod = nullptr;

// How many blocks may run before control goes back to the dispatcher. The
// dispatcher checks for interrupts and services SVCs between blocks, so an
// unbounded chain would let a guest loop run uninterruptibly; it also derives
// the executed-block count from how much of this budget was spent.
inline constexpr int kChainBudget = 256;

// Conditional branches deliberately do not chain. Measured: chaining them too
// takes 2.00 ms/frame against 1.80, and turns a tight distribution into one
// spanning 1.70-2.54 across six reps while the dynarmic arm stays at 2.905 in
// the same runs. It also pushed JIT transitions from 69,008 to 88,983 and more
// than doubled lookup misses, which is unexplained and worth understanding
// before trying this again.
inline std::string ChainTo(u64 t) {
    char b[256];
    if (g_chain_blocks && g_chain_mod && g_chain_blocks->count(t)) {
        char nm[64];
        FuncNameTo(nm, g_chain_mod, t);
        // The declaration is at block scope so a unit needs no list of the
        // blocks it reaches. `return f(c)` is written as a tail call, though
        // nothing may come of that below -O2 - which is why the budget has to
        // bound the depth rather than assume it stays at one frame.
        snprintf(b, sizeof b,
                 "{ void %s(GuestContext*); c->pc=g_module_base+0x%llxULL; "
                 "if (--c->chain_budget <= 0) return; %s(c); return; }",
                 nm, (unsigned long long)t, nm);
    } else {
        snprintf(b, sizeof b, "{ c->pc=g_module_base+0x%llxULL; return; }",
                 (unsigned long long)t);
    }
    return b;
}

// The same for a computed target. A chain of blocks otherwise ends at the first
// BR, BLR or RET, and every one of those is a round trip out to the host
// dispatcher: RunThread plus the dispatch lambda are 24.7% of all cycles in a
// JIT-free image against 17.7% in a hybrid one. recomp_lookup is this module's
// own flat block index - one bounds check and one load - so resolving the
// target here keeps execution inside the image whenever it stays in the module.
// A target in another module misses the index and falls back to the dispatcher,
// which is where it had to go anyway.
inline std::string ChainIndirect(const std::string& target) {
    return "{ c->pc=" + target +
           "; { BlockFn _f=recomp_lookup(c->pc-g_module_base); "
           "if (_f && --c->chain_budget > 0) { _f(c); return; } } return; }";
}

// The condition is a literal at every site, so the switch inside recomp_cond
// is resolvable here rather than at run time. It was 2.1% of all cycles as a
// call - and the expression that replaces it is smaller than the call was,
// which matters: the memory helpers were measured as *worse* inlined because
// the instruction cache cost more than the call did.
// NZCV, with the operation width and add/subtract resolved here. Same argument
// as Cond: is_sub and is64 are literals at every site, and a call with six
// arguments costs more to set up than the arithmetic it hides.
inline std::string SetFlags(bool is_sub, const std::string& a, const std::string& b,
                            const std::string& r, bool is64) {
    const std::string ty = is64 ? "uint64_t" : "uint32_t";
    const std::string cast = is64 ? "" : "(uint32_t)";
    const char* top = is64 ? "63" : "31";
    std::string s = "{ " + ty + " _fa=" + cast + "(" + a + "),_fb=" + cast + "(" + b +
                    "),_fr=" + cast + "(" + r + "); ";
    s += "c->z=(_fr==0); c->n=(int)((_fr>>" + std::string(top) + ")&1); ";
    if (is_sub) {
        s += "c->c=(_fa>=_fb); ";
        s += "c->v=(int)((((_fa^_fb)&(_fa^_fr))>>" + std::string(top) + ")&1); }";
    } else {
        s += "c->c=(_fr<_fa); ";
        s += "c->v=(int)((((" + ty + ")~(_fa^_fb)&(_fa^_fr))>>" + std::string(top) + ")&1); }";
    }
    return s;
}

inline std::string Cond(u32 cond) {
    switch (cond) {
    case 0:  return "(c->z)";
    case 1:  return "(!c->z)";
    case 2:  return "(c->c)";
    case 3:  return "(!c->c)";
    case 4:  return "(c->n)";
    case 5:  return "(!c->n)";
    case 6:  return "(c->v)";
    case 7:  return "(!c->v)";
    case 8:  return "(c->c && !c->z)";
    case 9:  return "(!(c->c && !c->z))";
    case 10: return "(c->n == c->v)";
    case 11: return "(c->n != c->v)";
    case 12: return "((c->n == c->v) && !c->z)";
    case 13: return "(!((c->n == c->v) && !c->z))";
    default: return "(1)";   // AL and the NV encoding, which also always passes
    }
}

inline std::string Xz(u32 r) {
    return r == 31 ? std::string("(uint64_t)0") : ("c->x[" + std::to_string(r) + "]");
}
inline std::string Wz(u32 r) {
    return r == 31 ? std::string("(uint32_t)0") : ("(uint32_t)c->x[" + std::to_string(r) + "]");
}

/// Register 31 as the stack pointer rather than the zero register.
///
/// AArch64 spells both with the same encoding and the meaning depends on the
/// instruction: ADD/SUB with an immediate or an extended register read it as
/// SP, while the shifted-register and logical forms read it as XZR. Using the
/// zero-register spelling everywhere makes every function prologue -
/// "sub sp, sp, #N", "add x29, sp, #N" - compute from zero, so the frame lands
/// at a tiny address and every local access writes into unmapped memory near
/// null.
inline std::string Xsp(u32 r) {
    return "c->x[" + std::to_string(r) + "]";
}

// Append C for one instruction. Returns false if the instruction terminates the block.
/// Translate one AArch64 instruction to C.
///
/// Returns true if the block stays open. `unhandled`, when supplied, reports
/// whether this instruction fell through the decoder to recomp_unhandled -
/// which the return value cannot express, because an unhandled instruction is
/// stepped over and leaves the block open exactly like a translated one. Without
/// this signal the emitter cannot tell a full translation from a stream of
/// fallbacks, which is how a whole module of misdecoded ARM32 once passed for a
/// successful export.
// Emit one baseline S/D addition/subtraction from raw uint64_t _a/_b to _v.
// Three guard bits plus sticky alignment preserve exact rounding, including
// cancellation. No host FP arithmetic or host FP state is involved.
/// Emit a native host-FP fast path in front of a soft-float value body.
///
/// The guest is AArch64 and, on iOS, so is the host. With FPCR in its default
/// configuration the host FPU is bit-identical to the guest for these
/// operations: same IEEE-754 rounding, same signed zeros, and - because both
/// are ARM - the same NaN propagation order. The soft-float body spends
/// hundreds of integer operations per lane on work one FMLA does natively,
/// which the device profile showed to be the single largest cost in the frame.
///
/// The fast path is refused whenever the guest has selected a non-default
/// rounding mode, flush-to-zero, default-NaN, alternative half precision, or
/// enabled any exception trap - every FPCR bit that would make the host
/// disagree. In those cases the soft-float body runs exactly as before.
///
/// It is restricted to AArch64 hosts on purpose. On x86-64 the NaN chosen by a
/// multiply-add differs from ARM's order, so a native path there would change
/// results relative to the desktop reference export. Keeping it ARM-only means
/// the exported module stays bit-identical on the machine it was verified on.
///
/// Known limitation: FPSR exception flags are not raised on this path. They are
/// sticky status bits, so a guest that reads FPSR after arithmetic that only
/// took the fast path sees them clear. Define RECOMP_NO_NATIVE_FP to compile
/// the generated module with the soft-float path only, which restores flag
/// behaviour exactly.
///
/// `op` is one of "add", "sub", "div" or "fma"; "fma" reads the addend from _z.
inline std::string EmitFPNativeValue(bool dbl, const char* op) {
    const std::string ft = dbl ? "double" : "float";
    const std::string it = dbl ? "uint64_t" : "uint32_t";
    const std::string fma = dbl ? "__builtin_fma" : "__builtin_fmaf";
    const std::string sz = dbl ? "8" : "4";
    std::string expr;
    if (!std::strcmp(op, "add")) expr = "_na+_nb";
    else if (!std::strcmp(op, "sub")) expr = "_na-_nb";
    else if (!std::strcmp(op, "div")) expr = "_na/_nb";
    else expr = fma + "(_na,_nb,_nz)";
    std::string s =
        "\n#if defined(__aarch64__) && !defined(RECOMP_NO_NATIVE_FP)\n"
        "if(!(c->fpcr&0x07C8FF07ULL)){" + ft + " _na,_nb,_nr;" + it + " _nt;"
        "_nt=(" + it + ")_a;memcpy(&_na,&_nt," + sz + ");"
        "_nt=(" + it + ")_b;memcpy(&_nb,&_nt," + sz + ");";
    if (!std::strcmp(op, "fma"))
        s += ft + " _nz;_nt=(" + it + ")_z;memcpy(&_nz,&_nt," + sz + ");";
    s += "_nr=" + expr + ";memcpy(&_nt,&_nr," + sz + ");_v=(uint64_t)_nt;} else\n"
         "#endif\n";
    return s;
}

inline std::string EmitFPAddSubValue(bool dbl, bool subtract) {
    return EmitFPNativeValue(dbl, subtract ? "sub" : "add") +
        "{ const unsigned _f=" + std::string(dbl?"52":"23") +
        ";const uint64_t _hidden=1ULL<<_f,_frac=_hidden-1,_quiet=_hidden>>1,_exp="+
        (dbl?"0x7ff0000000000000ULL":"0x7f800000ULL")+",_sign="+
        (dbl?"0x8000000000000000ULL":"0x80000000ULL")+";"
        "unsigned _mode=(unsigned)(c->fpcr>>22)&3;"
        "if(c->fpcr&(1ULL<<24)){if(!(_a&_exp)&&(_a&_frac)){_a&=_sign;c->fpsr|=128;}"
        "if(!(_b&_exp)&&(_b&_frac)){_b&=_sign;c->fpsr|=128;}}"
        "int _an=(_a&_exp)==_exp&&(_a&_frac),_bn=(_b&_exp)==_exp&&(_b&_frac);"
        "unsigned _sa=!!(_a&_sign),_sb="+std::string(subtract?"!(_b&_sign)":"!!(_b&_sign)")+";"
        "if(_an||_bn){int _as=_an&&!(_a&_quiet),_bs=_bn&&!(_b&_quiet);"
        "_v=(_as?_a:_bs?_b:_an?_a:_b)|_quiet;if(_as||_bs){c->fpsr|=1;}"
        "if(c->fpcr&(1ULL<<25)){_v=_exp|_quiet;}}"
        "else if((_a&_exp)==_exp||(_b&_exp)==_exp){"
        "if((_a&_exp)==_exp&&(_b&_exp)==_exp&&_sa!=_sb){_v=_exp|_quiet;c->fpsr|=1;}"
        "else _v=_exp|(((_a&_exp)==_exp?_sa:_sb)?_sign:0);}"
        "else {unsigned _ea=(unsigned)((_a&_exp)>>_f),_eb=(unsigned)((_b&_exp)>>_f);"
        "uint64_t _ma=(_a&_frac)|(_ea?_hidden:0),_mb=(_b&_frac)|(_eb?_hidden:0);"
        "if(!_ea){_ea=1;}if(!_eb){_eb=1;}"
        "if(_ea<_eb||(_ea==_eb&&_ma<_mb)){unsigned _t=_ea;_ea=_eb;_eb=_t;_t=_sa;_sa=_sb;_sb=_t;"
        "uint64_t _u=_ma;_ma=_mb;_mb=_u;}"
        "_ma<<=3;_mb<<=3;unsigned _distance=_ea-_eb;"
        "if(_distance>=64)_mb=!!_mb;else if(_distance)_mb=(_mb>>_distance)|!!(_mb&((1ULL<<_distance)-1));"
        "uint64_t _magnitude=_sa==_sb?_ma+_mb:_ma-_mb;"
        "if(!_magnitude)_v=(_sa==_sb?_sa:_mode==2)?_sign:0;"
        "else {if(_magnitude>=(_hidden<<4)){_magnitude=(_magnitude>>1)|(_magnitude&1);++_ea;}"
        "while(_magnitude<(_hidden<<3)&&_ea>1){_magnitude<<=1;--_ea;}"
        "int _tiny=_ea==1&&_magnitude<(_hidden<<3);"
        "if(_tiny&&(c->fpcr&(1ULL<<24))){c->fpsr|=8;_v=0;}"
        "else {unsigned _lost=(unsigned)(_magnitude&7);uint64_t _rounded=_magnitude>>3;"
        "if(_lost){c->fpsr|=16;if(_tiny)c->fpsr|=8;"
        "if((_mode==0&&(_lost>4||(_lost==4&&(_rounded&1))))||(_mode==1&&!_sa)||(_mode==2&&_sa))++_rounded;}"
        "if(_rounded>=(_hidden<<1)){_rounded>>=1;++_ea;}"
        "if(_ea>=(unsigned)(_exp>>_f)){c->fpsr|=20;_v=(_mode==0||(_mode==1&&!_sa)||(_mode==2&&_sa))?_exp:_exp-1;}"
        "else _v=((_rounded>=_hidden?(uint64_t)_ea:0)<<_f)|(_rounded&_frac);}"
        "if(_sa)_v|=_sign;}}}";
}

// Generated exact product/addend lattice. Inputs _a,_b,_z and output _v are
// IEEE bit patterns. A multiply supplies a same-sign zero addend so its zero
// result has the product sign in every rounding mode.
inline std::string EmitFPDivideValue(bool dbl) {
    return EmitFPNativeValue(dbl, "div") +
        "{ const unsigned _f="+std::string(dbl?"52":"23")+",_bias="+(dbl?"1023":"127")+R"C(;
const uint64_t _hidden=1ULL<<_f,_frac=_hidden-1,_quiet=_hidden>>1;
const uint64_t _exp=((uint64_t)(2*_bias+1))<<_f,_sign=1ULL<<(_f+(_f==52?11:8));
const unsigned _mode=(unsigned)(c->fpcr>>22)&3;unsigned _negative=!!((_a^_b)&_sign);
if(c->fpcr&(1ULL<<24)) {
 if(!(_a&_exp)&&(_a&_frac)){_a&=_sign;c->fpsr|=128;}
 if(!(_b&_exp)&&(_b&_frac)){_b&=_sign;c->fpsr|=128;}
}
int _an=(_a&_exp)==_exp&&(_a&_frac),_bn=(_b&_exp)==_exp&&(_b&_frac);
int _as=_an&&!(_a&_quiet),_bs=_bn&&!(_b&_quiet);
int _ai=(_a&~_sign)==_exp,_bi=(_b&~_sign)==_exp,_az=!(_a&~_sign),_bz=!(_b&~_sign);
if(_an||_bn){_v=(_as?_a:_bs?_b:_an?_a:_b)|_quiet;if(_as||_bs)c->fpsr|=1;
 if(c->fpcr&(1ULL<<25))_v=_exp|_quiet;
}else if((_ai&&_bi)||(_az&&_bz)){_v=_exp|_quiet;c->fpsr|=1;}
else if(_ai||_bz){_v=_exp|(_negative?_sign:0);if(!_ai)c->fpsr|=2;}
else if(_az||_bi)_v=_negative?_sign:0;
else {
 unsigned _ea=(unsigned)((_a&_exp)>>_f),_eb=(unsigned)((_b&_exp)>>_f);
 uint64_t _ma=(_a&_frac)|(_ea?_hidden:0),_mb=(_b&_frac)|(_eb?_hidden:0);
 int _ae=_ea?(int)_ea-(int)_bias:1-(int)_bias,_be=_eb?(int)_eb-(int)_bias:1-(int)_bias;
 while(_ma<_hidden){_ma<<=1;--_ae;}while(_mb<_hidden){_mb<<=1;--_be;}
 int _e=_ae-_be,_emin=1-(int)_bias;if(_ma<_mb){_ma<<=1;--_e;}
 if(_e<_emin&&(c->fpcr&(1ULL<<24))){_v=_negative?_sign:0;c->fpsr|=8;}
 else {
  int _bits=(int)_f+(_e<_emin?_e-_emin:0);uint64_t _mant=0,_rem=_ma-_mb;
  unsigned _roundbit=0,_sticky=0;
  if(_bits>=0){_mant=1;for(int _j=0;_j<_bits;++_j){_mant<<=1;_rem<<=1;if(_rem>=_mb){++_mant;_rem-=_mb;}}
   _rem<<=1;_roundbit=_rem>=_mb;if(_roundbit)_rem-=_mb;_sticky=_rem!=0;
  }else{_roundbit=_bits==-1;_sticky=_bits<-1||_rem!=0;}
  unsigned _lost=_roundbit|_sticky;if(_lost){c->fpsr|=16;if(_e<_emin)c->fpsr|=8;
   if((_mode==0&&_roundbit&&(_sticky||(_mant&1)))||(_mode==1&&!_negative)||(_mode==2&&_negative))++_mant;}
  if(_e<_emin){_e=_emin;}if(_mant>=(_hidden<<1)){_mant>>=1;++_e;}
  if(_e>(int)_bias){c->fpsr|=20;_v=(_mode==0||(_mode==1&&!_negative)||(_mode==2&&_negative))?_exp:_exp-1;}
  else _v=(_mant>=_hidden?(uint64_t)(_e+(int)_bias)<<_f:0)|(_mant&_frac);
  if(_negative)_v|=_sign;
 }
}
}
)C";
}

// Architectural reciprocal estimate. The estimate is deliberately computed
// with the integer table algorithm from the ARM pseudocode.
inline std::string EmitFPRecipEstimateValue(bool dbl) {
    std::string s = "{ const unsigned _f=" + std::string(dbl ? "52" : "23") +
                    ",_bias=" + (dbl ? "1023" : "127") + ";";
    s += R"C(
const int _emin=1-(int)_bias;const uint64_t _hidden=1ULL<<_f,_frac=_hidden-1,_quiet=_hidden>>1;
const uint64_t _exp=((uint64_t)(2*_bias+1))<<_f,_sign=1ULL<<(_f+(_f==52?11:8));
unsigned _eraw=(unsigned)((_x&_exp)>>_f);uint64_t _fb=_x&_frac;unsigned _negative=!!(_x&_sign);
if(_eraw==2*_bias+1){
 if(_fb){if(!(_x&_quiet))c->fpsr|=1;_v=_x|_quiet;if(c->fpcr&(1ULL<<25))_v=_exp|_quiet;}
 else _v=_negative?_sign:0;
}else if(!_eraw&&!_fb){c->fpsr|=2;_v=(_x&_sign)|_exp;}
else if(!_eraw&&_fb&&(c->fpcr&(1ULL<<24))){c->fpsr|=130;_v=(_x&_sign)|_exp;}
else {
 uint64_t _mant;int _e;
 if(_eraw){_mant=_hidden|_fb;_e=(int)_eraw-(int)_bias;}
 else{_mant=_fb;_e=_emin;while(!(_mant&_hidden)){_mant<<=1;--_e;}}
 if(_e<_emin-2){
  unsigned _mode=(unsigned)(c->fpcr>>22)&3;
  unsigned _to_inf=_mode==0||(_mode==1&&!_negative)||(_mode==2&&_negative);
  c->fpsr|=20;_v=(_x&_sign)|(_to_inf?_exp:_exp-1);
 }else if((c->fpcr&(1ULL<<24))&&_e>=-_emin){c->fpsr|=8;_v=_x&_sign;}
 else {
  unsigned _index=(unsigned)(_mant>>(_f-8));uint64_t _q=_index*2+1;
  uint64_t _estimate=(uint8_t)((((1ULL<<19)/_q)+1)/2);_estimate<<=_f-8;
  int _re=-(_e+1);
  if(_re<_emin){if(_re==_emin-1){_estimate|=_hidden;_estimate>>=1;}
   else{_estimate|=_hidden;_estimate>>=2;++_re;}}
  _v=(_x&_sign)|((uint64_t)(_re+(int)_bias)<<_f)|(_estimate&_frac);
 }
}
}
)C";
    return s;
}

// Architectural reciprocal-square-root estimate. The estimate is deliberately
// computed with the integer table algorithm from the ARM pseudocode: host
// rsqrt instructions do not guarantee the same eight result bits.
inline std::string EmitFPRSqrtEstimateValue(bool dbl) {
    std::string s = "{ const unsigned _f=" + std::string(dbl ? "52" : "23") +
                    ",_bias=" + (dbl ? "1023" : "127") + ";";
    s += R"C(
const uint64_t _hidden=1ULL<<_f,_frac=_hidden-1,_quiet=_hidden>>1;
const uint64_t _exp=((uint64_t)(2*_bias+1))<<_f,_sign=1ULL<<(_f+(_f==52?11:8));
unsigned _eraw=(unsigned)((_x&_exp)>>_f);uint64_t _fb=_x&_frac;
if(!_eraw&&!_fb){c->fpsr|=2;_v=(_x&_sign)|_exp;}
else if(!_eraw&&_fb&&(c->fpcr&(1ULL<<24))){c->fpsr|=130;_v=(_x&_sign)|_exp;}
else if(_eraw==2*_bias+1){
 if(_fb){if(!(_x&_quiet))c->fpsr|=1;_v=_x|_quiet;if(c->fpcr&(1ULL<<25))_v=_exp|_quiet;}
 else if(_x&_sign){c->fpsr|=1;_v=_exp|_quiet;}else _v=0;
}else if(_x&_sign){c->fpsr|=1;_v=_exp|_quiet;}
else {
 uint64_t _mant;int _e;
 if(_eraw){_mant=_hidden|_fb;_e=(int)_eraw-(int)_bias;}
 else{_mant=_fb;_e=1-(int)_bias;while(!(_mant&_hidden)){_mant<<=1;--_e;}}
 unsigned _index=(unsigned)(_mant>>(_f-((_e%2)==0?7:8)));
 uint64_t _q=_index<256?_index*2+1:(_index|1)*2,_b=512;
 while(_q*(_b+1)*(_b+1)<(1ULL<<28))++_b;
 uint64_t _estimate=(uint8_t)((_b+1)/2);int _re=(-(_e+1))>>1;
 _v=((uint64_t)(_re+(int)_bias)<<_f)|((_estimate<<(_f-8))&_frac);
}
}
)C";
    return s;
}

inline std::string EmitFPMulAddValue(bool dbl) {
    std::string s=EmitFPNativeValue(dbl, "fma")+
        "{ const unsigned _f="+std::string(dbl?"52":"23")+
        ",_bias="+(dbl?"1023":"127")+",_count="+(dbl?"67":"9")+
        "; const int _base="+(dbl?"-2148":"-298")+"; uint64_t _p["+
        (dbl?"67":"9")+"]={0},_c["+(dbl?"67":"9")+"]={0};";
    s+=R"C(
const uint64_t _hidden=1ULL<<_f,_frac=_hidden-1,_quiet=_hidden>>1;
const uint64_t _exp=((uint64_t)(2*_bias+1))<<_f,_sign=1ULL<<(_f+(_f==52?11:8));
unsigned _mode=(unsigned)(c->fpcr>>22)&3;
if(c->fpcr&(1ULL<<24)) {
 if(!(_a&_exp)&&(_a&_frac)){_a&=_sign;c->fpsr|=128;}
 if(!(_b&_exp)&&(_b&_frac)){_b&=_sign;c->fpsr|=128;}
 if(!(_z&_exp)&&(_z&_frac)){_z&=_sign;c->fpsr|=128;}
}
int _an=(_a&_exp)==_exp&&(_a&_frac),_bn=(_b&_exp)==_exp&&(_b&_frac),_zn=(_z&_exp)==_exp&&(_z&_frac);
int _as=_an&&!(_a&_quiet),_bs=_bn&&!(_b&_quiet),_zs=_zn&&!(_z&_quiet);
int _ai=(_a&~_sign)==_exp,_bi=(_b&~_sign)==_exp,_zi=(_z&~_sign)==_exp;
int _az=!(_a&~_sign),_bz=!(_b&~_sign),_zz=!(_z&~_sign),_badproduct=(_ai&&_bz)||(_bi&&_az);
unsigned _ps=!!((_a^_b)&_sign),_cs=!!(_z&_sign);
if(_an||_bn||_zn) {
 _v=(_zs?_z:_as?_a:_bs?_b:_zn?_z:_an?_a:_b)|_quiet;
 if(_as||_bs||_zs)c->fpsr|=1;
 if((c->fpcr&(1ULL<<25))||(_zn&&!_zs&&_badproduct))_v=_exp|_quiet;
 if(_zn&&!_zs&&_badproduct)c->fpsr|=1;
}else if(_badproduct||(_zi&&(_ai||_bi)&&_ps!=_cs)){_v=_exp|_quiet;c->fpsr|=1;}
else if(_zi||_ai||_bi)_v=_exp|((_zi?_cs:_ps)?_sign:0);
else {
 unsigned _ea=(unsigned)((_a&_exp)>>_f),_eb=(unsigned)((_b&_exp)>>_f),_ez=(unsigned)((_z&_exp)>>_f);
 uint64_t _ma=(_a&_frac)|(_ea?_hidden:0),_mb=(_b&_frac)|(_eb?_hidden:0),_mz=(_z&_frac)|(_ez?_hidden:0);
 uint64_t _a0=(uint32_t)_ma,_a1=_ma>>32,_b0=(uint32_t)_mb,_b1=_mb>>32;
 uint64_t _lo=_a0*_b0,_x=_a0*_b1,_y=_a1*_b0,_hi=_a1*_b1+(_x>>32)+(_y>>32);
 uint64_t _old=_lo;_lo+=_x<<32;_hi+=_lo<_old;_old=_lo;_lo+=_y<<32;_hi+=_lo<_old;
 unsigned _shift=(_ea?_ea-1:0)+(_eb?_eb-1:0),_j=_shift/64,_s=_shift%64;
 _p[_j]=_lo<<_s;_p[_j+1]=(_hi<<_s)|(_s?_lo>>(64-_s):0);
 if(_s&&_j+2<_count)_p[_j+2]=_hi>>(64-_s);
 _shift=(_ez?_ez-1:0)+_bias+_f-1;_j=_shift/64;_s=_shift%64;
 _c[_j]=_mz<<_s;if(_s)_c[_j+1]=_mz>>(64-_s);
 unsigned _negative=_ps;
 if(_ps!=_cs) {
  int _compare=0;for(int _k=(int)_count-1;_k>=0;--_k){if(_p[_k]!=_c[_k]){_compare=_p[_k]>_c[_k]?1:-1;break;}}
  _negative=_compare>=0?_ps:_cs;uint64_t _borrow=0;
  for(unsigned _k=0;_k<_count;++_k){uint64_t _left=_compare>=0?_p[_k]:_c[_k],_right=_compare>=0?_c[_k]:_p[_k];
   uint64_t _diff=_left-_right,_next=(_left<_right)||(_diff<_borrow);_p[_k]=_diff-_borrow;_borrow=_next;}
 }else {uint64_t _carry=0;for(unsigned _k=0;_k<_count;++_k){uint64_t _sum=_p[_k]+_c[_k],_next=_sum<_p[_k];
  uint64_t _total=_sum+_carry;_carry=_next||_total<_sum;_p[_k]=_total;}}
 int _top=(int)(_count*64)-1;while(_top>=0&&!((_p[(unsigned)_top/64]>>(_top%64))&1))--_top;
 if(_top<0)_v=((_zz&&(_az||_bz)&&_ps==_cs)?_ps:_mode==2)?_sign:0;
 else {int _e=_top+_base,_emin=1-(int)_bias;
  if(_e<_emin&&(c->fpcr&(1ULL<<24))){c->fpsr|=8;_v=_negative?_sign:0;}
  else {int _cut=_top-(int)_f,_mincut=_emin-(int)_f-_base;if(_cut<_mincut)_cut=_mincut;
   unsigned _index=(unsigned)_cut/64,_offset=(unsigned)_cut%64;
   uint64_t _mant=_p[_index]>>_offset;if(_offset&&_index+1<_count)_mant|=_p[_index+1]<<(64-_offset);
   unsigned _roundbit=(unsigned)((_p[(unsigned)(_cut-1)/64]>>((_cut-1)%64))&1),_sticky=0;
   for(int _k=0;_k<_cut-1;++_k)_sticky|=(unsigned)((_p[(unsigned)_k/64]>>(_k%64))&1);
   unsigned _lost=_roundbit|_sticky;if(_lost){c->fpsr|=16;if(_e<_emin)c->fpsr|=8;
    if((_mode==0&&_roundbit&&(_sticky||(_mant&1)))||(_mode==1&&!_negative)||(_mode==2&&_negative))++_mant;}
   if(_e<_emin){_e=_emin;}if(_mant>=(_hidden<<1)){_mant>>=1;++_e;}
   if(_e>(int)_bias){c->fpsr|=20;_v=(_mode==0||(_mode==1&&!_negative)||(_mode==2&&_negative))?_exp:_exp-1;}
   else _v=(_mant>=_hidden?(uint64_t)(_e+(int)_bias)<<_f:0)|(_mant&_frac);
   if(_negative)_v|=_sign;
  }
 }
}
}
)C";
    return s;
}

inline bool Translate(u32 i, u64 pc, std::string& out, bool* unhandled = nullptr) {
    if (unhandled) {
        *unhandled = false;
    }
    char buf[256];
    // Appends in place. Taking a string_view and appending piecewise (rather
    // than `out += "    " + s + "\n"`) matters at scale: that expression built
    // two temporary std::strings per emitted line, and a full title is tens of
    // millions of lines - it was the single largest cost in AOT translation.
    auto put = [&](std::string_view s) {
        out.append("    ", 4);
        out.append(s);
        out.push_back('\n');
    };
    // Every "we cannot translate this" path goes through here. Three sites used
    // to emit the call by hand and return early, which meant they never set the
    // coverage flag - the exclusives alone were 5,036 instructions per export
    // that the static coverage figure silently did not count.
    auto put_unhandled = [&]() {
        if (unhandled) {
            *unhandled = true;
        }
        char b[128];
        snprintf(b, sizeof b,
                 "recomp_unhandled(c,0x%08xU,g_module_base+0x%llxULL); if(c->halted) return;", i,
                 (unsigned long long)pc);
        put(b);
    };
    // S/D FRECPE and FRSQRTE share one lane core; U selects reciprocal sqrt.
    // Decode before the legacy conversion class, whose opcode is shared here.
    const bool estimate_scalar = (i & 0xDFBFFC00u) == 0x5EA1D800u;
    const bool estimate_vector = (i & 0x9FBFFC00u) == 0x0EA1D800u;
    if (estimate_scalar || estimate_vector) {
        const bool dbl = (i & (1u << 22)) != 0;
        const bool q = (i & (1u << 30)) != 0;
        if (estimate_vector && dbl && !q) { put_unhandled(); return true; }
        const unsigned rn = (i >> 5) & 31, rd = i & 31;
        const unsigned width = dbl ? 64 : 32;
        const unsigned bytes = estimate_scalar ? width / 8 : q ? 16 : 8;
        const unsigned lanes = bytes / (width / 8);
        const std::string ty = dbl ? "uint64_t" : "uint32_t";
        std::string s = "{ " + ty + " _a[" + std::to_string(lanes) + "],_r[" +
                        std::to_string(lanes) + "]; memcpy(_a,c->vreg[" +
                        std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
        s += "for(unsigned _i=0;_i<" + std::to_string(lanes) + ";++_i) _r[_i]=(" + ty +
             ")recomp_fp_estimate(c,_a[_i]," + std::to_string(width) + "," +
             std::to_string((i >> 29) & 1) + "); ";
        s += "memset(c->vreg[" + std::to_string(rd) + "],0,16); memcpy(c->vreg[" +
             std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
        put(s);
        return true;
    }
    const u64 next = pc + 4;

    if ((i & 0xFFFFF01F) == 0xD503201F) { put("/* nop/hint */"); return true; }

    // Memory barriers: DSB, DMB, ISB and CLREX. The generated runtime executes
    // one guest thread on one host thread, so there is no other observer for
    // these to order against and nothing to synchronise - they are genuinely
    // no-ops here. Under Core::ArmRecomp, where real threads exist, these need
    // the host's own barriers instead; noted so the assumption stays visible.
    // Emitting nothing at all is only right for the standalone runtime, which
    // drives one guest thread on one host thread and so has no second observer
    // to order against. Under Core::ArmRecomp the guest really is
    // multi-threaded, and a comment does not stop the host compiler reordering
    // the recomp_load/recomp_store calls around it - so a publish pattern (fill
    // an object, barrier, store the pointer) can be observed pointer-first by
    // another thread, surfacing much later as a live vtable pointer reading
    // back as zero.
    // CLREX before the barrier group: it shares their encoding shape but has to
    // drop the exclusive mark, and treating it as a plain barrier left a stale
    // mark that could make a later unrelated STXR succeed.
    if ((i & 0xFFFFF0FF) == 0xD503305F) { put("recomp_clrex(c);"); return true; }

    if ((i & 0xFFFFF0FF) == 0xD50330DF) {
        snprintf(buf, sizeof buf, "recomp_barrier(); c->pc=g_module_base+0x%llxULL; return;", (unsigned long long)next);
        put(buf); return false;
    }
    if ((i & 0xFFFFF01F) == 0xD503301F) { put("recomp_barrier();"); return true; }

    if ((i & 0x1F800000) == 0x12800000) { // MOVZ/MOVN/MOVK
        u32 sf = i >> 31, opc = (i >> 29) & 3, hw = (i >> 21) & 3, imm16 = (i >> 5) & 0xFFFF, rd = i & 31;
        if (rd != 31) {
            u64 shift = (u64)hw * 16;
            if (opc == 2) {
                snprintf(buf, sizeof buf, "c->x[%u] = 0x%llxULL;", rd, (unsigned long long)((u64)imm16 << shift));
                put(buf);
            } else if (opc == 0) {
                u64 v = ~((u64)imm16 << shift); if (!sf) v &= 0xFFFFFFFF;
                snprintf(buf, sizeof buf, "c->x[%u] = 0x%llxULL;", rd, (unsigned long long)v); put(buf);
            } else if (opc == 3) {
                snprintf(buf, sizeof buf, "c->x[%u] = (c->x[%u] & ~(0xFFFFULL<<%llu)) | (0x%xULL<<%llu);",
                         rd, rd, (unsigned long long)shift, imm16, (unsigned long long)shift); put(buf);
            }
            if (!sf) { snprintf(buf, sizeof buf, "c->x[%u] &= 0xFFFFFFFFULL;", rd); put(buf); }
        }
        return true;
    }

    if ((i & 0x1F000000) == 0x11000000) { // ADD/SUB immediate
        u32 sf = i >> 31, op = (i >> 30) & 1, S = (i >> 29) & 1, sh = (i >> 22) & 1;
        u32 imm12 = (i >> 10) & 0xFFF, rn = (i >> 5) & 31, rd = i & 31;
        u64 imm = sh ? ((u64)imm12 << 12) : imm12;
        snprintf(buf, sizeof buf, "{ uint64_t _a=c->x[%u], _b=%lluULL; uint64_t _r=%s; ", rn,
                 (unsigned long long)imm, op ? "_a-_b" : "_a+_b");
        std::string s = buf;
        if (!sf) s += "_r&=0xFFFFFFFFULL; ";
        // With the flag-setting form, register 31 as the destination is the
        // zero register - that encoding is CMP - so the result is dropped.
        // Without it, register 31 is SP and the write is real.
        if (!(rd == 31 && S)) s += "c->x[" + std::to_string(rd) + "]=_r; ";
        if (S) s += SetFlags(op != 0, "_a", "_b", "_r", sf != 0) + " ";
        s += "}";
        // Register 31 is SP here, not the zero register, so a write to it is
        // real and must not be discarded: dropping it throws away every
        // "sub sp, sp, #N" that opens a stack frame.
        put(s);
        return true;
    }

    // Build a shifted-register operand. LSR/ASR/ROR all used to collapse to a
    // plain ">>" on a uint64_t, which zero-fills - so ASR lost the sign (the
    // "bic w0, w0, w0, asr #31" max(x,0) idiom produced ~0 instead of 0) and
    // ROR dropped the wrapped-around bits entirely (breaking, among other
    // things, software CRC32). The 32-bit forms also have to be narrowed
    // before shifting, whatever the shift amount, because the register may
    // still carry high garbage from an earlier 64-bit write.
    auto shifted_operand = [](const std::string& v, u32 shift, u32 imm6, u32 sf) -> std::string {
        char sb[256];
        if (!sf) {
            const std::string w = "((uint32_t)(" + v + "))";
            switch (shift) {
            case 0: snprintf(sb, sizeof sb, "((uint64_t)(uint32_t)(%s << %u))", w.c_str(), imm6); break;
            case 1: snprintf(sb, sizeof sb, "((uint64_t)(%s >> %u))", w.c_str(), imm6); break;
            case 2: snprintf(sb, sizeof sb, "((uint64_t)(uint32_t)((int32_t)%s >> %u))", w.c_str(), imm6); break;
            default:
                if (imm6 == 0) { snprintf(sb, sizeof sb, "((uint64_t)%s)", w.c_str()); }
                else { snprintf(sb, sizeof sb, "((uint64_t)(uint32_t)((%s >> %u) | (%s << %u)))", w.c_str(), imm6, w.c_str(), 32 - imm6); }
                break;
            }
        } else {
            switch (shift) {
            case 0: snprintf(sb, sizeof sb, "((%s) << %u)", v.c_str(), imm6); break;
            case 1: snprintf(sb, sizeof sb, "((%s) >> %u)", v.c_str(), imm6); break;
            case 2: snprintf(sb, sizeof sb, "((uint64_t)((int64_t)(%s) >> %u))", v.c_str(), imm6); break;
            default:
                if (imm6 == 0) { snprintf(sb, sizeof sb, "(%s)", v.c_str()); }
                else { snprintf(sb, sizeof sb, "(((%s) >> %u) | ((%s) << %u))", v.c_str(), imm6, v.c_str(), 64 - imm6); }
                break;
            }
        }
        return sb;
    };

    if ((i & 0x1F000000) == 0x0A000000) { // logical shifted register
        u32 sf = i >> 31, opc = (i >> 29) & 3, rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        u32 shift = (i >> 22) & 3, imm6 = (i >> 10) & 0x3F, N = (i >> 21) & 1;
        std::string rmv = shifted_operand(Xz(rm), shift, imm6, sf);
        std::string a = Xz(rn);
        const char* lop = opc == 0 ? "&" : opc == 1 ? "|" : opc == 2 ? "^" : "&";
        std::string expr = N ? ("(" + a + " " + lop + " ~" + rmv + ")") : ("(" + a + " " + lop + " " + rmv + ")");
        // opc==3 is ANDS/BICS: it sets the flags, and with rd==31 it is TST,
        // whose only effect IS the flag update. Gating the whole instruction on
        // rd != 31 discarded every TST, leaving the following b.cond to branch
        // on whatever flags happened to be left over from an earlier compare.
        std::string s = "{ uint64_t _r = " + expr + "; ";
        if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
        if (rd != 31) s += "c->x[" + std::to_string(rd) + "] = _r; ";
        if (opc == 3) s += SetFlags(false, "_r", "0", "_r", sf != 0) + " ";
        s += "}";
        put(s);
        return true;
    }

    if ((i & 0x1F200000) == 0x0B000000) { // ADD/SUB shifted register
        u32 sf = i >> 31, op = (i >> 30) & 1, S = (i >> 29) & 1, shift = (i >> 22) & 3, rm = (i >> 16) & 31, imm6 = (i >> 10) & 0x3F, rn = (i >> 5) & 31, rd = i & 31;
        // ROR is reserved for ADD/SUB shifted register - decoding it as a shift
        // would silently invent an instruction the CPU does not have.
        if (shift == 3) {
            put_unhandled();
            return true;
        }
        std::string rmv = shifted_operand(Xz(rm), shift, imm6, sf);
        std::string a = Xz(rn);
        snprintf(buf, sizeof buf, "{ uint64_t _a=%s,_b=%s,_r=%s; ", a.c_str(), rmv.c_str(), op ? "_a-_b" : "_a+_b");
        std::string s = buf; if (!sf) s += "_r&=0xFFFFFFFFULL; ";
        if (rd != 31) s += "c->x[" + std::to_string(rd) + "]=_r; ";
        if (S) s += SetFlags(op != 0, "_a", "_b", "_r", sf != 0) + " ";
        s += "}"; put(s); return true;
    }

    if ((i & 0x1F000000) == 0x10000000) { // ADR/ADRP
        u32 op = i >> 31, rd = i & 31; s64 immhi = (s32)(((i >> 5) & 0x7FFFF) << 13) >> 13; u32 immlo = (i >> 29) & 3;
        if (rd != 31) {
            // These produce data pointers the guest then dereferences, so they
            // have to be real addresses. Everything the static pass knows is
            // module-relative, so the module's load base is added at run time -
            // without it every computed pointer lands near null.
            if (op) { u64 b = (pc & ~0xFFFULL); s64 imm = ((immhi << 2) | immlo) << 12; snprintf(buf, sizeof buf, "c->x[%u]=g_module_base + 0x%llxULL + (int64_t)%lld;", rd, (unsigned long long)b, (long long)imm); }
            else { s64 imm = (immhi << 2) | immlo; snprintf(buf, sizeof buf, "c->x[%u]=g_module_base + 0x%llxULL + (int64_t)%lld;", rd, (unsigned long long)pc, (long long)imm); }
            put(buf);
        }
        return true;
    }

    // PRFM / PRFUM - prefetch hints.
    //
    // A hint has no architectural effect, so emitting nothing is not an
    // approximation, it is the correct translation. These were reaching the
    // "size==3 with opc>=2 is not a defined load/store" path and being sent to
    // the JIT, which is both wrong and expensive: 1,606 instructions in Super
    // a second title, 16.8% of the static gap, for something that does nothing.
    if ((i & 0xFFC00000) == 0xF9800000 ||        // PRFM (unsigned offset)
        (i & 0xFFE00C00) == 0xF8A00800 ||        // PRFM (register offset)
        (i & 0xFFE00C00) == 0xF8800000) {        // PRFUM (unscaled)
        put("/* prfm: hint, no effect */");
        return true;
    }

    // PC-relative literal loads. GPR register 31 discards the value, but still
    // performs the memory access; vector register 31 is an ordinary register.
    if ((i & 0x3B000000) == 0x18000000) {
        const u32 opc = i >> 30, V = (i >> 26) & 1, rt = i & 31;
        const u32 field = (i >> 5) & 0x7FFFF;
        const s64 offset = (field & 0x40000) ? (s64)field - 0x80000 : (s64)field;
        const u64 address = pc + (u64)(offset * 4);
        char literal[32];
        snprintf(literal, sizeof literal, "0x%llxULL", (unsigned long long)address);
        const std::string addr = literal;
        if (!V && opc == 3) {
            put("/* prfm literal: hint, no effect */");
            return true;
        }
        if (opc < 3) {
            const std::string bits = opc == 0 || (!V && opc == 2) ? "32" : "64";
            std::string s = "{ uint64_t _v=recomp_load" + bits + "(c," + addr + "); ";
            if (V) {
                s += "c->vreg[" + std::to_string(rt) + "][0]=_v; ";
                s += "c->vreg[" + std::to_string(rt) + "][1]=" +
                     (opc == 2 ? "recomp_load64(c," + addr + "+8)" : "0") + "; ";
            } else if (rt != 31) {
                // Unsigned subtraction defines signed-word extension without
                // an out-of-range signed cast in the generated C.
                if (opc == 2) s += "_v=(_v^0x80000000ULL)-0x80000000ULL; ";
                s += "c->x[" + std::to_string(rt) + "]=_v; ";
            } else {
                s += "(void)_v; ";
            }
            put(s + "}");
            return true;
        }
    }

    // LDR/STR immediate unsigned offset. Bit 26 is the V bit: it selects the
    // SIMD/FP register file rather than the general registers. Leaving it out
    // of the mask made every "LDR s0, [x1, #8]" compile into an integer load
    // of x0 - silently wrong code rather than an honest fallback. The SIMD
    // form is handled separately further down.
    if ((i & 0x3F000000) == 0x39000000) {
        u32 size = (i >> 30) & 3, opc = (i >> 22) & 3, imm12 = (i >> 10) & 0xFFF, rn = (i >> 5) & 31, rt = i & 31;
        u64 off = (u64)imm12 << size;
        std::string addr = "c->x[" + std::to_string(rn) + "] + " + std::to_string(off);
        const char* ty = size == 0 ? "8" : size == 1 ? "16" : size == 2 ? "32" : "64";
        // opc: 00 store, 01 load (zero-extend), 10 load signed to 64-bit,
        // 11 load signed to 32-bit. Testing (opc & 1) got this wrong in both
        // directions - opc 11 loaded without sign extension, and opc 10 (a
        // *load*) fell into the store branch and wrote to memory instead.
        if (opc == 0) {
            snprintf(buf, sizeof buf, "recomp_store%s(c,%s,%s);", ty, addr.c_str(), Xz(rt).c_str());
            put(buf);
        } else if (opc == 1) {
            if (rt != 31) {
                snprintf(buf, sizeof buf, "c->x[%u]=recomp_load%s(c,%s);", rt, ty, addr.c_str());
                put(buf);
            }
        } else if (size < 3) {
            // Signed load: sign-extend from the accessed width.
            const char* st = size == 0 ? "int8_t" : size == 1 ? "int16_t" : "int32_t";
            if (rt != 31) {
                std::string s = "{ uint64_t _r = (uint64_t)(int64_t)(" + std::string(st) +
                                ")recomp_load" + ty + "(c," + addr + "); ";
                if (opc == 3) s += "_r &= 0xFFFFFFFFULL; ";   // 32-bit destination
                s += "c->x[" + std::to_string(rt) + "] = _r; }";
                put(s);
            }
        } else {
            // size==3 with opc>=2 is not a defined load/store here.
            put_unhandled();
            return true;
        }
        return true;
    }

    u64 t = 0;
    // Only the unconditional forms here: DirectBranchTarget also decodes
    // B.cond/CBZ/TBZ for block discovery, but those have their own translations
    // further down and must not be turned into unconditional jumps.
    if (((i & 0xFC000000) == 0x14000000 || (i & 0xFC000000) == 0x94000000) &&
        DirectBranchTarget(i, pc, t)) {
        if ((i & 0xFC000000) == 0x94000000) { snprintf(buf, sizeof buf, "c->x[30]=g_module_base+0x%llxULL;", (unsigned long long)next); put(buf); }
        // Call the target block directly when it is one we emitted. The
        // declaration is at block scope so a unit needs no list of the blocks it
        // reaches, and `return f(c)` gives the compiler a sibling call, which
        // keeps the chain from growing the stack. Falling out of budget parks
        // the PC and returns exactly as before, so the dispatcher stays the only
        // thing that decides what runs when a chain ends.
        put(ChainTo(t));
        return false;
    }
    if ((i & 0xFFFFFC1F) == 0xD65F0000) { put(ChainIndirect("c->x[30]") + " /* RET */"); return false; }
    if ((i & 0xFFFFFC1F) == 0xD61F0000) { u32 rn = (i >> 5) & 31; put(ChainIndirect("c->x[" + std::to_string(rn) + "]") + " /* BR */"); return false; }
    if ((i & 0xFFFFFC1F) == 0xD63F0000) { u32 rn = (i >> 5) & 31; snprintf(buf, sizeof buf, "c->x[30]=g_module_base+0x%llxULL;", (unsigned long long)next); put(std::string(buf) + " " + ChainIndirect("c->x[" + std::to_string(rn) + "]") + " /* BLR */"); return false; }
    if ((i & 0xFF000010) == 0x54000000) { s64 off = ((s32)((i >> 5) << 13) >> 13); u64 tt = pc + off * 4; u32 cond = i & 15; snprintf(buf, sizeof buf, "if %s { c->pc=g_module_base+0x%llxULL; } else { c->pc=g_module_base+0x%llxULL; } return;", Cond(cond).c_str(), (unsigned long long)tt, (unsigned long long)next); put(buf); return false; }
    if ((i & 0x7E000000) == 0x34000000) { u32 sf = i >> 31; bool nz = (i >> 24) & 1; u32 rt = i & 31; s64 off = ((s32)(((i >> 5) & 0x7FFFF) << 13) >> 13); u64 tt = pc + off * 4; std::string v = sf ? Xz(rt) : Wz(rt); snprintf(buf, sizeof buf, "if ((%s)%s0) { c->pc=g_module_base+0x%llxULL; } else { c->pc=g_module_base+0x%llxULL; } return;", v.c_str(), nz ? "!=" : "==", (unsigned long long)tt, (unsigned long long)next); put(buf); return false; }
    if ((i & 0x7E000000) == 0x36000000) { bool nz = (i >> 24) & 1; u32 b = ((i >> 31) << 5) | ((i >> 19) & 31); u32 rt = i & 31; s64 off = ((s32)(((i >> 5) & 0x3FFF) << 18) >> 18); u64 tt = pc + off * 4; snprintf(buf, sizeof buf, "if (((%s>>%u)&1)%s0) { c->pc=g_module_base+0x%llxULL; } else { c->pc=g_module_base+0x%llxULL; } return;", Xz(rt).c_str(), b, nz ? "!=" : "==", (unsigned long long)tt, (unsigned long long)next); put(buf); return false; }
    if ((i & 0xFFE0001F) == 0xD4000001) { u32 imm = (i >> 5) & 0xFFFF; snprintf(buf, sizeof buf, "c->pc=g_module_base+0x%llxULL; c->pending_svc=%uULL; recomp_svc(c,%u); return;", (unsigned long long)next, imm, imm); put(buf); return false; }
    if ((i & 0xFFE0001F) == 0xD4200000) {
        const u32 imm = (i >> 5) & 0xFFFF;
        snprintf(buf, sizeof buf, "/* brk #0x%x */ c->pc=g_module_base+0x%llxULL; c->halted=RECOMP_HALT_BREAKPOINT; return;",
                 imm, (unsigned long long)pc);
        put(buf);
        return false;
    }

    // STP/LDP - load/store pair. Every non-leaf AArch64 function opens and
    // closes with these, so without them a real game stops at its first
    // prologue. Covers the signed-offset, pre-index and post-index forms for
    // both 32- and 64-bit operands.
    // Bit 26 (V) must be clear here; the SIMD/FP pair form is handled below.
    if ((i & 0x3E000000) == 0x28000000) {
        const u32 opc = i >> 30;            // 0 = 32-bit, 2 = 64-bit
        const bool is_load = (i >> 22) & 1;
        const u32 mode = (i >> 23) & 3;     // 1 post-index, 2 signed offset, 3 pre-index
        const u32 rt2 = (i >> 10) & 31, rn = (i >> 5) & 31, rt = i & 31;
        s32 imm7 = (s32)((i >> 15) & 0x7F);
        if (imm7 & 0x40) imm7 |= ~0x7F;     // sign-extend 7 bits
        if ((opc == 0 || opc == 2) && mode >= 1 && mode <= 3) {
            const u32 sz = (opc == 2) ? 8 : 4;
            const s64 off = (s64)imm7 * sz;
            std::string s = "{ uint64_t _b=c->x[" + std::to_string(rn) + "]; ";
            // Pre-index and post-index both write the new base back; only
            // pre-index applies the offset before the access.
            const char* addr = (mode == 1) ? "_b" : "(_b+_o)";
            s += "int64_t _o=" + std::to_string((long long)off) + "; ";
            const std::string w = std::to_string(sz * 8);
            if (is_load) {
                // Both words come back through temporaries. Rt/Rt2 == 31 is XZR
                // here, so that half is simply not written back - writing it
                // would land on c->x[31], which is where SP lives. The load
                // itself still happens, which is what the architecture does.
                s += "{ uint64_t _p0,_p1; recomp_ldp" + w + "(c," + addr + ",&_p0,&_p1); ";
                if (rt != 31)  s += "c->x[" + std::to_string(rt) + "]=_p0; ";
                if (rt2 != 31) s += "c->x[" + std::to_string(rt2) + "]=_p1; ";
                s += "} ";
            } else {
                s += "recomp_stp" + w + "(c," + addr + "," +
                     (rt == 31 ? std::string("(uint64_t)0") : ("c->x[" + std::to_string(rt) + "]")) +
                     "," +
                     (rt2 == 31 ? std::string("(uint64_t)0") : ("c->x[" + std::to_string(rt2) + "]")) +
                     "); ";
            }
            if (mode == 1 || mode == 3) {
                s += "c->x[" + std::to_string(rn) + "]=_b+_o; ";
            }
            s += "}";
            put(s);
            return true;
        }
    }

    // Logical immediate: AND/ORR/EOR/ANDS. By far the largest single gap in
    // real code - the immediate is a repeating bit pattern, not a literal.
    if ((i & 0x1F800000) == 0x12000000) {
        const u32 sf = i >> 31, opc = (i >> 29) & 3, N = (i >> 22) & 1;
        const u32 immr = (i >> 16) & 0x3F, imms = (i >> 10) & 0x3F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        u64 imm = 0;
        if (DecodeBitMasks(N, imms, immr, sf != 0, imm)) {
            const char* op = (opc == 0 || opc == 3) ? "&" : (opc == 1 ? "|" : "^");
            std::string s = "{ uint64_t _r = " + Xz(rn) + " " + op + " 0x" ;
            char hb[32]; snprintf(hb, sizeof hb, "%llxULL", (unsigned long long)imm);
            s += hb; s += "; ";
            if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
            // Rd==31 is SP for AND/ORR/EOR immediate and only reads as XZR
            // for ANDS (opc==3). Treating it as XZR everywhere silently
            // dropped every "and sp, xN, #imm" stack realignment.
            if (!(rd == 31 && opc == 3)) s += "c->x[" + std::to_string(rd) + "] = _r; ";
            if (opc == 3) s += SetFlags(false, "_r", "0", "_r", sf != 0) + " ";
            s += "}";
            put(s);
            return true;
        }
    }

    // Bitfield: UBFM/SBFM/BFM - the encoding behind LSL/LSR/ASR/UBFX/SBFX.
    if ((i & 0x1F800000) == 0x13000000) {
        const u32 sf = i >> 31, opc = (i >> 29) & 3;
        const u32 immr = (i >> 16) & 0x3F, imms = (i >> 10) & 0x3F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const u32 width = sf ? 64 : 32;

        // BFM (opc 1) - the encoding behind BFI and BFXIL. Unlike UBFM/SBFM it
        // merges into the destination, leaving the bits outside the inserted
        // field untouched, so it needs an explicit mask rather than a shift.
        if (rd != 31 && opc == 1 && immr < width && imms < width) {
            u32 nbits, src_shift, dst_pos;
            if (imms >= immr) {          // BFXIL: extract to the bottom of Rd
                nbits = imms - immr + 1;
                src_shift = immr;
                dst_pos = 0;
            } else {                     // BFI: insert at (width - immr)
                nbits = imms + 1;
                src_shift = 0;
                dst_pos = width - immr;
            }
            if (nbits + dst_pos <= width) {
                // Build the field mask at 64 bits; nbits is < 64 here because
                // dst_pos + nbits <= width <= 64 and a 64-wide field would
                // make the shift below undefined.
                const u64 field = (nbits >= 64) ? ~0ULL : ((1ULL << nbits) - 1ULL);
                const u64 mask = field << dst_pos;
                char mb[48], fb[48];
                snprintf(mb, sizeof mb, "0x%llxULL", (unsigned long long)mask);
                snprintf(fb, sizeof fb, "0x%llxULL", (unsigned long long)field);
                std::string s = "{ uint64_t _s = " + Xz(rn);
                if (src_shift) s += " >> " + std::to_string(src_shift);
                s += "; uint64_t _r = (c->x[" + std::to_string(rd) + "] & ~" + mb +
                     ") | ((_s & " + fb + ") << " + std::to_string(dst_pos) + "); ";
                if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
                s += "c->x[" + std::to_string(rd) + "] = _r; }";
                put(s);
                return true;
            }
        }

        // UBFM (opc 2) and SBFM (opc 0).
        if (rd != 31 && opc != 1 && immr < width && imms < width) {
            const char* mask = sf ? "" : " & 0xFFFFFFFFULL";
            std::string src = Xz(rn);
            std::string s = "{ uint64_t _s = " + src + mask + "; uint64_t _r; ";
            if (imms >= immr) {
                // Extract (imms-immr+1) bits starting at immr.
                const u32 nbits = imms - immr + 1;
                s += "_r = (_s >> " + std::to_string(immr) + ")";
                if (nbits < 64) s += " & ((1ULL << " + std::to_string(nbits) + ") - 1)";
                s += "; ";
                if (opc == 0 && nbits < 64) { // SBFM: sign-extend from nbits
                    s += "if (_r & (1ULL << " + std::to_string(nbits - 1) + ")) _r |= ~((1ULL << " +
                         std::to_string(nbits) + ") - 1); ";
                }
            } else {
                // Insert: bits [0..imms] moved to start at (width-immr).
                const u32 nbits = imms + 1;
                const u32 shift = width - immr;
                s += "_r = (_s";
                if (nbits < 64) s += " & ((1ULL << " + std::to_string(nbits) + ") - 1)";
                s += ") << " + std::to_string(shift) + "; ";
                if (opc == 0 && shift + nbits < 64) {
                    s += "if (_r & (1ULL << " + std::to_string(shift + nbits - 1) +
                         ")) _r |= ~((1ULL << " + std::to_string(shift + nbits) + ") - 1); ";
                }
            }
            if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
            s += "c->x[" + std::to_string(rd) + "] = _r; }";
            put(s);
            return true;
        }
    }

    // Conditional compare: CCMP/CCMN, both the register and 5-bit immediate
    // forms. When the condition holds this behaves as an ordinary compare;
    // when it does not, the flags are loaded straight from the nzcv field.
    // Short-circuit conditionals in C compile to chains of these.
    if ((i & 0x1FE00000) == 0x1A400000 && ((i >> 29) & 1) && ((i >> 10) & 1) == 0 &&
        ((i >> 4) & 1) == 0) {
        const u32 sf = i >> 31, op = (i >> 30) & 1;   // op: 0 = CCMN, 1 = CCMP
        const u32 imm_or_rm = (i >> 16) & 31, cond = (i >> 12) & 15;
        const bool is_imm = ((i >> 11) & 1) != 0;
        const u32 rn = (i >> 5) & 31, nzcv = i & 15;
        const std::string b = is_imm ? (std::to_string(imm_or_rm) + "ULL") : Xz(imm_or_rm);
        std::string s = "{ if " + Cond(cond) + " { ";
        s += "uint64_t _a=" + Xz(rn) + ", _b=" + b + ", _r=" +
             std::string(op ? "_a-_b" : "_a+_b") + "; ";
        if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
        s += SetFlags(op != 0, "_a", "_b", "_r", sf != 0) + " ";
        s += "} else { ";
        s += "c->n=" + std::to_string((nzcv >> 3) & 1) + "; ";
        s += "c->z=" + std::to_string((nzcv >> 2) & 1) + "; ";
        s += "c->c=" + std::to_string((nzcv >> 1) & 1) + "; ";
        s += "c->v=" + std::to_string(nzcv & 1) + "; } }";
        put(s);
        return true;
    }

    // Conditional select: CSEL/CSINC/CSINV/CSNEG.
    // Bit 11 must be clear: with it set this encoding is not a conditional
    // select at all, and decoding it as one silently invents an instruction.
    if ((i & 0x1FE00800) == 0x1A800000) {
        const u32 sf = i >> 31, op = (i >> 30) & 1, o2 = (i >> 10) & 1;
        const u32 rm = (i >> 16) & 31, cond = (i >> 12) & 15, rn = (i >> 5) & 31, rd = i & 31;
        if (rd != 31) {
            std::string a = Xz(rn), b = Xz(rm);
            std::string els = b;
            if (!op && o2) els = "(" + b + " + 1)";            // CSINC
            else if (op && !o2) els = "(~" + b + ")";           // CSINV
            else if (op && o2) els = "((uint64_t)(0 - " + b + "))"; // CSNEG
            std::string s = "{ uint64_t _r = " + Cond(cond) + " ? " +
                            a + " : " + els + "; ";
            if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
            s += "c->x[" + std::to_string(rd) + "] = _r; }";
            put(s);
            return true;
        }
    }

    // Load/store with unscaled 9-bit signed offset (LDUR/STUR) and the
    // pre/post-indexed immediate forms.
    // Again bit 26 (V) must be clear - the SIMD/FP form is handled below.
    // Bit 21 set with mode==0 is the ARMv8.1 LSE atomic group, not LDUR/STUR;
    // decoding those here would read a garbage imm9 out of the register field.
    if ((i & 0x3F000000) == 0x38000000 && ((i >> 24) & 1) == 0 && ((i >> 21) & 1) == 0) {
        const u32 size = i >> 30, opc = (i >> 22) & 3, mode = (i >> 10) & 3;
        const u32 rn = (i >> 5) & 31, rt = i & 31;
        s32 imm9 = (s32)((i >> 12) & 0x1FF);
        if (imm9 & 0x100) imm9 |= ~0x1FF;
        // mode 0 = LDUR/STUR, 1 = post-index, 3 = pre-index
        if ((mode == 0 || mode == 1 || mode == 3) && size <= 3) {
            const u32 bits = 8u << size;
            std::string s = "{ uint64_t _b=c->x[" + std::to_string(rn) + "]; int64_t _o=" +
                            std::to_string((long long)imm9) + "; ";
            const char* addr = (mode == 1) ? "_b" : "(_b+_o)";
            // opc selects the operation, and testing only its low bit gets this
            // wrong in both directions - the same mistake the unsigned-offset
            // handler already documents, never applied here. opc==2 (LDURSW /
            // LDURSB / LDURSH, sign-extending into a 64-bit destination) has
            // bit 0 clear and so was emitted as a STORE, turning tens of
            // thousands of ordinary signed loads into wild writes over live
            // guest memory; opc==3 loaded but never sign-extended.
            const char* signed_cast = size == 0   ? "(int8_t)"
                                      : size == 1 ? "(int16_t)"
                                                  : "(int32_t)";
            if (opc == 0) {
                s += "recomp_store" + std::to_string(bits) + "(c," + addr + "," + Xz(rt) + "); ";
            } else if (opc == 1) {
                if (rt != 31) {
                    s += "c->x[" + std::to_string(rt) + "]=recomp_load" + std::to_string(bits) +
                         "(c," + addr + "); ";
                }
            } else if (size == 3) {
                // opc>=2 with size==3 is PRFUM, a prefetch hint. It has no
                // architectural effect, so emitting nothing is exact - what it
                // must never do is store.
            } else if (rt != 31) {
                s += "c->x[" + std::to_string(rt) + "]=(uint64_t)(int64_t)" + signed_cast +
                     "recomp_load" + std::to_string(bits) + "(c," + addr + "); ";
                // opc==3 sign-extends into a 32-bit destination, so the result
                // is truncated back to W width after the extension.
                if (opc == 3) s += "c->x[" + std::to_string(rt) + "]&=0xFFFFFFFFULL; ";
            }
            if (mode == 1 || mode == 3) s += "c->x[" + std::to_string(rn) + "]=_b+_o; ";
            s += "}";
            put(s);
            return true;
        }
    }

    // Load/store exclusive and acquire/release.
    //
    // The non-exclusive acquire/release forms (LDAR/STLR, o2 set) are ordinary
    // loads and stores as far as this backend is concerned - suyu's memory
    // accessors are already atomic at these widths, and there is no weaker
    // ordering here to fence against - so they are translated directly.
    //
    // The genuinely exclusive forms (LDXR/LDAXR/STXR/STLXR, o2 clear) are not.
    // They used to become a plain load/store with STXR unconditionally
    // reporting success, which is exact only when nothing else can touch the
    // address. Under this backend real guest threads run concurrently, so an
    // always-succeeds STXR makes every compare-and-swap non-atomic: two
    // threads both "win" the same lock, the data it protects is then updated
    // from both, and the next thread to wait on it spins forever. Hand these
    // to the fallback engine, which owns the kernel's real exclusive monitor.
    if ((i & 0x3F000000) == 0x08000000) {
        const u32 size = i >> 30, o2 = (i >> 23) & 1, L = (i >> 22) & 1, o1 = (i >> 21) & 1;
        const u32 rt2 = (i >> 10) & 31, rn = (i >> 5) & 31, rt = i & 31;
        // Pair forms: LDXP/LDAXP/STXP/STLXP. size 00/01 is unallocated here,
        // so only the 32- and 64-bit register widths exist.
        //
        // These carry the lock traffic. A threaded engine implements its mutex
        // as a 128-bit compare-and-swap over {owner, count} or {ptr, tag}, and
        // on a second title the pair forms alone were 73.5% of every
        // transition to the fallback engine once cntpct_el0 was implemented.
        if (o1 && !o2 && (size == 2 || size == 3)) {
            const u32 rbytes = (size == 3) ? 8u : 4u;
            const std::string addr = "c->x[" + std::to_string(rn) + "]";
            const std::string sz = std::to_string(rbytes);
            if (L) {
                // LDXP/LDAXP. Rt takes the low half, Rt2 the high half - the
                // same order DynarmicExclusiveMonitor::ExclusiveRead128 fills,
                // and for the 32-bit form the low and high words of the
                // doubleword at the address.
                std::string s2 = "{ uint64_t _lo,_hi; recomp_ldxp(c," + addr + "," + sz +
                                 ",&_lo,&_hi); ";
                // A W-register destination is written zero-extended.
                const char* cast = (rbytes == 4) ? "(uint32_t)" : "";
                if (rt != 31) {
                    s2 += "c->x[" + std::to_string(rt) + "] = " + cast + "_lo; ";
                }
                if (rt2 != 31) {
                    s2 += "c->x[" + std::to_string(rt2) + "] = " + cast + "_hi; ";
                }
                s2 += "}";
                put(s2);
            } else {
                // STXP/STLXP. Rs (bits 20:16) receives the status, 0 on success.
                const u32 rs = (i >> 16) & 31;
                const std::string lo =
                    (rbytes == 4) ? ("(uint64_t)(uint32_t)" + Xz(rt)) : Xz(rt);
                const std::string hi =
                    (rbytes == 4) ? ("(uint64_t)(uint32_t)" + Xz(rt2)) : Xz(rt2);
                std::string s2 = "{ uint32_t _st = recomp_stxp(c," + addr + "," + sz + "," +
                                 lo + "," + hi + "); ";
                if (rs != 31) {
                    s2 += "c->x[" + std::to_string(rs) + "] = _st; ";
                }
                s2 += "}";
                put(s2);
            }
            return true;
        }
        if (!o1 && rt2 == 31) {
            const u32 bits = 8u << size;
            if (!o2) {
                // LDXR/LDAXR and STXR/STLXR. Both route through the emulator's
                // own exclusive monitor, so an exclusive taken here is visible
                // to a thread running on the fallback JIT.
                //
                // Acquire/release ordering (the LDAXR/STLXR spelling, o0 set)
                // is not modelled separately: suyu's memory accessors are
                // already atomic at these widths and the host is x86-64, whose
                // ordering is strong enough that the fence these imply is a
                // no-op in practice. Noted rather than silently assumed.
                const std::string addr = "c->x[" + std::to_string(rn) + "]";
                if (L) {
                    // LDXR/LDAXR: mark and load.
                    if (rt != 31) {
                        put("c->x[" + std::to_string(rt) + "] = recomp_ldxr(c," + addr + "," +
                            std::to_string(bits / 8) + ");");
                    } else {
                        put("(void)recomp_ldxr(c," + addr + "," + std::to_string(bits / 8) + ");");
                    }
                } else {
                    // STXR/STLXR: Rs receives the status, 0 on success. Rs is
                    // held in the Rt2 field's neighbour (bits 20:16).
                    const u32 rs = (i >> 16) & 31;
                    std::string s2 = "{ uint32_t _st = recomp_stxr(c," + addr + "," +
                                     std::to_string(bits / 8) + "," + Xz(rt) + "); ";
                    if (rs != 31) {
                        s2 += "c->x[" + std::to_string(rs) + "] = _st; ";
                    }
                    s2 += "}";
                    put(s2);
                }
                return true;
            }
            const std::string addr = "c->x[" + std::to_string(rn) + "]";
            if (L) {
                // LDAR
                if (rt != 31) {
                    put("c->x[" + std::to_string(rt) + "] = recomp_load" +
                        std::to_string(bits) + "(c," + addr + ");");
                }
            } else {
                // STLR - no status register.
                put("recomp_store" + std::to_string(bits) + "(c," + addr + "," + Xz(rt) + ");");
            }
            return true;
        }
    }

    // Data-processing 3-source: MADD/MSUB and the widening multiplies.
    if ((i & 0x1F000000) == 0x1B000000) {
        const u32 sf = i >> 31, op54 = (i >> 29) & 3, op31 = (i >> 21) & 7, o0 = (i >> 15) & 1;
        const u32 rm = (i >> 16) & 31, ra = (i >> 10) & 31, rn = (i >> 5) & 31, rd = i & 31;
        if (rd != 31 && op54 == 0) {
            std::string expr;
            if (op31 == 0) {
                // MADD / MSUB
                const std::string prod = "(" + Xz(rn) + " * " + Xz(rm) + ")";
                expr = Xz(ra) + (o0 ? " - " : " + ") + prod;
            } else if (op31 == 1 && !o0) {           // SMADDL
                expr = Xz(ra) + " + (uint64_t)((int64_t)(int32_t)" + Xz(rn) +
                       " * (int64_t)(int32_t)" + Xz(rm) + ")";
            } else if (op31 == 1 && o0) {            // SMSUBL
                expr = Xz(ra) + " - (uint64_t)((int64_t)(int32_t)" + Xz(rn) +
                       " * (int64_t)(int32_t)" + Xz(rm) + ")";
            } else if (op31 == 5 && !o0) {           // UMADDL
                expr = Xz(ra) + " + ((uint64_t)(uint32_t)" + Xz(rn) +
                       " * (uint64_t)(uint32_t)" + Xz(rm) + ")";
            } else if (op31 == 5 && o0) {            // UMSUBL
                expr = Xz(ra) + " - ((uint64_t)(uint32_t)" + Xz(rn) +
                       " * (uint64_t)(uint32_t)" + Xz(rm) + ")";
            } else if (op31 == 2) {                  // SMULH
                expr = "recomp_smulh(" + Xz(rn) + "," + Xz(rm) + ")";
            } else if (op31 == 6) {                  // UMULH
                expr = "recomp_umulh(" + Xz(rn) + "," + Xz(rm) + ")";
            }
            if (!expr.empty()) {
                std::string s = "{ uint64_t _r = " + expr + "; ";
                if (!sf && op31 == 0) s += "_r &= 0xFFFFFFFFULL; ";
                s += "c->x[" + std::to_string(rd) + "] = _r; }";
                put(s);
                return true;
            }
        }
    }

    // ADD/SUB extended register - the form used for pointer arithmetic with a
    // 32-bit index, so extremely common around array and struct accesses.
    if ((i & 0x1F200000) == 0x0B200000) {
        const u32 sf = i >> 31, op = (i >> 30) & 1, S = (i >> 29) & 1;
        const u32 rm = (i >> 16) & 31, option = (i >> 13) & 7, imm3 = (i >> 10) & 7;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        if (imm3 <= 4) {
            std::string ext;
            switch (option) {
            case 0: ext = "(uint64_t)(uint8_t)" + Xz(rm); break;           // UXTB
            case 1: ext = "(uint64_t)(uint16_t)" + Xz(rm); break;          // UXTH
            case 2: ext = "(uint64_t)(uint32_t)" + Xz(rm); break;          // UXTW
            case 3: ext = Xz(rm); break;                                    // UXTX/LSL
            case 4: ext = "(uint64_t)(int64_t)(int8_t)" + Xz(rm); break;   // SXTB
            case 5: ext = "(uint64_t)(int64_t)(int16_t)" + Xz(rm); break;  // SXTH
            case 6: ext = "(uint64_t)(int64_t)(int32_t)" + Xz(rm); break;  // SXTW
            case 7: ext = Xz(rm); break;                                    // SXTX
            default: break;
            }
            if (!ext.empty()) {
                if (imm3) ext = "((" + ext + ") << " + std::to_string(imm3) + ")";
                // Rn is SP here, not the zero register, and so is Rd unless the
                // flag-setting form is used - where it really is the zero
                // register, because that is CMP.
                std::string s = "{ uint64_t _a=" + Xsp(rn) + ", _b=" + ext + "; uint64_t _r=" +
                                std::string(op ? "_a-_b" : "_a+_b") + "; ";
                if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
                if (rd != 31 || !S) s += "c->x[" + std::to_string(rd) + "]=_r; ";
                if (S) s += SetFlags(op != 0, "_a", "_b", "_r", sf != 0) + " ";
                s += "}";
                put(s);
                return true;
            }
        }
    }

    // Load/store with a register offset (the [base, Xm{, extend}] form).
    // Bit 26 (V) clear: general registers only, SIMD/FP form handled below.
    if ((i & 0x3F200C00) == 0x38200800) {
        const u32 size = i >> 30, opc = (i >> 22) & 3;
        const u32 rm = (i >> 16) & 31, option = (i >> 13) & 7, S = (i >> 12) & 1;
        const u32 rn = (i >> 5) & 31, rt = i & 31;
        // opc 00 stores, 01 loads zero-extended, 10 loads sign-extended to 64
        // bits and 11 sign-extended to 32. The signed forms are common - an
        // indexed read of an int32 array compiles to LDRSW with a register
        // offset - so leaving them out put thousands of real loads on the
        // fallback.
        if (size <= 3 && !(opc >= 2 && size == 3)) {
            const u32 bits = 8u << size;
            std::string idx;
            switch (option) {
            case 2: idx = "(uint64_t)(uint32_t)" + Xz(rm); break;          // UXTW
            case 3: idx = Xz(rm); break;                                    // LSL
            case 6: idx = "(uint64_t)(int64_t)(int32_t)" + Xz(rm); break;  // SXTW
            case 7: idx = Xz(rm); break;                                    // SXTX
            default: break;
            }
            if (!idx.empty()) {
                if (S && size) idx = "((" + idx + ") << " + std::to_string(size) + ")";
                const std::string addr = "(c->x[" + std::to_string(rn) + "] + " + idx + ")";
                if (opc == 0) {
                    put("recomp_store" + std::to_string(bits) + "(c," + addr + "," + Xz(rt) + ");");
                } else if (opc == 1) {
                    if (rt != 31) {
                        put("c->x[" + std::to_string(rt) + "] = recomp_load" +
                            std::to_string(bits) + "(c," + addr + ");");
                    }
                } else if (rt != 31) {
                    const char* st = size == 0 ? "int8_t" : size == 1 ? "int16_t" : "int32_t";
                    std::string s = "{ uint64_t _r = (uint64_t)(int64_t)(" + std::string(st) +
                                    ")recomp_load" + std::to_string(bits) + "(c," + addr + "); ";
                    if (opc == 3) s += "_r &= 0xFFFFFFFFULL; ";   // 32-bit destination
                    s += "c->x[" + std::to_string(rt) + "] = _r; }";
                    put(s);
                }
                return true;
            }
        }
    }

    // Non-saturating immediate shifts. Unsigned intermediates implement sign
    // extension and rounding without signed overflow or a shift by 64.
    if ((i & 0x8F800400) == 0x0F000400) {
        const u32 scalar = (i >> 28) & 1, Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 opcode = (i >> 11) & 31, imm = (i >> 16) & 127;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const bool narrow = opcode == 16 || opcode == 17;
        const bool left = opcode == 10;
        const bool insert = opcode == 8 || (left && U);
        const bool round = opcode == 4 || opcode == 6 || opcode == 17;
        const bool acc = opcode == 2 || opcode == 6;
        const bool known = opcode == 0 || opcode == 2 || opcode == 4 || opcode == 6 ||
                           (opcode == 8 && U) || left || (narrow && !U);
        u32 bits = 8;
        while (bits < 64 && imm >= bits * 2) bits *= 2;
        if (known && (!left || U || g_translate_all) && imm >= 8 &&
            (!scalar || (Q && !narrow && bits == 64)) &&
            (scalar || narrow || bits != 64 || Q) && (!narrow || bits < 64)) {
            const u32 sbits = narrow ? bits * 2 : bits;
            const u32 shift = left ? imm - bits : bits * 2 - imm;
            const u32 bytes = scalar ? 8 : Q ? 16 : 8;
            const u32 lanes = narrow ? 64 / bits : bytes * 8 / bits;
            const std::string st = "uint" + std::to_string(sbits) + "_t";
            const std::string dt = "uint" + std::to_string(bits) + "_t";
            std::string s = "{ " + st + " _a[" + std::to_string(lanes) + "]; " + dt +
                            " _d[" + std::to_string(lanes) + "],_r[" + std::to_string(lanes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," +
                 std::to_string(narrow ? 16 : bytes) + "); ";
            s += "memcpy(_d,c->vreg[" + std::to_string(rd) + "]," +
                 std::to_string(narrow ? 8 : bytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";++_i) { uint64_t _v=_a[_i],_z; ";
            if (left) {
                s += "_z=_v<<" + std::to_string(shift) + "; ";
                if (insert) s += "_z|=(uint64_t)_d[_i]&" +
                    std::to_string(shift ? (uint64_t{1} << shift) - 1 : 0) + "ULL; ";
            } else {
                s += shift == 64 ? "_z=0; " : "_z=_v>>" + std::to_string(shift) + "; ";
                if (!U && !narrow) {
                    const uint64_t mask = shift == 64 ? ~uint64_t{0} :
                        (~uint64_t{0} << (bits - shift));
                    s += "if(_v&(1ULL<<" + std::to_string(bits - 1) + ")) _z|=" +
                         std::to_string(mask) + "ULL; ";
                }
                if (round) s += "_z+=(_v>>" + std::to_string(shift - 1) + ")&1; ";
                if (acc) s += "_z+=(uint64_t)_d[_i]; ";
                if (insert) {
                    const uint64_t mask = shift == bits ? ~uint64_t{0} :
                        (~uint64_t{0} << (bits - shift));
                    s += "_z|=(uint64_t)_d[_i]&" + std::to_string(mask) + "ULL; ";
                }
            }
            s += "_r[_i]=(" + dt + ")_z; } ";
            if (!narrow || !Q) s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" +
                std::to_string(rd) + "][1]=0; ";
            s += "memcpy((uint8_t*)c->vreg[" + std::to_string(rd) + "]+" +
                 std::to_string(narrow && Q ? 8 : 0) + ",_r," +
                 std::to_string(narrow ? 8 : bytes) + "); }";
            put(s);
            return true;
        }
    }

    // SHLL widens and shifts by exactly the source width (not SSHLL's range).
    if ((i & 0xBF3FFC00) == 0x2E213800 && ((i >> 22) & 3) != 3) {
        const u32 Q = (i >> 30) & 1, bits = 8u << ((i >> 22) & 3);
        const u32 rn = (i >> 5) & 31, rd = i & 31, lanes = 64 / bits;
        const std::string st = "uint" + std::to_string(bits) + "_t";
        const std::string dt = "uint" + std::to_string(bits * 2) + "_t";
        put("{ " + st + " _a[" + std::to_string(lanes) + "]; " + dt + " _r[" +
            std::to_string(lanes) + "]; memcpy(_a,(uint8_t*)c->vreg[" + std::to_string(rn) +
            "]+" + std::to_string(Q ? 8 : 0) + ",8); for(int _i=0;_i<" + std::to_string(lanes) +
            ";++_i) _r[_i]=(" + dt + ")((uint64_t)_a[_i]<<" + std::to_string(bits) +
            "); memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }");
        return true;
    }

    // SSHR / USHR / SHRN: shift right by an immediate. SSHR and USHR keep the
    // element width; SHRN halves it and writes one half of the destination.
    if ((i & 0x9F80FC00) == 0x0F000400 || (i & 0x9F80FC00) == 0x0F008400) {
        const bool narrow = ((i >> 10) & 0x3F) == 0x21;
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 immh = (i >> 19) & 15, immb = (i >> 16) & 7;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        u32 size = 0;
        bool shaped = true;
        if (immh & 8)       size = 3;
        else if (immh & 4)  size = 2;
        else if (immh & 2)  size = 1;
        else if (immh & 1)  size = 0;
        else                shaped = false;
        if (narrow && (size == 3 || U)) shaped = false;   // no 128-bit source element
        if (shaped) {
            // The shift is relative to the *destination* element in both
            // cases; for SHRN the source is twice that, which is the whole
            // point of the instruction.
            const int dbits = 8 << size;
            const int sbits = narrow ? (dbits * 2) : dbits;
            const u32 shift = (u32)(dbits * 2) - ((immh << 3) | immb);
            if (shift >= 1 && shift <= (u32)dbits) {
                const int ssz = sbits / 8;
                if (narrow) {
                    const int lanes = 8 / (ssz / 2);
                    const std::string sty = "uint" + std::to_string(sbits) + "_t";
                    const std::string dty = "uint" + std::to_string(sbits / 2) + "_t";
                    std::string s = "{ " + sty + " _a[" + std::to_string(lanes) + "]; " + dty +
                                    " _r[" + std::to_string(lanes) + "]; ";
                    s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "],16); ";
                    s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(" + dty +
                         ")(_a[_i]>>" + std::to_string(shift) + "); ";
                    if (!Q) {
                        s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" +
                             std::to_string(rd) + "][1]=0; ";
                    }
                    s += "memcpy((uint8_t*)c->vreg[" + std::to_string(rd) + "]+" +
                         std::to_string(Q ? 8 : 0) + ",_r,8); }";
                    put(s);
                    return true;
                }
                const int bytes = Q ? 16 : 8;
                if (!(size == 3 && !Q)) {
                    const int lanes = bytes / ssz;
                    const std::string uty = "uint" + std::to_string(sbits) + "_t";
                    const std::string ity = "int" + std::to_string(sbits) + "_t";
                    // A shift of the full width is defined here and undefined in
                    // C, so it is folded to the all-sign / all-zero result.
                    const bool full = shift == (u32)sbits;
                    std::string s = "{ " + uty + " _a[" + std::to_string(lanes) + "],_r[" +
                                    std::to_string(lanes) + "]; ";
                    s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) +
                         "); ";
                    s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=";
                    if (U) {
                        s += full ? ("(" + uty + ")0")
                                  : ("(" + uty + ")(_a[_i]>>" + std::to_string(shift) + ")");
                    } else {
                        s += "(" + uty + ")((" + ity + ")_a[_i]>>" +
                             std::to_string(full ? (u32)(sbits - 1) : shift) + ")";
                    }
                    s += "; ";
                    s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" +
                         std::to_string(rd) + "][1]=0; ";
                    s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) +
                         "); }";
                    put(s);
                    return true;
                }
            }
        }
    }

    // SMULL / UMULL and the accumulating forms. The sources are half-width, so
    // Q picks which half of the source registers feeds the full-width result.
    if ((i & 0x9F20FC00) == 0x0E208000 || (i & 0x9F20FC00) == 0x0E20A000 ||
        (i & 0x9F20FC00) == 0x0E20C000) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 size = (i >> 22) & 3, opcode = (i >> 12) & 15;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        if (size != 3) {
            const int sbits = 8 << size, dbits = sbits * 2;
            const int lanes = 64 / sbits;
            const std::string sty =
                std::string(U ? "uint" : "int") + std::to_string(sbits) + "_t";
            const std::string dty =
                std::string(U ? "uint" : "int") + std::to_string(dbits) + "_t";
            // The accumulator is held unsigned: the wrap is defined there, and
            // signed overflow in the generated C would not be.
            const std::string aty = "uint" + std::to_string(dbits) + "_t";
            const int off = Q ? 8 : 0;
            const char* acc = (opcode == 8) ? "+=" : (opcode == 0xA) ? "-=" : "=";
            std::string s = "{ " + sty + " _a[" + std::to_string(lanes) + "],_b[" +
                            std::to_string(lanes) + "]; " + aty + " _r[" +
                            std::to_string(lanes) + "]; ";
            s += "memcpy(_a,(const uint8_t*)c->vreg[" + std::to_string(rn) + "]+" +
                 std::to_string(off) + ",8); ";
            s += "memcpy(_b,(const uint8_t*)c->vreg[" + std::to_string(rm) + "]+" +
                 std::to_string(off) + ",8); ";
            if (opcode != 0xC) {
                s += "memcpy(_r,c->vreg[" + std::to_string(rd) + "],16); ";
            }
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]" +
                 std::string(acc) + "(" + aty + ")((" + dty + ")_a[_i]*(" + dty + ")_b[_i]); ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }";
            put(s);
            return true;
        }
    }

    // SQSHRN / SQRSHRN and the unsigned forms: shift right, round for the R
    // variants, then saturate into the half-width destination.
    if ((i & 0x8F80F400) == 0x0F009400 || (i & 0xAF80F400) == 0x2F008400) {
        const bool round = ((i >> 11) & 1) != 0;
        const bool scalar = ((i >> 28) & 1) != 0;
        const bool to_unsigned = (i & 0x1000) == 0;
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 immh = (i >> 19) & 15, immb = (i >> 16) & 7;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        u32 size = 0;
        bool shaped = true;
        if (scalar && !Q) shaped = false;
        if (immh & 8)       shaped = false;   // no 128-bit source element
        else if (immh & 4)  size = 2;
        else if (immh & 2)  size = 1;
        else if (immh & 1)  size = 0;
        else                shaped = false;
        if (shaped) {
            const int dbits = 8 << size, sbits = dbits * 2;
            const u32 shift = (u32)dbits * 2 - ((immh << 3) | immb);
            if (shift >= 1 && shift <= (u32)dbits) {
                const int lanes = scalar ? 1 : 64 / dbits;
                const int result_bytes = scalar ? dbits / 8 : 8;
                const std::string sty =
                    std::string(U && !to_unsigned ? "uint" : "int") + std::to_string(sbits) + "_t";
                const std::string dty = "uint" + std::to_string(dbits) + "_t";
                const std::string wide = U && !to_unsigned ? "uint64_t" : "int64_t";
                std::string s = "{ " + sty + " _a[" + std::to_string(lanes) + "]; " + dty +
                                " _r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," +
                     std::to_string(scalar ? sbits / 8 : 16) + "); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++){ " + wide +
                     " _v=(" + wide + ")_a[_i]; ";
                // Rounding as add-then-shift can overflow the source type; taking
                // the dropped bit out of the shifted value cannot.
                s += wide + " _x=(_v>>" + std::to_string(shift) + ")";
                if (round) {
                    s += "+((_v>>" + std::to_string(shift - 1) + ")&1)";
                }
                s += "; ";
                if (U) {
                    if (to_unsigned) s += "if(_x<0){_x=0;c->fpsr|=1ULL<<27;} ";
                    s += "if(_x>(" + wide + ")UINT" + std::to_string(dbits) + "_MAX){_x=(" + wide + ")UINT" +
                         std::to_string(dbits) + "_MAX;c->fpsr|=1ULL<<27;} ";
                } else {
                    s += "if(_x>(int64_t)INT" + std::to_string(dbits) + "_MAX){_x=(int64_t)INT" +
                         std::to_string(dbits) + "_MAX;c->fpsr|=1ULL<<27;} ";
                    s += "else if(_x<(int64_t)INT" + std::to_string(dbits) +
                         "_MIN){_x=(int64_t)INT" + std::to_string(dbits) + "_MIN;c->fpsr|=1ULL<<27;} ";
                }
                s += "_r[_i]=(" + dty + ")_x; } ";
                if (scalar || !Q) {
                    s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" +
                         std::to_string(rd) + "][1]=0; ";
                }
                s += "memcpy((uint8_t*)c->vreg[" + std::to_string(rd) + "]+" +
                     std::to_string(!scalar && Q ? 8 : 0) + ",_r," +
                     std::to_string(result_bytes) + "); }";
                put(s);
                return true;
            }
        }
    }

    // CMGT / CMGE / CMHI / CMHS, register forms. Scalar forms only allow D.
    if ((i & 0x9F20F400) == 0x0E203400 || (i & 0xDF20F400) == 0x5E203400) {
        const bool scalar = (i & 0x10000000) != 0;
        const bool Q = (i & 0x40000000) != 0, U = (i & 0x20000000) != 0;
        const u32 size = (i >> 22) & 3;
        if ((scalar && size == 3) || (!scalar && (size != 3 || Q))) {
            const int bits = 8 << size, bytes = scalar ? 8 : (Q ? 16 : 8);
            const int lanes = bytes * 8 / bits;
            const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
            const std::string ty = std::string(U ? "uint" : "int") + std::to_string(bits) + "_t";
            const std::string uty = "uint" + std::to_string(bits) + "_t";
            const std::string op = (i & 0x800) ? ">=" : ">";
            std::string s = "{ " + ty + " _a[" + std::to_string(lanes) + "],_b[" +
                std::to_string(lanes) + "]; " + uty + " _r[" + std::to_string(lanes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(bytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(_a[_i]" +
                op + "_b[_i])?(" + uty + ")~(" + uty + ")0:0; ";
            s += "memset(c->vreg[" + std::to_string(rd) + "],0,16); memcpy(c->vreg[" +
                std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
            put(s);
            return true;
        }
    }

    // Saturating variable shifts use only the signed low byte of each shift
    // lane. Right shifts never saturate; rounding belongs to that branch only.
    if ((i & 0x8F20EC00) == 0x0E204C00) {
        const bool scalar = (i & 0x10000000) != 0, Q = (i & 0x40000000) != 0;
        const bool U = (i & 0x20000000) != 0, round = (i & 0x1000) != 0;
        const u32 size = (i >> 22) & 3, rn = (i >> 5) & 31, rd = i & 31, rm = (i >> 16) & 31;
        if ((!scalar || Q) && (scalar || Q || size != 3)) {
            const int bits = 8 << size, bytes = scalar ? bits / 8 : Q ? 16 : 8;
            const int lanes = bytes * 8 / bits;
            const std::string ty = "uint" + std::to_string(bits) + "_t";
            const std::string n = std::to_string(bits);
            std::string s = "{ " + ty + " _a[" + std::to_string(lanes) + "],_b[" +
                std::to_string(lanes) + "],_r[" + std::to_string(lanes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) +
                ");memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(bytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";++_i){uint64_t _x=_a[_i],_z;int _sh=(int)(_b[_i]&255);if(_sh>=128)_sh-=256; ";
            s += "if(_sh>=0){ ";
            if (U) {
                s += "if(_x&&(_sh>=" + n + "||_x>((uint64_t)UINT" + n + "_MAX>>_sh))){_z=UINT" + n + "_MAX;c->fpsr|=UINT64_C(1)<<27;}else _z=_sh>=" + n + "?0:_x<<_sh; ";
            } else {
                s += "uint64_t _sign=UINT64_C(1)<<" + std::to_string(bits - 1) + ";int _neg=(_x&_sign)!=0;uint64_t _mag=_neg?(" + ty + ")((uint64_t)0-_x):_x,_lim=_neg?_sign:_sign-1; ";
                s += "if(_mag&&(_sh>=" + n + "||_mag>(_lim>>_sh))){_z=_neg?_sign:_sign-1;c->fpsr|=UINT64_C(1)<<27;}else _z=_sh>=" + n + "?0:_x<<_sh; ";
            }
            s += "}else{unsigned _rsh=(unsigned)-_sh; ";
            if (U) {
                s += "_z=_rsh>=" + n + "?0:_x>>_rsh; ";
                if (round) s += "if(_rsh<=" + n + ")_z+=(_x>>(_rsh-1))&1; ";
            } else {
                s += "int _neg=(_x&(UINT64_C(1)<<" + std::to_string(bits - 1) + "))!=0; ";
                s += "if(_rsh>=" + n + ")_z=" + std::string(round ? "0" : "_neg?UINT" + n + "_MAX:0") + ";else{_z=_x>>_rsh;if(_neg)_z|=UINT" + n + "_MAX^(UINT" + n + "_MAX>>_rsh); ";
                if (round) s += "_z+=(_x>>(_rsh-1))&1; ";
                s += "} ";
            }
            s += "}_r[_i]=(" + ty + ")_z;}memset(c->vreg[" + std::to_string(rd) + "],0,16);memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + ");}";
            put(s);
            return true;
        }
    }

    // Saturating integer arithmetic. Carry/sign tests use unsigned values so
    // even the 64-bit forms have no signed-overflow dependency. QC is sticky.
    if ((i & 0x8F20DC00) == 0x0E200C00 ||
        (i & 0x8F3FFC00) == 0x0E207800 ||
        (i & 0x8F3FFC00) == 0x0E214800 ||
        (i & 0xAF3FFC00) == 0x2E212800 ||
        (i & 0x8F20FC00) == 0x0E20B400 ||
        (i & 0xAF00E400) == 0x0F00C000) {
        const bool scalar = (i & 0x10000000) != 0, Q = (i & 0x40000000) != 0;
        const bool U = (i & 0x20000000) != 0;
        const u32 size = (i >> 22) & 3, rn = (i >> 5) & 31, rd = i & 31;
        const u32 rm = (i >> 16) & 31;
        const bool binary = (i & 0x8F20DC00) == 0x0E200C00;
        const bool unary = (i & 0x8F3FFC00) == 0x0E207800;
        const bool indexed_mul = (i & 0xAF00E400) == 0x0F00C000;
        const bool mul = (i & 0x8F20FC00) == 0x0E20B400 || indexed_mul;
        const bool narrow = !binary && !unary && !mul;
        const bool to_unsigned = narrow && (i & 0x2000) != 0;
        if ((!scalar || Q) && (narrow ? size != 3 : mul ? (size == 1 || size == 2) :
                              (scalar || Q || size != 3))) {
            const int bits = 8 << size, sbits = narrow ? bits * 2 : bits;
            const int bytes = scalar ? bits / 8 : narrow ? 8 : Q ? 16 : 8;
            const int lanes = bytes * 8 / bits;
            const std::string ty = "uint" + std::to_string(bits) + "_t";
            const std::string sty = "uint" + std::to_string(sbits) + "_t";
            std::string s = "{ " + sty + " _a[" + std::to_string(lanes) + "]; " + ty +
                " _r[" + std::to_string(lanes) + "]; ";
            if (binary || mul) s += ty + " _b[" + std::to_string(lanes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," +
                 std::to_string(lanes * sbits / 8) + "); ";
            if (indexed_mul) {
                const u32 index = (((i >> 11) & 1) << (size == 1 ? 2 : 1)) |
                    (((i >> 21) & 1) << (size == 1 ? 1 : 0)) |
                    (size == 1 ? ((i >> 20) & 1) : 0);
                s += "for(int _j=0;_j<" + std::to_string(lanes) + ";++_j)memcpy(&_b[_j],(const uint8_t*)c->vreg[" +
                    std::to_string(size == 1 ? rm & 15 : rm) + "]+" +
                    std::to_string(index * bits / 8) + ",sizeof(_b[_j])); ";
            } else if (binary || mul) s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(bytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";++_i){ uint64_t _z; ";
            if (binary || unary || (narrow && U && !to_unsigned)) s += "uint64_t _x=_a[_i]; ";
            const std::string sign = "(UINT64_C(1)<<" + std::to_string(bits - 1) + ")";
            const std::string max = "UINT" + std::to_string(bits) + "_MAX";
            if (binary) {
                const bool sub = (i & 0x2000) != 0;
                s += "uint64_t _y=_b[_i]; _z=(" + ty + ")(_x" + (sub ? "-" : "+") + "_y); ";
                if (U) s += "if(" + std::string(sub ? "_x<_y" : "_z<_x") + "){_z=" + (sub ? "0" : max) + ";c->fpsr|=UINT64_C(1)<<27;} ";
                else s += "if(((" + std::string(sub ? "_x^_y" : "~(_x^_y)") + ")&(_x^_z)&" + sign + ")!=0){_z=(_x&" + sign + ")?" + sign + ":" + sign + "-1;c->fpsr|=UINT64_C(1)<<27;} ";
            } else if (unary) {
                s += "_z=_x; if(_x==" + sign + "){_z=" + sign + "-1;c->fpsr|=UINT64_C(1)<<27;} else ";
                s += U ? "_z=(uint64_t)0-_x; " : "if(_x&" + sign + ")_z=(uint64_t)0-_x; ";
            } else if (narrow) {
                if (U && !to_unsigned) s += "_z=_x;if(_z>" + max + "){_z=" + max + ";c->fpsr|=UINT64_C(1)<<27;} ";
                else {
                    s += "int" + std::to_string(sbits) + "_t _v;memcpy(&_v,&_a[_i],sizeof(_v)); ";
                    const std::string hi = to_unsigned ? max : "INT" + std::to_string(bits) + "_MAX";
                    const std::string lo = to_unsigned ? "0" : "INT" + std::to_string(bits) + "_MIN";
                    s += "if(_v>" + hi + "){_z=" + hi + ";c->fpsr|=UINT64_C(1)<<27;}else if(_v<" + lo + "){_z=(uint64_t)(int64_t)(" + lo + ");c->fpsr|=UINT64_C(1)<<27;}else _z=(uint64_t)_v; ";
                }
            } else {
                // The undoubled product fits int64_t, including S x S. Divide
                // by 2^(bits-1) with explicit floor instead of shifting negatives.
                s += "int" + std::to_string(bits) + "_t _sx,_sy;memcpy(&_sx,&_a[_i],sizeof(_sx));memcpy(&_sy,&_b[_i],sizeof(_sy));int64_t _p=(int64_t)_sx*_sy; ";
                if (indexed_mul ? (i & 0x1000) != 0 : U) s += "_p+=INT64_C(1)<<" + std::to_string(bits - 2) + "; ";
                s += "int64_t _d=INT64_C(1)<<" + std::to_string(bits - 1) + ",_v=_p/_d;if(_p<0&&_p%_d)--_v;if(_v>INT" + std::to_string(bits) + "_MAX){_v=INT" + std::to_string(bits) + "_MAX;c->fpsr|=UINT64_C(1)<<27;}_z=(uint64_t)_v; ";
            }
            s += "_r[_i]=(" + ty + ")_z;} ";
            if (!(narrow && Q && !scalar)) s += "memset(c->vreg[" + std::to_string(rd) + "],0,16); ";
            s += "memcpy((uint8_t*)c->vreg[" + std::to_string(rd) + "]+" +
                 std::to_string(narrow && Q && !scalar ? 8 : 0) + ",_r," + std::to_string(bytes) + "); }";
            put(s);
            return true;
        }
    }

    // Scalar D ADD/SUB and CMEQ/CMTST. Snapshot both sources for aliases.
    if ((i & 0xDFE0F400) == 0x5EE08400) {
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const bool U = (i & 0x20000000) != 0, compare = (i & 0x800) != 0;
        const std::string expr = compare ? (U ? "(_a==_b)?~0ULL:0" : "(_a&_b)?~0ULL:0")
                                         : (U ? "_a-_b" : "_a+_b");
        put("{ uint64_t _a=c->vreg[" + std::to_string(rn) + "][0],_b=c->vreg[" +
            std::to_string(rm) + "][0]; c->vreg[" + std::to_string(rd) + "][0]=" + expr +
            "; c->vreg[" + std::to_string(rd) + "][1]=0; }");
        return true;
    }

    // Saturating adds, min/max, equality and bit-test comparisons.
    if ((i & 0x9F20FC00) == 0x0E200C00 || (i & 0x9F20FC00) == 0x0E208C00 ||
        (i & 0x9F20FC00) == 0x0E206400 || (i & 0x9F20FC00) == 0x0E206C00) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 size = (i >> 22) & 3, opcode = (i >> 11) & 0x1F;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const int esz = 1 << size;
        const int bytes = Q ? 16 : 8;
        if (!(size == 3 && !Q)) {
            const int lanes = bytes / esz;
            const int bits = esz * 8;
            const std::string uty = "uint" + std::to_string(bits) + "_t";
            const std::string ity = "int" + std::to_string(bits) + "_t";
            std::string body;
            if (opcode == 0x01) {
                if (U) {
                    // Unsigned saturating add: the sum wrapping below an input
                    // is exactly the overflow condition.
                    body = "{ " + uty + " _s=(" + uty + ")(_a[_i]+_b[_i]); _r[_i]=(_s<_a[_i])?(" +
                           uty + ")~(" + uty + ")0:_s; }";
                } else {
                    body = "{ " + ity + " _x=(" + ity + ")_a[_i], _y=(" + ity + ")_b[_i]; " + ity +
                           " _s=(" + ity + ")((" + uty + ")_x+(" + uty + ")_y); " +
                           "if(((_x^_s)&(_y^_s))<0) _r[_i]=(" + uty + ")(_x<0?(" + ity + ")((" +
                           uty + ")1<<" + std::to_string(bits - 1) + "):(" + ity + ")(((" + uty +
                           ")1<<" + std::to_string(bits - 1) + ")-1)); else _r[_i]=(" + uty +
                           ")_s; }";
                }
            } else if (opcode == 0x0C || opcode == 0x0D) {
                // SMAX / SMIN and their unsigned twins.
                const char* op = (opcode == 0x0C) ? ">" : "<";
                if (U) {
                    body = "_r[_i]=(_a[_i]" + std::string(op) + "_b[_i])?_a[_i]:_b[_i];";
                } else {
                    body = "{ " + ity + " _x=(" + ity + ")_a[_i],_y=(" + ity + ")_b[_i]; _r[_i]=(" +
                           uty + ")(_x" + std::string(op) + "_y?_x:_y); }";
                }
            } else if (opcode == 0x11) {
                // CMEQ when U is set, CMTST when it is not.
                body = U ? ("_r[_i]=(_a[_i]==_b[_i])?(" + uty + ")~(" + uty + ")0:(" + uty + ")0;")
                         : ("_r[_i]=((_a[_i]&_b[_i])!=0)?(" + uty + ")~(" + uty + ")0:(" + uty +
                            ")0;");
            }
            if (!body.empty()) {
                std::string s = "{ " + uty + " _a[" + std::to_string(lanes) + "],_b[" +
                                std::to_string(lanes) + "],_r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) +
                     "); ";
                s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(bytes) +
                     "); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) " + body + " ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) +
                     "); }";
                put(s);
                return true;
            }
        }
    }

    // Scalar/vector FADD/FSUB and pairwise FADDP share one exact FP-state core.
    {
        const bool scalar = (i & 0xFF20EC00) == 0x1E202800 && ((i >> 22) & 3) <= 1;
        const bool vector = (i & 0xBF20FC00) == 0x0E20D400;
        const bool scalar_pair = (i & 0xFFBFFC00) == 0x7E30D800;
        const bool vector_pair = (i & 0xBFA0FC00) == 0x2E20D400;
        const bool dbl=(i&0x00400000)!=0,q=(i&0x40000000)!=0;
        if (scalar || scalar_pair || ((vector || vector_pair) && (!dbl || q))) {
            const unsigned rd=i&31,rn=(i>>5)&31,rm=(i>>16)&31;
            const unsigned lanes=scalar||scalar_pair?1:(q?16:8)/(dbl?8:4);
            const bool sub=scalar?(i&0x1000)!=0:vector&&(i&0x800000)!=0;
            const std::string ct=dbl?"uint64_t":"uint32_t",count=dbl?"2":"4";
            put("{ "+ct+" _n["+count+"],_m["+count+"],_r["+count+"]={0};"
                "memcpy(_n,c->vreg["+std::to_string(rn)+"],16);memcpy(_m,c->vreg["+std::to_string(rm)+"],16);");
            for(unsigned e=0;e<lanes;++e) {
                const std::string src=vector_pair&&e>=lanes/2?"_m":"_n";
                const unsigned a=scalar_pair?0:vector_pair?2*(e%(lanes/2)):e;
                const unsigned b=scalar_pair?1:vector_pair?a+1:e;
                put("{uint64_t _a="+src+"["+std::to_string(a)+"],_b="+
                    (scalar_pair||vector_pair?src:"_m")+"["+std::to_string(b)+"],_v=0;"+
                    EmitFPAddSubValue(dbl,sub)+"_r["+std::to_string(e)+"]=("+ct+")_v;}");
            }
            put("memcpy(c->vreg["+std::to_string(rd)+"],_r,16);}");
            return true;
        }
    }

    // Integer pairwise add/min/max and min/max reductions. Snapshot both
    // sources before writing so all register aliases remain valid.
    {
        const u32 pair = i & 0xBF20FC00;
        const bool add = pair == 0x0E20BC00;
        const bool extrema = (i & 0x9F20F400) == 0x0E20A400;
        const bool reduction = (i & 0x9F3EFC00) == 0x0E30A800;
        const bool scalar_add = (i & 0xFFFFFC00) == 0x5EF1B800;
        const u32 Q = (i >> 30) & 1, size = (i >> 22) & 3;
        if (scalar_add || ((add || extrema || reduction) &&
            (size < 3 || (add && Q)) && !(reduction && size == 2 && !Q))) {
            const u32 rn = (i >> 5) & 31, rm = (i >> 16) & 31, rd = i & 31;
            const int esz = scalar_add ? 8 : 1 << size;
            const int bytes = scalar_add ? 16 : Q ? 16 : 8;
            const int lanes = bytes / esz;
            const bool minimum = ((i >> (reduction ? 16 : 11)) & 1) != 0;
            const bool uns = ((i >> 29) & 1) != 0;
            const std::string ty = std::string((add || scalar_add || uns) ? "uint" : "int") +
                                   std::to_string(esz * 8) + "_t";
            std::string s = "{ " + ty + " _a[" + std::to_string(lanes) + "],_r[" +
                std::to_string(lanes) + "]={0}; ";
            if (!reduction && !scalar_add) s += ty + " _b[" + std::to_string(lanes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            if (reduction) {
                s += "_r[0]=_a[0]; for(int _j=1;_j<" + std::to_string(lanes) +
                     ";++_j) if(_a[_j]" + (minimum ? "<" : ">") + "_r[0]) _r[0]=_a[_j]; ";
            } else if (scalar_add) {
                s += "_r[0]=_a[0]+_a[1]; ";
            } else {
                s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(bytes) + "); ";
                s += "for(int _j=0;_j<" + std::to_string(lanes / 2) + ";++_j) { ";
                for (const char* src : {"_a", "_b"}) {
                    const std::string a = std::string(src) + "[2*_j]", b = std::string(src) + "[2*_j+1]";
                    s += "_r[_j" + std::string(src[1] == 'b' ? "+" + std::to_string(lanes / 2) : "") + "]=";
                    s += add ? "(" + ty + ")(" + a + "+" + b + "); " :
                         a + (minimum ? "<" : ">") + b + "?" + a + ":" + b + "; ";
                }
                s += "} ";
            }
            s += "memset(c->vreg[" + std::to_string(rd) + "],0,16); memcpy(c->vreg[" +
                 std::to_string(rd) + "],_r," + std::to_string(reduction || scalar_add ? esz : bytes) + "); }";
            put(s);
            return true;
        }
    }

    // ADDV: sum every lane into the scalar destination. This is the
    // across-lanes class - bits 21..17 are 11000, not the 10000 of the
    // two-register-misc ops it otherwise resembles.
    if ((i & 0x9F3FFC00) == 0x0E31B800) {
        const u32 Q = (i >> 30) & 1;
        const u32 size = (i >> 22) & 3;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const int esz = 1 << size;
        const int bytes = Q ? 16 : 8;
        // 64-bit elements have no ADDV form, and 32-bit needs the full register.
        if (size != 3 && !(size == 2 && !Q)) {
            const int lanes = bytes / esz;
            const std::string ty = "uint" + std::to_string(esz * 8) + "_t";
            std::string s = "{ " + ty + " _a[" + std::to_string(lanes) + "],_s=0; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _s=(" + ty + ")(_s+_a[_i]); ";
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],&_s," + std::to_string(esz) + "); }";
            put(s);
            return true;
        }
    }

    // FRECPS: the Newton-Raphson step for reciprocal estimation, 2 - n*m.
    // Bit 23 selects FRSQRTS, whose step is (3 - n*m)/2.
    if ((i & 0xBF20FC00) == 0x0E20FC00) {
        const u32 Q = (i >> 30) & 1;
        const bool rsqrt = ((i >> 23) & 1) != 0;
        const bool dbl = ((i >> 22) & 1) != 0;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const char* ct = dbl ? "double" : "float";
        const int fsz = dbl ? 8 : 4;
        const int bytes = Q ? 16 : 8;
        const int lanes = bytes / fsz;
        if (!(dbl && !Q)) {
            std::string s = "{ " + std::string(ct) + " _a[" + std::to_string(lanes) + "],_b[" +
                            std::to_string(lanes) + "],_r[" + std::to_string(lanes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(bytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=";
            if (rsqrt) {
                s += "((" + std::string(ct) + ")3.0-_a[_i]*_b[_i])*(" + std::string(ct) + ")0.5; ";
            } else {
                s += "(" + std::string(ct) + ")2.0-_a[_i]*_b[_i]; ";
            }
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
            put(s);
            return true;
        }
    }

    // SHA256SU0: the sigma0 half of the message schedule update.
    //   W[t] = W[t-16] + s0(W[t-15]) + W[t-7] + s1(W[t-2])
    // This instruction contributes the first two terms; SHA256SU1 adds the
    // rest. Verified against the scalar recurrence, not transcribed.
    if ((i & 0xFFFE0C00) == 0x5E280800) {
        const u32 opcode = (i >> 12) & 0x1F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        if (opcode == 0) {
            // SHA1H: rotate left by 30.
            std::string s = "{ uint32_t _x; memcpy(&_x,c->vreg[" + std::to_string(rn) +
                            "],4); _x=(_x<<30)|(_x>>2); ";
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; memcpy(c->vreg[" + std::to_string(rd) + "],&_x,4); }";
            put(s);
            return true;
        }
        if (opcode == 1) {
            // SHA1SU1: finish the schedule group. The fourth word folds in the
            // first of this very group, so it cannot be one loop.
            std::string s = "{ uint32_t _d[4],_n[4],_t[4],_r[4]; ";
            s += "memcpy(_d,c->vreg[" + std::to_string(rd) + "],16); ";
            s += "memcpy(_n,c->vreg[" + std::to_string(rn) + "],16); ";
            s += "_t[0]=_d[0]^_n[1]; _t[1]=_d[1]^_n[2]; _t[2]=_d[2]^_n[3]; _t[3]=_d[3]; ";
            s += "for(int _e=0;_e<3;_e++) _r[_e]=(_t[_e]<<1)|(_t[_e]>>31); ";
            s += "{ uint32_t _v=_t[3]^_r[0]; _r[3]=(_v<<1)|(_v>>31); } ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }";
            put(s);
            return true;
        }
        if (opcode == 2) {
            std::string s = "{ uint32_t _d[4],_n[4],_t[4],_r[4]; ";
            s += "memcpy(_d,c->vreg[" + std::to_string(rd) + "],16); ";
            s += "memcpy(_n,c->vreg[" + std::to_string(rn) + "],16); ";
            // T is the window one word along: Vn<31:0> : Vd<127:32>.
            s += "_t[0]=_d[1]; _t[1]=_d[2]; _t[2]=_d[3]; _t[3]=_n[0]; ";
            s += "for(int _e=0;_e<4;_e++){ uint32_t _x=_t[_e]; ";
            s += "uint32_t _s0=((_x>>7)|(_x<<25))^((_x>>18)|(_x<<14))^(_x>>3); ";
            s += "_r[_e]=_s0+_d[_e]; } ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }";
            put(s);
            return true;
        }
    }

    // SHA1C / SHA1P / SHA1M and SHA1SU0. Same class as SHA256SU1, picked
    // apart by opcode: 000 choose, 001 parity, 010 majority, 011 schedule.
    if ((i & 0xFFE08C00) == 0x5E000000) {
        const u32 opcode = (i >> 12) & 7;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        if (opcode <= 2) {
            const char* f = (opcode == 0) ? "((_b&_cc)|(~_b&_dd))"
                          : (opcode == 1) ? "(_b^_cc^_dd)"
                                          : "((_b&_cc)|(_b&_dd)|(_cc&_dd))";
            std::string s = "{ uint32_t _v[4],_w[4],_x,_y; ";
            s += "memcpy(_v,c->vreg[" + std::to_string(rd) + "],16); ";
            s += "memcpy(_w,c->vreg[" + std::to_string(rm) + "],16); ";
            s += "memcpy(&_y,c->vreg[" + std::to_string(rn) + "],4); ";
            s += "_x=_v[0]; ";
            s += "for(int _e=0;_e<4;_e++){ uint32_t _b=_v[1],_cc=_v[2],_dd=_v[3]; ";
            s += "uint32_t _t=((_x<<5)|(_x>>27))+" + std::string(f) + "+_y+_w[_e]; ";
            s += "_y=_dd; _v[3]=_cc; _v[2]=(_b<<30)|(_b>>2); _v[1]=_x; _x=_t; _v[0]=_t; } ";
            // Only Vd is written; the running e stays inside the instruction.
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_v,16); }";
            put(s);
            return true;
        }
        if (opcode == 4 || opcode == 5) {
            // SHA256H / SHA256H2. Four rounds over a 256-bit state split across
            // two registers; the pair differ only in which half is X and which
            // half is returned.
            const bool part1 = opcode == 4;
            const u32 xs = part1 ? rd : rn, ys = part1 ? rn : rd;
            std::string s = "{ uint32_t _X[4],_Y[4],_W[4],_nx[4],_ny[4]; ";
            s += "memcpy(_X,c->vreg[" + std::to_string(xs) + "],16); ";
            s += "memcpy(_Y,c->vreg[" + std::to_string(ys) + "],16); ";
            s += "memcpy(_W,c->vreg[" + std::to_string(rm) + "],16); ";
            s += "for(int _e=0;_e<4;_e++){ uint32_t _y0=_Y[0],_x0=_X[0]; ";
            s += "uint32_t _chs=(_y0&_Y[1])^(~_y0&_Y[2]); ";
            s += "uint32_t _maj=(_x0&_X[1])^(_x0&_X[2])^(_X[1]&_X[2]); ";
            s += "uint32_t _s1=((_y0>>6)|(_y0<<26))^((_y0>>11)|(_y0<<21))^((_y0>>25)|(_y0<<7)); ";
            s += "uint32_t _s0=((_x0>>2)|(_x0<<30))^((_x0>>13)|(_x0<<19))^((_x0>>22)|(_x0<<10)); ";
            s += "uint32_t _t=_Y[3]+_s1+_chs+_W[_e]; ";
            // The 256-bit state rotates left by one word each round, so the two
            // freshly written words land at the bottom of the other half.
            s += "_nx[0]=_t+_s0+_maj; _nx[1]=_X[0]; _nx[2]=_X[1]; _nx[3]=_X[2]; ";
            s += "_ny[0]=_t+_X[3]; _ny[1]=_Y[0]; _ny[2]=_Y[1]; _ny[3]=_Y[2]; ";
            s += "memcpy(_X,_nx,16); memcpy(_Y,_ny,16); } ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "]," +
                 std::string(part1 ? "_X" : "_Y") + ",16); }";
            put(s);
            return true;
        }
        if (opcode == 3) {
            // SHA1SU0: the window two words along, folded with Vd and Vm.
            std::string s = "{ uint32_t _d[4],_n[4],_m[4],_r[4]; ";
            s += "memcpy(_d,c->vreg[" + std::to_string(rd) + "],16); ";
            s += "memcpy(_n,c->vreg[" + std::to_string(rn) + "],16); ";
            s += "memcpy(_m,c->vreg[" + std::to_string(rm) + "],16); ";
            s += "_r[0]=_d[2]; _r[1]=_d[3]; _r[2]=_n[0]; _r[3]=_n[1]; ";
            s += "for(int _e=0;_e<4;_e++) _r[_e]^=_d[_e]^_m[_e]; ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }";
            put(s);
            return true;
        }
    }

    // SHA256SU1: the sigma1 half, plus the two carried terms. The upper two
    // words need sigma1 of the two just computed, because W[t+2] depends on
    // W[t] - which is why this cannot be written as one loop.
    if ((i & 0xFFE0FC00) == 0x5E006000) {
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        std::string s = "{ uint32_t _d[4],_n[4],_m[4],_t0[4],_t1[2],_r[4]; ";
        s += "memcpy(_d,c->vreg[" + std::to_string(rd) + "],16); ";
        s += "memcpy(_n,c->vreg[" + std::to_string(rn) + "],16); ";
        s += "memcpy(_m,c->vreg[" + std::to_string(rm) + "],16); ";
        s += "_t0[0]=_n[1]; _t0[1]=_n[2]; _t0[2]=_n[3]; _t0[3]=_m[0]; ";
        s += "_t1[0]=_m[2]; _t1[1]=_m[3]; ";
        s += "for(int _e=0;_e<2;_e++){ uint32_t _x=_t1[_e]; ";
        s += "uint32_t _s1=((_x>>17)|(_x<<15))^((_x>>19)|(_x<<13))^(_x>>10); ";
        s += "_r[_e]=_s1+_d[_e]+_t0[_e]; } ";
        s += "for(int _e=2;_e<4;_e++){ uint32_t _x=_r[_e-2]; ";
        s += "uint32_t _s1=((_x>>17)|(_x<<15))^((_x>>19)|(_x<<13))^(_x>>10); ";
        s += "_r[_e]=_s1+_d[_e]+_t0[_e]; } ";
        s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }";
        put(s);
        return true;
    }

    // LD1-LD4 / ST1-ST4, whole registers. opcode says how many registers and
    // whether they are interleaved: LD2/3/4 spread consecutive elements across
    // the registers, while the LD1 forms are plain consecutive blocks.
    if ((i & 0xBFBF0000) == 0x0C000000 || (i & 0xBFA00000) == 0x0C800000) {
        const u32 Q = (i >> 30) & 1;
        const bool post = ((i >> 23) & 1) != 0;
        const bool load = ((i >> 22) & 1) != 0;
        const u32 rm = (i >> 16) & 31;
        const u32 opcode = (i >> 12) & 15, size = (i >> 10) & 3;
        const u32 rn = (i >> 5) & 31, rt = i & 31;
        int regs = 0, step = 0;
        switch (opcode) {
        case 0x0: regs = 4; step = 4; break;   // LD4/ST4, interleaved
        case 0x2: regs = 4; step = 1; break;   // LD1/ST1, four registers
        case 0x4: regs = 3; step = 3; break;   // LD3/ST3, interleaved
        case 0x6: regs = 3; step = 1; break;
        case 0x7: regs = 1; step = 1; break;
        case 0x8: regs = 2; step = 2; break;   // LD2/ST2, interleaved
        case 0xA: regs = 2; step = 1; break;
        default: break;
        }
        const int esz = 1 << size;
        const int bytes = Q ? 16 : 8;
        // The .1D arrangement exists only for the non-interleaved LD1/ST1 forms.
        const bool shaped = regs > 0 && !(size == 3 && !Q && step != 1);
        if (shaped) {
            const int lanes = bytes / esz;
            const int bits = esz * 8;
            std::string s = "{ uint64_t _a=" + Xsp(rn) + "; uint64_t _v; ";
            // Emit in ascending memory order, including non-interleaved LD1/ST1.
            for (int k = 0; k < lanes * regs; ++k) {
                const int e = step == 1 ? k % lanes : k / regs;
                const int r = step == 1 ? k / lanes : k % regs;
                const std::string off = std::to_string(k * esz);
                const std::string vr = std::to_string((rt + (u32)r) & 31);
                const std::string lane = std::to_string(e * esz);
                if (load) {
                    s += "_v=recomp_load" + std::to_string(bits) + "(c,_a+" +
                         off + "); memcpy((uint8_t*)c->vreg[" + vr + "]+" +
                         lane + ",&_v," + std::to_string(esz) + "); ";
                } else {
                    s += "_v=0; memcpy(&_v,(const uint8_t*)c->vreg[" + vr + "]+" + lane + "," +
                         std::to_string(esz) + "); recomp_store" + std::to_string(bits) +
                         "(c,_a+" + off + ",_v); ";
                }
            }
            if (load && !Q) {
                // The 64-bit forms clear the top half of every register written.
                for (int r = 0; r < regs; ++r) {
                    s += "c->vreg[" + std::to_string((rt + (u32)r) & 31) + "][1]=0; ";
                }
            }
            if (post) {
                const std::string adv = (rm == 31)
                                            ? (std::to_string(regs * bytes) + "ULL")
                                            : ("c->x[" + std::to_string(rm) + "]");
                s += "c->x[" + std::to_string(rn) + "]=_a+" + adv + "; ";
            }
            s += "}";
            put(s);
            return true;
        }
    }

    // AESMC / AESIMC: the MixColumns step and its inverse. The matrix and the
    // xtime-based GF(2^8) multiply below are dynarmic's
    // (common/crypto/aes.cpp), not written from the specification.
    if ((i & 0xFFFE0C00) == 0x4E280800) {
        const u32 opcode = (i >> 12) & 0x1F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        if (opcode == 4 || opcode == 5) {
            // AESE: Vd = SubBytes(ShiftRows(Vd EOR Vn)).
            // AESD is the same with the inverse of each step. ShiftRows rotates
            // row r left by r, which over the column-major state is a shift of
            // 4*(i&3) - the permutation dynarmic spells out byte by byte.
            const bool dec = (opcode == 5);
            std::string s = "{ uint8_t _s[16],_k[16],_t[16]; const uint8_t* _sb=recomp_aes_sbox(" +
                            std::string(dec ? "1" : "0") + "); ";
            s += "memcpy(_s,c->vreg[" + std::to_string(rd) + "],16); ";
            s += "memcpy(_k,c->vreg[" + std::to_string(rn) + "],16); ";
            s += "for(int _i=0;_i<16;_i++) _s[_i]=(uint8_t)(_s[_i]^_k[_i]); ";
            if (dec) {
                s += "for(int _i=0;_i<16;_i++) _t[_i]=_s[(_i+16-4*(_i&3))&15]; ";
            } else {
                s += "for(int _i=0;_i<16;_i++) _t[_i]=_s[(_i+4*(_i&3))&15]; ";
            }
            s += "for(int _i=0;_i<16;_i++) _t[_i]=_sb[_t[_i]]; ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_t,16); }";
            put(s);
            return true;
        }
        if (opcode == 6 || opcode == 7) {
            const char* mtx = (opcode == 7)
                                  ? "{14,11,13,9},{9,14,11,13},{13,9,14,11},{11,13,9,14}"
                                  : "{2,3,1,1},{1,2,3,1},{1,1,2,3},{3,1,1,2}";
            std::string s = "{ uint8_t _s[16],_r[16]; static const uint8_t _mx[4][4]={";
            s += mtx;
            s += "}; memcpy(_s,c->vreg[" + std::to_string(rn) + "],16); ";
            s += "for(int _cl=0;_cl<16;_cl+=4) for(int _o=0;_o<4;_o++){ uint8_t _acc=0; ";
            s += "for(int _k=0;_k<4;_k++){ uint8_t _x=_s[_cl+_k],_y=_mx[_o][_k],_pp=0; ";
            s += "while(_y){ if(_y&1) _pp=(uint8_t)(_pp^_x); ";
            s += "_x=(uint8_t)((_x<<1)^((_x>>7)*0x1B)); _y=(uint8_t)(_y>>1); } ";
            s += "_acc=(uint8_t)(_acc^_pp); } _r[_cl+_o]=_acc; } ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }";
            put(s);
            return true;
        }
    }

    // PMULL / PMULL2: carry-less multiply of the low or high 64-bit halves.
    if ((i & 0xBF20FC00) == 0x0E20E000) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 size = (i >> 22) & 3;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        if (!U && size == 3) {
            const int half = Q ? 1 : 0;   // PMULL2 takes the top half of each source
            std::string s = "{ uint64_t _a=c->vreg[" + std::to_string(rn) + "][" +
                            std::to_string(half) + "], _b=c->vreg[" + std::to_string(rm) + "][" +
                            std::to_string(half) + "]; uint64_t _lo=0,_hi=0; ";
            s += "for(int _k=0;_k<64;_k++) if((_b>>_k)&1ULL){ _lo^=_a<<_k; ";
            // Shifting by 64 is undefined, and that is exactly the k=0 case.
            s += "if(_k) _hi^=_a>>(64-_k); } ";
            s += "c->vreg[" + std::to_string(rd) + "][0]=_lo; c->vreg[" + std::to_string(rd) +
                 "][1]=_hi; }";
            put(s);
            return true;
        }
    }

    // CRC32 / CRC32C. Computed a bit at a time rather than from a table: this
    // is the same recurrence dynarmic's tables encode, and a cold instruction
    // does not justify carrying 2 KB of tables in every generated image.
    if ((i & 0x7FE0E000) == 0x1AC04000) {
        const u32 sf = i >> 31;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const u32 castagnoli = (i >> 12) & 1, sz = (i >> 10) & 3;
        // The 64-bit variant is the sz=11 form only, and it needs sf set.
        if (!(sz == 3 && !sf) && !(sz != 3 && sf) && rd != 31) {
            const int nbytes = 1 << sz;
            const char* poly = castagnoli ? "0x82F63B78UL" : "0xEDB88320UL";
            std::string s = "{ uint32_t _crc=(uint32_t)" + Xz(rn) + "; uint64_t _v=" + Xz(rm) +
                            "; ";
            s += "for(int _i=0;_i<" + std::to_string(nbytes) + ";_i++){ ";
            s += "_crc^=(uint8_t)(_v>>(8*_i)); ";
            s += "for(int _b=0;_b<8;_b++) _crc=(_crc>>1)^(" + std::string(poly) +
                 " & (uint32_t)(-(int32_t)(_crc&1u))); } ";
            s += "c->x[" + std::to_string(rd) + "]=(uint64_t)_crc; }";
            put(s);
            return true;
        }
    }

    // BSL / BIT / BIF: bitwise select. All three are the same operation with a
    // different choice of which register supplies the mask and which the
    // destination, so size picks the variant rather than an element width.
    // U must be set; the U=0 half of this encoding is AND/BIC/ORR/ORN.
    if ((i & 0xBF20FC00) == 0x2E201C00) {
        const u32 Q = (i >> 30) & 1;
        const u32 size = (i >> 22) & 3;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        if (size != 0) {   // size 0 is EOR, handled with the other logicals
            const int halves = Q ? 2 : 1;
            std::string s = "{ ";
            for (int h = 0; h < halves; ++h) {
                const std::string k = "[" + std::to_string(h) + "]";
                const std::string d = "c->vreg[" + std::to_string(rd) + "]" + k;
                const std::string n = "c->vreg[" + std::to_string(rn) + "]" + k;
                const std::string m = "c->vreg[" + std::to_string(rm) + "]" + k;
                if (size == 1) {
                    // BSL: the destination is the mask.
                    s += d + " = (" + d + " & " + n + ") | (~" + d + " & " + m + "); ";
                } else if (size == 2) {
                    // BIT: insert where the second source has bits set.
                    s += d + " = (" + d + " & ~" + m + ") | (" + n + " & " + m + "); ";
                } else {
                    // BIF: insert where it has them clear.
                    s += d + " = (" + d + " & " + m + ") | (" + n + " & ~" + m + "); ";
                }
            }
            if (!Q) {
                s += "c->vreg[" + std::to_string(rd) + "][1]=0; ";
            }
            s += "}";
            put(s);
            return true;
        }
    }

    // TBL / TBX: byte-wise table lookup. The table is 1-4 consecutive vector
    // registers starting at Rn, wrapping at 32, and each byte of Rm indexes it.
    // An index past the end gives zero for TBL and leaves the byte alone for
    // TBX, which is the only difference between them.
    if ((i & 0xBFE08C00) == 0x0E000000) {
        const u32 Q = (i >> 30) & 1;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const u32 len = (i >> 13) & 3, op = (i >> 12) & 1;
        const int regs = (int)len + 1;
        const int bytes = Q ? 16 : 8;
        const int tbl_bytes = regs * 16;
        std::string s = "{ uint8_t _t[" + std::to_string(tbl_bytes) + "],_x[" +
                        std::to_string(bytes) + "],_r[" + std::to_string(bytes) + "]; ";
        for (int k = 0; k < regs; ++k) {
            // The table wraps at v31, so a run starting near the top comes back
            // round to v0 rather than reading off the end of the register file.
            s += "memcpy(_t+" + std::to_string(k * 16) + ",c->vreg[" +
                 std::to_string((rn + (u32)k) & 31) + "],16); ";
        }
        s += "memcpy(_x,c->vreg[" + std::to_string(rm) + "]," + std::to_string(bytes) + "); ";
        if (op) {
            s += "memcpy(_r,c->vreg[" + std::to_string(rd) + "]," + std::to_string(bytes) + "); ";
        }
        s += "for(int _i=0;_i<" + std::to_string(bytes) + ";_i++){ unsigned _k=_x[_i]; ";
        s += "if(_k<" + std::to_string(tbl_bytes) + "U) _r[_i]=_t[_k];";
        if (!op) {
            s += " else _r[_i]=0;";
        }
        s += " } ";
        s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
             "][1]=0; ";
        s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
        put(s);
        return true;
    }

    // LD1-LD4 / ST1-ST4, one element per register, preserving every other lane.
    // Bit 23 selects post-index; Rm=31 advances by the structure's byte size.
    // Other Rm values name a register containing the increment.
    if ((i & 0xBF000000) == 0x0D000000) {
        const u32 Q = (i >> 30) & 1;
        const bool post = ((i >> 23) & 1) != 0;
        const bool load = ((i >> 22) & 1) != 0;
        const u32 rm = (i >> 16) & 31;
        const u32 opcode = (i >> 13) & 7, S = (i >> 12) & 1, size = (i >> 10) & 3;
        const u32 rn = (i >> 5) & 31, rt = i & 31;
        const u32 regs = (((opcode & 1) << 1) | ((i >> 21) & 1)) + 1;
        int esz = 0, index = 0;
        bool shaped = true;
        if ((opcode >> 1) == 0) {
            esz = 1;
            index = (int)((Q << 3) | (S << 2) | size);
        } else if ((opcode >> 1) == 1 && (size & 1) == 0) {
            esz = 2;
            index = (int)((Q << 2) | (S << 1) | (size >> 1));
        } else if ((opcode >> 1) == 2 && size == 0) {
            esz = 4;
            index = (int)((Q << 1) | S);
        } else if ((opcode >> 1) == 2 && size == 1 && S == 0) {
            esz = 8;
            index = (int)Q;
        } else {
            shaped = false;   // Reserved shapes and the replicating forms below.
        }
        // Only the no-offset form may have a register field of zero meaning
        // "no offset"; in the post-index form that field is Rm.
        if (shaped && (post || rm == 0)) {
            const int bits = esz * 8;
            std::string s = "{ uint64_t _a=" + Xsp(rn) + "; ";
            for (u32 r = 0; r < regs; ++r) {
                const std::string vr = std::to_string((rt + r) & 31);
                const std::string addr = "_a+" + std::to_string(r * esz);
                if (load) {
                    s += "{ uint64_t _v=recomp_load" + std::to_string(bits) + "(c," + addr + "); ";
                    s += "memcpy((uint8_t*)c->vreg[" + vr + "]+" +
                         std::to_string(index * esz) + ",&_v," + std::to_string(esz) + "); ";
                } else {
                    s += "{ uint64_t _v=0; memcpy(&_v,(const uint8_t*)c->vreg[" + vr +
                         "]+" + std::to_string(index * esz) + "," + std::to_string(esz) + "); ";
                    s += "recomp_store" + std::to_string(bits) + "(c," + addr + ",_v); ";
                }
                s += "} ";
            }
            if (post) {
                const std::string step =
                    (rm == 31) ? (std::to_string(regs * esz) + "ULL") : ("c->x[" + std::to_string(rm) + "]");
                s += "c->x[" + std::to_string(rn) + "]=_a+" + step + "; ";
            }
            s += "}";
            put(s);
            return true;
        }
    }

    // SSHLL / USHLL: widen half the source elements and shift them left. SXTL
    // and UXTL are these with a shift of zero, which is how the assembler spells
    // them and why they never appeared as their own encoding.
    if ((i & 0x9F00FC00) == 0x0F00A400) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 immh = (i >> 19) & 15, immb = (i >> 16) & 7;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        u32 size = 0;
        bool shaped = true;
        if (immh & 8)       shaped = false;   // reserved for this encoding
        else if (immh & 4)  size = 2;
        else if (immh & 2)  size = 1;
        else if (immh & 1)  size = 0;
        else                shaped = false;
        if (shaped) {
            const int sbits = 8 << size;
            const u32 shift = ((immh << 3) | immb) - (u32)sbits;
            const int ssz = sbits / 8;
            const int lanes = 8 / ssz;            // always half a register in
            const int off = Q ? 8 : 0;            // ...the top half when Q is set
            const std::string sty =
                (U ? std::string("uint") : std::string("int")) + std::to_string(sbits) + "_t";
            const std::string dty = "uint" + std::to_string(sbits * 2) + "_t";
            std::string s = "{ " + sty + " _a[" + std::to_string(lanes) + "]; " + dty + " _r[" +
                            std::to_string(lanes) + "]; ";
            s += "memcpy(_a,(const uint8_t*)c->vreg[" + std::to_string(rn) + "]+" +
                 std::to_string(off) + ",8); ";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(" + dty + ")((" +
                 dty + ")_a[_i]<<" + std::to_string(shift) + "); ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }";
            put(s);
            return true;
        }
    }

    // ZIP/UZP/TRN. All six are the same read of two registers with a different
    // index pattern, so they share one emitter.
    if ((i & 0xBF208C00) == 0x0E000800) {
        const u32 Q = (i >> 30) & 1;
        const u32 size = (i >> 22) & 3;
        const u32 opcode = (i >> 12) & 7;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const int esz = 1 << size;
        const int bytes = Q ? 16 : 8;
        const int lanes = bytes / esz;
        const char* pattern = nullptr;
        switch (opcode) {
        case 1: pattern = "uzp1"; break;
        case 2: pattern = "trn1"; break;
        case 3: pattern = "zip1"; break;
        case 5: pattern = "uzp2"; break;
        case 6: pattern = "trn2"; break;
        case 7: pattern = "zip2"; break;
        default: break;
        }
        if (pattern && !(size == 3 && !Q)) {
            const std::string ty = "uint" + std::to_string(esz * 8) + "_t";
            const int half = lanes / 2;
            std::string idx;
            if (opcode == 3 || opcode == 7) {
                // ZIP: interleave one half of each source.
                const int base = (opcode == 7) ? half : 0;
                idx = "_r[2*_i]=_a[" + std::to_string(base) + "+_i]; _r[2*_i+1]=_b[" +
                      std::to_string(base) + "+_i];";
            } else if (opcode == 1 || opcode == 5) {
                // UZP: take every other element, all of a then all of b.
                const int first = (opcode == 5) ? 1 : 0;
                idx = "_r[_i]=_a[2*_i+" + std::to_string(first) + "]; _r[" +
                      std::to_string(half) + "+_i]=_b[2*_i+" + std::to_string(first) + "];";
            } else {
                // TRN: pair up matching even or odd elements.
                const int first = (opcode == 6) ? 1 : 0;
                idx = "_r[2*_i]=_a[2*_i+" + std::to_string(first) + "]; _r[2*_i+1]=_b[2*_i+" +
                      std::to_string(first) + "];";
            }
            std::string s = "{ " + ty + " _a[" + std::to_string(lanes) + "],_b[" +
                            std::to_string(lanes) + "],_r[" + std::to_string(lanes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(bytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(half) + ";_i++){ " + idx + " } ";
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
            put(s);
            return true;
        }
    }

    // EXT: a window into Rn:Rm starting imm4 bytes in.
    if ((i & 0xBFE08400) == 0x2E000000) {
        const u32 Q = (i >> 30) & 1;
        const u32 imm4 = (i >> 11) & 15;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const int bytes = Q ? 16 : 8;
        if ((int)imm4 < bytes) {
            std::string s = "{ uint8_t _a[" + std::to_string(bytes) + "],_b[" +
                            std::to_string(bytes) + "],_r[" + std::to_string(bytes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(bytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(bytes) + ";_i++){ int _k=_i+" +
                 std::to_string(imm4) + "; _r[_i] = (_k<" + std::to_string(bytes) +
                 ") ? _a[_k] : _b[_k-" + std::to_string(bytes) + "]; } ";
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
            put(s);
            return true;
        }
    }

    // FCMEQ / FCMGE / FCMGT, register forms. The compare-against-zero forms are
    // handled with the other two-register-misc ops; these take a second vector.
    // U and size<1> pick which comparison: the two bits are the operator.
    if ((i & 0x9F20FC00) == 0x0E20E400) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const u32 size = (i >> 22) & 3;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const bool dbl = (size & 1) != 0;
        const char* cmp = nullptr;
        if (!U && !(size & 2))      cmp = "==";   // FCMEQ
        else if (U && !(size & 2))  cmp = ">=";   // FCMGE
        else if (U && (size & 2))   cmp = ">";    // FCMGT
        if (cmp && !(dbl && !Q)) {
            const char* ct = dbl ? "double" : "float";
            const int fsz = dbl ? 8 : 4;
            const int bytes = Q ? 16 : 8;
            const int lanes = bytes / fsz;
            const std::string uty = "uint" + std::to_string(fsz * 8) + "_t";
            std::string s = "{ " + std::string(ct) + " _a[" + std::to_string(lanes) +
                            "],_b[" + std::to_string(lanes) + "]; " + uty + " _r[" +
                            std::to_string(lanes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(bytes) + "); ";
            // A NaN operand compares false and the lane comes out zero, which is
            // what the architecture specifies and what C's comparison already
            // does - so no NaN test is needed here.
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(_a[_i]" + cmp +
                 "_b[_i]) ? (" + uty + ")~(" + uty + ")0 : (" + uty + ")0; ";
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
            put(s);
            return true;
        }
    }

    // USHL/SSHL and URSHL/SRSHL, vector and scalar D. Only Rm's signed low
    // byte controls the shift. Saturating neighboring opcodes remain separate.
    if ((i & 0x8F20EC00) == 0x0E204400) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
        const bool scalar = (i & 0x10000000) != 0, round = (i & 0x1000) != 0;
        const u32 size = (i >> 22) & 3;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const int esz = 1 << size;
        const int ebits = esz * 8;
        const int bytes = scalar ? 8 : Q ? 16 : 8;
        if (scalar ? Q && size == 3 : !(size == 3 && !Q)) {
            const int lanes = bytes / esz;
            const std::string uty = "uint" + std::to_string(ebits) + "_t";
            const std::string eb = std::to_string(ebits);
            std::string s = "{ " + uty + " _a[" + std::to_string(lanes) + "],_m[" +
                            std::to_string(lanes) + "],_r[" + std::to_string(lanes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            s += "memcpy(_m,c->vreg[" + std::to_string(rm) + "]," + std::to_string(bytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++){ ";
            s += "int _s=(int)(_m[_i]&0xFF); if(_s>=128) _s-=256; uint64_t _v=_a[_i],_z; ";
            // A shift of the element width or more is defined by the
            // architecture but undefined in C, so both ends are special-cased.
            s += "if(_s>=0) _z=(_s>=" + eb + ")?0:(_v<<_s); ";
            s += "else { int _t=-_s; ";
            s += "if(_t>" + eb + ") _z=" +
                 std::string(round || U ? "0" : "(_v>>(" + eb + "-1))?~0ULL:0") + "; ";
            s += "else { _z=(_t==64)?0:(_v>>_t); ";
            if (!U) s += "if(_v>>(" + eb + "-1)) _z|=~0ULL<<(" + eb + "-_t); ";
            if (round) s += "_z+=(_v>>(_t-1))&1; ";
            s += "} } _r[_i]=(" + uty + ")_z; } ";
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
            put(s);
            return true;
        }
    }

    // SHL by immediate. Bit 29 is U and must stay in the mask: the same opcode
    // with U set is SLI, handled above. immh selects the element width and
    // immh:immb encodes the shift as a bias above it.
    // Off deliberately, and measured. Enabling this frees the hot block that also
    // contains MOVI - transitions drop 47% - and costs 2.13 ms/frame against
    // 1.95. The block is SIMD-heavy and the JIT compiles it better than the
    // emitted C does, so paying the transition to stay in the JIT is cheaper
    // than owning the block. Re-measure before flipping this.
    const bool kTranslateShiftLeftImmediate = g_translate_all;
    if (kTranslateShiftLeftImmediate && (i & 0xBF80FC00) == 0x0F005400) {
        const u32 Q = (i >> 30) & 1;
        const u32 immh = (i >> 19) & 15, immb = (i >> 16) & 7;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        u32 size = 0;
        bool ok = true;
        if (immh & 8)        size = 3;
        else if (immh & 4)   size = 2;
        else if (immh & 2)   size = 1;
        else if (immh & 1)   size = 0;
        else                 ok = false;   // immh 0000 is the modified-immediate space
        // A 64-bit element only exists as 2D.
        if (size == 3 && !Q) ok = false;
        if (ok) {
            const int ebits = 8 << size;
            const u32 shift = ((immh << 3) | immb) - (u32)ebits;
            const int esz = ebits / 8;
            const int bytes = Q ? 16 : 8;
            const int lanes = bytes / esz;
            const std::string uty = "uint" + std::to_string(ebits) + "_t";
            std::string s = "{ " + uty + " _a[" + std::to_string(lanes) + "],_r[" +
                            std::to_string(lanes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(" + uty + ")((" +
                 uty + ")_a[_i]<<" + std::to_string(shift) + "); ";
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
            put(s);
            return true;
        }
    }

    // MOVI/MVNI and ORR/BIC modified immediates. Odd cmode below 1100 reads
    // and combines with the destination; other forms replace it. o2 is zero.
    if ((i & 0x9FF80C00) == 0x0F000400) {
        const u32 Q = (i >> 30) & 1, op = (i >> 29) & 1;
        const u32 cmode = (i >> 12) & 15;
        const u32 imm8 = ((((i >> 16) & 7) << 5) | ((i >> 5) & 31)) & 0xFF;
        const u32 rd = i & 31;
        const u32 hi3 = cmode >> 1;
        bool ok = true;
        u64 imm64 = 0;
        if (hi3 <= 3) {
            const u64 v = (u64)imm8 << (8 * hi3);
            imm64 = (v << 32) | v;
        } else if (hi3 == 4 || hi3 == 5) {
            const u64 h = ((u64)imm8 << (8 * (hi3 - 4))) & 0xFFFF;
            imm64 = (h << 48) | (h << 32) | (h << 16) | h;
        } else if (hi3 == 6) {
            const u64 v = (cmode & 1) ? (((u64)imm8 << 16) | 0xFFFF)
                                      : (((u64)imm8 << 8) | 0xFF);
            imm64 = (v << 32) | v;
        } else if (cmode == 14) {
            if (op == 0) {
                for (int k = 0; k < 8; ++k) imm64 |= (u64)imm8 << (8 * k);
            } else {
                // Each bit of imm8 expands to a whole byte of the result.
                for (int k = 0; k < 8; ++k) {
                    if ((imm8 >> k) & 1) imm64 |= 0xFFULL << (8 * k);
                }
            }
        } else if (cmode == 15) {
            // FMOV (vector, immediate). VFPExpandImm, built at double width and
            // narrowed for the 32-bit form the way the scalar FMOV above does.
            const u32 sgn = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
            const u64 e11 = ((u64)(b6 ^ 1) << 10) | (b6 ? (0xFFULL << 2) : 0ULL) |
                            ((imm8 >> 4) & 3);
            const u64 dbits = ((u64)sgn << 63) | (e11 << 52) | ((u64)(imm8 & 0xF) << 48);
            if (op == 0) {
                double dv;
                memcpy(&dv, &dbits, 8);
                const float fv = (float)dv;
                u32 fb;
                memcpy(&fb, &fv, 4);
                imm64 = ((u64)fb << 32) | fb;
            } else if (Q) {
                imm64 = dbits;   // the 64-bit form is 2D only
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }
        if (ok) {
            // MVNI inverts, but cmode 1110 is MOVI in both op encodings.
            if (op == 1 && cmode < 14) imm64 = ~imm64;
            char lo[32];
            snprintf(lo, sizeof lo, "0x%llxULL", (unsigned long long)imm64);
            const bool combine = cmode < 12 && (cmode & 1);
            const std::string assignment = combine ? (op ? "&=" : "|=") : "=";
            put("c->vreg[" + std::to_string(rd) + "][0]" + assignment + lo + "; c->vreg[" +
                std::to_string(rd) + "][1]" + (Q ? assignment + lo : "=0") + ";");
            return true;
        }
    }

    // LD1R-LD4R: load consecutive elements and replicate each into its register.
    // R and opcode<0> select the structure count; S must be zero.
    if ((i & 0xBFDFD000) == 0x0D40C000 || (i & 0xBFC0D000) == 0x0DC0C000) {
        const u32 Q = (i >> 30) & 1;
        const bool post = ((i >> 23) & 1) != 0;
        const u32 rm = (i >> 16) & 31;
        const u32 size = (i >> 10) & 3;
        const u32 rn = (i >> 5) & 31, rt = i & 31;
        const u32 regs = ((((i >> 13) & 1) << 1) | ((i >> 21) & 1)) + 1;
        const int esz = 1 << size;
        const int bytes = Q ? 16 : 8;
        const int lanes = bytes / esz;
        const std::string uty = "uint" + std::to_string(esz * 8) + "_t";
        std::string s = "{ uint64_t _a=" + Xsp(rn) + "; ";
        for (u32 r = 0; r < regs; ++r) {
            const std::string vr = std::to_string((rt + r) & 31);
            s += "{ " + uty + " _e = (" + uty + ")recomp_load" +
                 std::to_string(esz * 8) + "(c,_a+" + std::to_string(r * esz) + "); ";
            s += uty + " _r[" + std::to_string(lanes) + "]; ";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=_e; ";
            s += "c->vreg[" + vr + "][0]=0; c->vreg[" + vr + "][1]=0; ";
            s += "memcpy(c->vreg[" + vr + "],_r," + std::to_string(bytes) + "); } ";
        }
        if (post) {
            const std::string step =
                (rm == 31) ? (std::to_string(regs * esz) + "ULL") : ("c->x[" + std::to_string(rm) + "]");
            s += "c->x[" + std::to_string(rn) + "]=_a+" + step + "; ";
        }
        s += "}";
        put(s);
        return true;
    }

    // FP <-> fixed-point conversions: the same shape as the integer forms
    // below but with bit 21 clear and a scale field. fbits is 64 - scale, and
    // the value is shifted by 2^fbits around the conversion. ldexp does that
    // exactly; multiplying by a built-up power of two does not.
    // Off deliberately, and measured: enabling this costs 25.0s against 20.7s on
    // the reference replay. Translating one instruction pulls its whole block out
    // of the JIT and into generated C, and these sit in float-heavy blocks the JIT
    // compiles well - a NaN test, two bound compares and a cast cannot beat the
    // single native instruction it replaces. Coverage only pays when the emitted C
    // is faster than the JIT for that block. Re-measure before flipping this.
    const bool kTranslateFixedPointConversions = g_translate_all;
    if (kTranslateFixedPointConversions &&
        (i & 0x5F200000) == 0x1E000000 && ((i >> 21) & 1) == 0) {
        const u32 sf = i >> 31, ftype = (i >> 22) & 3;
        const u32 rmode = (i >> 19) & 3, opcode = (i >> 16) & 7;
        const u32 fbits = 64 - ((i >> 10) & 0x3F);
        // fbits is fixed at translation time, so the scale is a literal rather
        // than a call into libm. 2^n is exact in binary floating point for every
        // n this encoding can name, so "%.1f" round-trips it without loss.
        char scale_lit[64];
        snprintf(scale_lit, sizeof scale_lit, "%.1f", std::pow(2.0, (double)fbits));
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        // A 32-bit destination only encodes scales that leave fbits in 1..32.
        const bool shaped = (ftype == 0 || ftype == 1) && fbits >= 1 && (sf || fbits <= 32);
        if (shaped) {
            const bool dbl = (ftype == 1);
            const char* ct = dbl ? "double" : "float";
            const int fsz = dbl ? 8 : 4;
                if (rmode == 3 && (opcode == 0 || opcode == 1) && rd != 31) {
                const bool is_signed = (opcode == 0);
                const char* it = sf ? (is_signed ? "int64_t" : "uint64_t")
                                    : (is_signed ? "int32_t" : "uint32_t");
                const char* lo_bound = sf ? (is_signed ? "-9223372036854775808.0" : "0.0")
                                          : (is_signed ? "-2147483648.0" : "0.0");
                const char* hi_bound = sf ? (is_signed ? "9223372036854775807.0"
                                                       : "18446744073709551615.0")
                                          : (is_signed ? "2147483647.0" : "4294967295.0");
                const char* sat_lo = sf ? (is_signed ? "0x8000000000000000ULL" : "0ULL")
                                        : (is_signed ? "0xFFFFFFFF80000000ULL" : "0ULL");
                const char* sat_hi = sf ? (is_signed ? "0x7FFFFFFFFFFFFFFFULL"
                                                     : "0xFFFFFFFFFFFFFFFFULL")
                                        : (is_signed ? "0x7FFFFFFFULL" : "0xFFFFFFFFULL");
                std::string s = "{ " + std::string(ct) + " _a; memcpy(&_a,&c->vreg[" +
                                std::to_string(rn) + "][0]," + std::to_string(fsz) + "); ";
                s += std::string("_a = _a * (") + ct + ")" + scale_lit + "; ";
                s += "uint64_t _r; if (_a != _a) _r = 0ULL; ";
                s += std::string("else if (!(_a > (") + ct + ")" + lo_bound + ")) _r = " + sat_lo + "; ";
                s += std::string("else if (!(_a < (") + ct + ")" + hi_bound + ")) _r = " + sat_hi + "; ";
                s += "else _r = (uint64_t)(" + std::string(it) + ")_a; ";
                if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
                s += "c->x[" + std::to_string(rd) + "] = _r; }";
                put(s);
                return true;
            }
            if (rmode == 0 && (opcode == 2 || opcode == 3)) {
                const std::string src = sf ? (opcode == 2 ? "(int64_t)" + Xz(rn)
                                                          : "(uint64_t)" + Xz(rn))
                                           : (opcode == 2 ? "(int32_t)" + Xz(rn)
                                                          : "(uint32_t)" + Xz(rn));
                std::string s = "{ double _t = (double)(" + src + "); ";
                s += std::string(ct) + " _r = (" + ct + ")(_t / " + scale_lit + "); ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; ";
                s += "memcpy(&c->vreg[" + std::to_string(rd) + "][0],&_r," +
                     std::to_string(fsz) + "); }";
                put(s);
                return true;
            }
        }
    }

    // FP <-> integer conversions and FMOV between register files. These share
    // bit 21 with the FP arithmetic forms and are distinguished by bits 15..10
    // being zero, so they must be decoded ahead of the arithmetic/compare block
    // below or every one of them is mis-decoded as FCMP.
    if ((i & 0x5F200000) == 0x1E200000 && ((i >> 21) & 1) && ((i >> 10) & 0x3F) == 0) {
        const u32 sf = i >> 31, ftype = (i >> 22) & 3;
        const u32 rmode = (i >> 19) & 3, opcode = (i >> 16) & 7;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        if (ftype == 0 || ftype == 1) {
            const bool dbl = (ftype == 1);
            const char* ct = dbl ? "double" : "float";
            const int fsz = dbl ? 8 : 4;
            const std::string zero_d = "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" +
                                       std::to_string(rd) + "][1]=0; ";

            // SCVTF / UCVTF: integer register -> FP register.
            if (rmode == 0 && (opcode == 2 || opcode == 3)) {
                const std::string src = sf ? (opcode == 2 ? "(int64_t)" + Xz(rn)
                                                          : "(uint64_t)" + Xz(rn))
                                           : (opcode == 2 ? "(int32_t)" + Xz(rn)
                                                          : "(uint32_t)" + Xz(rn));
                put("{ " + std::string(ct) + " _r = (" + ct + ")(" + src + "); " + zero_d +
                    "memcpy(&c->vreg[" + std::to_string(rd) + "][0],&_r," +
                    std::to_string(fsz) + "); }");
                return true;
            }

            // FCVT{N,P,M,Z}{S,U} and FCVTA{S,U}: FP register -> integer register.
            // rmode names the rounding mode; FCVTA{S,U} is the odd one out,
            // encoded as opcode 4/5 with rmode 0 and rounding halfway cases away
            // from zero.
            const bool cvt_away = (rmode == 0 && (opcode == 4 || opcode == 5));
            if ((opcode == 0 || opcode == 1 || cvt_away) && rd != 31) {
                const bool is_signed = cvt_away ? (opcode == 4) : (opcode == 0);
                const char* rnd = nullptr;
                if (cvt_away) {
                    rnd = dbl ? "round" : "roundf";
                } else if (rmode == 0) {
                    rnd = dbl ? "nearbyint" : "nearbyintf";
                } else if (rmode == 1) {
                    rnd = dbl ? "ceil" : "ceilf";
                } else if (rmode == 2) {
                    rnd = dbl ? "floor" : "floorf";
                }
                // rmode 3 needs no call: the cast below already truncates.
                const char* it = sf ? (is_signed ? "int64_t" : "uint64_t")
                                    : (is_signed ? "int32_t" : "uint32_t");
                std::string s = "{ " + std::string(ct) + " _a; memcpy(&_a,&c->vreg[" +
                                std::to_string(rn) + "][0]," + std::to_string(fsz) + "); ";
                // Round before saturating, the order the architecture specifies.
                if (rnd) s += std::string("_a = ") + rnd + "(_a); ";
                // FCVTZS/FCVTZU saturate: NaN gives 0, and anything outside the
                // destination's range clamps to that range's min or max. A bare
                // C cast is undefined for exactly those inputs, and on x86 it
                // compiles to cvttss2si, which answers "integer indefinite"
                // (INT_MIN / LLONG_MIN) for NaN, +inf and -inf alike. Float-heavy
                // game code converts out-of-range values constantly - clamped
                // indices, hashes, fixed-point - so the wrong answer here is a
                // steady drip of corruption rather than an immediate fault.
                const char* lo_bound = sf ? (is_signed ? "-9223372036854775808.0" : "0.0")
                                          : (is_signed ? "-2147483648.0" : "0.0");
                const char* hi_bound = sf ? (is_signed ? "9223372036854775807.0"
                                                       : "18446744073709551615.0")
                                          : (is_signed ? "2147483647.0" : "4294967295.0");
                const char* sat_lo = sf ? (is_signed ? "0x8000000000000000ULL" : "0ULL")
                                        : (is_signed ? "0xFFFFFFFF80000000ULL" : "0ULL");
                const char* sat_hi = sf ? (is_signed ? "0x7FFFFFFFFFFFFFFFULL"
                                                     : "0xFFFFFFFFFFFFFFFFULL")
                                        : (is_signed ? "0x7FFFFFFFULL" : "0xFFFFFFFFULL");
                s += "uint64_t _r; if (_a != _a) _r = 0ULL; ";
                s += std::string("else if (!(_a > (") + ct + ")" + lo_bound + ")) _r = " + sat_lo + "; ";
                s += std::string("else if (!(_a < (") + ct + ")" + hi_bound + ")) _r = " + sat_hi + "; ";
                s += "else _r = (uint64_t)(" + std::string(it) + ")_a; ";
                if (!sf) s += "_r &= 0xFFFFFFFFULL; ";
                s += "c->x[" + std::to_string(rd) + "] = _r; }";
                put(s);
                return true;
            }

            // FMOV between a general register and an FP register.
            if (rmode == 0 && (opcode == 6 || opcode == 7)) {
                if (opcode == 7) {           // GPR -> FP
                    put("{ " + zero_d + "memcpy(&c->vreg[" + std::to_string(rd) + "][0],&" +
                        (rn == 31 ? std::string("(uint64_t){0}") : "c->x[" + std::to_string(rn) + "]") +
                        "," + std::to_string(fsz) + "); }");
                    return true;
                }
                if (rd != 31) {              // FP -> GPR
                    put("{ uint64_t _r=0; memcpy(&_r,&c->vreg[" + std::to_string(rn) + "][0]," +
                        std::to_string(fsz) + "); c->x[" + std::to_string(rd) + "]=_r; }");
                    return true;
                }
            }
        }
    }

    // FCSEL - the floating-point conditional select. Shares the scalar FP
    // encoding but is picked out by bits 11..10 being 11, so it is matched
    // before the arithmetic and compare forms below.
    if ((i & 0x5F200C00) == 0x1E200C00) {
        const u32 ftype = (i >> 22) & 3;
        const u32 rm = (i >> 16) & 31, cond = (i >> 12) & 15;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        if (ftype == 0 || ftype == 1) {
            const int fsz = (ftype == 1) ? 8 : 4;
            // Selecting whole register halves rather than reinterpreting the
            // value keeps this exact for NaN payloads too.
            put("{ uint64_t _r = " + Cond(cond) + " ? c->vreg[" +
                std::to_string(rn) + "][0] : c->vreg[" + std::to_string(rm) + "][0]; " +
                (fsz == 4 ? "_r &= 0xFFFFFFFFULL; " : "") + "c->vreg[" + std::to_string(rd) +
                "][0]=_r; c->vreg[" + std::to_string(rd) + "][1]=0; }");
            return true;
        }
    }

    // Data-processing (1 source): RBIT, REV16, REV32, REV, CLZ and CLS.
    // The mask must stop at bit 16: bits 15..10 are the opcode being switched
    // on below, so including them would pin this to RBIT alone.
    if ((i & 0x7FFF0000) == 0x5AC00000) {
        const u32 sf = i >> 31, opcode = (i >> 10) & 0x3F;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const u32 width = sf ? 64 : 32;
        if (rd != 31) {
            const std::string w = std::to_string(width);
            const std::string src =
                sf ? Xz(rn) : ("(" + Xz(rn) + " & 0xFFFFFFFFULL)");
            std::string s;
            switch (opcode) {
            case 0:   // RBIT - reverse every bit
                s = "{ uint64_t _v=" + src + ", _r=0; for(int _i=0;_i<" + w +
                    ";_i++){ _r=(_r<<1)|((_v>>_i)&1ULL); } c->x[" + std::to_string(rd) + "]=_r; }";
                break;
            case 1:   // REV16 - reverse bytes within each halfword
                s = "{ uint64_t _v=" + src + ", _r=0; for(int _i=0;_i<" + w +
                    "/8;_i++){ int _p=(_i^1)*8; _r|=((_v>>(_i*8))&0xFFULL)<<_p; } c->x[" +
                    std::to_string(rd) + "]=_r; }";
                break;
            case 2:   // REV32 on 64-bit, REV on 32-bit: bytes within each word
                if (!sf) {
                    s = "{ uint64_t _v=" + src + ", _r=0; for(int _i=0;_i<4;_i++){ "
                        "_r|=((_v>>(_i*8))&0xFFULL)<<((3-_i)*8); } c->x[" +
                        std::to_string(rd) + "]=_r; }";
                } else {
                    s = "{ uint64_t _v=" + src + ", _r=0; for(int _i=0;_i<8;_i++){ "
                        "int _p=((_i&4)|(3-(_i&3)))*8; _r|=((_v>>(_i*8))&0xFFULL)<<_p; } c->x[" +
                        std::to_string(rd) + "]=_r; }";
                }
                break;
            case 3:   // REV (64-bit only) - reverse all bytes
                if (sf) {
                    s = "{ uint64_t _v=" + src + ", _r=0; for(int _i=0;_i<8;_i++){ "
                        "_r|=((_v>>(_i*8))&0xFFULL)<<((7-_i)*8); } c->x[" +
                        std::to_string(rd) + "]=_r; }";
                }
                break;
            case 4:   // CLZ
                s = "{ uint64_t _v=" + src + "; int _n=0; while(_n<" + w + " && !((_v>>(" + w +
                    "-1-_n))&1ULL)) _n++; c->x[" + std::to_string(rd) + "]=(uint64_t)_n; }";
                break;
            case 5:   // CLS - leading sign bits, not counting the sign itself
                s = "{ uint64_t _v=" + src + "; int _n=0; while(_n<" + w +
                    "-1 && (((_v>>(" + w + "-1-_n))&1ULL)==((_v>>(" + w +
                    "-2-_n))&1ULL))) _n++; c->x[" + std::to_string(rd) + "]=(uint64_t)_n; }";
                break;
            default: break;
            }
            if (!s.empty()) { put(s); return true; }
        }
    }

    // MRS/MSR against the small set of system registers a user-mode program
    // actually touches. Anything else stays on the fallback rather than being
    // silently invented.
    // SYS: 1101 0101 0000 1 op1 CRn CRm op2 Rt - the DC/IC/AT/TLBI space, which
    // the MRS/MSR handler below does not cover (that one matches op0=3 only).
    if ((i & 0xFFF80000) == 0xD5080000) {
        const u32 sys_op1 = (i >> 16) & 7;
        const u32 sys_crn = (i >> 12) & 0xF;
        const u32 sys_crm = (i >> 8) & 0xF;
        const u32 sys_op2 = (i >> 5) & 7;

        // DC ZVA. The host config reports DCZID_EL0=4: 16 words, or 64 bytes.
        // The operand may point anywhere within that naturally aligned block.
        // Use guest-memory helpers so hosted writes retain normal translation.
        if (sys_op1 == 3 && sys_crn == 7 && sys_crm == 4 && sys_op2 == 1) {
            put("{ uint64_t _base=" + Xz(i & 31) +
                "&~UINT64_C(63); for(unsigned _off=0;_off<64;_off+=8) "
                "recomp_store64(c,_base+_off,UINT64_C(0)); }");
            return true;
        }

        if ((i & 0xFFFFFFE0) == 0xD50B7520) {
            snprintf(buf, sizeof buf, "c->pc=g_module_base+0x%llxULL; ", (unsigned long long)pc);
            put(std::string(buf) + "recomp_ic_ivau(c," + Xz(i & 31) + ",g_recomp_guard_host_v2); return;");
            return false;
        }
        // Data cache clean / invalidate by VA: DC CVAC, CVAU, CVAP, CVADP,
        // CIVAC. Guest memory is host memory here with no emulated cache
        // hierarchy, so there is nothing to write back or discard.
        //
        // This is not an assumption about what is safe to skip: suyu never sets
        // Dynarmic's hook_data_cache_operations, so it stays false and the
        // fallback JIT compiles these to nothing. Emitting nothing matches the
        // other engine exactly.
        //
        // Deliberately narrow. DC ZVA above writes memory, whereas the
        // following neighbour requires instruction-cache handling:
        //   IC IVAU (CRm=5) invalidates the instruction cache, which suyu does
        //                   act on (InvalidateCacheRange, then halts the JIT).
        const bool is_dc_clean_invalidate =
            sys_op1 == 3 && sys_crn == 7 && sys_op2 == 1 &&
            (sys_crm == 10 || sys_crm == 11 || sys_crm == 12 || sys_crm == 13 || sys_crm == 14);
        if (is_dc_clean_invalidate) {
            put("/* dc clean/invalidate: no cache to maintain */");
            return true;
        }
        // Everything else in this space - the IC family, AT, TLBI -
        // goes to the fallback engine.
    }
    if ((i & 0xFFF00000) == 0xD5300000 || (i & 0xFFF00000) == 0xD5100000) {
        const bool is_read = ((i >> 21) & 1) != 0;   // MRS reads, MSR writes
        const u32 sysreg = (i >> 5) & 0x7FFF;
        const u32 rt = i & 31;
        // TPIDR_EL0 (thread pointer) and TPIDRRO_EL0 are the ones that matter
        // for ordinary code; both live in the context already. They are kept
        // in *separate* fields deliberately: TPIDR_EL0 is the guest's own
        // thread pointer, while TPIDRRO_EL0 is written by the kernel and holds
        // the thread-local region whose first bytes are the IPC message
        // buffer. Folding them together corrupts both.
        // NZCV. The flags live in the context as separate fields, so this is
        // a pack and an unpack rather than a plain load and store.
        constexpr u32 kNzcv = 0x5A10;
        if (sysreg == kNzcv) {
            if (is_read) {
                if (rt != 31) {
                    put("c->x[" + std::to_string(rt) +
                        "] = ((uint64_t)(c->n&1)<<31)|((uint64_t)(c->z&1)<<30)|"
                        "((uint64_t)(c->c&1)<<29)|((uint64_t)(c->v&1)<<28);");
                }
            } else {
                put("{ uint64_t _f=" + Xz(rt) +
                    "; c->n=(_f>>31)&1; c->z=(_f>>30)&1; c->c=(_f>>29)&1; c->v=(_f>>28)&1; }");
            }
            return true;
        }
        constexpr u32 kTpidrEl0 = 0x5E82;
        constexpr u32 kTpidrroEl0 = 0x5E83;
        if (sysreg == kTpidrEl0) {
            if (is_read) {
                if (rt != 31) put("c->x[" + std::to_string(rt) + "] = c->tpidr_el0;");
            } else {
                put("c->tpidr_el0 = " + Xz(rt) + ";");
            }
            return true;
        }
        // FPCR/FPSR. sysreg here is o0 op1 CRn CRm op2, so FPCR (op0=3, op1=3,
        // CRn=4, CRm=4, op2=0) packs to 0x5A20 and FPSR to 0x5A21.
        //
        // These are stored and returned rather than acted on: the generated C
        // computes in the host's default rounding mode and nothing here changes
        // that. Keeping the value is still strictly better than dropping it -
        // code that saves FPCR, changes it, and restores it now round-trips,
        // and the value survives a transition to the JIT, which does honour it.
        // A recompiler that actually implemented the rounding modes would set
        // the host FP mode here instead.
        constexpr u32 kFpcr = 0x5A20;
        constexpr u32 kFpsr = 0x5A21;
        if (sysreg == kFpcr || sysreg == kFpsr) {
            const char* field = (sysreg == kFpcr) ? "fpcr" : "fpsr";
            if (is_read) {
                if (rt != 31) {
                    put("c->x[" + std::to_string(rt) + "] = c->" + std::string(field) + ";");
                }
            } else {
                put("c->" + std::string(field) + " = " + Xz(rt) + ";");
            }
            return true;
        }
        // CNTPCT_EL0 / CNTVCT_EL0 / CNTFRQ_EL0. Packed the same way as the
        // registers above: o0 op1 CRn CRm op2, so CNTPCT (op0=3, op1=3, CRn=14,
        // CRm=0, op2=1) is 0x5F01.
        //
        // The counter is read from the emulator's own timing source, the same
        // one the fallback JIT uses (DynarmicCallbacks64::GetCNTPCT ->
        // CoreTiming::GetClockTicks). Two independent clocks would let the
        // guest observe time moving backwards across an engine transition.
        constexpr u32 kCntfrqEl0 = 0x5F00;
        constexpr u32 kCntpctEl0 = 0x5F01;
        constexpr u32 kCntvctEl0 = 0x5F02;
        if (sysreg == kCntpctEl0 || sysreg == kCntvctEl0) {
            // CNTVCT is the virtual counter. With no hypervisor offset it reads
            // the same as the physical one, which is what the guest sees on a
            // Switch and what the JIT reports.
            if (is_read) {
                if (rt != 31) {
                    put("c->x[" + std::to_string(rt) + "] = recomp_cntpct(c);");
                } else {
                    put("(void)recomp_cntpct(c);");
                }
            } else {
                // Writing the counter traps at EL0; swallow it.
                put("(void)" + Xz(rt) + ";");
            }
            return true;
        }
        if (sysreg == kCntfrqEl0) {
            if (is_read) {
                if (rt != 31) {
                    // Core::Hardware::CNTFREQ. Fixed on this platform.
                    put("c->x[" + std::to_string(rt) + "] = 19200000ULL;");
                }
            } else {
                put("(void)" + Xz(rt) + ";");
            }
            return true;
        }
        // CTR_EL0, the cache type register (op0=3 op1=3 CRn=0 CRm=0 op2=1).
        //
        // A constant, and read constantly: 31% of every transition to the
        // fallback engine on a matrix-heavy title was this one instruction, most
        // of a matrix-heavy title's remaining gap spent marshalling the whole
        // context into dynarmic to fetch a number that never changes.
        //
        // The value is the one suyu configures dynarmic with
        // (arm_dynarmic_64.cpp: config.ctr_el0), not a plausible-looking
        // constant. Code that reads a cache line size once and relies on it
        // later must not see it change when a thread crosses between engines.
        constexpr u32 kCtrEl0 = 0x5801;
        if (sysreg == kCtrEl0) {
            if (is_read) {
                if (rt != 31) {
                    put("c->x[" + std::to_string(rt) + "] = 0x8444c004ULL;");
                }
            } else {
                // Read-only at EL0; a write traps on hardware.
                put("(void)" + Xz(rt) + ";");
            }
            return true;
        }
        if (sysreg == kTpidrroEl0) {
            if (is_read) {
                if (rt != 31) put("c->x[" + std::to_string(rt) + "] = c->tpidrro_el0;");
            } else {
                // Read-only at EL0; a write traps on hardware. Swallow it
                // rather than letting it destroy the kernel's TLS pointer.
                put("(void)" + Xz(rt) + ";");
            }
            return true;
        }
    }

    // Scalar S/D register FP comparisons. Ordered comparisons signal every
    // NaN; equality signals only sNaN. Baseline FZ/IDC, no host FP operations.
    {
        const u32 key = i & 0xFF20FC00;
        const bool eq = key == 0x5E20E400 && !(i & 0x00800000);
        const bool ordered = key == 0x7E20E400;
        if (eq || ordered) {
            const bool dbl = (i & 0x00400000) != 0, gt = (i & 0x00800000) != 0;
            const unsigned rd=i&31,rn=(i>>5)&31,rm=(i>>16)&31;
            const std::string ct=dbl?"uint64_t":"uint32_t",sz=dbl?"8":"4";
            put("{ "+ct+" _aa,_bb;memcpy(&_aa,c->vreg["+std::to_string(rn)+"],"+sz+
                ");memcpy(&_bb,c->vreg["+std::to_string(rm)+"],"+sz+
                ");uint64_t _a=_aa,_b=_bb,_r=0;const uint64_t _sign="+
                (dbl?"0x8000000000000000ULL":"0x80000000ULL")+",_exp="+
                (dbl?"0x7ff0000000000000ULL":"0x7f800000ULL")+",_frac="+
                (dbl?"0xfffffffffffffULL":"0x7fffffULL")+",_quiet="+
                (dbl?"0x8000000000000ULL":"0x400000ULL")+";"
                "if(c->fpcr&(1ULL<<24)) {if(!(_a&_exp)&&(_a&_frac)) {_a&=_sign;c->fpsr|=128;}"
                "if(!(_b&_exp)&&(_b&_frac)) {_b&=_sign;c->fpsr|=128;}}"
                "int _an=(_a&_exp)==_exp&&(_a&_frac),_bn=(_b&_exp)==_exp&&(_b&_frac);"
                "if(_an||_bn) { if("+std::string(eq?"(_an&&!(_a&_quiet))||(_bn&&!(_b&_quiet))":"1")+
                ")c->fpsr|=1; }else {int _equal=_a==_b||!((_a|_b)&~_sign);"
                "int _greater=!_equal&&(((_a^_b)&_sign)?!(_a&_sign):((_a&_sign)?_a<_b:_a>_b));"
                "(void)_quiet;(void)_greater; if("+std::string(eq?"_equal":gt?"_greater":"_equal||_greater")+
                ")_r="+(dbl?"UINT64_MAX":"UINT32_MAX")+";}c->vreg["+std::to_string(rd)+
                "][0]=_r;c->vreg["+std::to_string(rd)+"][1]=0;}");
            return true;
        }
    }

    // Scalar FRECPS/FRSQRTS S/D, evaluated exactly before one rounding.
    // A fixed multiword product lattice covers every finite input exponent.
    // This deliberately favors correctness over speed; no host fma or
    // intermediate halve can introduce double rounding. Baseline FP modes
    // only: exception/access trap delivery and FEAT_AFP remain unsupported.
    if ((i & 0xFF20FC00) == 0x5E20FC00) {
        const bool dbl=(i&0x00400000)!=0,half=(i&0x00800000)!=0;
        const unsigned rd=i&31,rn=(i>>5)&31,rm=(i>>16)&31;
        const std::string ct=dbl?"uint64_t":"uint32_t",sz=dbl?"8":"4";
        put("{ "+ct+" _aa,_bb;memcpy(&_aa,c->vreg["+std::to_string(rn)+"],"+sz+
            ");memcpy(&_bb,c->vreg["+std::to_string(rm)+"],"+sz+
            ");const unsigned _f="+(dbl?"52":"23")+",_bias="+(dbl?"1023":"127")+
            ",_count="+(dbl?"67":"9")+";const int _base="+
            std::to_string((dbl?-2148:-298)-(half?1:0))+";"
            "const uint64_t _hidden=1ULL<<_f,_frac=_hidden-1,_quiet=_hidden>>1,_exp="+
            (dbl?"0x7ff0000000000000ULL":"0x7f800000ULL")+",_sign="+
            (dbl?"0x8000000000000000ULL":"0x80000000ULL")+";"
            // FPNeg precedes NaN processing in the architectural pseudocode.
            "uint64_t _a=(uint64_t)_aa^_sign,_b=_bb,_v=0;unsigned _mode=(unsigned)(c->fpcr>>22)&3;"
            "if(c->fpcr&(1ULL<<24)) {if(!(_a&_exp)&&(_a&_frac)) {_a&=_sign;c->fpsr|=128;}"
            "if(!(_b&_exp)&&(_b&_frac)) {_b&=_sign;c->fpsr|=128;}}"
            "int _an=(_a&_exp)==_exp&&(_a&_frac),_bn=(_b&_exp)==_exp&&(_b&_frac);"
            "if(_an||_bn) {int _as=_an&&!(_a&_quiet),_bs=_bn&&!(_b&_quiet);"
            "_v=(_as?_a:_bs?_b:_an?_a:_b)|_quiet;if(_as||_bs) {c->fpsr|=1;}"
            "if(c->fpcr&(1ULL<<25)) {_v=_exp|_quiet;}}"
            "else if(((_a&_exp)==_exp&&!(_b&~_sign))||((_b&_exp)==_exp&&!(_a&~_sign)))"
            "_v="+(dbl?(half?"0x3ff8000000000000ULL":"0x4000000000000000ULL"):
                         (half?"0x3fc00000ULL":"0x40000000ULL"))+";"
            "else if((_a&_exp)==_exp||(_b&_exp)==_exp)_v=_exp|((_a^_b)&_sign);"
            "else {unsigned _ea=(unsigned)((_a&_exp)>>_f),_eb=(unsigned)((_b&_exp)>>_f);"
            "uint64_t _ma=(_a&_frac)|(_ea?_hidden:0),_mb=(_b&_frac)|(_eb?_hidden:0);"
            "uint64_t _p["+std::string(dbl?"67":"9")+"]={0},_c["+(dbl?"67":"9")+"]={0};"
            "uint64_t _a0=(uint32_t)_ma,_a1=_ma>>32,_b0=(uint32_t)_mb,_b1=_mb>>32;"
            "uint64_t _lo=_a0*_b0,_x=_a0*_b1,_y=_a1*_b0,_hi=_a1*_b1+(_x>>32)+(_y>>32);"
            "uint64_t _old=_lo;_lo+=_x<<32;_hi+=_lo<_old;_old=_lo;_lo+=_y<<32;_hi+=_lo<_old;"
            "unsigned _shift=(_ea?_ea-1:0)+(_eb?_eb-1:0),_j=_shift/64,_s=_shift%64;"
            "_p[_j]=_lo<<_s;_p[_j+1]=(_hi<<_s)|(_s?_lo>>(64-_s):0);"
            "if(_s&&_j+2<_count)_p[_j+2]=_hi>>(64-_s);"
            "unsigned _bit="+std::string(dbl?"2149":"299")+";_c[_bit/64]|=1ULL<<(_bit%64);");
        if(half)put("--_bit;_c[_bit/64]|=1ULL<<(_bit%64);");
        put("unsigned _negative=0;"
            "if((_a^_b)&_sign) {int _compare=0;for(int _k=(int)_count-1;_k>=0;--_k)"
            "{if(_p[_k]!=_c[_k]) {_compare=_p[_k]>_c[_k]?1:-1;break;}}"
            "_negative=_compare>0;uint64_t _borrow=0;for(unsigned _k=0;_k<_count;++_k)"
            "{uint64_t _left=_negative?_p[_k]:_c[_k],_right=_negative?_c[_k]:_p[_k];"
            "uint64_t _diff=_left-_right,_next=(_left<_right)||(_diff<_borrow);"
            "_p[_k]=_diff-_borrow;_borrow=_next;}}"
            "else {uint64_t _carry=0;for(unsigned _k=0;_k<_count;++_k)"
            "{uint64_t _sum=_p[_k]+_c[_k],_next=_sum<_p[_k];"
            "uint64_t _total=_sum+_carry;_carry=_next||_total<_sum;_p[_k]=_total;}}"
            "int _top=(int)(_count*64)-1;while(_top>=0&&!((_p[(unsigned)_top/64]>>(_top%64))&1))--_top;"
            "if(_top<0)_v=_mode==2?_sign:0;else {int _e=_top+_base,_emin=1-(int)_bias;"
            "if(_e<_emin&&(c->fpcr&(1ULL<<24))) {c->fpsr|=8;_v=_negative?_sign:0;}"
            "else {int _cut=_top-(int)_f,_mincut=_emin-(int)_f-_base;if(_cut<_mincut)_cut=_mincut;"
            "unsigned _index=(unsigned)_cut/64,_offset=(unsigned)_cut%64;"
            "uint64_t _mant=_p[_index]>>_offset;if(_offset&&_index+1<_count)_mant|=_p[_index+1]<<(64-_offset);"
            "unsigned _roundbit=(unsigned)((_p[(unsigned)(_cut-1)/64]>>((_cut-1)%64))&1),_sticky=0;"
            "for(int _k=0;_k<_cut-1;++_k)_sticky|=(unsigned)((_p[(unsigned)_k/64]>>(_k%64))&1);"
            "unsigned _lost=_roundbit|_sticky;if(_lost) {c->fpsr|=16;if(_e<_emin)c->fpsr|=8;"
            "if((_mode==0&&_roundbit&&(_sticky||(_mant&1)))||(_mode==1&&!_negative)||(_mode==2&&_negative))++_mant;}"
            "if(_e<_emin)_e=_emin;if(_mant>=(_hidden<<1)) {_mant>>=1;++_e;}"
            "if(_e>(int)_bias) {c->fpsr|=20;_v=(_mode==0||(_mode==1&&!_negative)||(_mode==2&&_negative))?_exp:_exp-1;}"
            "else _v=(_mant>=_hidden?(uint64_t)(_e+(int)_bias)<<_f:0)|(_mant&_frac);"
            "if(_negative)_v|=_sign;}}}c->vreg["+std::to_string(rd)+"][0]=_v;c->vreg["+
            std::to_string(rd)+"][1]=0;}");
        return true;
    }

    // FSQRT vector and FABD vector/scalar, S/D. Integer significands make
    // rounding independent of host FP modes. Baseline DN/FZ/RMode and FPSR
    // flags are supported; FP access/exception traps and FEAT_AFP are not.
    {
        const bool sqrt_vec = (i & 0xBFBFFC00) == 0x2EA1F800;
        const bool abd_vec = (i & 0xBFA0FC00) == 0x2EA0D400;
        const bool abd_scalar = (i & 0xFFA0FC00) == 0x7EA0D400;
        const bool dbl = ((i >> 22) & 1) != 0;
        const bool q = ((i >> 30) & 1) != 0;
        if (((sqrt_vec || abd_vec) && (!dbl || q)) || abd_scalar) {
            const unsigned rd = i & 31, rn = (i >> 5) & 31, rm = (i >> 16) & 31;
            const unsigned lanes = abd_scalar ? 1 : (q ? 16 : 8) / (dbl ? 8 : 4);
            const std::string ct = dbl ? "uint64_t" : "uint32_t";
            put("{ " + ct + " _n[" + std::string(dbl ? "2" : "4") + "],_m[" +
                (dbl ? "2" : "4") + "],_r[" + (dbl ? "2" : "4") +
                "]={0}; memcpy(_n,c->vreg[" + std::to_string(rn) +
                "],16); memcpy(_m,c->vreg[" + std::to_string(rm) + "],16);");
            put("for(unsigned _lane=0;_lane<" + std::to_string(lanes) +
                ";++_lane) { uint64_t _a=_n[_lane],_b=" +
                std::string(sqrt_vec ? "0" : "_m[_lane]") + ",_v=0;"
                " const unsigned _f=" + (dbl ? "52" : "23") + "; const uint64_t _hidden=1ULL<<_f,"
                " _frac=_hidden-1,_exp=" + (dbl ? "0x7ff0000000000000ULL" : "0x7f800000ULL") +
                ",_sign=" + (dbl ? "0x8000000000000000ULL" : "0x80000000ULL") +
                ",_quiet=_hidden>>1; " + std::string(sqrt_vec ? "unsigned _mode=(unsigned)(c->fpcr>>22)&3;" : "") +
                " if(c->fpcr&(1ULL<<24)) {"
                " if(!(_a&_exp)&&(_a&_frac)) { _a&=_sign;c->fpsr|=128; }"
                " if(!(_b&_exp)&&(_b&_frac)) { _b&=_sign;c->fpsr|=128; } }"
                " int _an=(_a&_exp)==_exp&&(_a&_frac),_bn=(_b&_exp)==_exp&&(_b&_frac);"
                " int _as=_an&&!(_a&_quiet),_bs=_bn&&!(_b&_quiet);"
                " if(_an||_bn) { _v=(_as?_a:_bs?_b:_an?_a:_b)|_quiet;"
                " if(_as||_bs) { c->fpsr|=1; } if(c->fpcr&(1ULL<<25)) { _v=_exp|_quiet; } }");
            if (sqrt_vec) {
                put("else if(!(_a&~_sign))_v=_a;"
                    " else if(_a&_sign) { _v=_exp|_quiet;c->fpsr|=1; }"
                    " else if((_a&_exp)==_exp)_v=_a;"
                    " else { const int _bias=" + std::string(dbl ? "1023" : "127") +
                    "; uint64_t _magnitude=_a&_frac;"
                    " int _e=(int)((_a&_exp)>>_f)-(int)_bias;"
                    " if(_a&_exp)_magnitude|=_hidden; else { ++_e;"
                    " while(_magnitude<_hidden) { _magnitude<<=1;--_e; } }"
                    " if(_e%2) { _magnitude<<=1;--_e; }"
                    // Restoring square root of magnitude * 2^fraction_bits.
                    // The radicand can exceed 64 bits, but only two bits are
                    // consumed each step; root and remainder fit uint64_t.
                    " uint64_t _root=0,_rem=0;"
                    " for(int _k=(int)_f;_k>=0;--_k) { int _shift=2*_k-(int)_f;"
                    " uint64_t _pair=(_shift>=0?_magnitude>>_shift:_magnitude<<(-_shift))&3;"
                    " _rem=(_rem<<2)|_pair;uint64_t _trial=(_root<<2)|1;_root<<=1;"
                    " if(_rem>=_trial) { _rem-=_trial;_root|=1; } }"
                    " if(_rem) { c->fpsr|=16;"
                    " if(_mode==1||(_mode==0&&_rem>_root))++_root; }"
                    " _e=_e/2+(int)_bias;"
                    " if(_root>=(_hidden<<1)) { _root>>=1;++_e; }"
                    " _v=((uint64_t)_e<<_f)|(_root&_frac); }");
            } else {
                put("else " + EmitFPAddSubValue(dbl, true));
            }
            put("_r[_lane]=(" + ct + ")(_v" + (sqrt_vec ? std::string("") : "&~_sign") +
                "); } memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }");
            return true;
        }
    }

    // SIMD S/D min/max: Arm FPMin/FPMax/FPMinNum/FPMaxNum, with bitwise
    // ordering so host NaN, signed-zero and denormal modes cannot intervene.
    // Implements baseline FPCR.DN/FZ and cumulative FPSR.IOC/IDC. FP exception
    // trap delivery and later FEAT_AFP modes are not implemented here.
    {
        const u32 vec_key = i & 0x9F20FC00;
        const u32 pair_key = i & 0xFF3FFC00;
        const u32 reduce_key = i & 0xFF7FFC00;
        const bool vec = vec_key == 0x0E20F400 || vec_key == 0x0E20C400;
        const bool scalar_pair = pair_key == 0x7E30F800 || pair_key == 0x7E30C800;
        const bool reduce = reduce_key == 0x6E30F800 || reduce_key == 0x6E30C800;
        const bool dbl = ((i >> 22) & 1) != 0;
        const bool q = ((i >> 30) & 1) != 0;
        if ((vec && (!dbl || q)) || scalar_pair || reduce) {
            const bool minimum = ((i >> 23) & 1) != 0;
            const bool numeric = ((i >> 12) & 3) == 0;
            const bool pair = vec && ((i >> 29) & 1);
            const u32 rn = (i >> 5) & 31, rm = (i >> 16) & 31, rd = i & 31;
            const unsigned lanes = dbl ? 2 : 4;
            const unsigned active = q ? lanes : lanes / 2;
            const std::string ct = dbl ? "uint64_t" : "uint32_t";
            put("{ " + ct + " _n[" + std::to_string(lanes) + "],_m[" +
                std::to_string(lanes) + "],_r[" + std::to_string(lanes) +
                "]={0}; memcpy(_n,c->vreg[" + std::to_string(rn) +
                "],16); memcpy(_m,c->vreg[" + std::to_string(rm) + "],16);");
            const auto element = [](const char* array, unsigned e) {
                return std::string(array) + "[" + std::to_string(e) + "]";
            };
            const auto emit_pair = [&](const std::string& a, const std::string& b,
                                       const std::string& dest) {
                put("{ uint64_t _a=" + a + ",_b=" + b + ",_v; const uint64_t _sign=" +
                    (dbl ? std::string("0x8000000000000000ULL") : "0x80000000ULL") +
                    ",_exp=" + (dbl ? "0x7ff0000000000000ULL" : "0x7f800000ULL") +
                    ",_frac=" + (dbl ? "0xfffffffffffffULL" : "0x7fffffULL") +
                    ",_quiet=" + (dbl ? "0x8000000000000ULL" : "0x400000ULL") + ";");
                put("if(c->fpcr & (1ULL<<24)) {"
                    " if(!(_a&_exp)&&(_a&_frac)) { _a&=_sign; c->fpsr|=128; }"
                    " if(!(_b&_exp)&&(_b&_frac)) { _b&=_sign; c->fpsr|=128; } }");
                put("{ int _an=(_a&_exp)==_exp&&(_a&_frac),"
                    " _bn=(_b&_exp)==_exp&&(_b&_frac);"
                    " int _as=_an&&!(_a&_quiet),_bs=_bn&&!(_b&_quiet);"
                    " if(_as||_bs) { c->fpsr|=1; _v=(_as?_a:_b)|_quiet; }");
                if (numeric) {
                    put("else if(_an&&!_bn) _v=_b; else if(_bn&&!_an) _v=_a;");
                }
                put("else if(_an||_bn) _v=(_an?_a:_b)|_quiet;"
                    " else if(!((_a|_b)&~_sign)) _v=" +
                    std::string(minimum ? "(_a|_b)" : "(_a&_b)") + ";"
                    " else { int _less=((_a^_b)&_sign)?!!(_a&_sign):"
                    " ((_a&_sign)?_a>_b:_a<_b); _v=" +
                    std::string(minimum ? "(_less?_a:_b)" : "(_less?_b:_a)") + "; }");
                put("if((c->fpcr&(1ULL<<25))&&((_v&_exp)==_exp)&&(_v&_frac))"
                    " { _v=_exp|_quiet; } " + dest + "=(" + ct + ")_v; } }");
            };
            if (reduce) {
                // Architectural reduction is a balanced tree, not a left fold:
                // the distinction affects signaling/quiet NaN selection.
                emit_pair("_n[0]", "_n[1]", "_n[0]");
                emit_pair("_n[2]", "_n[3]", "_n[2]");
                emit_pair("_n[0]", "_n[2]", "_r[0]");
            } else if (scalar_pair) {
                emit_pair("_n[0]", "_n[1]", "_r[0]");
            } else {
                for (unsigned e = 0; e < active; ++e) {
                    const char* source = e < active / 2 ? "_n" : "_m";
                    emit_pair(pair ? element(source, 2 * (e % (active / 2))) : element("_n", e),
                              pair ? element(source, 2 * (e % (active / 2)) + 1) : element("_m", e),
                              element("_r", e));
                }
            }
            put("memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }");
            return true;
        }
    }

    // Absolute FP comparisons. IEEE magnitudes order as unsigned integers;
    // both quiet and signaling NaNs raise IOC for ordered GE/GT comparisons.
    {
        const bool scalar = (i & 0xFF20FC00) == 0x7E20EC00;
        const bool vector = (i & 0xBF20FC00) == 0x2E20EC00;
        const bool dbl = (i & (1U << 22)) != 0, q = (i & (1U << 30)) != 0;
        if (scalar || (vector && (!dbl || q))) {
            const unsigned rd=i&31, rn=(i>>5)&31, rm=(i>>16)&31;
            const unsigned lanes=dbl?2:4, active=scalar?1:q?lanes:lanes/2;
            const std::string ct=dbl?"uint64_t":"uint32_t";
            put("{ " + ct + " _n[" + std::to_string(lanes) + "],_m[" +
                std::to_string(lanes) + "],_r[" + std::to_string(lanes) +
                "]={0}; memcpy(_n,c->vreg[" + std::to_string(rn) +
                "],16); memcpy(_m,c->vreg[" + std::to_string(rm) + "],16);");
            put("for(unsigned _j=0;_j<" + std::to_string(active) + ";++_j) {"
                " uint64_t _a=_n[_j]&" + std::string(dbl?"0x7fffffffffffffffULL":"0x7fffffffULL") +
                ",_b=_m[_j]&" + (dbl?"0x7fffffffffffffffULL":"0x7fffffffULL") +
                "; const uint64_t _exp=" + (dbl?"0x7ff0000000000000ULL":"0x7f800000ULL") +
                ",_min=" + (dbl?"0x10000000000000ULL":"0x800000ULL") + ";"
                " if(c->fpcr&(1ULL<<24)) { if(_a&&_a<_min) { _a=0; c->fpsr|=128; }"
                " if(_b&&_b<_min) { _b=0; c->fpsr|=128; } }"
                " if(_a>_exp||_b>_exp) c->fpsr|=1; else if(_a" +
                std::string((i&(1U<<23))?">":">=") + "_b) _r[_j]=~(" + ct + ")0;"
                " } memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }");
            return true;
        }
    }

    // Vector float/fixed conversions and nearest integral conversions. Keep
    // saturation and rounding in integer fields; invalid suppresses inexact.
    {
        const u32 key=i&0x9FBFFC00, fixed_key=i&0x9F80FC00;
        const u32 scalar_key=i&0xDFBFFC00;
        const bool scalar=scalar_key==0x5E21A800||scalar_key==0x5E21C800;
        const bool nearest=key==0x0E21A800||scalar_key==0x5E21A800;
        const bool away=key==0x0E21C800||scalar_key==0x5E21C800;
        const bool fixed=fixed_key==0x0F00FC00, to_float=fixed_key==0x0F00E400;
        const unsigned imm=(i>>16)&127;
        const bool dbl=(fixed||to_float)?(imm&64)!=0:(i&(1U<<22))!=0;
        const bool q=(i&(1U<<30))!=0, uns=(i&(1U<<29))!=0;
        if((nearest||away||((fixed||to_float)&&imm>=32))&&(!dbl||q)) {
            const unsigned width=dbl?64:32,frac=dbl?52:23,bias=dbl?1023:127;
            const unsigned fbits=(fixed||to_float)?2*width-imm:0;
            const unsigned rd=i&31,rn=(i>>5)&31,active=scalar?1:q?128/width:64/width;
            const std::string ct=dbl?"uint64_t":"uint32_t";
            const auto lit=[](uint64_t v){return std::to_string(v)+"ULL";};
            put("{ "+ct+" _src["+std::to_string(128/width)+"],_dst["+
                std::to_string(128/width)+"]={0}; memcpy(_src,c->vreg["+std::to_string(rn)+"],16);"
                " for(unsigned _j=0;_j<"+std::to_string(active)+";++_j) { uint64_t _v=_src[_j],_r=0;");
            if(to_float) {
                put("int _neg="+std::string(uns?"0":"!!(_v&"+lit(1ULL<<(width-1))+")")+";"
                    " uint64_t _mag=_neg?((~_v+1)&"+lit(dbl?~0ULL:0xffffffffULL)+"):_v;"
                    " if(_mag) { unsigned _top=0; uint64_t _scan=_mag; while(_scan>>1) { _scan>>=1; ++_top; }"
                    " int _shift=(int)_top-"+std::to_string(frac)+";"
                    " uint64_t _whole=_shift>0?_mag>>_shift:_mag<<(-_shift);"
                    " uint64_t _lost=_shift>0?_mag&((1ULL<<_shift)-1):0; unsigned _mode=(unsigned)((c->fpcr>>22)&3);"
                    " int _up=_lost&&(_mode==0?(_lost>(1ULL<<(_shift-1))||(_lost==(1ULL<<(_shift-1))&&(_whole&1))):"
                    " _mode==1?!_neg:_mode==2?_neg:0); _whole+=_up; if(_whole>="+lit(1ULL<<(frac+1))+") { _whole>>=1; ++_top; }"
                    " _r=((uint64_t)_neg<<"+std::to_string(width-1)+")|((uint64_t)((int)_top-"+
                    std::to_string(fbits)+"+"+std::to_string(bias)+")<<"+std::to_string(frac)+")|(_whole&"+
                    lit((1ULL<<frac)-1)+"); if(_lost)c->fpsr|=16; }");
            } else {
                put("unsigned _exp=(unsigned)((_v>>"+std::to_string(frac)+")&"+std::to_string(2*bias+1)+");"
                    " uint64_t _mant=_v&"+lit((1ULL<<frac)-1)+"; int _neg=!!(_v&"+lit(1ULL<<(width-1))+");"
                    " int _invalid=0,_inexact=0; uint64_t _mag=0,_limit="+
                    std::string(uns?lit(dbl?~0ULL:0xffffffffULL):"(_neg?"+lit(1ULL<<(width-1))+":"+lit((1ULL<<(width-1))-1)+")")+";"
                    " if(_exp=="+std::to_string(2*bias+1)+") { _invalid=1; _mag=_mant?0:_limit;"
                    " if(_mant)_neg=0; } else if(!_exp&&_mant&&(c->fpcr&(1ULL<<24))) c->fpsr|=128;"
                    " else if(_exp||_mant) { int _e=(_exp?(int)_exp-"+std::to_string(bias)+":"+
                    std::to_string(1-(int)bias)+")+"+std::to_string(fbits)+"; if(_exp)_mant|="+lit(1ULL<<frac)+";"
                    " if(_e>="+std::to_string(width)+") { _invalid=1; _mag=_limit; } else {"
                    " int _shift="+std::to_string(frac)+"-_e; uint64_t _lost=_shift<=0?0:_shift<64?_mant&((1ULL<<_shift)-1):_mant;"
                    " _mag=_shift<=0?_mant<<(-_shift):_shift<64?_mant>>_shift:0; _inexact=_lost!=0;");
                if(nearest||away)put("if(_lost&&_shift<64&&(_lost>(1ULL<<(_shift-1))||(_lost==(1ULL<<(_shift-1))&&"+
                    std::string(away?"1":"(_mag&1)")+"))) ++_mag;");
                put("if(_mag>_limit) { _mag=_limit; _invalid=1; } } }");
                if(uns)put("if(_neg&&_mag) { _mag=0; _invalid=1; }");
                put("_r=_neg?0-_mag:_mag; if(_invalid)c->fpsr|=1; else if(_inexact)c->fpsr|=16;");
            }
            put("_dst[_j]=("+ct+")_r; } memcpy(c->vreg["+std::to_string(rd)+"],_dst,16); }");
            return true;
        }
    }

    // Baseline half conversions observe AHP, but ignore FZ16 (FPUnpackCV /
    // FPRoundCV). This is distinct from optional half arithmetic instructions.
    {
        const u32 key=i&0xFFFFFC00, vkey=i&0xBFFFFC00;
        const bool scalar=key==0x1E624000||key==0x1E22C000||key==0x1E23C000||
            key==0x1E63C000||key==0x1EE24000||key==0x1EE2C000;
        const bool vw=vkey==0x0E217800, vn=vkey==0x0E216800;
        if(scalar||vw||vn) {
            const unsigned rd=i&31,rn=(i>>5)&31;
            const bool upper=(i&(1U<<30))!=0;
            const unsigned from=vw?16:vn?32:((i>>22)&3)==0?32:((i>>22)&3)==1?64:16;
            const unsigned to=vw?32:vn?16:((i>>15)&3)==0?32:((i>>15)&3)==1?64:16;
            const unsigned sf=from==16?10:from==32?23:52, df=to==16?10:to==32?23:52;
            const unsigned sb=from==16?15:from==32?127:1023, db=to==16?15:to==32?127:1023;
            const auto literal=[](uint64_t v){return std::to_string(v)+"ULL";};
            const uint64_t sm=(1ULL<<sf)-1,dm=(1ULL<<df)-1;
            const std::string src="uint"+std::to_string(from)+"_t",dst="uint"+std::to_string(to)+"_t";
            put("{ "+src+" _src["+std::to_string(128/from)+"]; "+dst+" _dst["+
                std::to_string(128/to)+"]={0}; memcpy(_src,c->vreg["+std::to_string(rn)+"],16);");
            if(vn&&upper)put("memcpy(_dst,c->vreg["+std::to_string(rd)+"],16);");
            put("for(unsigned _j=0;_j<"+std::to_string(scalar?1:4)+";++_j) { uint64_t _v=_src[_j+"+
                std::to_string(vw&&upper?4:0)+"],_f=_v&"+literal(sm)+",_r; unsigned _e=(unsigned)((_v>>"+
                std::to_string(sf)+")&"+std::to_string(2*sb+1)+"); uint64_t _sign=(_v>>"+
                std::to_string(from-1)+")<<"+std::to_string(to-1)+"; unsigned _mode=(unsigned)((c->fpcr>>22)&3);"
                " int _ahp="+std::string(to==16?"!!(c->fpcr&(1ULL<<26))":"0")+"; _r=_sign;");
            put("if(_e=="+std::to_string(2*sb+1)+std::string(from==16?"&&!(c->fpcr&(1ULL<<26))":"")+
                ") { if(_f) { if(!(_f&"+literal(1ULL<<(sf-1))+")||_ahp) c->fpsr|=1; if(!_ahp) {"
                " _r|="+literal((uint64_t)(2*db+1)<<df)+"|"+
                std::string(sf>df?"(_f>>"+std::to_string(sf-df)+")":"(_f<<"+std::to_string(df-sf)+")")+
                "|"+literal(1ULL<<(df-1))+"; if(c->fpcr&(1ULL<<25)) _r="+
                literal(((uint64_t)(2*db+1)<<df)|(1ULL<<(df-1)))+"; } } else {"
                " if(_ahp) { _r|="+literal((1ULL<<(to-1))-1)+"; c->fpsr|=1; } else _r|="+
                literal((uint64_t)(2*db+1)<<df)+"; } }");
            if(from!=16)put("else if(!_e&&_f&&(c->fpcr&(1ULL<<24))) c->fpsr|=128;");
            put("else if(_e||_f) { int _unbiased=_e?(int)_e-"+std::to_string(sb)+":"+
                std::to_string(1-(int)sb)+"; uint64_t _mant=_f|(_e?"+literal(1ULL<<sf)+":0);"
                " while(!(_mant&"+literal(1ULL<<sf)+")) { _mant<<=1; --_unbiased; }");
            if(to!=16)put("if(_unbiased<"+std::to_string(1-(int)db)+"&&(c->fpcr&(1ULL<<24))) c->fpsr|=8; else");
            put("{ int _tiny=_unbiased<"+std::to_string(1-(int)db)+"; int _shift="+
                std::to_string((int)sf-(int)df)+"+(_tiny?"+std::to_string(1-(int)db)+"-_unbiased:0);"
                " uint64_t _whole=_shift<=0?_mant<<(-_shift):_shift<64?_mant>>_shift:0;"
                " uint64_t _lost=_shift<=0?0:_shift<64?_mant&((1ULL<<_shift)-1):_mant;"
                " int _up=_lost&&(_mode==0?(_shift<64&&(_lost>(1ULL<<(_shift-1))||"
                " (_lost==(1ULL<<(_shift-1))&&(_whole&1)))):_mode==1?!_sign:_mode==2?!!_sign:0);"
                " _whole+=_up; if(_whole>="+literal(1ULL<<(df+1))+") { _whole>>=1; ++_unbiased; }"
                " if(_unbiased>"+std::to_string(db)+"+_ahp) { if(_ahp) { _r|="+
                literal((1ULL<<(to-1))-1)+"; c->fpsr|=1; } else {"
                " int _inf=_mode==0||(_mode==1&&!_sign)||(_mode==2&&_sign); _r|=_inf?"+
                literal((uint64_t)(2*db+1)<<df)+":"+literal(((uint64_t)(2*db+1)<<df)-1)+"; c->fpsr|=20; } }"
                " else { _r|=_tiny?_whole:((uint64_t)(_unbiased+"+std::to_string(db)+")<<"+
                std::to_string(df)+")|(_whole&"+literal(dm)+"); if(_lost) { c->fpsr|=16; if(_tiny) c->fpsr|=8; } } } }"
                " _dst[_j+"+std::to_string(vn&&upper?4:0)+"]=("+dst+")_r; } memcpy(c->vreg["+
                std::to_string(rd)+"],_dst,16); }");
            return true;
        }
    }

    // S->D vector widening and D->S narrowing. Half conversions are above;
    // rounding-to-odd FCVTXN remains a separate, unmatched encoding.
    if ((i & 0xBFFFFC00) == 0x0E617800 || (i & 0xBFFFFC00) == 0x0E616800) {
        const unsigned rd = i & 31, rn = (i >> 5) & 31;
        const bool upper = (i & (1U << 30)) != 0, widen = (i & 0x1000) != 0;
        if (widen) {
            put("{ uint32_t _src[4]; uint64_t _dst[2]; memcpy(_src,c->vreg[" +
                std::to_string(rn) + "],16); for(unsigned _j=0;_j<2;++_j) {"
                " uint32_t _v=_src[_j+" + std::to_string(upper ? 2 : 0) +
                "],_f=_v&0x7fffff; unsigned _e=(_v>>23)&255;"
                " uint64_t _r=(uint64_t)(_v&0x80000000U)<<32;"
                " if(_e==255) { _r|=0x7ff0000000000000ULL; if(_f) {"
                " if(!(_f&0x400000)) c->fpsr|=1; _r|=((uint64_t)_f<<29)|0x8000000000000ULL;"
                " if(c->fpcr&(1ULL<<25)) _r=0x7ff8000000000000ULL; } }"
                " else if(_e) _r|=((uint64_t)(_e+896)<<52)|((uint64_t)_f<<29);"
                " else if(_f) { if(c->fpcr&(1ULL<<24)) c->fpsr|=128;"
                " else { int _unbiased=-126; while(!(_f&0x800000)) { _f<<=1; --_unbiased; }"
                " _r|=((uint64_t)(_unbiased+1023)<<52)|((uint64_t)(_f&0x7fffff)<<29); } }"
                " _dst[_j]=_r; } memcpy(c->vreg[" + std::to_string(rd) + "],_dst,16); }");
        } else {
            put("{ uint64_t _src[2]; uint32_t _dst[2]; memcpy(_src,c->vreg[" +
                std::to_string(rn) + "],16); for(unsigned _j=0;_j<2;++_j) {"
                " uint64_t _v=_src[_j],_f=_v&0xfffffffffffffULL; unsigned _e=(unsigned)((_v>>52)&2047);"
                " uint32_t _sign=(uint32_t)(_v>>32)&0x80000000U,_r=_sign;"
                " unsigned _mode=(unsigned)((c->fpcr>>22)&3);"
                " if(_e==2047) { _r|=0x7f800000U; if(_f) {"
                " if(!(_f&0x8000000000000ULL)) c->fpsr|=1; _r|=(uint32_t)(_f>>29)|0x400000U;"
                " if(c->fpcr&(1ULL<<25)) _r=0x7fc00000U; } }"
                " else if(!_e&&_f&&(c->fpcr&(1ULL<<24))) c->fpsr|=128;"
                " else if(_e||_f) { int _unbiased=_e?(int)_e-1023:-1022;"
                " uint64_t _mant=_f|(_e?0x10000000000000ULL:0);"
                " if(_unbiased < -126 && (c->fpcr&(1ULL<<24))) { c->fpsr|=8; }"
                " else { unsigned _shift=_unbiased < -126?(unsigned)(-97-_unbiased):29;"
                " uint64_t _whole=_shift<64?_mant>>_shift:0;"
                " uint64_t _lost=_shift<64?_mant&((1ULL<<_shift)-1):_mant;"
                " int _up=_lost&&(_mode==0?(_shift<64&&(_lost>(1ULL<<(_shift-1))||"
                " (_lost==(1ULL<<(_shift-1))&&(_whole&1)))):_mode==1?!_sign:_mode==2?!!_sign:0);"
                " _whole+=_up; if(_whole>=0x1000000ULL) { _whole>>=1; ++_unbiased; }"
                " if(_unbiased>127) { int _inf=_mode==0||(_mode==1&&!_sign)||(_mode==2&&_sign);"
                " _r|=_inf?0x7f800000U:0x7f7fffffU; c->fpsr|=20; }"
                " else { _r|=_unbiased < -126?(uint32_t)_whole:"
                " ((uint32_t)(_unbiased+127)<<23)|((uint32_t)_whole&0x7fffffU);"
                " if(_lost) { c->fpsr|=16; if(_unbiased < -126) c->fpsr|=8; } } } }"
                " _dst[_j]=_r; } memcpy((char*)c->vreg[" + std::to_string(rd) + "]+" +
                std::to_string(upper ? 8 : 0) + ",_dst,8);");
            if (!upper) put("c->vreg[" + std::to_string(rd) + "][1]=0;");
            put("}");
        }
        return true;
    }

    // Vector integral rounding, using integer IEEE-754 fields so guest rounding
    // and subnormal modes do not depend on the host floating-point environment.
    {
        const u32 key = i & 0xBFBFFC00;
        const bool rounding = key == 0x0E218800 || key == 0x0EA18800 ||
            key == 0x0E219800 || key == 0x0EA19800 || key == 0x2E218800 ||
            key == 0x2E219800 || key == 0x2EA19800;
        const bool dbl = (i & (1U << 22)) != 0, q = (i & (1U << 30)) != 0;
        if (rounding && (!dbl || q)) {
            const unsigned rd = i & 31, rn = (i >> 5) & 31;
            const unsigned lanes = dbl ? 2 : 4, active = q ? lanes : lanes / 2;
            const std::string ct = dbl ? "uint64_t" : "uint32_t";
            const std::string mode = key == 0x0E218800 ? "0" : key == 0x0EA18800 ? "1" :
                key == 0x0E219800 ? "2" : key == 0x0EA19800 ? "3" :
                key == 0x2E218800 ? "4" : "((c->fpcr>>22)&3)";
            put("{ " + ct + " _src[" + std::to_string(lanes) + "],_dst[" +
                std::to_string(lanes) + "]={0}; memcpy(_src,c->vreg[" +
                std::to_string(rn) + "],16); const unsigned _mode=" + mode + ";");
            put("for(unsigned _j=0;_j<" + std::to_string(active) + ";++_j) {"
                " uint64_t _v=_src[_j],_a,_r; const uint64_t _sign=" +
                std::string(dbl ? "0x8000000000000000ULL" : "0x80000000ULL") +
                ",_exp=" + (dbl ? "0x7ff0000000000000ULL" : "0x7f800000ULL") +
                ",_frac=" + (dbl ? "0xfffffffffffffULL" : "0x7fffffULL") +
                ",_quiet=" + (dbl ? "0x8000000000000ULL" : "0x400000ULL") + ";");
            put("if((c->fpcr&(1ULL<<24))&&!(_v&_exp)&&(_v&_frac))"
                " { _v&=_sign; c->fpsr|=128; } _a=_v&~_sign; _r=_v;"
                " if((_a&_exp)==_exp) { if(_a&_frac) {"
                " if(!(_a&_quiet)) c->fpsr|=1; _r=_v|_quiet;"
                " if(c->fpcr&(1ULL<<25)) _r=_exp|_quiet; } }"
                " else { int _e=(int)(_a>>" + std::to_string(dbl ? 52 : 23) + ")-" +
                std::to_string(dbl ? 1023 : 127) + "; if(_a&&_e<" +
                std::to_string(dbl ? 52 : 23) + ") { int _up=0; if(_e<0) {"
                " const uint64_t _half=" + (dbl ? "0x3fe0000000000000ULL" : "0x3f000000ULL") +
                "; _up=(_mode==0?_a>_half:_mode==4?_a>=_half:"
                " _mode==1?!(_v&_sign):_mode==2?!!(_v&_sign):0);"
                " _r=(_v&_sign)|(_up?" + (dbl ? "0x3ff0000000000000ULL" : "0x3f800000ULL") + ":0);"
                " } else { unsigned _shift=" + std::to_string(dbl ? 52 : 23) +
                "-_e; uint64_t _unit=1ULL<<_shift,_mask=_unit-1,_lost=_a&_mask;"
                " _up=_lost&&(_mode==0?(_lost>(_unit>>1)||(_lost==(_unit>>1)&&(_a&_unit))):"
                " _mode==4?_lost>=(_unit>>1):_mode==1?!(_v&_sign):_mode==2?!!(_v&_sign):0);"
                " _r=(_v&_sign)|((_a&~_mask)+(_up?_unit:0)); } }");
            if (key == 0x2E219800) put("if(_r!=_v) c->fpsr|=16;");
            put("} _dst[_j]=(" + ct + ")_r; } memcpy(c->vreg[" +
                std::to_string(rd) + "],_dst,16); }");
            return true;
        }
    }

    // Scalar fused multiply-add: negate inputs before the single rounding.
    if ((i & 0xFF000000) == 0x1F000000 && ((i >> 22) & 3) <= 1) {
        const bool dbl = ((i >> 22) & 1) != 0;
        const bool negate_addend = ((i >> 21) & 1) != 0;
        const bool negate_product = negate_addend != (((i >> 15) & 1) != 0);
        const u32 rm = (i >> 16) & 31, ra = (i >> 10) & 31;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const std::string sz = dbl ? "8" : "4";
        put("{ uint64_t _a=0,_b=0,_z=0,_v; memcpy(&_a,c->vreg["+std::to_string(rn)+"],"+sz+
            ");memcpy(&_b,c->vreg["+std::to_string(rm)+"],"+sz+
            ");memcpy(&_z,c->vreg["+std::to_string(ra)+"],"+sz+");");
        if(negate_product)put("_a^="+std::string(dbl?"0x8000000000000000ULL":"0x80000000ULL")+";");
        if(negate_addend)put("_z^="+std::string(dbl?"0x8000000000000000ULL":"0x80000000ULL")+";");
        put(EmitFPMulAddValue(dbl));
        put("c->vreg["+std::to_string(rd)+"][0]=_v;c->vreg["+std::to_string(rd)+"][1]=0;}");
        return true;
    }

    if((i&0xFFA0FC00)==0x1E200800||(i&0xFFA0FC00)==0x1E201800||
       (i&0xFFA0FC00)==0x1E208800) {
        const bool divide=(i&0x1000)!=0;
        const bool negate=(i&0x8000)!=0;
        const bool dbl=(i&(1U<<22))!=0;const unsigned rd=i&31,rn=(i>>5)&31,rm=(i>>16)&31;
        const std::string sz=dbl?"8":"4";
        put("{uint64_t _a=0,_b=0,_z,_v;memcpy(&_a,c->vreg["+std::to_string(rn)+"],"+sz+
            ");memcpy(&_b,c->vreg["+std::to_string(rm)+"],"+sz+");_z=(_a^_b)&"+
            std::string(dbl?"0x8000000000000000ULL":"0x80000000ULL")+";");
        if(divide)put("(void)_z;");
        put(divide?EmitFPDivideValue(dbl):EmitFPMulAddValue(dbl));
        // FNMUL negates the rounded product, including NaN/zero sign. Moving
        // this sign change to an input would reverse directed rounding.
        if(negate)put("_v^="+std::string(dbl?"0x8000000000000000ULL":"0x80000000ULL")+";");
        put("c->vreg["+std::to_string(rd)+"][0]=_v;c->vreg["+std::to_string(rd)+"][1]=0;}");
        return true;
    }

    // Scalar floating point. Only single (ftype 00) and double (ftype 01) are
    // handled; half-precision and the SIMD vector forms still fall through.
    // Values move through memcpy rather than type punning so this stays
    // strictly conforming C.
    if ((i & 0x5F000000) == 0x1E000000 && ((i >> 21) & 1)) {
        const u32 ftype = (i >> 22) & 3;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        if (ftype == 0 || ftype == 1) {
            const bool dbl = (ftype == 1);
            const char* ct = dbl ? "double" : "float";
            const int sz = dbl ? 8 : 4;
            const std::string ld_n = std::string("{ ") + ct + " _a,_b,_r; memcpy(&_a,&c->vreg[" +
                                     std::to_string(rn) + "][0]," + std::to_string(sz) + "); ";
            const std::string ld_m = std::string("memcpy(&_b,&c->vreg[") + std::to_string(rm) +
                                     "][0]," + std::to_string(sz) + "); ";
            const std::string st_d = std::string("c->vreg[") + std::to_string(rd) +
                                     "][0]=0; c->vreg[" + std::to_string(rd) +
                                     "][1]=0; memcpy(&c->vreg[" + std::to_string(rd) + "][0],&_r," +
                                     std::to_string(sz) + "); }";

            // FMOV (immediate). This must be decoded before FCMP below: it
            // also has bits 11..10 clear, so an FMOV whose imm8 and Rd happen
            // to line up would otherwise be read as a compare and silently
            // clobber the flags instead of loading a constant.
            if (((i >> 10) & 7) == 4 && ((i >> 5) & 0x1F) == 0) {
                const u32 imm8 = (i >> 13) & 0xFF;
                // VFPExpandImm: sign, then exponent as NOT(b6) followed by b6
                // repeated, then imm8<5:4>, then imm8<3:0> as the top of the
                // mantissa. Built at double width and narrowed if needed.
                const u32 sgn = (imm8 >> 7) & 1;
                const u32 b6 = (imm8 >> 6) & 1;
                const u64 e11 = ((u64)(b6 ^ 1) << 10) | (b6 ? (0xFFULL << 2) : 0ULL) |
                                ((imm8 >> 4) & 3);
                const u64 dbits = ((u64)sgn << 63) | (e11 << 52) | ((u64)(imm8 & 0xF) << 48);
                char hb[64];
                if (dbl) {
                    snprintf(hb, sizeof hb, "0x%llxULL", (unsigned long long)dbits);
                } else {
                    double dv; memcpy(&dv, &dbits, 8);
                    const float fv = (float)dv;
                    u32 fbits; memcpy(&fbits, &fv, 4);
                    snprintf(hb, sizeof hb, "0x%xULL", fbits);
                }
                put("c->vreg[" + std::to_string(rd) + "][0]=" + hb + "; c->vreg[" +
                    std::to_string(rd) + "][1]=0;");
                return true;
            }

            // Remaining two-source scalar operations (opcode in bits 15..12).
            if (((i >> 10) & 3) == 2) {
                const u32 opcode = (i >> 12) & 15;
                // FMAX/FMIN propagate a NaN operand; the NM forms return the
                // other operand instead, which is exactly what fmax/fmin do.
                std::string expr2;
                switch (opcode) {
                case 4: expr2 = "(_a!=_a||_b!=_b) ? (_a+_b) : (_a>_b?_a:_b)"; break;  // FMAX
                case 5: expr2 = "(_a!=_a||_b!=_b) ? (_a+_b) : (_a<_b?_a:_b)"; break;  // FMIN
                case 6: expr2 = dbl ? "fmax(_a,_b)" : "fmaxf(_a,_b)"; break;          // FMAXNM
                case 7: expr2 = dbl ? "fmin(_a,_b)" : "fminf(_a,_b)"; break;          // FMINNM
                default: break;
                }
                if (!expr2.empty()) {
                    put(ld_n + ld_m + "_r = " + expr2 + "; " + st_d);
                    return true;
                }
            }

            // FCVT between precisions. It shares the one-source encoding but
            // its source and destination widths differ, so it cannot use the
            // shared load/store fragments below, which assume both are ftype.
            // Opcode 0001xx, where the low two bits name the destination:
            // 00 single, 01 double, 11 half. Half stays on the fallback.
            if (((i >> 10) & 0x1F) == 0x10 && (((i >> 15) & 0x3C) == 0x04)) {
                const u32 dst = (i >> 15) & 3;
                if (ftype == 0 && dst == 1) {           // single -> double
                    put("{ float _s; double _d; memcpy(&_s,&c->vreg[" + std::to_string(rn) +
                        "][0],4); _d=(double)_s; c->vreg[" + std::to_string(rd) +
                        "][1]=0; memcpy(&c->vreg[" + std::to_string(rd) + "][0],&_d,8); }");
                    return true;
                }
                if (ftype == 1 && dst == 0) {           // double -> single
                    put("{ double _d; float _s; memcpy(&_d,&c->vreg[" + std::to_string(rn) +
                        "][0],8); _s=(float)_d; c->vreg[" + std::to_string(rd) +
                        "][0]=0; c->vreg[" + std::to_string(rd) +
                        "][1]=0; memcpy(&c->vreg[" + std::to_string(rd) + "][0],&_s,4); }");
                    return true;
                }
            }

            // One-source: FMOV/FABS/FNEG/FSQRT (opcode in bits 20..15 low bits).
            if (((i >> 10) & 0x1F) == 0x10) {
                const u32 opcode = (i >> 15) & 0x3F;
                std::string expr;
                switch (opcode) {
                case 0: expr = "_a"; break;                             // FMOV
                case 1: expr = dbl ? "fabs(_a)" : "fabsf(_a)"; break;   // FABS
                case 2: expr = "-_a"; break;                            // FNEG
                case 3: expr = dbl ? "sqrt(_a)" : "sqrtf(_a)"; break;   // FSQRT
                // FRINTN, FRINTX and FRINTI all round to nearest-even under the
                // default FPCR, which is the only mode the exported code runs in.
                case 8:
                case 14:
                case 15: expr = dbl ? "nearbyint(_a)" : "nearbyintf(_a)"; break;
                case 9: expr = dbl ? "ceil(_a)" : "ceilf(_a)"; break;    // FRINTP
                case 10: expr = dbl ? "floor(_a)" : "floorf(_a)"; break; // FRINTM
                case 11: expr = dbl ? "trunc(_a)" : "truncf(_a)"; break; // FRINTZ
                case 12: expr = dbl ? "round(_a)" : "roundf(_a)"; break; // FRINTA
                default: break;
                }
                if (!expr.empty()) {
                    put(ld_n + "(void)_b; _r = " + expr + "; " + st_d);
                    return true;
                }
            }

            // FCCMP / FCCMPE: compare when the condition holds, otherwise take
            // the flags straight from nzcv. Bits 11..10 are 01 here, where an
            // ordinary FCMP has 1000 in bits 13..10, so the two do not overlap.
            if (((i >> 10) & 3) == 1) {
                const u32 cond = (i >> 12) & 15, nzcv = i & 15;
                std::string s = "{ if " + Cond(cond) + " ";
                s += ld_n + ld_m;
                s += "if (_a != _a || _b != _b) { c->n=0; c->z=0; c->c=1; c->v=1; } ";
                s += "else { c->n = (_a < _b); c->z = (_a == _b); "
                     "c->c = (_a >= _b); c->v = 0; } ";
                s += "(void)_r; } ";
                s += "else { c->n=" + std::to_string((nzcv >> 3) & 1) + "; ";
                s += "c->z=" + std::to_string((nzcv >> 2) & 1) + "; ";
                s += "c->c=" + std::to_string((nzcv >> 1) & 1) + "; ";
                s += "c->v=" + std::to_string(nzcv & 1) + "; } }";
                put(s);
                return true;
            }

            // FCMP / FCMPE, including the compare-against-zero forms. The
            // low five bits are opcode2: bit 3 selects the #0.0 variant (Rm is
            // then not a register at all) and bit 4 selects the signalling
            // form, which differs only in how it reports NaNs and so produces
            // the same flags here. Requiring all five to be clear, as before,
            // matched only a quarter of the compares in real code.
            if (((i >> 10) & 0xF) == 8 && ((i >> 14) & 3) == 0 && (i & 7) == 0) {
                const bool cmp_zero = ((i >> 3) & 1) != 0;
                std::string s = ld_n;
                if (cmp_zero) {
                    s += std::string("_b = (") + ct + ")0; ";
                } else {
                    s += ld_m;
                }
                // Unordered (either NaN) sets C and V per the architecture.
                s += "if (_a != _a || _b != _b) { c->n=0; c->z=0; c->c=1; c->v=1; } ";
                s += "else { c->n = (_a < _b); c->z = (_a == _b); "
                     "c->c = (_a >= _b); c->v = 0; } ";
                s += "(void)_r; }";
                put(s);
                return true;
            }
        }
    }

    // Data-processing 2-source: UDIV, SDIV, and variable-shift forms LSLV/LSRV/ASRV/RORV.
    // Encoding: sf 0 0 1 1 0 1 0 1 Rm opcode Rn Rd
    if ((i & 0x5FE00000) == 0x1AC00000) {
        const u32 sf = i >> 31;
        const u32 rm = (i >> 16) & 31, opcode = (i >> 10) & 63;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        if (rd != 31) {
            // Register 31 reads as XZR in this group, not SP.
            const std::string xn = Xz(rn);
            const std::string xm = Xz(rm);
            std::string s;
            switch (opcode) {
            case 2:  // UDIV - divide by 0 yields 0 per ARM spec
                if (sf) s = "{ uint64_t _r=" + xm + "?" + xn + "/" + xm + ":0; c->x[" + std::to_string(rd) + "]=_r; }";
                else    s = "{ uint32_t _a=(uint32_t)" + xn + ",_b=(uint32_t)" + xm + "; c->x[" + std::to_string(rd) + "]=(uint64_t)(_b?_a/_b:0); }";
                break;
            case 3:  // SDIV
                if (sf) s = "{ int64_t _a=(int64_t)" + xn + ",_b=(int64_t)" + xm + "; c->x[" + std::to_string(rd) + "]=(uint64_t)(_b?_a/_b:0); }";
                else    s = "{ int32_t _a=(int32_t)(uint32_t)" + xn + ",_b=(int32_t)(uint32_t)" + xm + "; c->x[" + std::to_string(rd) + "]=(uint64_t)(uint32_t)(_b?_a/_b:0); }";
                break;
            case 8:  // LSLV
                if (sf) s = "{ c->x[" + std::to_string(rd) + "]=" + xn + "<<(" + xm + "&63); }";
                else    s = "{ c->x[" + std::to_string(rd) + "]=(uint64_t)(uint32_t)((uint32_t)" + xn + "<<(" + xm + "&31)); }";
                break;
            case 9:  // LSRV
                if (sf) s = "{ c->x[" + std::to_string(rd) + "]=" + xn + ">>(" + xm + "&63); }";
                else    s = "{ c->x[" + std::to_string(rd) + "]=(uint64_t)((uint32_t)" + xn + ">>(" + xm + "&31)); }";
                break;
            case 10: // ASRV
                if (sf) s = "{ c->x[" + std::to_string(rd) + "]=(uint64_t)((int64_t)" + xn + ">>(" + xm + "&63)); }";
                else    s = "{ c->x[" + std::to_string(rd) + "]=(uint64_t)(uint32_t)((int32_t)(uint32_t)" + xn + ">>(" + xm + "&31)); }";
                break;
            case 11: // RORV
                if (sf) s = "{ uint64_t _a=" + xn + ",_s=" + xm + "&63; c->x[" + std::to_string(rd) + "]=_s?(_a>>_s)|(_a<<(64-_s)):_a; }";
                else    s = "{ uint32_t _a=(uint32_t)" + xn + ",_s=(uint32_t)" + xm + "&31; c->x[" + std::to_string(rd) + "]=(uint64_t)(uint32_t)(_s?(_a>>_s)|(_a<<(32-_s)):_a); }";
                break;
            }
            if (!s.empty()) { put(s); return true; }
        }
    }

    // Advanced SIMD two-register misc, floating-point compare against zero:
    // FCMGT, FCMGE, FCMEQ, FCMLE and FCMLT. A true lane is all-ones, which is
    // what the following select/AND normally consumes. Only the bit-23-clear
    // group is decoded; the rest stays on the fallback.
    {
        const bool vec_misc = (i & 0x9F3E0C00) == 0x0E200800;
        // Bit 29 is U and must stay out of the mask, or only the U=0 half of
        // each pair (FCMEQ but not FCMLE, FCMGT but not FCMGE) would match.
        const bool scl_misc = (i & 0xDF3E0C00) == 0x5E200800;
        if (vec_misc || scl_misc) {
            const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
            const u32 opcode = (i >> 12) & 0x1F;
            const bool dbl = ((i >> 22) & 1) != 0;
            const u32 rn = (i >> 5) & 31, rd = i & 31;
            const char* cmp = nullptr;
            // NOT and RBIT: both opcode 5 with U set, told apart by size.
            if (opcode == 0x05 && U && vec_misc) {
                const u32 size = (i >> 22) & 3;
                const int bytes = Q ? 16 : 8;
                if (size <= 1) {
                    std::string s = "{ uint8_t _a[" + std::to_string(bytes) + "]; ";
                    s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) +
                         "); ";
                    if (size == 0) {
                        s += "for(int _i=0;_i<" + std::to_string(bytes) +
                             ";_i++) _a[_i]=(uint8_t)~_a[_i]; ";
                    } else {
                        s += "for(int _i=0;_i<" + std::to_string(bytes) + ";_i++){ uint8_t _v=_a[_i]; ";
                        s += "_v=(uint8_t)((_v>>4)|(_v<<4)); ";
                        s += "_v=(uint8_t)(((_v&0xCC)>>2)|((_v&0x33)<<2)); ";
                        s += "_v=(uint8_t)(((_v&0xAA)>>1)|((_v&0x55)<<1)); _a[_i]=_v; } ";
                    }
                    s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                         "][1]=0; ";
                    s += "memcpy(c->vreg[" + std::to_string(rd) + "],_a," + std::to_string(bytes) +
                         "); }";
                    put(s);
                    return true;
                }
            }
            // The integer compares against zero share this class; their
            // opcodes sit just below the floating-point ones.
            if (opcode >= 0x08 && opcode <= 0x0A && !(opcode == 0x0A && U)) {
                const u32 size = (i >> 22) & 3;
                const int esz = 1 << size;
                const int bytes = scl_misc ? esz : (Q ? 16 : 8);
                const int lanes = bytes / esz;
                const std::string uty = "uint" + std::to_string(esz * 8) + "_t";
                const std::string ity = "int" + std::to_string(esz * 8) + "_t";
                const char* op = (opcode == 0x08) ? (U ? ">=" : ">")
                               : (opcode == 0x09) ? (U ? "<=" : "==")
                                                  : "<";
                if (!(size == 3 && !Q && !scl_misc)) {
                    std::string s = "{ " + ity + " _a[" + std::to_string(lanes) + "]; " + uty +
                                    " _r[" + std::to_string(lanes) + "]; ";
                    s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) +
                         "); ";
                    s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(_a[_i]" +
                         std::string(op) + "0)?(" + uty + ")~(" + uty + ")0:(" + uty + ")0; ";
                    s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                         "][1]=0; ";
                    s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) +
                         "); }";
                    put(s);
                    return true;
                }
            }
            if (opcode == 0x0C) cmp = U ? ">=" : ">";      // FCMGE / FCMGT
            else if (opcode == 0x0D) cmp = U ? "<=" : "=="; // FCMLE / FCMEQ
            else if (opcode == 0x0E && !U) cmp = "<";       // FCMLT
            // SCVTF / UCVTF: convert the integer in each lane to a float of the
            // same width. This is the vector counterpart of the general-register
            // conversion handled further up - the operand is a lane here, not a
            // general register.
            // Bit 23 selects reciprocal/reciprocal-square-root estimates,
            // not integer conversion. U selects the square-root form.
            if (opcode == 0x1D && (i & (1U << 23)) != 0 && (!dbl || Q)) {
                const int fsz = dbl ? 8 : 4;
                const int bytes = scl_misc ? fsz : (Q ? 16 : 8);
                const int lanes = bytes / fsz;
                const std::string ct = dbl ? "uint64_t" : "uint32_t";
                std::string s = "{ " + ct + " _a[" + std::to_string(lanes) + "],_r[" +
                                std::to_string(lanes) + "]={0}; memcpy(_a,c->vreg[" +
                                std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) +
                     ";++_i){uint64_t _v=0,_x=_a[_i];";
                s += U ? EmitFPRSqrtEstimateValue(dbl) : EmitFPRecipEstimateValue(dbl);
                s += "_r[_i]=(" + ct + ")_v;} c->vreg[" + std::to_string(rd) +
                     "][0]=0;c->vreg[" + std::to_string(rd) + "][1]=0;memcpy(c->vreg[" +
                     std::to_string(rd) + "],_r," + std::to_string(bytes) + ");}";
                put(s);
                return true;
            }
            if (opcode == 0x1D && (i & (1U << 23)) == 0) {
                const char* ct = dbl ? "double" : "float";
                const int fsz = dbl ? 8 : 4;
                const int bytes = scl_misc ? fsz : (Q ? 16 : 8);
                const int lanes = bytes / fsz;
                const std::string ity = std::string(U ? "uint" : "int") +
                                        std::to_string(fsz * 8) + "_t";
                std::string s = "{ " + ity + " _a[" + std::to_string(lanes) + "]; " +
                                std::string(ct) + " _r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(" +
                     std::string(ct) + ")_a[_i]; ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
                put(s);
                return true;
            }
            // FABS/FNEG share opcode 0x0F; U selects negation. Opcode 0x0E
            // is FCMLT and must reach the comparison below. Move sign bits
            // directly so NaN payloads and FPSR remain untouched.
            if ((i & 0x9FBFFC00) == 0x0EA0F800 && (!dbl || Q)) {
                const int fsz = dbl ? 8 : 4;
                const int bytes = Q ? 16 : 8;
                const int lanes = bytes / fsz;
                const std::string ct = dbl ? "uint64_t" : "uint32_t";
                const std::string sign = dbl ? "0x8000000000000000ULL" : "0x80000000U";
                const std::string expr = U ? "_a[_i]^" + sign : "_a[_i]&~" + sign;
                std::string s = "{ " + ct + " _a[" + std::to_string(lanes) +
                                "],_r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) +
                     "); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=" + expr + "; ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) +
                     "); }";
                put(s);
                return true;
            }

            // XTN / XTN2: take the low half of each element. XTN2 writes the
            // top half of the destination and leaves the bottom alone, which is
            // the only reason this is not a plain narrowing.
            if (opcode == 0x12 && !U && !scl_misc) {
                const u32 size = (i >> 22) & 3;
                if (size != 3) {
                    const int dsz = 1 << size;            // destination element
                    const int lanes = 8 / dsz;            // always half a register out
                    const std::string sty = "uint" + std::to_string(dsz * 16) + "_t";
                    const std::string dty = "uint" + std::to_string(dsz * 8) + "_t";
                    std::string s = "{ " + sty + " _a[" + std::to_string(lanes) + "]; " + dty +
                                    " _r[" + std::to_string(lanes) + "]; ";
                    s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "],16); ";
                    s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(" + dty +
                         ")_a[_i]; ";
                    if (!Q) {
                        s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" +
                             std::to_string(rd) + "][1]=0; ";
                    }
                    s += "memcpy((uint8_t*)c->vreg[" + std::to_string(rd) + "]+" +
                         std::to_string(Q ? 8 : 0) + ",_r,8); }";
                    put(s);
                    return true;
                }
            }

            // FCVTZS / FCVTZU, vector. Round toward zero and saturate, the same
            // contract as the scalar forms further up.
            if (opcode == 0x1B) {
                const char* ct = dbl ? "double" : "float";
                const int fsz = dbl ? 8 : 4;
                const int bytes = scl_misc ? fsz : (Q ? 16 : 8);
                const int lanes = bytes / fsz;
                const std::string ity =
                    (U ? std::string("uint") : std::string("int")) + std::to_string(fsz * 8) + "_t";
                const std::string uty = "uint" + std::to_string(fsz * 8) + "_t";
                const char* lo_bound = dbl ? (U ? "0.0" : "-9223372036854775808.0")
                                           : (U ? "0.0" : "-2147483648.0");
                const char* hi_bound = dbl ? (U ? "18446744073709551615.0"
                                                : "9223372036854775807.0")
                                           : (U ? "4294967295.0" : "2147483647.0");
                const char* sat_lo = dbl ? (U ? "0ULL" : "0x8000000000000000ULL")
                                         : (U ? "0ULL" : "0xFFFFFFFF80000000ULL");
                const char* sat_hi = dbl ? (U ? "0xFFFFFFFFFFFFFFFFULL"
                                              : "0x7FFFFFFFFFFFFFFFULL")
                                         : (U ? "0xFFFFFFFFULL" : "0x7FFFFFFFULL");
                std::string s = "{ " + std::string(ct) + " _a[" + std::to_string(lanes) + "]; " +
                                uty + " _r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) +
                     "); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++){ " + std::string(ct) +
                     " _v=_a[_i]; ";
                s += "if(_v!=_v) _r[_i]=(" + uty + ")0; ";
                s += std::string("else if(!(_v > (") + ct + ")" + lo_bound + ")) _r[_i]=(" + uty +
                     ")" + sat_lo + "; ";
                s += std::string("else if(!(_v < (") + ct + ")" + hi_bound + ")) _r[_i]=(" + uty +
                     ")" + sat_hi + "; ";
                s += "else _r[_i]=(" + uty + ")(" + ity + ")_v; } ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) +
                     "); }";
                put(s);
                return true;
            }

            // REV16 / REV32 / REV64: reverse bytes within each container.
            // opcode 0 is REV64, 1 is REV16, and REV32 is opcode 0 with U set;
            // size gives the element width being reversed inside.
            if ((opcode == 0 || opcode == 1) && !scl_misc) {
                const u32 size = (i >> 22) & 3;
                const int esz = 1 << size;                 // byte width of an element
                const int container = (opcode == 1) ? 2 : (U ? 4 : 8);
                if (esz < container) {
                    const int bytes = Q ? 16 : 8;
                    std::string s = "{ uint8_t _a[" + std::to_string(bytes) + "],_r[" +
                                    std::to_string(bytes) + "]; ";
                    s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," +
                         std::to_string(bytes) + "); ";
                    s += "for(int _i=0;_i<" + std::to_string(bytes) + ";_i++){ ";
                    s += "int _base=_i-(_i%" + std::to_string(container) + "); ";
                    s += "int _off=_i-_base; ";
                    s += "_r[_i]=_a[_base+(" + std::to_string(container) + "-" +
                         std::to_string(esz) + "-(_off-(_off%" + std::to_string(esz) +
                         ")))+(_off%" + std::to_string(esz) + ")]; } ";
                    s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" +
                         std::to_string(rd) + "][1]=0; ";
                    s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," +
                         std::to_string(bytes) + "); }";
                    put(s);
                    return true;
                }
            }

            // CNT: set bits per byte. Defined for byte elements only.
            if (opcode == 0x05 && ((i >> 22) & 3) == 0 && !U) {
                const int bytes = Q ? 16 : 8;
                std::string s = "{ uint8_t _a[" + std::to_string(bytes) + "],_r[" +
                                std::to_string(bytes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," +
                     std::to_string(bytes) + "); ";
                s += "for(int _i=0;_i<" + std::to_string(bytes) + ";_i++){ ";
                s += "uint8_t _v=_a[_i]; _v=(uint8_t)(_v-((_v>>1)&0x55)); ";
                s += "_v=(uint8_t)((_v&0x33)+((_v>>2)&0x33)); ";
                s += "_r[_i]=(uint8_t)((_v+(_v>>4))&0x0F); } ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" +
                     std::to_string(rd) + "][1]=0; ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," +
                     std::to_string(bytes) + "); }";
                put(s);
                return true;
            }

            // ABS / NEG, lanewise over integer elements. U picks NEG.
            if (opcode == 0x0B) {
                const u32 size = (i >> 22) & 3;
                const int esz = 1 << size;
                const int bytes = scl_misc ? 8 : (Q ? 16 : 8);
                // The 64-bit element only exists as 2D or as the scalar form.
                const bool shaped = scl_misc ? (size == 3) : !(size == 3 && !Q);
                if (shaped) {
                    const int lanes = bytes / esz;
                    const std::string uty = "uint" + std::to_string(esz * 8) + "_t";
                    const std::string ity = "int" + std::to_string(esz * 8) + "_t";
                    std::string s = "{ " + uty + " _a[" + std::to_string(lanes) + "],_r[" +
                                    std::to_string(lanes) + "]; ";
                    s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," +
                         std::to_string(bytes) + "); ";
                    // Unsigned throughout: negating the minimum signed value wraps
                    // on the architecture and is undefined on a signed C type.
                    if (U) {
                        s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(" +
                             uty + ")(0-_a[_i]); ";
                    } else {
                        s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=((" +
                             ity + ")_a[_i]<0) ? (" + uty + ")(0-_a[_i]) : _a[_i]; ";
                    }
                    s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" +
                         std::to_string(rd) + "][1]=0; ";
                    s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," +
                         std::to_string(bytes) + "); }";
                    put(s);
                    return true;
                }
            }
            if (cmp) {
                const char* ct = dbl ? "double" : "float";
                const int fsz = dbl ? 8 : 4;
                // A scalar form touches one lane; a vector form covers the
                // whole selected width.
                const int bytes = scl_misc ? fsz : (Q ? 16 : 8);
                const int lanes = bytes / fsz;
                const std::string uty = "uint" + std::to_string(fsz * 8) + "_t";
                std::string s = "{ " + std::string(ct) + " _a[" + std::to_string(lanes) + "]; " +
                                uty + " _r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(_a[_i]" + cmp +
                     "(" + ct + ")0) ? (" + uty + ")~(" + uty + ")0 : (" + uty + ")0; ";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
                put(s);
                return true;
            }
        }
    }

    // FMUL by indexed element. Rm is only four bits here, extended by M, and
    // the lane index is split across H and L - reading Rm as the usual five
    // bits would silently address the wrong register.
    {
        // Bit 29 (U) selects FMULX, a different operation - keep it out of FMUL.
        // Opcode 1001 is FMUL, 0001 FMLA, 0101 FMLS; the three share everything
        // except whether the product replaces the destination or accumulates
        // into it.
        const u32 idxop = (i >> 12) & 0xF;
        const bool idx_shape = (idxop == 0x9 || idxop == 0x1 || idxop == 0x5);
        const bool vec_idx = idx_shape && (i & 0xBF000400) == 0x0F000000;
        const bool scl_idx = idx_shape && (i & 0xFF000400) == 0x5F000000;
        if ((vec_idx || scl_idx) && ((i >> 23) & 1) == 1) {
            const u32 Q = (i >> 30) & 1;
            const bool dbl = ((i >> 22) & 1) != 0;
            const u32 rn = (i >> 5) & 31, rd = i & 31;
            const u32 rm = ((i >> 16) & 15) | (((i >> 20) & 1) << 4);
            const u32 H = (i >> 11) & 1, L = (i >> 21) & 1;
            const u32 index = dbl ? H : ((H << 1) | L);
            const char* ct = dbl ? "uint64_t" : "uint32_t";
            const int fsz = dbl ? 8 : 4;
            const int bytes = scl_idx ? fsz : (Q ? 16 : 8);
            const int lanes = bytes / fsz;
            if (!(dbl && L)) {   // L must be zero for the 64-bit form
                std::string s = "{ " + std::string(ct) + " _a[" + std::to_string(lanes) +
                                "],_r[" + std::to_string(lanes) + "],_m; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
                s += "memcpy(&_m,(const uint8_t*)c->vreg[" + std::to_string(rm) + "]+" +
                     std::to_string(index * fsz) + "," + std::to_string(fsz) + "); ";
                s += "memcpy(_r,c->vreg["+std::to_string(rd)+"],"+std::to_string(bytes)+");";
                // Local scalar names belong to an inner block; source arrays
                // are captured before entering it so every alias remains safe.
                s += "for(int _i=0;_i<"+std::to_string(lanes)+";++_i){uint64_t _n=_a[_i],_z0=_r[_i];"
                     "{uint64_t _a=_n,_b=_m,_z=_z0,_v;";
                const std::string sign=dbl?"0x8000000000000000ULL":"0x80000000ULL";
                if(idxop==9)s+="_z=(_a^_b)&"+sign+";";
                if(idxop==5)s+="_a^="+sign+";";
                s+=EmitFPMulAddValue(dbl);
                s+="_r[_i]=("+std::string(ct)+")_v;}}";
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
                put(s);
                return true;
            }
        }
    }

    // MUL / MLA / MLS by indexed element. Same shape as the floating-point
    // group above, but size selects an integer width, and for 16-bit elements
    // M is part of the lane index rather than the top bit of Rm.
    {
        const u32 idxop = (i >> 12) & 0xF;
        const u32 U = (i >> 29) & 1;
        const bool widening = idxop == 0x2 || idxop == 0x6 || idxop == 0xA;
        const bool shaped = widening || (idxop == 0x8 && !U) || ((idxop == 0x0 || idxop == 0x4) && U);
        if (shaped && (i & 0x9F00F400) == 0x0F000000 + (idxop << 12)) {
            const u32 Q = (i >> 30) & 1, size = (i >> 22) & 3;
            const u32 rn = (i >> 5) & 31, rd = i & 31;
            const u32 H = (i >> 11) & 1, L = (i >> 21) & 1, M = (i >> 20) & 1;
            u32 rm = 0, index = 0;
            bool ok = true;
            if (size == 1) {
                rm = (i >> 16) & 15;
                index = (H << 2) | (L << 1) | M;
            } else if (size == 2) {
                rm = ((i >> 16) & 15) | (M << 4);
                index = (H << 1) | L;
            } else {
                ok = false;
            }
            if (ok && widening) {
                const int esz = 1 << size, lanes = 8 / esz;
                const std::string sty = std::string(U ? "uint" : "int") + std::to_string(esz * 8) + "_t";
                const std::string dty = "uint" + std::to_string(esz * 16) + "_t";
                const std::string wide = U ? "uint64_t" : "int64_t";
                std::string s = "{ " + sty + " _a[" + std::to_string(lanes) + "],_m; " +
                                dty + " _r[" + std::to_string(lanes) + "]; ";
                s += "memcpy(_a,&c->vreg[" + std::to_string(rn) + "][" + std::to_string(Q) + "],8); ";
                s += "memcpy(&_m,(const uint8_t*)c->vreg[" + std::to_string(rm) + "]+" +
                     std::to_string(index * esz) + "," + std::to_string(esz) + "); ";
                if (idxop != 0xA) s += "memcpy(_r,c->vreg[" + std::to_string(rd) + "],16); ";
                s += "for(int _i=0;_i<" + std::to_string(lanes) + ";++_i) _r[_i]=(" + dty + ")(";
                if (idxop != 0xA) s += "(uint64_t)_r[_i]" + std::string(idxop == 2 ? "+" : "-");
                s += "(uint64_t)((" + wide + ")_a[_i]*(" + wide + ")_m)); ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r,16); }";
                put(s);
                return true;
            }
            if (ok) {
                const int esz = 1 << size;
                const int bytes = Q ? 16 : 8;
                const int lanes = bytes / esz;
                const std::string uty = "uint" + std::to_string(esz * 8) + "_t";
                std::string s = "{ " + uty + " _a[" + std::to_string(lanes) + "],_r[" +
                                std::to_string(lanes) + "],_m; ";
                s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) +
                     "); ";
                s += "memcpy(&_m,(const uint8_t*)c->vreg[" + std::to_string(rm) + "]+" +
                     std::to_string(index * esz) + "," + std::to_string(esz) + "); ";
                if (idxop == 0x8) {
                    s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(" + uty +
                         ")((uint64_t)_a[_i]*(uint64_t)_m); ";
                } else {
                    s += "memcpy(_r,c->vreg[" + std::to_string(rd) + "]," + std::to_string(bytes) +
                         "); ";
                    s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _r[_i]=(" + uty +
                         ")(_r[_i]" + std::string(idxop == 0x0 ? "+" : "-") + "(" + uty +
                         ")((uint64_t)_a[_i]*(uint64_t)_m)); ";
                }
                s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                     "][1]=0; ";
                s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) +
                     "); }";
                put(s);
                return true;
            }
        }
    }

    // Integer leading-bit counts, halving arithmetic, absolute differences,
    // and narrowing high-half arithmetic. These operations do not affect QC.
    {
        const bool leading = (i & 0x9F3FFC00) == 0x0E204800;
        const u32 half_op = i & 0x9F20FC00;
        const bool halving = half_op == 0x0E200400 || half_op == 0x0E201400 || half_op == 0x0E202400;
        const bool difference = (i & 0x9F20F400) == 0x0E207400;
        const bool high_narrow = (i & 0x9F20DC00) == 0x0E204000;
        const u32 size = (i >> 22) & 3;
        if ((leading || halving || difference || high_narrow) && size < 3) {
            const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
            const u32 rn = (i >> 5) & 31, rm = (i >> 16) & 31, rd = i & 31;
            const int esz = 1 << size, bits = esz * 8, bytes = Q ? 16 : 8;
            const int lanes = high_narrow ? 8 / esz : bytes / esz;
            const std::string dest = "uint" + std::to_string(bits) + "_t";
            const std::string src = std::string(high_narrow || leading || U ? "uint" : "int") +
                                    std::to_string(high_narrow ? bits * 2 : bits) + "_t";
            std::string s = "{ " + src + " _a[" + std::to_string(lanes) + "]; " + dest +
                            " _r[" + std::to_string(lanes) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(high_narrow ? 16 : bytes) + "); ";
            if (!leading) {
                s += src + " _b[" + std::to_string(lanes) + "]; memcpy(_b,c->vreg[" +
                     std::to_string(rm) + "]," + std::to_string(high_narrow ? 16 : bytes) + "); ";
            }
            const bool accumulate = difference && (i & 0x800);
            if (accumulate) s += "memcpy(_r,c->vreg[" + std::to_string(rd) + "]," + std::to_string(bytes) + "); ";
            s += "for(int _j=0;_j<" + std::to_string(lanes) + ";++_j) { ";
            if (leading) {
                s += "uint64_t _x=_a[_j]; unsigned _n=0; ";
                if (!U) s += "if(_x&(UINT64_C(1)<<" + std::to_string(bits - 1) + ")) _x=(" + dest + ")~_x; ";
                s += "for(int _b=" + std::to_string(bits - (U ? 1 : 2)) + ";_b>=0;--_b) { if((_x>>_b)&1) break; ++_n; } _r[_j]=(" + dest + ")_n; ";
            } else if (high_narrow) {
                s += "uint64_t _x=(uint64_t)_a[_j]" + std::string(i & 0x2000 ? "-" : "+") + "(uint64_t)_b[_j]";
                if (U) s += "+(UINT64_C(1)<<" + std::to_string(bits - 1) + ")";
                s += "; _r[_j]=(" + dest + ")(_x>>" + std::to_string(bits) + "); ";
            } else if (halving) {
                s += "int64_t _x=(int64_t)_a[_j]" + std::string(half_op == 0x0E202400 ? "-" : "+") + "(int64_t)_b[_j]";
                if (half_op == 0x0E201400) s += "+1";
                s += "; _r[_j]=(" + dest + ")(_x<0?-((-_x+1)/2):_x/2); ";
            } else {
                s += "int64_t _x=(int64_t)_a[_j]-(int64_t)_b[_j]; uint64_t _abs=(uint64_t)(_x<0?-_x:_x); ";
                s += "_r[_j]=(" + dest + ")(" + std::string(accumulate ? "(uint64_t)_r[_j]+" : "") + "_abs); ";
            }
            s += "} ";
            if (high_narrow) {
                if (!Q) s += "c->vreg[" + std::to_string(rd) + "][1]=0; ";
                s += "memcpy(&c->vreg[" + std::to_string(rd) + "][" + std::to_string(Q) + "],_r,8); }";
            } else s += "memset(c->vreg[" + std::to_string(rd) + "],0,16); memcpy(c->vreg[" +
                        std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
            put(s);
            return true;
        }
    }

    // Integer widening add/subtract and pairwise long add/accumulate.
    // Unsigned arithmetic at 64 bits implements modular destination writes,
    // including signed wide additions that would otherwise overflow C types.
    {
        const bool widen = (i & 0x9F20CC00) == 0x0E200000;
        const bool paired = (i & 0x9F3FBC00) == 0x0E202800;
        const bool absolute = (i & 0x9F20DC00) == 0x0E205000;
        const u32 size = (i >> 22) & 3;
        if ((widen || paired || absolute) && size < 3) {
            const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1;
            const u32 rn = (i >> 5) & 31, rm = (i >> 16) & 31, rd = i & 31;
            const bool wide = widen && (i & 0x1000), sub = widen && (i & 0x2000);
            const bool accumulate = (paired && (i & 0x4000)) || (absolute && !(i & 0x2000));
            const int esz = 1 << size, bytes = paired ? (Q ? 16 : 8) : 16;
            const int lanes = bytes / (2 * esz);
            const std::string narrow = std::string(U ? "uint" : "int") + std::to_string(esz * 8) + "_t";
            const std::string dest = "uint" + std::to_string(esz * 16) + "_t";
            std::string s = "{ " + dest + " _r[8]={0}; ";
            if (!wide) s += narrow + " _a[16]; ";
            if (widen || absolute) s += narrow + " _b[8]; ";
            if (wide || accumulate) s += dest + " _w[8]; ";
            if (wide) s += "memcpy(_w,c->vreg[" + std::to_string(rn) + "],16); ";
            else s += "memcpy(_a," + std::string(paired ? "c->vreg[" : "&c->vreg[") +
                      std::to_string(rn) + (paired ? "]," : "][" + std::to_string(Q) + "],") +
                      std::to_string(paired ? bytes : 8) + "); ";
            if (widen || absolute) s += "memcpy(_b,&c->vreg[" + std::to_string(rm) + "][" + std::to_string(Q) + "],8); ";
            if (accumulate) s += "memcpy(_w,c->vreg[" + std::to_string(rd) + "]," + std::to_string(bytes) + "); ";
            s += "for(int _j=0;_j<" + std::to_string(lanes) + ";++_j) { ";
            if (absolute) s += "int64_t _x=(int64_t)_a[_j]-(int64_t)_b[_j]; ";
            s += "_r[_j]=(" + dest + ")(";
            if (accumulate) s += "(uint64_t)_w[_j]+";
            if (absolute) s += "(uint64_t)(_x<0?-_x:_x)";
            else s += "(uint64_t)" + std::string(paired ? "_a[2*_j]" : wide ? "_w[_j]" : "_a[_j]") +
                      (sub ? "-" : "+") + "(uint64_t)" + (paired ? "_a[2*_j+1]" : "_b[_j]");
            s += "); } ";
            s += "memset(c->vreg[" + std::to_string(rd) + "],0,16); memcpy(c->vreg[" +
                 std::to_string(rd) + "],_r," + std::to_string(bytes) + "); }";
            put(s);
            return true;
        }
    }

    // SADDLV / UADDLV: sum every lane into one element of twice the width.
    if ((i & 0x9F3FFC00) == 0x0E303800) {
        const u32 Q = (i >> 30) & 1, U = (i >> 29) & 1, size = (i >> 22) & 3;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const int esz = 1 << size;
        const int bytes = Q ? 16 : 8;
        if (size < 3 && !(size == 2 && !Q)) {
            const int lanes = bytes / esz;
            const std::string sty =
                std::string(U ? "uint" : "int") + std::to_string(esz * 8) + "_t";
            const std::string dty =
                std::string(U ? "uint" : "int") + std::to_string(esz * 16) + "_t";
            std::string s = "{ " + sty + " _a[" + std::to_string(lanes) + "]; " + dty + " _s=0; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) _s=(" + dty + ")(_s+(" + dty +
                 ")_a[_i]); ";
            s += "c->vreg[" + std::to_string(rd) + "][0]=0; c->vreg[" + std::to_string(rd) +
                 "][1]=0; ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],&_s," + std::to_string(esz * 2) +
                 "); }";
            put(s);
            return true;
        }
    }

    // Advanced SIMD copy: DUP, INS, SMOV and UMOV. These move single lanes
    // between vector registers and the general registers, which is how any
    // scalar value gets into or out of vector code - so they turn up
    // constantly. imm5 encodes both the element width and the lane index:
    // the position of its lowest set bit gives the width, and the bits above
    // it give the index.
    {
        const bool vector_copy = (i & 0x9FE08400) == 0x0E000400;
        const bool scalar_copy = (i & 0xFFE08400) == 0x5E000400;
        if (vector_copy || scalar_copy) {
            const u32 Q = (i >> 30) & 1, op = (i >> 29) & 1;
            const u32 imm5 = (i >> 16) & 0x1F, imm4 = (i >> 11) & 0xF;
            const u32 rn = (i >> 5) & 31, rd = i & 31;
            int size = -1;
            u32 index = 0;
            if (imm5 & 1)      { size = 0; index = imm5 >> 1; }
            else if (imm5 & 2) { size = 1; index = imm5 >> 2; }
            else if (imm5 & 4) { size = 2; index = imm5 >> 3; }
            else if (imm5 & 8) { size = 3; index = imm5 >> 4; }
            if (size >= 0) {
                const int esz = 1 << size;
                const std::string sn = std::to_string(rn), sd = std::to_string(rd);
                const std::string off = std::to_string(index * esz);
                // Read the source lane into _e as a byte copy; this avoids any
                // assumption about how the 128-bit register is split in two.
                const std::string read_lane =
                    "uint8_t _s[16]; memcpy(_s,c->vreg[" + sn + "],16); uint64_t _e=0; "
                    "memcpy(&_e,_s+" + off + "," + std::to_string(esz) + "); ";

                if (scalar_copy && !op && imm4 == 0) {
                    // DUP (scalar): the named lane alone, rest of the register zeroed.
                    put("{ " + read_lane + "c->vreg[" + sd + "][0]=_e; c->vreg[" + sd +
                        "][1]=0; }");
                    return true;
                }
                if (vector_copy && !op && (imm4 == 0 || imm4 == 1)) {
                    // DUP (element) or DUP (general): every lane takes the value.
                    const int lanes = (Q ? 16 : 8) / esz;
                    std::string s = "{ ";
                    s += (imm4 == 0) ? read_lane
                                     : ("uint64_t _e=" + Xz(rn) + "; ");
                    s += "uint8_t _d[16]; memset(_d,0,16); ";
                    s += "for(int _i=0;_i<" + std::to_string(lanes) + ";_i++) memcpy(_d+_i*" +
                         std::to_string(esz) + ",&_e," + std::to_string(esz) + "); ";
                    s += "memcpy(c->vreg[" + sd + "],_d,16); }";
                    put(s);
                    return true;
                }
                if (vector_copy && !op && (imm4 == 5 || imm4 == 7) && rd != 31) {
                    // SMOV / UMOV: one lane out into a general register.
                    std::string s = "{ " + read_lane;
                    if (imm4 == 5) {
                        // Sign-extend from the element width.
                        const char* st = size == 0 ? "int8_t"
                                       : size == 1 ? "int16_t"
                                                   : "int32_t";
                        s += "uint64_t _r=(uint64_t)(int64_t)(" + std::string(st) + ")_e; ";
                        if (!Q) s += "_r &= 0xFFFFFFFFULL; ";
                        s += "c->x[" + sd + "]=_r; }";
                    } else {
                        s += "c->x[" + sd + "]=_e; }";
                    }
                    put(s);
                    return true;
                }
                if (vector_copy && !op && imm4 == 3) {
                    // INS (general): overwrite one lane, leave the others alone.
                    put("{ uint8_t _d[16]; memcpy(_d,c->vreg[" + sd + "],16); uint64_t _e=" +
                        Xz(rn) + "; memcpy(_d+" + off + ",&_e," + std::to_string(esz) +
                        "); memcpy(c->vreg[" + sd + "],_d,16); }");
                    return true;
                }
                if (vector_copy && op) {
                    // INS (element): imm4 holds the source lane, above the bits
                    // the element width occupies.
                    const u32 src_index = imm4 >> size;
                    put("{ uint8_t _s[16],_d[16]; memcpy(_s,c->vreg[" + sn +
                        "],16); memcpy(_d,c->vreg[" + sd + "],16); memcpy(_d+" + off + ",_s+" +
                        std::to_string(src_index * esz) + "," + std::to_string(esz) +
                        "); memcpy(c->vreg[" + sd + "],_d,16); }");
                    return true;
                }
            }
        }
    }

    // MLA / MLS (vector): accumulate the low element-width product modulo 2^bits.
    // Widen before multiplication to avoid signed promotion overflow for H lanes.
    if ((i & 0x9F20FC00) == 0x0E209400 && ((i >> 22) & 3) < 3) {
        const u32 size = (i >> 22) & 3, Q = (i >> 30) & 1;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const bool subtract = ((i >> 29) & 1) != 0;
        const int bytes = Q ? 16 : 8, lanes = bytes >> size;
        const std::string ty = "uint" + std::to_string(8 << size) + "_t";
        std::string s = "{ " + ty + " _a[" + std::to_string(lanes) + "],_b[" +
                        std::to_string(lanes) + "],_d[" + std::to_string(lanes) + "],_r[" +
                        std::to_string(lanes) + "]; ";
        s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(bytes) + "); ";
        s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(bytes) + "); ";
        s += "memcpy(_d,c->vreg[" + std::to_string(rd) + "]," + std::to_string(bytes) + "); ";
        s += "for(int _i=0;_i<" + std::to_string(lanes) + ";++_i) _r[_i]=(" + ty +
             ")((uint64_t)_d[_i]" + (subtract ? "-" : "+") +
             "(uint64_t)_a[_i]*(uint64_t)_b[_i]); ";
        s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(bytes) + "); ";
        if (!Q) s += "c->vreg[" + std::to_string(rd) + "][1]=0; ";
        put(s + "}");
        return true;
    }

    // Advanced SIMD three-register same (0x0E/0x4E/0x6E group).
    // Bit pattern: Q U 0 1 1 1 0 size 1 Rm opcode 1 Rn Rd
    // Fixed bits: [28:24]=01110, bit[21]=1, bit[10]=1
    // Handles the most common vector operations needed by games.
    // Element operations use memcpy to stay strictly conforming C.
    if ((i & 0x9F200400) == 0x0E200400) {
        const u32 Q    = (i >> 30) & 1;   // 0=64-bit half-register, 1=128-bit full
        const u32 U    = (i >> 29) & 1;
        const u32 size = (i >> 22) & 3;   // 0=B 1=H 2=S 3=D (integer); sz for FP
        const u32 rm   = (i >> 16) & 31;
        const u32 opc5 = (i >> 11) & 31;  // bits[15:11]
        const u32 rn   = (i >> 5)  & 31;
        const u32 rd   = i & 31;
        const int vbytes = Q ? 16 : 8;
        const int esz   = 1 << size;      // bytes per integer element
        const int nelems = vbytes / esz;

        // Bitwise ops (opcode=3): AND/BIC/ORR/ORN (U=0); EOR/BSL/BIT/BIF (U=1).
        // These operate on the full register; element size encodes which variant.
        if (opc5 == 3) {
            // Build body string; if left empty, fall through to unhandled.
            std::string body;
            const std::string vd0 = "c->vreg[" + std::to_string(rd) + "][0]";
            const std::string vd1 = "c->vreg[" + std::to_string(rd) + "][1]";
            const std::string vn0 = "c->vreg[" + std::to_string(rn) + "][0]";
            const std::string vn1 = "c->vreg[" + std::to_string(rn) + "][1]";
            const std::string vm0 = "c->vreg[" + std::to_string(rm) + "][0]";
            const std::string vm1 = "c->vreg[" + std::to_string(rm) + "][1]";
            if (!U) {
                const char* op0 = nullptr; const char* op1 = nullptr;
                if      (size == 0) { op0="&";  op1="&";  }   // AND
                else if (size == 1) { op0="&~"; op1="&~"; }   // BIC
                else if (size == 2) { op0="|";  op1="|";  }   // ORR
                else                { op0="|~"; op1="|~"; }   // ORN
                body = vd0 + "=" + vn0 + op0 + vm0 + "; " + vd1 + "=" + vn1 + op1 + vm1 + "; ";
            } else {
                if (size == 0) {  // EOR
                    body = vd0 + "=" + vn0 + "^" + vm0 + "; " + vd1 + "=" + vn1 + "^" + vm1 + "; ";
                } else if (size == 1) {  // BSL: Vd = (Vd & Vn) | (~Vd & Vm)
                    body = vd0 + "=(" + vd0 + "&" + vn0 + ")|(~" + vd0 + "&" + vm0 + "); ";
                    body += vd1 + "=(" + vd1 + "&" + vn1 + ")|(~" + vd1 + "&" + vm1 + "); ";
                }
                // BIT (size=2) / BIF (size=3): leave on fallback
            }
            if (!body.empty()) {
                std::string s = "{ " + body;
                if (!Q) s += vd1 + "=0; ";
                s += "}";
                put(s);
                return true;
            }
        }

        // Integer ADD (opc5=16, U=0) and SUB (opc5=16, U=1), element-wise.
        if (opc5 == 16) {
            const char* iop = U ? "-" : "+";
            const int b8 = 8 * esz;
            const std::string ty = "uint" + std::to_string(b8) + "_t";
            std::string s = "{ " + ty + " _a[" + std::to_string(nelems) + "],_b[" + std::to_string(nelems) + "],_r[" + std::to_string(nelems) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(vbytes) + "); ";
            s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(vbytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(nelems) + ";_i++) _r[_i]=(" + ty + ")(_a[_i]" + iop + "_b[_i]); ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(vbytes) + "); ";
            if (!Q) s += "c->vreg[" + std::to_string(rd) + "][1]=0; ";
            s += "}";
            put(s);
            return true;
        }

        // MUL (opcode 10011, U=0) - element-wise multiply, integer only.
        // U=1 at the same opcode is PMUL (polynomial), which is not the same
        // operation and stays on the fallback.
        if (opc5 == 0x13 && !U && size < 3) {
            const int b8 = 8 * esz;
            const std::string ty = "uint" + std::to_string(b8) + "_t";
            std::string s = "{ " + ty + " _a[" + std::to_string(nelems) + "],_b[" + std::to_string(nelems) + "],_r[" + std::to_string(nelems) + "]; ";
            s += "memcpy(_a,c->vreg[" + std::to_string(rn) + "]," + std::to_string(vbytes) + "); ";
            s += "memcpy(_b,c->vreg[" + std::to_string(rm) + "]," + std::to_string(vbytes) + "); ";
            s += "for(int _i=0;_i<" + std::to_string(nelems) + ";_i++) _r[_i]=(" + ty + ")(_a[_i]*_b[_i]); ";
            s += "memcpy(c->vreg[" + std::to_string(rd) + "],_r," + std::to_string(vbytes) + "); ";
            if (!Q) s += "c->vreg[" + std::to_string(rd) + "][1]=0; ";
            s += "}";
            put(s);
            return true;
        }

        // FP three-register same. Here the two "size" bits mean something
        // different from the integer forms: bit 23 is an opcode-extension bit
        // (it picks FSUB over FADD, FMLS over FMLA) and bit 22 is sz, the
        // element width. Treating bit 23 as part of an element size is what
        // makes FADD look like an unrelated instruction, so the two are split
        // apart explicitly here.
        {
            const u32 a   = (i >> 23) & 1;   // opcode extension, not a size bit
            const bool dbl = ((i >> 22) & 1) != 0;   // sz: 0 = float, 1 = double
            const int fsz  = dbl ? 8 : 4;
            const int fne  = vbytes / fsz;
            // opcode 11001: FMLA (a=0) / FMLS (a=1), both U=0.
            if (opc5 == 0x19 && !U) {
                const std::string ty=dbl?"uint64_t":"uint32_t";
                put("{"+ty+" _n["+std::to_string(fne)+"],_m["+std::to_string(fne)+"],_d["+std::to_string(fne)+"];"
                    "memcpy(_n,c->vreg["+std::to_string(rn)+"],"+std::to_string(vbytes)+");"
                    "memcpy(_m,c->vreg["+std::to_string(rm)+"],"+std::to_string(vbytes)+");"
                    "memcpy(_d,c->vreg["+std::to_string(rd)+"],"+std::to_string(vbytes)+");"
                    "for(unsigned _j=0;_j<"+std::to_string(fne)+";++_j){uint64_t _a=_n[_j],_b=_m[_j],_z=_d[_j],_v;");
                if(a)put("_a^="+std::string(dbl?"0x8000000000000000ULL":"0x80000000ULL")+";");
                put(EmitFPMulAddValue(dbl));
                put("_d[_j]=("+ty+")_v;}memcpy(c->vreg["+std::to_string(rd)+"],_d,"+std::to_string(vbytes)+");");
                if(!Q)put("c->vreg["+std::to_string(rd)+"][1]=0;");
                put("}");
                return true;
            }
            // opcode 11011 with U=1 and a=0 is FMUL. The U=0 encoding at the
            // same opcode is FMULX, which differs on infinity times zero, so
            // it is deliberately left alone rather than aliased to FMUL.
            if (opc5 == 0x1B && U && !a) {
                const std::string ty=dbl?"uint64_t":"uint32_t";
                put("{"+ty+" _n["+std::to_string(fne)+"],_m["+std::to_string(fne)+"],_d["+std::to_string(fne)+"];"
                    "memcpy(_n,c->vreg["+std::to_string(rn)+"],"+std::to_string(vbytes)+");"
                    "memcpy(_m,c->vreg["+std::to_string(rm)+"],"+std::to_string(vbytes)+");"
                    "for(unsigned _j=0;_j<"+std::to_string(fne)+";++_j){uint64_t _a=_n[_j],_b=_m[_j],_z=(_a^_b)&"+
                    std::string(dbl?"0x8000000000000000ULL":"0x80000000ULL")+",_v;");
                put(EmitFPMulAddValue(dbl));
                put("_d[_j]=("+ty+")_v;}memcpy(c->vreg["+std::to_string(rd)+"],_d,"+std::to_string(vbytes)+");");
                if(!Q)put("c->vreg["+std::to_string(rd)+"][1]=0;");
                put("}");
                return true;
            }
            // opcode 11111 with U=1 and a=0 is FDIV.
            if (opc5 == 0x1F && U && !a) {
                const std::string ty=dbl?"uint64_t":"uint32_t";
                put("{"+ty+" _n["+std::to_string(fne)+"],_m["+std::to_string(fne)+"],_d["+std::to_string(fne)+"];"
                    "memcpy(_n,c->vreg["+std::to_string(rn)+"],"+std::to_string(vbytes)+");"
                    "memcpy(_m,c->vreg["+std::to_string(rm)+"],"+std::to_string(vbytes)+");"
                    "for(unsigned _j=0;_j<"+std::to_string(fne)+";++_j){uint64_t _a=_n[_j],_b=_m[_j],_v;");
                put(EmitFPDivideValue(dbl));
                put("_d[_j]=("+ty+")_v;}memcpy(c->vreg["+std::to_string(rd)+"],_d,"+std::to_string(vbytes)+");");
                if(!Q)put("c->vreg["+std::to_string(rd)+"][1]=0;");
                put("}");
                return true;
            }
        }
    }

    // SIMD/FP load/store pair with V=1 (LDP/STP for S, D, Q registers).
    // Same general format as the integer pair handler, but with SIMD registers.
    // Sizes: opc=00→32-bit(S), opc=01→64-bit(D), opc=10→128-bit(Q).
    if ((i & 0x3E000000) == 0x2C000000) {
        const u32 opc = i >> 30;
        const bool is_load = (i >> 22) & 1;
        const u32 mode = (i >> 23) & 3;    // 1=post, 2=signed-offset, 3=pre
        const u32 rt2 = (i >> 10) & 31, rn = (i >> 5) & 31, rt = i & 31;
        s32 imm7 = (s32)((i >> 15) & 0x7F);
        if (imm7 & 0x40) imm7 |= ~0x7F;
        if ((opc <= 2) && mode >= 1 && mode <= 3) {
            const u32 sz = opc == 0 ? 4 : opc == 1 ? 8 : 16;
            const s64 off = (s64)imm7 * (s64)sz;
            const int bits = sz * 8;
            std::string s = "{ uint64_t _b=c->x[" + std::to_string(rn) + "]; int64_t _o=" + std::to_string((long long)off) + "; ";
            // std::string, not const char*: this gets concatenated with byte
            // offsets below, and as a pointer that silently becomes pointer
            // arithmetic on the literal rather than building an expression.
            const std::string addr = (mode == 1) ? std::string("_b") : std::string("(_b+_o)");
            if (is_load) {
                if (sz <= 8) {
                    s += "{ uint64_t _p0,_p1; recomp_ldp" + std::to_string(bits) + "(c," + addr +
                         ",&_p0,&_p1); ";
                    // The pair helper zero-extends S values; write the entire low half.
                    s += "c->vreg[" + std::to_string(rt) + "][0]=_p0; c->vreg[" +
                         std::to_string(rt) + "][1]=0; ";
                    s += "c->vreg[" + std::to_string(rt2) + "][0]=_p1; c->vreg[" +
                         std::to_string(rt2) + "][1]=0; }";
                } else {
                    // 128-bit: one pair per register.
                    s += "recomp_ldp64(c," + addr + ",&c->vreg[" + std::to_string(rt) +
                         "][0],&c->vreg[" + std::to_string(rt) + "][1]); ";
                    s += "recomp_ldp64(c," + addr + "+16,&c->vreg[" + std::to_string(rt2) +
                         "][0],&c->vreg[" + std::to_string(rt2) + "][1]); ";
                }
            } else {
                if (sz <= 8) {
                    s += "{ uint64_t _p0=0,_p1=0; memcpy(&_p0,&c->vreg[" + std::to_string(rt) +
                         "][0]," + std::to_string(sz) + "); memcpy(&_p1,&c->vreg[" +
                         std::to_string(rt2) + "][0]," + std::to_string(sz) + "); ";
                    s += "recomp_stp" + std::to_string(bits) + "(c," + addr + ",_p0,_p1); }";
                } else {
                    s += "recomp_stp64(c," + addr + ",c->vreg[" + std::to_string(rt) +
                         "][0],c->vreg[" + std::to_string(rt) + "][1]); ";
                    s += "recomp_stp64(c," + addr + "+16,c->vreg[" + std::to_string(rt2) +
                         "][0],c->vreg[" + std::to_string(rt2) + "][1]); ";
                }
            }
            if (mode == 1 || mode == 3) s += "c->x[" + std::to_string(rn) + "]=_b+_o; ";
            s += "}";
            put(s);
            return true;
        }
    }

    // SIMD/FP single-register load and store: the V=1 counterparts of the
    // integer forms handled further up. Access width is a scale value where
    // 0..3 mean 1/2/4/8 bytes and 4 means a whole 16-byte Q register; the
    // 16-byte case is encoded as size==00 with the high bit of opc set, which
    // is why the width cannot simply be read off the size field.
    const auto simd_scale = [](u32 size, u32 opc) {
        return (size == 0 && (opc & 2)) ? 4 : (int)size;
    };
    const auto simd_mem = [&](u32 rt, const std::string& ea, int scale, bool is_load) {
        const int nb = 1 << scale;
        const std::string v0 = "c->vreg[" + std::to_string(rt) + "][0]";
        const std::string v1 = "c->vreg[" + std::to_string(rt) + "][1]";
        if (is_load) {
            if (nb == 16) {
                return "recomp_ldp64(c," + ea + ",&" + v0 + ",&" + v1 + "); ";
            }
            // Narrower loads zero the rest of the register, as the architecture
            // requires - the destination is written whole, not merged into.
            return "{ uint64_t _v=recomp_load" + std::to_string(nb * 8) + "(c," + ea + "); " +
                   v0 + "=_v; " + v1 + "=0; } ";
        }
        if (nb == 16) {
            return "recomp_stp64(c," + ea + "," + v0 + "," + v1 + "); ";
        }
        return "{ uint64_t _v=0; memcpy(&_v,&" + v0 + "," + std::to_string(nb) + "); recomp_store" +
               std::to_string(nb * 8) + "(c," + ea + ",_v); } ";
    };

    // LDR/STR (SIMD, register offset): [Xn, Xm{, extend {amount}}].
    if ((i & 0x3F200C00) == 0x3C200800) {
        const u32 size = i >> 30, opc = (i >> 22) & 3;
        const u32 rn = (i >> 5) & 31, rt = i & 31;
        const u32 option = (i >> 13) & 7, S = (i >> 12) & 1;
        const int scale = simd_scale(size, opc);
        // Index register 31 is XZR, not SP.
        const std::string rm_str = Xz((i >> 16) & 31);
        std::string idx;
        switch (option) {
        case 2: idx = "(uint64_t)(uint32_t)" + rm_str; break;                      // UXTW
        case 3: idx = rm_str; break;                                                // LSL/UXTX
        case 6: idx = "(uint64_t)(int64_t)(int32_t)(uint32_t)" + rm_str; break;    // SXTW
        case 7: idx = rm_str; break;                                                // SXTX
        default: break;
        }
        if (!idx.empty()) {
            // The shift amount, when enabled, is the access scale - not the
            // size field, which disagrees with it for the 16-byte form.
            if (S && scale) idx = "((" + idx + ")<<" + std::to_string(scale) + ")";
            const std::string ea = "(c->x[" + std::to_string(rn) + "]+" + idx + ")";
            put("{ " + simd_mem(rt, ea, scale, (opc & 1) != 0) + "}");
            return true;
        }
    }

    // LDR/STR (SIMD, unsigned 12-bit immediate offset) - the commonest form.
    if ((i & 0x3F000000) == 0x3D000000) {
        const u32 size = i >> 30, opc = (i >> 22) & 3;
        const u32 imm12 = (i >> 10) & 0xFFF, rn = (i >> 5) & 31, rt = i & 31;
        const int scale = simd_scale(size, opc);
        const std::string ea = "(c->x[" + std::to_string(rn) + "]+" +
                               std::to_string((unsigned long long)imm12 << scale) + "ULL)";
        put("{ " + simd_mem(rt, ea, scale, (opc & 1) != 0) + "}");
        return true;
    }

    // LDUR/STUR (SIMD) and the pre/post-indexed immediate forms.
    if ((i & 0x3F000000) == 0x3C000000 && ((i >> 24) & 1) == 0) {
        const u32 size = i >> 30, opc = (i >> 22) & 3, mode = (i >> 10) & 3;
        const u32 rn = (i >> 5) & 31, rt = i & 31;
        s32 imm9 = (s32)((i >> 12) & 0x1FF);
        if (imm9 & 0x100) imm9 |= ~0x1FF;
        if (mode == 0 || mode == 1 || mode == 3) {
            const int scale = simd_scale(size, opc);
            std::string s = "{ uint64_t _b=c->x[" + std::to_string(rn) + "]; int64_t _o=" +
                            std::to_string((long long)imm9) + "; ";
            // Post-index accesses the unmodified base; the other two add the
            // offset first. Pre- and post-index both write the base back.
            const std::string ea = (mode == 1) ? "_b" : "(_b+_o)";
            s += simd_mem(rt, ea, scale, (opc & 1) != 0);
            if (mode == 1 || mode == 3) s += "c->x[" + std::to_string(rn) + "]=_b+_o; ";
            s += "}";
            put(s);
            return true;
        }
    }

    // An instruction the decoder doesn't know is reported and stepped over
    // rather than ending the block. recomp_unhandled is a non-fatal stub, and
    // ending the block here would set pc to the following instruction - an
    // address that block discovery never marked as a block start, so the
    // dispatcher could not resolve it and execution would stop dead at the
    // first unimplemented opcode instead of continuing past it.
    // Leaving the block the moment the handler halts is what makes the
    // hand-off to the fallback engine correct: the remaining instructions in
    // this block must not run twice, since the fallback resumes at this same
    // PC and will execute them itself.
    // ---- EXTR (and therefore ROR immediate) ----------------------------
    //
    // ROR Rd, Rn, #imm is EXTR with Rn == Rm, and between the two widths it was
    // 13.1% of every unhandled instruction in a second title - the single
    // largest gap after EXT, and pure integer work. Compilers emit it for
    // rotates in hashing and bit-twiddling code.
    //
    // sf 0 0 100111 N 0 Rm imms Rn Rd. N must track sf: N==sf is the only
    // allocated combination, and imms must be < 32 in the 32-bit form.
    if ((i & 0x7FA00000) == 0x13800000) {
        const u32 sf = i >> 31, N = (i >> 22) & 1;
        const u32 rm = (i >> 16) & 31, imms = (i >> 10) & 63;
        const u32 rn = (i >> 5) & 31, rd = i & 31;
        const u32 width = sf ? 64u : 32u;
        if (N == sf && imms < width) {
            if (rd != 31) {
                std::string s2 = "{ uint64_t _hi=" + (sf ? Xz(rn) : ("(uint64_t)" + Wz(rn))) +
                                 ", _lo=" + (sf ? Xz(rm) : ("(uint64_t)" + Wz(rm))) + ", _r; ";
                if (imms == 0) {
                    // Shifting by the full width is undefined in C, and lsb==0
                    // is the common case (a plain MOV-through-EXTR).
                    s2 += "_r = _lo; ";
                } else {
                    s2 += "_r = (_lo >> " + std::to_string(imms) + ") | (_hi << " +
                          std::to_string(width - imms) + "); ";
                }
                if (!sf) s2 += "_r &= 0xFFFFFFFFULL; ";
                s2 += "c->x[" + std::to_string(rd) + "] = _r; }";
                put(s2);
            } else {
                put("/* extr -> xzr */");
            }
            return true;
        }
    }

    // ---- ADC / ADCS / SBC / SBCS ---------------------------------------
    //
    // sf op S 11010000 Rm 000000 Rn Rd. op: 0 = ADC, 1 = SBC. Small but real -
    // 1.4% of the gap - and it appears in multi-word arithmetic where getting
    // the carry wrong is silent.
    if ((i & 0x1FE0FC00) == 0x1A000000) {
        const u32 sf = i >> 31, op = (i >> 30) & 1, S = (i >> 29) & 1;
        const u32 rm = (i >> 16) & 31, rn = (i >> 5) & 31, rd = i & 31;
        const char* sign = sf ? "0x8000000000000000ULL" : "0x80000000ULL";
        std::string s2 = "{ uint64_t _a=" + (sf ? Xz(rn) : ("(uint64_t)" + Wz(rn))) +
                         ", _b=" + (sf ? Xz(rm) : ("(uint64_t)" + Wz(rm))) +
                         ", _ci=(uint64_t)c->c, _r; ";
        // SBC is ADC against the inverted operand, which is also what makes the
        // borrow come out right: carry-clear means borrow.
        if (op) s2 += sf ? "_b = ~_b; " : "_b = (~_b) & 0xFFFFFFFFULL; ";
        s2 += "_r = _a + _b + _ci; ";
        if (!sf) s2 += "_r &= 0xFFFFFFFFULL; ";
        if (S) {
            s2 += std::string("c->z = (_r == 0); c->n = (_r & ") + sign + ") ? 1 : 0; ";
            if (sf) {
                // Unsigned overflow with a carry-in: the sum wraps either when
                // it lands below _a, or when it lands exactly on _a with the
                // carry set (_b == ~0 and _ci == 1).
                s2 += "c->c = (_r < _a) || (_ci && _r == _a); ";
            } else {
                s2 += "c->c = ((_a + _b + _ci) > 0xFFFFFFFFULL); ";
            }
            s2 += std::string("c->v = ((~(_a ^ _b) & (_a ^ _r)) & ") + sign + ") ? 1 : 0; ";
        }
        if (rd != 31) {
            s2 += "c->x[" + std::to_string(rd) + "] = _r; ";
        }
        s2 += "}";
        put(s2);
        return true;
    }

    // ---- LDPSW ----------------------------------------------------------
    //
    // Load pair of 32-bit words, each sign-extended to 64. opc=01 V=0 L=1, so
    // it sits outside the LDP/STP handler above, which only covers opc 00/10.
    // 3.1% of the gap, and it turns up wherever a pair of ints is read into
    // 64-bit registers at once.
    //
    // 0110100xx1 ... : signed offset 0x69400000, post-index 0x68C00000,
    // pre-index 0x69C00000. imm7 is scaled by 4.
    if ((i & 0xFFC00000) == 0x69400000 || (i & 0xFFC00000) == 0x68C00000 ||
        (i & 0xFFC00000) == 0x69C00000) {
        const u32 rt = i & 31, rn = (i >> 5) & 31, rt2 = (i >> 10) & 31;
        const s32 imm7 = ((s32)(((i >> 15) & 0x7F) << 25) >> 25) * 4;
        const bool post = (i & 0xFFC00000) == 0x68C00000;
        const bool pre = (i & 0xFFC00000) == 0x69C00000;

        std::string s2 = "{ uint64_t _base=" + Xsp(rn) + "; uint64_t _addr = _base";
        if (!post) s2 += " + (int64_t)" + std::to_string(imm7);
        s2 += "; ";
        // Both words are read before either register is written: Rn may be Rt
        // or Rt2, and writing early would corrupt the address for the second
        // load.
        s2 += "uint64_t _v1 = (uint64_t)(int64_t)(int32_t)(uint32_t)recomp_load32(c,_addr); ";
        s2 += "uint64_t _v2 = (uint64_t)(int64_t)(int32_t)(uint32_t)recomp_load32(c,_addr+4); ";
        if (rt != 31) s2 += "c->x[" + std::to_string(rt) + "] = _v1; ";
        if (rt2 != 31) s2 += "c->x[" + std::to_string(rt2) + "] = _v2; ";
        if (post || pre) {
            s2 += "c->x[" + std::to_string(rn) + "] = _base + (int64_t)" +
                  std::to_string(imm7) + "; ";
        }
        s2 += "}";
        put(s2);
        return true;
    }

    put_unhandled();
    return true;
}


const char* RuntimeH();
const char* RuntimeC();

/// One bucket of the unhandled-instruction histogram.
struct UnhandledSite {
    size_t count = 0;
    u32 example_insn = 0;   ///< a representative full encoding, for hand-decoding
    u64 example_pc = 0;     ///< where that example lives, for disassembling in context
};

/// AArch64 top-level encoding group, from bits 28:25 of the instruction.
/// Table C4-1 in the ARM ARM. Coarse, but enough to say "the gap is SIMD"
/// versus "the gap is loads and stores" at a glance.
inline const char* EncodingGroupName(u32 op0) {
    switch (op0 & 0xF) {
    case 0x0: return "reserved/sme";
    case 0x1: case 0x3: return "unallocated";
    case 0x2: return "sve";
    case 0x8: case 0x9: return "dp-immediate";
    case 0xA: case 0xB: return "branch/exception/system";
    case 0x4: case 0x6: case 0xC: case 0xE: return "load/store";
    case 0x5: case 0xD: return "dp-register";
    case 0x7: case 0xF: return "dp-simd/fp";
    default: return "unknown";
    }
}

// Stats returned to the caller for manifest/reporting.
struct RecompileStats {
    size_t blocks = 0;
    size_t instructions = 0;          ///< n_bytes/4: every word in .text, padding included
    size_t translated_terminators = 0;
    size_t emitted = 0;               ///< instructions actually walked inside discovered blocks
    size_t unhandled = 0;             ///< of those, how many fell through to recomp_unhandled

    /// Unhandled counts keyed by encoding group (bits 28:25).
    std::map<u32, size_t> unhandled_by_group;
    /// Unhandled counts keyed by the top 10 bits, which usually identifies the
    /// opcode family while dropping register operands. This is the list that
    /// says which instruction to implement next.
    std::map<u32, UnhandledSite> unhandled_by_signature;

    /// Fraction of walked instructions the decoder could not translate.
    double UnhandledFraction() const {
        return emitted ? double(unhandled) / double(emitted) : 0.0;
    }
};

// Emit a buildable C project that statically recompiles `text` (raw AArch64 .text at `base`).
// Layout written into out_dir:
//   CMakeLists.txt, main.c, recomp_export.c, recomp_runtime.{c,h}   <- hand-readable top level
//   src/recompiled_<mod>.c, src/recompiled_<mod>_<n>.c              <- generated translation units
//   data/{text,rodata,data}.bin                                     <- bundled guest segments
//   build/                                                          <- cmake's one canonical build dir
// The translation units are kept in src/ rather than the module root: a large
// title emits hundreds of them, and loose in the root they bury the four files
// anyone actually needs to look at.
// Optional rodata/data parameters bundle those segments so the exported exe is self-contained.
inline RecompileStats EmitProject(const std::string& mod, const u8* text, size_t n_bytes, u64 base,
                                  const std::string& out_dir, bool source_only,
                                  const u8* rodata = nullptr, size_t rodata_size = 0,
                                  const u8* data_seg = nullptr, size_t data_size = 0,
                                  // Guest address to begin executing at. Defaults to `base`, but a
                                  // real NSO starts with a MOD0 header rather than code, so the
                                  // loader passes the module's actual entry here.
                                  u64 entry_pc = 0,
                                  // Human-readable game name, shown by the generated executable so
                                  // it identifies itself rather than printing raw addresses.
                                  const std::string& display_title = {},
                                  // Addresses of this module's exported dynsym symbols, so
                                  // block discovery seeds a root at each even when nothing in
                                  // this module's own .text branches there directly.
                                  const std::vector<u64>* extra_roots = nullptr) {
    RecompileStats stats;
    auto blocks = DiscoverBlocks(text, n_bytes, base, entry_pc, extra_roots);
    const u32* p = reinterpret_cast<const u32*>(text);
    stats.blocks = blocks.size();
    stats.instructions = n_bytes / 4;

    // Block bodies are split across several translation units. A whole game is
    // millions of blocks, which as one .c file runs to hundreds of megabytes -
    // enough that a compiler needs hours and a great deal of memory, or gives
    // up outright. Splitting keeps each unit to a sane size and lets the build
    // compile them in parallel.
    constexpr size_t kBlocksPerUnit = 20000;
    const size_t unit_count =
        blocks.empty() ? 1 : (blocks.size() + kBlocksPerUnit - 1) / kBlocksPerUnit;
    // Written straight to disk rather than buffered. A whole module is over a
    // gigabyte of C, and holding every unit in memory before writing meant the
    // exporter needed that much RAM on top of the game's own data - enough that
    // exporting a large title died partway, after the biggest module and before
    // the rest, leaving an incomplete and inconsistent set of images behind.
    const auto make_dir = [](const std::string& dir) {
        std::error_code ec;
        std::filesystem::create_directories(Utf8Path(dir), ec);
    };
    make_dir(out_dir);
    // Every generated translation unit lands here, keeping the module root down
    // to the few files a human reads.
    const std::string src_dir = out_dir + "/src";
    make_dir(src_dir);
    std::vector<std::ofstream> units;
    units.reserve(unit_count);
    for (size_t u = 0; u < unit_count; ++u) {
        units.emplace_back(Utf8Path(src_dir + "/recompiled_" + mod + "_" + std::to_string(u) + ".c"),
                           std::ios::binary);
        units.back() << "/* auto-generated by suyu static recompiler - DO NOT EDIT */\n"
                        "#include \"recomp_runtime.h\"\n"
                        "#include <stdint.h>\n"
                        "#include <string.h>\n"   // memcpy, used by the scalar FP paths
                        "#include <math.h>\n"
                        "struct _recomp_ent{uint64_t lo,hi; BlockFn fn;};\n\n";
        if (u == 0) units.back() << "int g_recomp_guard_host_v2=0;\n";
    }

    // Each unit accumulates into a string and is written out in large blocks.
    // Streaming every fragment through operator<< meant a sentry construction
    // and a virtual xsputn per token (several per instruction) straight into a
    // default 4 KiB filebuf; batching turns the whole emit into a handful of
    // multi-megabyte writes per unit at no cost in output bytes.
    // Blocks are visited in unit order, so a single buffer serves every unit -
    // one buffer per unit would cost hundreds of megabytes of slack on a title
    // that emits hundreds of units.
    constexpr size_t kUnitFlushBytes = 8u << 20;
    std::string rcu;
    rcu.reserve(kUnitFlushBytes + (1u << 20));
    const auto flush_unit = [&](size_t u, bool force) {
        if (rcu.size() >= kUnitFlushBytes || (force && !rcu.empty())) {
            units[u].write(rcu.data(), (std::streamsize)rcu.size());
            rcu.clear();
        }
    };

    size_t block_index = 0;
    // Hoisted out of the per-instruction loop: a fresh std::string per guest
    // instruction is one allocation per instruction across the whole title.
    std::string body;
    body.reserve(4096);
    char namebuf[64];

    // Each unit carries the dispatch-table entries for its own blocks. Putting
    // the whole table in one file instead meant a single translation unit that
    // grew with the module - hundreds of megabytes of forward declarations and
    // one array initialiser for a large title, compiled by one cl.exe with no
    // parallelism and enormous peak memory, which is both the longest pole in
    // the build and a plausible way for it to die outright. Here the entries
    // sit next to the definitions they point at, so they cost no extra
    // declarations and compile across every core with the bodies.
    std::vector<u64> seg_hi(unit_count, 0);
    const auto emit_seg = [&](size_t u) {
        const size_t lo = u * kBlocksPerUnit;
        size_t hi = lo + kBlocksPerUnit;
        if (hi > blocks.size()) {
            hi = blocks.size();
        }
        char line[160];
        snprintf(line, sizeof line, "\nconst struct _recomp_ent _seg_%s_%zu[] = {\n", mod.c_str(),
                 u);
        rcu += line;
        for (size_t i = lo; i < hi; ++i) {
            FuncNameTo(namebuf, mod.c_str(), blocks[i].vaddr);
            snprintf(line, sizeof line, "  {0x%llxULL, 0x%llxULL, %s},\n",
                     (unsigned long long)blocks[i].vaddr,
                     (unsigned long long)(blocks[i].vaddr + (u64)(blocks[i].count - 1) * 4),
                     namebuf);
            rcu += line;
        }
        // C has no empty initialiser list; a zero-length segment still needs a
        // well-formed array, and its declared count of 0 keeps it unsearchable.
        if (lo >= hi) {
            rcu += "  {0ULL, 0ULL, 0}\n";
        } else {
            const auto& last = blocks[hi - 1];
            seg_hi[u] = last.vaddr + (u64)(last.count - 1) * 4;
        }
        snprintf(line, sizeof line, "};\nconst unsigned _segn_%s_%zu = %zuU;\n", mod.c_str(), u,
                 hi - lo);
        rcu += line;
    };

    // Every block's address, so a direct branch can be turned into a direct call
    // when its target is a block we actually emitted.
    std::unordered_set<u64> chain_blocks;
    chain_blocks.reserve(blocks.size() * 2);
    for (const auto& b : blocks) {
        chain_blocks.insert(b.vaddr);
    }
    g_chain_blocks = &chain_blocks;
    g_chain_mod = mod.c_str();
    struct ChainScope {
        ~ChainScope() {
            g_chain_blocks = nullptr;
            g_chain_mod = nullptr;
        }
    } chain_scope;

    size_t cur_unit = 0;
    for (const auto& b : blocks) {
        const size_t unit = block_index / kBlocksPerUnit;
        if (unit != cur_unit) {
            emit_seg(cur_unit);
            flush_unit(cur_unit, true);
            cur_unit = unit;
        }
        ++block_index;
        FuncNameTo(namebuf, mod.c_str(), b.vaddr);
        rcu += "void ";
        rcu += namebuf;
        rcu += "(GuestContext* c){\n";
        const u32 first = (u32)((b.vaddr - base) / 4);
        rcu += "    static const uint32_t _expected[]={";
        for (u32 k = 0; k < b.count; ++k) {
            char word[32]; snprintf(word, sizeof word, "0x%08xU,", p[first + k]); rcu += word;
        }
        // Verify on the first entry, and after that only when the host says
        // guest code may have changed. Checking the bytes on every entry made
        // the guard the largest single cost in the frame: it ran before any
        // guest work, and cost either a callback per instruction or a memcmp of
        // the whole block. Comparing one counter replaces both.
        //
        // This keeps the guarantee the guard actually claims - synchronized
        // instruction fetch - because on AArch64 the guest cannot synchronize
        // new code without IC IVAU, and the host bumps the generation there, on
        // any cache-range invalidation, on a full icache flush, and on every
        // page-table refresh. What it does not do, and never did, is catch
        // another thread rewriting a block while that block executes.
        rcu += "};\n"
               "    static uint64_t _guard_seen=0;\n"
               "    { /* +1 so a fresh block never matches generation zero. */\n"
               "      const uint64_t _gen=(c->host_mem&&c->host_mem->guard_generation)\n"
               "          ? *c->host_mem->guard_generation+1 : 1;\n"
               "      if(_guard_seen!=_gen){\n"
               "        recomp_code_guard(c,g_module_base+" + std::to_string(b.vaddr) +
               "ULL,_expected," + std::to_string(b.count) + "U,g_recomp_guard_host_v2);\n"
               "        _guard_seen=_gen;\n"
               "      } }\n";
        // Lookup indexes every emitted instruction, not only block starts. An
        // indirect transfer can therefore enter the middle of this function.
        // Direct chains publish their exact target PC before calling, so any
        // address outside this block is an invariant violation, never a request
        // to start silently at the first instruction.
        {
            char addr[32];
            rcu += "    { uint64_t _entry=c->pc-g_module_base; if(_entry!=0x";
            snprintf(addr, sizeof addr, "%llx", (unsigned long long)b.vaddr);
            rcu += addr;
            rcu += "ULL){ if(_entry<0x";
            rcu += addr;
            rcu += "ULL || _entry>0x";
            snprintf(addr, sizeof addr, "%llx",
                     (unsigned long long)(b.vaddr + (u64)(b.count - 1) * 4));
            rcu += addr;
            rcu += "ULL || ((_entry-0x";
            snprintf(addr, sizeof addr, "%llx", (unsigned long long)b.vaddr);
            rcu += addr;
            rcu += "ULL)&3ULL)!=0){ recomp_unhandled(c,0U,c->pc); return; } switch((unsigned)((_entry-0x";
            rcu += addr;
            rcu += "ULL)>>2)){\n";
            for (u32 k = 1; k < b.count; ++k) {
                char entry[96];
                snprintf(entry, sizeof entry, "      case %uU: goto _recomp_entry_%u;\n", k, k);
                rcu += entry;
            }
            rcu += "      default: recomp_unhandled(c,0U,c->pc); return;\n    } } }\n";
        }
        bool open = true;
        for (u32 k = 0; k < b.count; ++k) {
            if (k > 0) {
                char label[48];
                snprintf(label, sizeof label, "_recomp_entry_%u: ;\n", k);
                rcu += label;
            }
            body.clear();
            const u32 insn = p[first + k];
            const u64 insn_pc = b.vaddr + (u64)k * 4;
            bool unhandled = false;
            open = Translate(insn, insn_pc, body, &unhandled);
            rcu += body;
            ++stats.emitted;
            if (unhandled) {
                ++stats.unhandled;
                ++stats.unhandled_by_group[(insn >> 25) & 0xF];
                auto& site = stats.unhandled_by_signature[insn & 0xFFC00000u];
                if (site.count == 0) {
                    site.example_insn = insn;
                    site.example_pc = insn_pc;
                }
                ++site.count;
            }
            if (!open) ++stats.translated_terminators;
        }
        // A block that ends by running off its own end still has to hand the
        // dispatcher an absolute guest address, exactly as every branch
        // terminator above does. Emitting the bare module-relative offset here
        // was silently catastrophic: the host dispatcher picks the owning image
        // by "greatest base not exceeding the PC", so a small unbased value has
        // no owner at all and falls through to the "try every image at the raw
        // PC" path - which happily returns *some other module's* block at the
        // same offset and runs it against this module's register state.
        if (open) {
            char tail[80];
            snprintf(tail, sizeof tail, "    c->pc=g_module_base+0x%llxULL; return;\n",
                     (unsigned long long)(b.vaddr + b.size));
            rcu += tail;
        }
        rcu += "}\n\n";
        flush_unit(unit, false);
    }
    emit_seg(cur_unit);
    flush_unit(cur_unit, true);

    // The dispatch table is now just a directory of the per-unit segments
    // emitted above: a few lines per unit instead of two entries per block, so
    // this file stays a few kilobytes no matter how large the module is.
    std::string rc;
    rc.reserve(unit_count * 200 + 4096);
    rc += "/* auto-generated by suyu static recompiler - DO NOT EDIT */\n"
          "#include \"recomp_runtime.h\"\n#include <stdint.h>\n\n"
          "struct _recomp_ent{uint64_t lo,hi; BlockFn fn;};\n\n";
    {
        char line[192];
        for (size_t u = 0; u < unit_count; ++u) {
            snprintf(line, sizeof line,
                     "extern const struct _recomp_ent _seg_%s_%zu[];\nextern const unsigned "
                     "_segn_%s_%zu;\n",
                     mod.c_str(), u, mod.c_str(), u);
            rc += line;
        }
        rc += "\nstatic const struct _recomp_ent* const _segs[] = {\n";
        for (size_t u = 0; u < unit_count; ++u) {
            snprintf(line, sizeof line, "  _seg_%s_%zu,\n", mod.c_str(), u);
            rc += line;
        }
        rc += "};\nstatic const unsigned* const _segn[] = {\n";
        for (size_t u = 0; u < unit_count; ++u) {
            snprintf(line, sizeof line, "  &_segn_%s_%zu,\n", mod.c_str(), u);
            rc += line;
        }
        // Highest guest address in each segment. Segments follow the block list,
        // which is sorted ascending, so they are disjoint and ordered - this
        // lets the lookup binary-search for the owning segment rather than
        // walking every one of them on each dispatch.
        rc += "};\nstatic const uint64_t _seg_hi[] = {\n";
        for (size_t u = 0; u < unit_count; ++u) {
            snprintf(line, sizeof line, "  0x%llxULL,\n", (unsigned long long)seg_hi[u]);
            rc += line;
        }
        rc += "};\n";
    }
    rc += "#define _NSEG (sizeof(_segs)/sizeof(_segs[0]))\n"
          "/* Direct block index.\n"
          "\n"
          "   Every block boundary lands in recomp_lookup - tens of millions of\n"
          "   times a second - and it used to run two binary searches, one over\n"
          "   segments and one over a segment's entries, which for the main image\n"
          "   means about twenty probes over 600k entries per dispatch.\n"
          "\n"
          "   Guest instructions are 4-byte aligned and a module's blocks span one\n"
          "   contiguous address range, so (pc - lo) >> 2 indexes a flat array\n"
          "   directly. One bounds check and one load, no search.\n"
          "\n"
          "   Built once from the sorted tables at load time and read-only after,\n"
          "   so it needs no locking and no per-thread state - a thread-local\n"
          "   cache was tried instead and cost 34%% in the race phase, because the\n"
          "   footprint per thread stops being free once the title is actually\n"
          "   using its threads.\n"
          "\n"
          "   Costs one pointer per guest instruction word in the module, so about\n"
          "   24 MB for a 3M-instruction image. If the allocation fails the binary\n"
          "   search below still answers, just slowly. */\n"
          "static BlockFn* _idx = 0;\n"
          "static uint64_t _idx_lo = 0, _idx_hi = 0;\n"
          "static int _idx_tried = 0;\n"
          "static void _build_idx(void){\n"
          "  size_t i, j, n;\n"
          "  if(_idx_tried) return;\n"
          "  _idx_tried = 1;\n"
          "  if(_NSEG == 0 || *_segn[0] == 0) return;\n"
          "  _idx_lo = _segs[0][0].lo;\n"
          "  _idx_hi = _seg_hi[_NSEG-1];\n"
          "  if(_idx_hi < _idx_lo) return;\n"
          "  n = (size_t)((_idx_hi - _idx_lo) >> 2) + 1;\n"
          "  _idx = (BlockFn*)calloc(n, sizeof(BlockFn));\n"
          "  if(!_idx) return;\n"
          "  for(i=0;i<_NSEG;i++){\n"
          "    const struct _recomp_ent* t=_segs[i];\n"
          "    unsigned m=*_segn[i];\n"
          "    for(j=0;j<m;j++){ uint64_t k,count=((t[j].hi-t[j].lo)>>2)+1;\n"
          "      for(k=0;k<count;k++) _idx[(size_t)(((t[j].lo+4*k)-_idx_lo)>>2)]=t[j].fn; }\n"
          "  }\n"
          "}\n"
          "/* Called from recomp_image_set_base, which the loader runs once per\n"
          "   module before any guest thread exists - so the build below is not\n"
          "   racing anything. */\n"
          "void recomp_build_index(void){ _build_idx(); }\n"
          "/* Module-relative view of the block index, for the host dispatcher.\n"
          "   The index itself is static to this unit, so the exported wrapper in\n"
          "   recomp_export.c has to come through here. */\n"
          "int _recomp_index_view(uint64_t* lo, uint64_t* hi, BlockFn** idx){\n"
          "  _build_idx();\n"
          "  if(!_idx) return 0;\n"
          "  *lo = _idx_lo; *hi = _idx_hi; *idx = _idx;\n"
          "  return 1;\n"
          "}\n"
          "\n"
          "BlockFn recomp_lookup(uint64_t pc){\n"
          "  if(_idx && pc>=_idx_lo && pc<=_idx_hi && ((pc-_idx_lo)&3ULL)==0) return _idx[(size_t)((pc-_idx_lo)>>2)];\n"
          "  {\n"
          "  unsigned slo=0, shi=(unsigned)_NSEG;\n"
          "  while(slo<shi){ unsigned m=slo+(shi-slo)/2; if(_seg_hi[m]<pc) slo=m+1; else shi=m; }\n"
          "  if(slo>=_NSEG) return 0;\n"
          "  {\n"
          "    const struct _recomp_ent* t=_segs[slo];\n"
          "    unsigned n=*_segn[slo], lo=0, hi=n;\n"
          "    while(lo<hi){ unsigned m=lo+(hi-lo)/2; if(t[m].lo<=pc) lo=m+1; else hi=m; }\n"
          "    if(lo==0) return 0; --lo;\n"
          "    return (pc<=t[lo].hi && ((pc-t[lo].lo)&3ULL)==0)?t[lo].fn:0;\n"
          "  }\n"
          "  }\n}\n";

    const std::string title_str = display_title.empty() ? mod : display_title;
    std::ostringstream mc;
    mc << "#include \"recomp_runtime.h\"\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n";
    mc << "#ifdef HAVE_SDL2\n#include <SDL2/SDL.h>\n#endif\n\n";
    mc << "#define GUEST_MEM_SIZE (256ULL * 1024 * 1024) /* 256 MB */\n\n";
    mc << "static const char* kGameTitle = \"" << title_str << "\";\n\n";
    mc << "#ifdef HAVE_SDL2\n";
    mc << "static SDL_Window*   g_sdl_window   = NULL;\n";
    mc << "static SDL_Renderer* g_sdl_renderer = NULL;\n";
    mc << "static SDL_Texture*  g_sdl_texture  = NULL;\n";
    mc << "static int g_fb_w = 1280, g_fb_h = 720;\n\n";
    mc << "/* Called from the SVC handler when the guest flushes a framebuffer.\n";
    mc << "   fb: CPU-accessible RGBA8 pixels, w x h. */\n";
    mc << "void recomp_sdl_blit(const void* fb, int w, int h) {\n";
    mc << "  if(!g_sdl_renderer) return;\n";
    mc << "  if(w!=g_fb_w || h!=g_fb_h || !g_sdl_texture) {\n";
    mc << "    if(g_sdl_texture) SDL_DestroyTexture(g_sdl_texture);\n";
    mc << "    g_sdl_texture = SDL_CreateTexture(g_sdl_renderer,\n";
    mc << "      SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, w, h);\n";
    mc << "    g_fb_w=w; g_fb_h=h;\n";
    mc << "  }\n";
    mc << "  SDL_UpdateTexture(g_sdl_texture, NULL, fb, w*4);\n";
    mc << "  SDL_RenderClear(g_sdl_renderer);\n";
    mc << "  SDL_RenderCopy(g_sdl_renderer, g_sdl_texture, NULL, NULL);\n";
    mc << "  SDL_RenderPresent(g_sdl_renderer);\n}\n\n";
    mc << "static int sdl_pump_events(void) {\n";
    mc << "  SDL_Event e;\n";
    mc << "  while(SDL_PollEvent(&e)) {\n";
    mc << "    if(e.type==SDL_QUIT) return 0;\n";
    mc << "    if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE) return 0;\n";
    mc << "  }\n  return 1;\n}\n#endif /* HAVE_SDL2 */\n\n";
    mc << "int main(int argc, char** argv){\n";
    mc << "  printf(\"=== %s ===\\n\", kGameTitle);\n\n";
    mc << "#ifdef HAVE_SDL2\n";
    mc << "  if(SDL_Init(SDL_INIT_VIDEO) == 0) {\n";
    mc << "    g_sdl_window = SDL_CreateWindow(kGameTitle,\n";
    mc << "      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_RESIZABLE);\n";
    mc << "    if(g_sdl_window)\n";
    mc << "      g_sdl_renderer = SDL_CreateRenderer(g_sdl_window, -1, SDL_RENDERER_ACCELERATED);\n";
    mc << "  } else {\n";
    mc << "    fprintf(stderr, \"[recomp] SDL2 init failed: %s (running headless)\\n\", SDL_GetError());\n";
    mc << "  }\n#endif\n\n";
    mc << "  uint8_t* mem = (uint8_t*)calloc(1, (size_t)GUEST_MEM_SIZE);\n";
    mc << "  if(!mem){ fprintf(stderr,\"Failed to allocate guest memory\\n\"); return 1; }\n";
    mc << "  GuestContext c; memset(&c,0,sizeof c);\n";
    mc << "  c.mem=mem; c.mem_size=GUEST_MEM_SIZE;\n";
    mc << "  c.mem_base_vaddr=0x" << std::hex << base << std::dec << "ULL;\n";
    mc << "  c.heap_base=c.mem_base_vaddr+GUEST_MEM_SIZE/2;\n";
    mc << "  c.heap_cur=c.heap_base; c.heap_end=c.mem_base_vaddr+GUEST_MEM_SIZE;\n";
    mc << "  c.x[31]=c.mem_base_vaddr + GUEST_MEM_SIZE - 16; /* SP */\n";
    mc << "  c.pc=0x" << std::hex << (entry_pc ? entry_pc : base) << std::dec << "ULL;\n\n";
    mc << "  recomp_save_init(&c, argv[0]);\n\n";
    mc << "  { char data_dir[512];\n";
    mc << "    snprintf(data_dir,sizeof data_dir,\"%s\",argv[0]);\n";
    mc << "    char* sl=strrchr(data_dir,'\\\\'); if(!sl) sl=strrchr(data_dir,'/'); if(sl) *(sl+1)=0; else data_dir[0]=0;\n";
    mc << "    strncat(data_dir,\"data\",sizeof(data_dir)-strlen(data_dir)-1);\n";
    mc << "    recomp_load_segments(&c,data_dir);\n  }\n\n";
    mc << "  { uint64_t sz=0;\n";
    mc << "    if(recomp_save_exists(&c,\"autosave.bin\")){\n";
    mc << "      recomp_save_read(&c,\"autosave.bin\",c.mem,(uint64_t)GUEST_MEM_SIZE,&sz);\n";
    mc << "      printf(\"[recomp] Restored autosave (%llu bytes)\\n\",(unsigned long long)sz);\n";
    mc << "    }\n  }\n\n";
    mc << "  printf(\"[recomp] Starting at pc=0x%llx\\n\",(unsigned long long)c.pc);\n";
    mc << "  /* Main loop: pump SDL events while the guest runs */\n";
    mc << "#ifdef HAVE_SDL2\n";
    mc << "  while(!c.halted) {\n";
    mc << "    if(!sdl_pump_events()) break;\n";
    mc << "    recomp_run(&c); /* runs until SVC or halt */\n";
    mc << "  }\n";
    mc << "#else\n";
    mc << "  recomp_run(&c);\n";
    mc << "#endif\n\n";
    mc << "  recomp_save_write(&c,\"autosave.bin\",c.mem,(uint64_t)GUEST_MEM_SIZE);\n";
    mc << "  printf(\"[recomp] halted at pc=0x%llx\\n\",(unsigned long long)c.pc);\n";
    mc << "#ifdef HAVE_SDL2\n";
    mc << "  if(g_sdl_texture)  SDL_DestroyTexture(g_sdl_texture);\n";
    mc << "  if(g_sdl_renderer) SDL_DestroyRenderer(g_sdl_renderer);\n";
    mc << "  if(g_sdl_window)   SDL_DestroyWindow(g_sdl_window);\n";
    mc << "  SDL_Quit();\n";
    mc << "#endif\n";
    mc << "  free(mem);\n  return 0;\n}\n";

    std::ostringstream cm;
    // The generated units live in src/ and include "recomp_runtime.h" from the
    // module root, so the root has to be on the include path.
    cm << "cmake_minimum_required(VERSION 3.13)\n"
       << "# When this directory is pulled into a bigger build (suyu-cmd linking the\n"
       << "# modules statically) it must not start a project of its own - it just\n"
       << "# contributes targets. Standalone it is still a complete project.\n"
       << "if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)\n"
       << "  project(suyu_recompiled C)\n"
       << "else()\n"
       << "  enable_language(C)\n"
       << "endif()\n"
       << "set(CMAKE_C_STANDARD 11)\n"
       << "include_directories(${CMAKE_CURRENT_SOURCE_DIR})\n"
       << "# A host project (suyu) may apply C++ flags to every language; this\n"
       << "# directory is plain C, and MSVC rejects /std:c11 together with\n"
       << "# /std:c++20 outright.\n"
       << "get_directory_property(_recomp_opts COMPILE_OPTIONS)\n"
       << "if(_recomp_opts)\n"
       << "  list(FILTER _recomp_opts EXCLUDE REGEX \"std:c\\\\+\\\\+|std=c\\\\+\\\\+|EHsc|permissive\")\n"
       << "  set_directory_properties(PROPERTIES COMPILE_OPTIONS \"${_recomp_opts}\")\n"
       << "endif()\n\n"
       << "# RECOMP_STATIC_ONLY: build just the static library this module\n"
       << "# contributes to a host executable, skipping the portable standalone exe\n"
       << "# and the loadable shared image.\n"
       << "if(NOT RECOMP_STATIC_ONLY)\n"
       << "# Optional: SDL2 window for display output.\n"
       << "# Install SDL2 (e.g. vcpkg install sdl2) to enable the game window.\n"
       << "find_package(SDL2 QUIET)\n"
       << "endif()\n\n"
       << "# Generated translation units, all under src/.\nset(RECOMP_SOURCES\n"
       << "    src/recompiled_" << mod << ".c";
    for (size_t u = 0; u < unit_count; ++u) {
        cm << "\n    src/recompiled_" << mod << "_" << u << ".c";
    }
    cm << ")\n\n"
       << "# Generated block bodies are huge flat switch/if chains translated\n"
       << "# straight from machine code - there's no loop nesting or hot path for\n"
       << "# -O2 to meaningfully improve, just a lot of blocks for it to chew\n"
       << "# through. Compiling them at -O1 cuts single-TU compile time sharply\n"
       << "# on large modules with no measurable runtime cost.\n"
       << "if(MSVC)\n"
       // /MP is what actually decides wall-clock time here. The Visual Studio
       // generator compiles the files of a single project strictly in
       // sequence, so a module with 100+ generated translation units serialises
       // onto one core no matter what --parallel is passed to `cmake --build`.
       // /MP fans them out across every core. Ninja parallelises on its own and
       // warns about /MP, so gate it on the generator, not just on MSVC.
       // The warning suppressions are the noisy ones the translation
       // unavoidably produces (constant conditionals, provably-taken
       // divide/shift paths); silencing them keeps cl.exe from spending real
       // time formatting hundreds of thousands of diagnostics.
       // /we4189 (from the top-level target's inherited warning-as-error
       // set) turns "unused local" into a hard build failure; the codegen
       // legitimately computes and drops _r on some flag-only paths, so
       // downgrade it back to a warning for generated sources specifically.
       << "  set(_recomp_msvc_opts \"/O1\" \"/wd4127\" \"/wd4723\" \"/wd4102\" \"/wd4101\" "
          "\"/wd4189\")\n"
       << "  if(NOT CMAKE_GENERATOR MATCHES \"Ninja\")\n"
       << "    list(APPEND _recomp_msvc_opts \"/MP\")\n"
       << "  endif()\n"
       << "  set_source_files_properties(${RECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS "
          "\"${_recomp_msvc_opts}\")\n"
       << "else()\n"
       // Overridable: -O1 suits a hybrid image, where the SIMD-heavy blocks stay
       // on the JIT anyway, but a JIT-free image owns those blocks and GCC does
       // not vectorise at all below -O2.
       << "  set(RECOMP_OPT_FLAGS \"-O1 -foptimize-sibling-calls\" CACHE STRING\n"
          "      \"optimisation flags for the generated block bodies\")\n"
       << "  separate_arguments(_recomp_opt NATIVE_COMMAND \"${RECOMP_OPT_FLAGS}\")\n"
       << "  set_source_files_properties(${RECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS "
          "\"${_recomp_opt}\")\n"
       << "endif()\n\n"
       << "if(NOT RECOMP_STATIC_ONLY)\n"
       << "add_executable(recompiled main.c recomp_runtime.c ${RECOMP_SOURCES})\n"
       << "if(EXISTS \"${CMAKE_CURRENT_SOURCE_DIR}/data\")\n"
       << "  add_custom_command(TARGET recompiled POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory"
          " \"${CMAKE_CURRENT_SOURCE_DIR}/data\" \"$<TARGET_FILE_DIR:recompiled>/data\")\n"
       << "endif()\n"
       << "if(SDL2_FOUND)\n"
       << "  target_compile_definitions(recompiled PRIVATE HAVE_SDL2)\n"
       << "  target_include_directories(recompiled PRIVATE ${SDL2_INCLUDE_DIRS})\n"
       << "  target_link_libraries(recompiled ${SDL2_LIBRARIES})\n"
       << "  message(STATUS \"SDL2 found — recompiled will open a game window\")\n"
       << "else()\n"
       << "  message(STATUS \"SDL2 not found — running headless (no window)\")\n"
       << "endif()\n"
       << "# Portable C11: Windows->.exe, Linux/FreeBSD/OpenBSD->ELF, macOS->Mach-O\n"
       << "endif()\n\n";

    // A second target builds the same code as a shared library exporting the
    // block lookup. That is what suyu loads to run this image on
    // Core::ArmRecomp: the emulator supplies memory and services through the
    // host bridge, so main.c - which owns a flat buffer and a stub SVC handler
    // - is deliberately left out of this target.
    //
    // Target/output name is per-module so every NSO's DLL builds side by side
    // without colliding: suyu-cmd's loader (src/suyu_cmd/suyu.cpp) looks for
    // recompiled_rtld.dll / recompiled_image.dll (main) / recompiled_sdk.dll /
    // recompiled_subsdkN.dll next to the exe, in NSO load order.
    const std::string dll_target = (mod == "main") ? "recompiled_image" : ("recompiled_" + mod);
    cm << "if(NOT RECOMP_STATIC_ONLY)\n"
       << "add_library(" << dll_target << " SHARED recomp_export.c recomp_runtime.c "
          "${RECOMP_SOURCES})\n"
       << "set_target_properties(" << dll_target << " PROPERTIES C_VISIBILITY_PRESET hidden "
          "OUTPUT_NAME \"" << dll_target << "\")\n"
       << "target_compile_definitions(" << dll_target
       << " PRIVATE SUYU_HOSTED_RECOMP=1 RECOMP_SHARED_MODULE=1)\n"
       << "endif()\n\n";

    // Static-library variant. Several of these get linked into ONE host
    // executable (the per-game suyu-cmd build), so every symbol a module owns
    // has to be unique. The generated C is written once and renamed at compile
    // time through -D, which keeps the sources identical between the shared and
    // the static shape.
    //
    // recomp_runtime.c is deliberately NOT part of this target: its contents
    // (recomp_svc, the load/store helpers, recomp_cond, ...) are generic and
    // must exist exactly once in the final link. It is built separately, once,
    // as recomp_runtime_shared.
    const std::string static_target = "recomp_static_" + mod;
    cm << "if(NOT TARGET recomp_runtime_shared)\n"
       << "  add_library(recomp_runtime_shared STATIC recomp_runtime.c)\n"
       << "  target_compile_definitions(recomp_runtime_shared PRIVATE SUYU_HOSTED_RECOMP=1 "
          "RECOMP_STATIC_HOST=1)\n"
       << "  target_include_directories(recomp_runtime_shared PUBLIC "
          "${CMAKE_CURRENT_SOURCE_DIR})\n"
       << "endif()\n"
       << "add_library(" << static_target << " STATIC recomp_export.c ${RECOMP_SOURCES})\n"
       << "target_include_directories(" << static_target
       << " PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})\n"
       << "target_link_libraries(" << static_target << " PUBLIC recomp_runtime_shared)\n"
       << "target_compile_definitions(" << static_target
       << " PRIVATE SUYU_HOSTED_RECOMP=1 RECOMP_STATIC_MODULE=1"
       << " g_module_base=g_module_base_" << mod
       << " g_recomp_guard_host_v2=g_recomp_guard_host_v2_" << mod
       << " recomp_image_guard_v2=recomp_image_guard_v2_" << mod
       << " recomp_lookup=recomp_lookup_" << mod
       << " recomp_image_lookup=recomp_image_lookup_" << mod
       << " recomp_image_set_base=recomp_image_set_base_" << mod
       << " recomp_image_entry=recomp_image_entry_" << mod << ")\n";

    std::ostringstream ex;
    ex << "/* auto-generated by suyu static recompiler - DO NOT EDIT */\n"
          "#include \"recomp_runtime.h\"\n\n"
          "/* The one symbol suyu resolves out of a recompiled image. Named\n"
          "   distinctly from the internal recomp_lookup so the export is\n"
          "   unambiguous, and so the internal one can stay hidden. */\n"
          "#ifdef RECOMP_STATIC_MODULE\n"
          "/* Linked straight into the host executable: static-library symbols are\n"
          "   visible to the linker on their own, and the names are already made\n"
          "   unique per module by the -D renames the build applies. */\n"
          "#define RECOMP_API\n"
          "/* The shared runtime is compiled once for the whole link and therefore\n"
          "   cannot own this - each module needs its own load base. */\n"
          "uint64_t g_module_base = 0;\n"
          "#elif defined(_WIN32)\n"
          "#define RECOMP_API __declspec(dllexport)\n"
          "#else\n"
          "#define RECOMP_API __attribute__((visibility(\"default\")))\n"
          "#endif\n\n"
          "RECOMP_API unsigned recomp_image_guard_v2(unsigned host_version){\n"
          "  g_recomp_guard_host_v2=(host_version==2)?2:0; return 2;\n}\n"
          "RECOMP_API BlockFn recomp_image_lookup(uint64_t pc){ return recomp_lookup(pc - g_module_base); }\n\n"
          /* Hands the block index out so a host dispatcher can do the lookup
             itself. Going through recomp_image_lookup costs three nested calls
             across the shared-object boundary on every block edge; with this it
             is a bounds check and one load. Addresses are absolute so the caller
             needs to know nothing about the module base. Only valid once
             recomp_image_set_base has run, which is when the index is built. */
          "extern int _recomp_index_view(uint64_t*, uint64_t*, BlockFn**);\n"
          "RECOMP_API int recomp_image_index(uint64_t* lo, uint64_t* hi, BlockFn** idx){\n"
          "  if(!_recomp_index_view(lo, hi, idx)) return 0;\n"
          "  *lo += g_module_base; *hi += g_module_base;\n"
          "  return 1;\n"
          "}\n"
          "\n"
          "/* Tells this image where its module actually got loaded, so the\n"
          "   addresses it computes are real rather than module-relative. */\n"
          "RECOMP_API void recomp_image_set_base(uint64_t base){ g_module_base = base;\n"
          "  /* Single-threaded here, which is what makes the index safe to\n"
          "     build without locking. */\n"
          "  recomp_build_index(); }\n\n"
          "/* Reports the entry PC so the loader does not have to be told it\n"
          "   separately or parse the image again. */\n"
       << "RECOMP_API uint64_t recomp_image_entry(void){ return 0x"
       << std::hex << (entry_pc ? entry_pc : base) << std::dec << "ULL; }\n";

    auto write = [&](const std::string& name, const std::string& data) {
        std::ofstream o(Utf8Path(out_dir + "/" + name), std::ios::binary);
        o.write(data.data(), (std::streamsize)data.size());
    };
    write("recomp_runtime.h", RuntimeH());
    write("recomp_runtime.c", RuntimeC());
    // The dispatch table is generated code like the block bodies, so it belongs
    // with them rather than at the top level.
    write("src/recompiled_" + mod + ".c", rc);
    // The unit files were streamed out as blocks were translated; just make
    // sure everything has reached disk.
    for (auto& u : units) {
        u.flush();
        u.close();
    }
    write("main.c", mc.str());
    write("recomp_export.c", ex.str());
    write("CMakeLists.txt", cm.str());

    // Coverage report. Without this the exporter cannot describe its own output:
    // block and instruction counts say how much code was walked, not how much of
    // it was actually translated, and an export made entirely of fallbacks looks
    // identical to a complete one. The signature histogram is the actionable
    // part - it ranks which opcode family to implement next by how often it is
    // actually hit, rather than by which gaps look important.
    {
        std::ostringstream cov;
        cov << "{\n";
        cov << "  \"module\": \"" << mod << "\",\n";
        cov << "  \"blocks\": " << stats.blocks << ",\n";
        cov << "  \"text_words\": " << stats.instructions << ",\n";
        cov << "  \"instructions_emitted\": " << stats.emitted << ",\n";
        cov << "  \"instructions_unhandled\": " << stats.unhandled << ",\n";
        cov << "  \"unhandled_fraction\": " << std::fixed << std::setprecision(6)
            << stats.UnhandledFraction() << ",\n";
        cov.unsetf(std::ios::floatfield);

        cov << "  \"unhandled_by_group\": {";
        bool first_group = true;
        for (const auto& [group, count] : stats.unhandled_by_group) {
            if (!first_group) cov << ",";
            first_group = false;
            cov << "\n    \"" << EncodingGroupName(group) << "\": " << count;
        }
        cov << (first_group ? "}" : "\n  }") << ",\n";

        // Ranked, because the only question this file exists to answer is
        // "what should I implement next".
        std::vector<std::pair<u32, UnhandledSite>> ranked(stats.unhandled_by_signature.begin(),
                                                          stats.unhandled_by_signature.end());
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.second.count > b.second.count; });

        cov << "  \"unhandled_by_signature\": [";
        bool first_sig = true;
        size_t shown = 0;
        for (const auto& [sig, site] : ranked) {
            if (shown++ >= 64) break;
            if (!first_sig) cov << ",";
            first_sig = false;
            char line[256];
            snprintf(line, sizeof line,
                     "\n    { \"signature\": \"0x%08X\", \"count\": %zu, "
                     "\"share\": %.4f, \"example_insn\": \"0x%08X\", "
                     "\"example_pc\": \"0x%llX\", \"group\": \"%s\" }",
                     sig, site.count,
                     stats.unhandled ? double(site.count) / double(stats.unhandled) : 0.0,
                     site.example_insn, (unsigned long long)site.example_pc,
                     EncodingGroupName((site.example_insn >> 25) & 0xF));
            cov << line;
        }
        cov << (first_sig ? "]" : "\n  ]") << ",\n";
        cov << "  \"distinct_signatures\": " << stats.unhandled_by_signature.size() << "\n";
        cov << "}\n";
        write("recomp_coverage.json", cov.str());
    }

    // Bundle text/rodata/data as binary blobs so the exe can load them at startup
    {
        std::string data_subdir = out_dir + "/data";
        make_dir(data_subdir);
        // Always write text.bin
        {
            std::ofstream o(Utf8Path(data_subdir + "/text.bin"), std::ios::binary);
            o.write(reinterpret_cast<const char*>(text), (std::streamsize)n_bytes);
        }
        if (rodata && rodata_size > 0) {
            std::ofstream o(Utf8Path(data_subdir + "/rodata.bin"), std::ios::binary);
            o.write(reinterpret_cast<const char*>(rodata), (std::streamsize)rodata_size);
        }
        if (data_seg && data_size > 0) {
            std::ofstream o(Utf8Path(data_subdir + "/data.bin"), std::ios::binary);
            o.write(reinterpret_cast<const char*>(data_seg), (std::streamsize)data_size);
        }
    }

    return stats;
}

inline const char* RuntimeH() {
    return R"RT(#ifndef SUYU_RECOMP_RUNTIME_H
#define SUYU_RECOMP_RUNTIME_H
#include <stdint.h>
#include <stddef.h>   /* offsetof, for the layout assertions below */
#include <string.h>   /* memcpy, for the memory accessors */
#include <stdlib.h>   /* calloc, for the block index

   Included here rather than in the generated module source because the module
   source only pulls in this header and <stdint.h>. Without it calloc is an
   implicit declaration returning int, the returned pointer is truncated to 32
   bits, and the first lookup through the index dereferences garbage - which is
   a crash roughly 20 seconds into boot with nothing in the log to explain it. */

/* The memory accessors below are the hottest code in a generated module, and
   leaving them to the compiler's discretion is not worth the risk: without a
   forced inline MSVC declines them at /O2 in the larger translation units,
   which puts a call back on the path this exists to remove. */
#if defined(_MSC_VER)
#define RECOMP_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define RECOMP_INLINE inline __attribute__((always_inline))
#else
#define RECOMP_INLINE inline
#endif

/* Supplied by the host when the recompiled image is driven by an emulator
   rather than run standalone. `size` is 1, 2, 4 or 8 bytes. */
typedef struct RecompHostMem {
    void* user;
    uint64_t (*load)(void* user, uint64_t va, uint32_t size);
    void (*store)(void* user, uint64_t va, uint32_t size, uint64_t value);
    /* Exclusive access. Appended rather than inserted so the existing two stay
       where they are, but note both sides of this struct must still be rebuilt
       together: a DLL built before these existed would leave them uninitialised
       and the first LDXR would call through a garbage pointer.

       These have to reach the *emulator's* monitor rather than any private one.
       The recompiled code and the fallback JIT run guest threads that contend
       for the same locks, so two monitors means both engines can win the same
       exclusive and the lock stops being a lock. */
    uint64_t (*excl_load)(void* user, uint64_t va, uint32_t size);
    uint32_t (*excl_store)(void* user, uint64_t va, uint32_t size, uint64_t value);
    void (*clear_excl)(void* user);
    /* The physical counter. Unlike FPCR this cannot live in the context: it has
       to advance, and it has to agree with what the HLE kernel and the fallback
       JIT report, or the guest sees time jump backwards whenever execution
       crosses between engines. */
    uint64_t (*read_cntpct)(void* user);
    /* Exclusive pair forms. `size` is the width of ONE register (4 or 8), so
       the reservation covers 2*size bytes. Split out rather than widening
       excl_load/excl_store because the 64-bit pair needs a 128-bit
       reservation, which no scalar return can carry. */
    void (*excl_load_pair)(void* user, uint64_t va, uint32_t size, uint64_t* lo,
                           uint64_t* hi);
    uint32_t (*excl_store_pair)(void* user, uint64_t va, uint32_t size, uint64_t lo,
                                uint64_t hi);
    /* The host page table, so a mapped access resolves inline rather than
       through `load`/`store`. Mirrors Memory::GetPointerImpl's fast path: mask
       the address, bounds check, read one entry, extract the backing pointer.

       An entry whose pointer is null means unmapped, debug, or GPU-tracked
       memory - all of which need the callback, because the rasterizer has to be
       told about the access. Only a non-null pointer is handled inline, so
       nothing is bypassed that suyu would have done itself.

       page_entries is null until the emulator hands over a real page table;
       standalone builds leave it null and take the callback path always. */
    const void* page_entries;
    uint64_t page_entry_stride;
    uint64_t page_bits;
    uint64_t pointer_mask;
    uint64_t address_space_max;
    /* Generation counter for the code guard, owned by the host and shared by
       every core. The host bumps it whenever guest code may have changed: an
       IC IVAU, a cache-range invalidation from another engine, a full icache
       flush, or a new page table. A block re-verifies its bytes only when this
       has moved since that block last checked, instead of on every entry.

       A pointer rather than a value because each core owns its own
       RecompHostMem, and one core invalidating code has to be seen by blocks
       running on all of them.

       Null is allowed and means "no host generation", which makes every block
       verify exactly once - the standalone runtime's situation, where nothing
       can remap or rewrite guest code behind the generated image. */
    const uint64_t* guard_generation;
} RecompHostMem;

typedef struct GuestContext {
    uint64_t x[32]; uint64_t pc; uint8_t n,z,c,v;
    uint8_t* mem; uint64_t mem_size; uint64_t mem_base_vaddr; int halted;
    /* SVC signalling. The emitted code sets this to the instruction's imm
       before calling recomp_svc(). The standalone runtime services the call
       and clears it; when the recompiled code is instead driven by suyu's
       Core::ArmRecomp backend, recomp_svc leaves it set so the emulator can
       see the pending call and dispatch it through the real HLE kernel.
       ~0ULL means "no SVC pending". */
    uint64_t pending_svc;
    /* SIMD/FP register file, 128 bits each held as two 64-bit halves.
       Deliberately placed after pending_svc: Core::ArmRecomp mirrors only the
       prefix of this struct up to that field, so appending here leaves that
       view valid. */
    uint64_t vreg[32][2];
    /* Thread pointer (TPIDR_EL0). Read/written by MRS/MSR; user code uses it
       for thread-local storage. */
    uint64_t tpidr_el0;
    /* Host memory bridge. NULL for the standalone runtime, which owns the flat
       buffer above. When suyu drives this code through Core::ArmRecomp the
       guest's address space belongs to the emulator, not to us, so every
       access has to go back out to Core::Memory instead of into `mem` -
       otherwise the recompiled code and the HLE kernel would be looking at two
       different memories. */
    const struct RecompHostMem* host_mem;
    /* Read-only thread pointer (TPIDRRO_EL0). Distinct from tpidr_el0 above:
       on Horizon the kernel publishes the calling thread's thread-local
       region here, and that region's first 0x100 bytes are the IPC message
       buffer the kernel reads a SendSyncRequest out of. tpidr_el0 in contrast
       belongs to the guest, which points it at its own thread structure.
       Aliasing the two makes the guest write its IPC header at its thread
       struct instead of the TLS region - the kernel then parses an all-zero
       buffer (magic 0 instead of 'SFCI') - and makes the kernel's per-switch
       publish of the TLS address clobber the guest's thread pointer, which
       turns every C++ thread-local access into a near-null read.
       Appended after host_mem so every pinned offset above stays put. */
    uint64_t tpidrro_el0;
    /* Floating-point control and status (FPCR/FPSR).
       Their absence was not a missing feature but a silent wrong answer: the
       guest sets a rounding mode or reads the exception flags, the value had
       nowhere to live, and every FP result afterwards used the host default.
       Worse, the state was dropped again on each transition to the fallback
       JIT, so the two engines disagreed about rounding mid-computation.
       Appended after tpidrro_el0 so every pinned offset above is unchanged. */
    uint64_t fpcr;
    uint64_t fpsr;
    /* Blocks a chain of direct calls may still run before returning to the
       dispatcher. It has to sit immediately after fpsr: Core::ArmRecomp mirrors
       this struct by hand and its view ends here, so a field placed after
       save_dir below lands 512 bytes further along on this side than on that
       one. The layout assertions below are what catch that. */
    int chain_budget;
    /* Save-data filesystem state */
    char save_dir[512];
    /* Heap break for SVC memory allocation */
    uint64_t heap_base; uint64_t heap_end; uint64_t heap_cur;
    /* IPC command buffer (simplified HLE) */
    uint32_t ipc_cmd[64];
    /* Open file handles for save data (simplified) */
    void* save_handles[16]; int save_handle_count;
} GuestContext;

/* Core::ArmRecomp mirrors the prefix of this struct with its own declaration
   so the emulator can read and write guest state without including this
   header. Nothing enforces that across the two builds, so the offsets are
   pinned here: if a field is inserted rather than appended, this fails loudly
   at generation time instead of silently handing the emulator the wrong
   register file. Keep in sync with GuestContextView in
   core/arm/recomp/arm_recomp.cpp. */
/* A negative array size rather than _Static_assert: the generated project is
   plain C compiled by whatever the user has, and MSVC's /TC mode rejects the
   C11 form outright. This construct is valid in every C dialect. */
typedef char recomp_layout_pc[offsetof(GuestContext, pc) == 256 ? 1 : -1];
typedef char recomp_layout_svc[offsetof(GuestContext, pending_svc) == 304 ? 1 : -1];
typedef char recomp_layout_vreg[offsetof(GuestContext, vreg) == 312 ? 1 : -1];
typedef char recomp_layout_tpidr[offsetof(GuestContext, tpidr_el0) == 824 ? 1 : -1];
typedef char recomp_layout_fpcr[offsetof(GuestContext, fpcr) == 848 ? 1 : -1];
typedef char recomp_layout_fpsr[offsetof(GuestContext, fpsr) == 856 ? 1 : -1];
typedef char recomp_layout_host[offsetof(GuestContext, host_mem) == 832 ? 1 : -1];
typedef char recomp_layout_chain[offsetof(GuestContext, chain_budget) == 864 ? 1 : -1];
typedef char recomp_layout_tpidrro[offsetof(GuestContext, tpidrro_el0) == 840 ? 1 : -1];

/* Where this module is actually loaded in the guest's address space.
   Every address the static pass bakes in - ADR/ADRP results, branch targets,
   return addresses - is relative to the module, because that is all it can
   know: the loader picks the real base at run time and it differs per run.
   Leaving it zero gives the module-relative behaviour the standalone runtime
   wants; the emulator sets it once at load so computed data pointers land in
   real memory rather than near null. One global per image, since each module
   is its own shared library. */
extern uint64_t g_module_base;
extern int g_recomp_guard_host_v2;
void recomp_code_guard(GuestContext*,uint64_t,const uint32_t*,uint32_t,int);
void recomp_ic_ivau(GuestContext*,uint64_t,int);

typedef void (*BlockFn)(GuestContext*);
BlockFn recomp_lookup(uint64_t pc); void recomp_run(GuestContext* c);
/* Builds the direct block index. Called once at load, before any
   guest thread runs. */
void recomp_build_index(void);
void recomp_set_flags(GuestContext*,int,uint64_t,uint64_t,uint64_t,int);
/* High 64 bits of a 64x64 multiply, for SMULH/UMULH. */
uint64_t recomp_smulh(uint64_t,uint64_t);
uint64_t recomp_umulh(uint64_t,uint64_t);
uint64_t recomp_fp_estimate(GuestContext*,uint64_t,unsigned,int);
int  recomp_cond(GuestContext*,unsigned);
/* Guest memory access.

   Defined once per module in recomp_runtime.c rather than inlined here. They
   resolve the address through the host page table the same way
   Memory::GetPointerImpl does, falling back to the emulator callback for
   unmapped, debug or GPU-tracked pages.

   Forcing them inline at every access site was measured and was worse: the
   generated main image went from 100 MB to 222 MB and the race-phase frame rate
   dropped 14%. Whatever the call costs, the instruction cache costs more. */
uint64_t recomp_load8(GuestContext*,uint64_t); uint64_t recomp_load16(GuestContext*,uint64_t);
uint64_t recomp_load32(GuestContext*,uint64_t); uint64_t recomp_load64(GuestContext*,uint64_t);
void recomp_store8(GuestContext*,uint64_t,uint64_t); void recomp_store16(GuestContext*,uint64_t,uint64_t);
void recomp_store32(GuestContext*,uint64_t,uint64_t); void recomp_store64(GuestContext*,uint64_t,uint64_t);
/* Pair forms, so LDP and STP resolve one address instead of two. */
void recomp_ldp32(GuestContext*,uint64_t,uint64_t*,uint64_t*);
void recomp_ldp64(GuestContext*,uint64_t,uint64_t*,uint64_t*);
void recomp_stp32(GuestContext*,uint64_t,uint64_t,uint64_t);
void recomp_stp64(GuestContext*,uint64_t,uint64_t,uint64_t);
void recomp_svc(GuestContext*,unsigned); void recomp_unhandled(GuestContext*,uint32_t,uint64_t);
void recomp_barrier(void);
/* AES S-box, forward or inverse. Built on first call. */
const uint8_t* recomp_aes_sbox(int inverse);
uint64_t recomp_fp_estimate(GuestContext*,uint64_t,unsigned,int);
/* Load-exclusive: marks the address and returns its contents.
   Store-exclusive: returns 0 on success and 1 if the mark was lost, which is
   the sense of the status register STXR writes (0 = stored). */
uint64_t recomp_ldxr(GuestContext* c, uint64_t addr, uint32_t size);
uint32_t recomp_stxr(GuestContext* c, uint64_t addr, uint32_t size, uint64_t value);
void recomp_clrex(GuestContext* c);
/* CNTPCT_EL0: the 19.2 MHz physical counter the guest reads for timing. */
uint64_t recomp_cntpct(GuestContext* c);
/* Exclusive pair forms. `size` is the width of one register, 4 or 8. */
void recomp_ldxp(GuestContext* c, uint64_t addr, uint32_t size, uint64_t* lo, uint64_t* hi);
uint32_t recomp_stxp(GuestContext* c, uint64_t addr, uint32_t size, uint64_t lo, uint64_t hi);
/* halted values. 1 is an ordinary stop; 2 asks the host to re-execute the
   current PC on its interpreter fallback because the decoder had no
   translation for the instruction there. */
#define RECOMP_HALT_UNHANDLED 2
/* A guest breakpoint, with PC parked on BRK. Never invokes fallback. */
#define RECOMP_HALT_BREAKPOINT 3
#define RECOMP_HALT_IC_IVAU 4
/* Save-data API callable from recompiled code and runtime */
int  recomp_save_init(GuestContext* c, const char* exe_path);
int  recomp_save_write(GuestContext* c, const char* name, const void* data, uint64_t size);
int  recomp_save_read(GuestContext* c, const char* name, void* buf, uint64_t buf_size, uint64_t* out_size);
int  recomp_save_delete(GuestContext* c, const char* name);
int  recomp_save_exists(GuestContext* c, const char* name);
/* Load bundled data segments into guest memory */
int  recomp_load_segments(GuestContext* c, const char* data_dir);
#endif
)RT";
}

inline const char* EstimateRuntimeC() {
    return R"EST(/* Isolated Armv8 S/D reciprocal-estimate prototype. Integer-only lane core.
 * Exception flags accumulate in *fpsr. Exception traps are not delivered here.
 * Interfaces deliberately do not depend on GuestContext or shared emitter code.
 */

uint64_t recomp_fp_estimate(GuestContext* c,uint64_t bits,unsigned width,int rsqrt){
 uint32_t fpcr=(uint32_t)c->fpcr;
 uint64_t *fpsr=&c->fpsr;
 const unsigned fracbits=width==32?23:52;
 const unsigned expbits=width==32?8:11;
 const int bias=width==32?127:1023,emin=1-bias;
 const uint64_t hidden=UINT64_C(1)<<fracbits;
 const uint64_t fracmask=hidden-1,expmask=((UINT64_C(1)<<expbits)-1)<<fracbits;
 const uint64_t sign=bits&(UINT64_C(1)<<(width-1));
 const uint64_t quiet=hidden>>1,defnan=expmask|quiet;
 uint64_t frac=bits&fracmask;
 unsigned rawexp=(unsigned)((bits&expmask)>>fracbits);
 if(rawexp==((1u<<expbits)-1)&&frac){
  if(!(frac&quiet))*fpsr|=1;
  return(fpcr&(1u<<25))?defnan:(bits|quiet);
 }
 if(!rawexp&&frac&&(fpcr&(1u<<24))){*fpsr|=128;frac=0;}
 if(!rawexp&&!frac){*fpsr|=2;return sign|expmask;}
 if(rsqrt&&sign){*fpsr|=1;return defnan;}
 if(rawexp==((1u<<expbits)-1))return sign;
 int exponent=rawexp?(int)rawexp-bias:emin;
 uint64_t mant=rawexp?(hidden|frac):frac;
 while(mant<hidden){mant<<=1;exponent--;}
 if(!rsqrt){
  if(exponent<emin-2){
   unsigned rm=(fpcr>>22)&3;
   int inf=rm==0||(rm==1&&!sign)||(rm==2&&sign);
   *fpsr|=4|16;return sign|(inf?expmask:(expmask-1));
  }
  if((fpcr&(1u<<24))&&exponent>=-emin){*fpsr|=8;return sign;}
  uint64_t scaled=mant>>(fracbits-8);
  uint64_t estimate=(((UINT64_C(1)<<19)/(2*scaled+1)+1)/2)&255;
  int out_exp=-exponent-1;
  uint64_t sig=(256+estimate)<<(fracbits-8);
  if(out_exp<emin)return sign|(sig>>(emin-out_exp));
  return sign|((uint64_t)(out_exp+bias)<<fracbits)|(estimate<<(fracbits-8));
 }
 uint64_t a=mant>>(fracbits-((exponent%2==0)?7:8));
 a=a<256?2*a+1:2*(a|1);
 unsigned lo=512,hi=1024;
 while(lo+1<hi){unsigned mid=(lo+hi)/2;if(a*mid*mid<(UINT64_C(1)<<28))lo=mid;else hi=mid;}
 uint64_t estimate=((lo+1)/2)&255;
 int neg=-exponent-1;
 int out_exp=neg>=0?neg/2:-((-neg+1)/2);
 return ((uint64_t)(out_exp+bias)<<fracbits)|(estimate<<(fracbits-8));
}

)EST";
}

inline const char* RuntimeC() {
    // MSVC caps one string literal at 16380 bytes (C2026) and this runtime is
    // past that, so it is assembled from two pieces at first use rather than
    // being a single literal. Adjacent-literal concatenation would not help:
    // the limit applies to the result as well.
    static const std::string text = std::string(R"RT(#include "recomp_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* clock()/CLOCKS_PER_SEC for the standalone recomp_cntpct fallback. */
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define MKDIR(p) _mkdir(p)
#define PATH_SEP '\\'
#else
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#define MKDIR(p) mkdir(p,0755)
#define PATH_SEP '/'
#endif

/* AES S-box, built once on first use.
   S(x) = affine(x^-1), and x^-1 is x^254 because x^255 == 1 for non-zero x.
   Generated rather than tabulated: two 256-byte tables written out as source
   would be 512 bytes of literal in a runtime already split to stay under
   MSVC's 16380-byte cap. Checked against FIPS 197 and for round-trip. */
static uint8_t recomp_gmul(uint8_t a, uint8_t b){
  uint8_t p=0;
  while(b){ if(b&1) p^=a; a=(uint8_t)((a<<1)^((a>>7)*0x1B)); b=(uint8_t)(b>>1); }
  return p;
}
static uint8_t recomp_ginv(uint8_t x){
  uint8_t p=1; int b;
  if(!x) return 0;
  for(b=7;b>=0;b--){ p=recomp_gmul(p,p); if((254>>b)&1) p=recomp_gmul(p,x); }
  return p;
}
static uint8_t recomp_rotl8(uint8_t x,int n){ return (uint8_t)((x<<n)|(x>>(8-n))); }
const uint8_t* recomp_aes_sbox(int inverse){
  static uint8_t fwd[256], inv[256];
  static int built = 0;
  if(!built){
    int i;
    for(i=0;i<256;i++){
      uint8_t b = recomp_ginv((uint8_t)i);
      fwd[i] = (uint8_t)(b ^ recomp_rotl8(b,1) ^ recomp_rotl8(b,2) ^ recomp_rotl8(b,3)
                           ^ recomp_rotl8(b,4) ^ 0x63);
    }
    for(i=0;i<256;i++){
      uint8_t b = (uint8_t)(recomp_rotl8((uint8_t)i,1) ^ recomp_rotl8((uint8_t)i,3)
                            ^ recomp_rotl8((uint8_t)i,6) ^ 0x05);
      inv[i] = recomp_ginv(b);
    }
    built = 1;
  }
  return inverse ? inv : fwd;
}
static uint8_t* memptr(GuestContext* c, uint64_t va, uint64_t sz){
  uint64_t off=va-c->mem_base_vaddr;
  if(off+sz>c->mem_size) return 0;
  return c->mem+off;
}

/* Every guest access funnels through these two, so the host bridge only has to
   be checked in one place per direction. */
static uint64_t memload(GuestContext* c, uint64_t a, uint32_t sz){
  if(c->host_mem) return c->host_mem->load(c->host_mem->user,a,sz);
  { uint8_t* p=memptr(c,a,sz); uint64_t v=0; if(p)memcpy(&v,p,sz); return v; }
}
static void memstore(GuestContext* c, uint64_t a, uint32_t sz, uint64_t v){
  if(c->host_mem){ c->host_mem->store(c->host_mem->user,a,sz,v); return; }
  { uint8_t* p=memptr(c,a,sz); if(p)memcpy(p,&v,sz); }
}

static unsigned char* recomp_host_ptr(GuestContext* c, uint64_t va);
static int recomp_scalar_same_page(const RecompHostMem* hm, uint64_t a, uint64_t bytes);

/* Every generated block checks its compilation input before any guest effect.
   This guards synchronized instruction fetch, not unsynchronized concurrent
   mutation of an already executing block. Failure must never enter a JIT.

   This runs on entry to every block, so the per-word loop below costs one
   indirect host callback per guest instruction before any guest work happens -
   the largest single cost in the profile. When the whole block lies inside one
   mapped page with a real backing pointer, the identical comparison is one
   memcmp, so take that. It is a faster spelling of the same check, not a weaker
   one: the same words are compared, and every case the fast path cannot prove
   safe (unmapped, debug or GPU-tracked memory, a page crossing, an address
   outside the space, a mismatch) falls through to the loop, which is also what
   produces the precise diagnostic naming the offending word. */
void recomp_code_guard(GuestContext* c,uint64_t pc,const uint32_t* expected,uint32_t count,int host_guard_version){
  uint32_t k;
  if(c->host_mem && host_guard_version!=2){
    c->pc=pc;
    fprintf(stderr,"[recomp] code guard requires a guard-v2 host and guarded modules at 0x%llx\n",(unsigned long long)pc);
    fflush(stderr); abort();
  }
  if(c->host_mem && count && recomp_scalar_same_page(c->host_mem,pc,(uint64_t)count*4)){
    const unsigned char* p=recomp_host_ptr(c,pc);
    if(p && memcmp(p,expected,(size_t)count*4)==0) return;
  }
  for(k=0;k<count;++k){
    uint64_t va=pc+(uint64_t)k*4, actual=0;
    int valid=va>=pc;
    if(c->host_mem){
      /* Guard-v2 reserved load size 0: bit32 is explicit mapping validity,
         independently of the low32 instruction word (which may be zero). */
      if(valid){
        actual=c->host_mem->load(c->host_mem->user,va,0);
        valid=(actual&UINT64_C(0x100000000))!=0;
      }
    }else{
      uint64_t off=va-c->mem_base_vaddr;
      valid=valid && c->mem && va>=c->mem_base_vaddr && off<=c->mem_size && c->mem_size-off>=4;
      if(valid) memcpy(&actual,c->mem+off,4);
    }
    if(!valid || (uint32_t)actual!=expected[k]){
      c->pc=va;
      fprintf(stderr,"[recomp] unsupported code change or unavailable code at 0x%llx (expected %08x, read %08x)\n",
              (unsigned long long)va,expected[k],(unsigned)(uint32_t)actual);
      fflush(stderr); abort();
    }
  }
}
void recomp_ic_ivau(GuestContext* c,uint64_t address,int host_guard_version){
  recomp_barrier();
  if(c->host_mem){
    if(host_guard_version!=2){
      fprintf(stderr,"[recomp] IC IVAU requires a guard-v2 host and guarded modules; refusing at 0x%llx\n",(unsigned long long)c->pc);
      fflush(stderr); abort();
    }
    c->pending_svc=address&~UINT64_C(63); /* payload tagged by halted, never an SVC */
    c->halted=RECOMP_HALT_IC_IVAU;
  }else{
    c->pending_svc=~UINT64_C(0);
    c->pc+=4; /* no fallback cache; the next block still verifies its bytes */
  }
}

/* Exclusive access.

   Hosted, these go straight to the emulator's exclusive monitor - the same one
   the fallback JIT uses - so a lock taken by recompiled code is visible to a
   thread running on the JIT and vice versa.

   Standalone there is exactly one guest thread and nothing else can touch the
   address, so a single-slot address latch is not an approximation: it gives the
   same answers a real monitor would. It still tracks the address rather than
   always succeeding, so code that deliberately fails an STXR (the usual
   compare-and-swap retry loop) still behaves. */
static uint64_t g_excl_addr = 0;
static int g_excl_valid = 0;

uint64_t recomp_ldxr(GuestContext* c, uint64_t addr, uint32_t size){
  if(c->host_mem && c->host_mem->excl_load) return c->host_mem->excl_load(c->host_mem->user,addr,size);
  g_excl_addr = addr; g_excl_valid = 1;
  return memload(c,addr,size);
}

uint32_t recomp_stxr(GuestContext* c, uint64_t addr, uint32_t size, uint64_t value){
  if(c->host_mem && c->host_mem->excl_store) return c->host_mem->excl_store(c->host_mem->user,addr,size,value);
  if(!g_excl_valid || g_excl_addr != addr) return 1;
  g_excl_valid = 0;
  memstore(c,addr,size,value);
  return 0;
}

void recomp_ldxp(GuestContext* c, uint64_t addr, uint32_t size, uint64_t* lo, uint64_t* hi){
  if(c->host_mem && c->host_mem->excl_load_pair){
    c->host_mem->excl_load_pair(c->host_mem->user,addr,size,lo,hi); return;
  }
  g_excl_addr = addr; g_excl_valid = 1;
  *lo = memload(c,addr,size);
  *hi = memload(c,addr+size,size);
}

uint32_t recomp_stxp(GuestContext* c, uint64_t addr, uint32_t size, uint64_t lo, uint64_t hi){
  if(c->host_mem && c->host_mem->excl_store_pair)
    return c->host_mem->excl_store_pair(c->host_mem->user,addr,size,lo,hi);
  if(!g_excl_valid || g_excl_addr != addr) return 1;
  g_excl_valid = 0;
  memstore(c,addr,size,lo);
  memstore(c,addr+size,size,hi);
  return 0;
}

void recomp_clrex(GuestContext* c){
  if(c->host_mem && c->host_mem->clear_excl){ c->host_mem->clear_excl(c->host_mem->user); return; }
  g_excl_valid = 0;
}

uint64_t recomp_cntpct(GuestContext* c){
  if(c->host_mem && c->host_mem->read_cntpct) return c->host_mem->read_cntpct(c->host_mem->user);
  /* Standalone: derive from the host clock at the guest's 19.2 MHz rate. A
     free-running counter would satisfy code that only measures deltas, but
     anything converting ticks to seconds would then be wrong by whatever the
     host happens to run at. */
  {
    static uint64_t base_ns = 0;
    uint64_t now_ns = (uint64_t)(clock() * (1000000000ULL / CLOCKS_PER_SEC));
    if(!base_ns) base_ns = now_ns;
    return ((now_ns - base_ns) / 1000ULL) * 19200ULL / 1000ULL;
  }
}

/* Resolve a guest address to a host pointer the way Memory::GetPointerImpl
   does: mask, bounds check, one page-table entry, extract the backing pointer.
   A null result means unmapped, debug, or GPU-tracked memory, all of which have
   to go through the emulator callback so the rasterizer is told about the
   access. Only a real backing pointer is handled here.

   Deliberately not inlined into the generated code. Forcing it inline at every
   access site was measured: main.dll went from 100 MB to 222 MB and the race
   phase lost 14%, so whatever the call cost, the instruction cache cost more. */
static unsigned char* recomp_host_ptr(GuestContext* c, uint64_t va){
  const RecompHostMem* hm = c->host_mem;
  uintptr_t raw, p;
  if(!hm || !hm->page_entries) return 0;
  va &= 0xffffffffffffULL;                 /* AArch64 ignores the top 16 bits */
  if(va >= hm->address_space_max) return 0;
  raw = *(const uintptr_t*)((const unsigned char*)hm->page_entries
                            + (va >> hm->page_bits) * hm->page_entry_stride);
  p = raw & (uintptr_t)hm->pointer_mask;
  return p ? (unsigned char*)(p + (uintptr_t)va) : 0;
}

/* Direct scalar access must remain inside one guest page and the address space.
   Crossing accesses use the host callback, which resolves each guest page. */
static int recomp_scalar_same_page(const RecompHostMem* hm, uint64_t a, uint64_t bytes){
  uint64_t psz;
  if(!hm || !hm->page_entries || hm->page_bits >= 64) return 0;
  a &= 0xffffffffffffULL;
  if(a >= hm->address_space_max || bytes > hm->address_space_max - a) return 0;
  psz = (uint64_t)1 << hm->page_bits;
  return bytes <= psz - (a & (psz - 1));
}

uint64_t recomp_load8 (GuestContext* c,uint64_t a){
  unsigned char* p=recomp_host_ptr(c,a); if(p) return (uint64_t)*p; return memload(c,a,1);}
uint64_t recomp_load16(GuestContext* c,uint64_t a){
  unsigned char* p=recomp_scalar_same_page(c->host_mem,a,2)?recomp_host_ptr(c,a):0; if(p){uint16_t v;memcpy(&v,p,2);return (uint64_t)v;} return memload(c,a,2);}
uint64_t recomp_load32(GuestContext* c,uint64_t a){
  unsigned char* p=recomp_scalar_same_page(c->host_mem,a,4)?recomp_host_ptr(c,a):0; if(p){uint32_t v;memcpy(&v,p,4);return (uint64_t)v;} return memload(c,a,4);}
uint64_t recomp_load64(GuestContext* c,uint64_t a){
  unsigned char* p=recomp_scalar_same_page(c->host_mem,a,8)?recomp_host_ptr(c,a):0; if(p){uint64_t v;memcpy(&v,p,8);return v;} return memload(c,a,8);}
void recomp_store8 (GuestContext* c,uint64_t a,uint64_t v){
  unsigned char* p=recomp_host_ptr(c,a); if(p){*p=(unsigned char)v;return;} memstore(c,a,1,v);}
void recomp_store16(GuestContext* c,uint64_t a,uint64_t v){
  unsigned char* p=recomp_scalar_same_page(c->host_mem,a,2)?recomp_host_ptr(c,a):0; if(p){uint16_t t=(uint16_t)v;memcpy(p,&t,2);return;} memstore(c,a,2,v);}
void recomp_store32(GuestContext* c,uint64_t a,uint64_t v){
  unsigned char* p=recomp_scalar_same_page(c->host_mem,a,4)?recomp_host_ptr(c,a):0; if(p){uint32_t t=(uint32_t)v;memcpy(p,&t,4);return;} memstore(c,a,4,v);}
void recomp_store64(GuestContext* c,uint64_t a,uint64_t v){
  unsigned char* p=recomp_scalar_same_page(c->host_mem,a,8)?recomp_host_ptr(c,a):0; if(p){memcpy(p,&v,8);return;} memstore(c,a,8,v);}

/* Pair access. LDP and STP open and close every non-leaf function, which makes
   them the most frequent guest memory operations there are, and as two separate
   calls they walked the page table twice for one address. One walk covers both
   words whenever the second does not cross out of the page; when it does, or
   when the page is not plain backed memory, the single-word helpers answer
   exactly as before - so nothing the emulator would have been told about an
   access is skipped. */
static int recomp_pair_same_page(const RecompHostMem* hm, uint64_t a, uint64_t bytes){
  uint64_t psz;
  if(!hm || !hm->page_entries) return 0;
  psz = (uint64_t)1 << hm->page_bits;
  return (a & (psz - 1)) + bytes <= psz;
}
void recomp_ldp64(GuestContext* c,uint64_t a,uint64_t* lo,uint64_t* hi){
  if(recomp_pair_same_page(c->host_mem,a,16)){
    unsigned char* p=recomp_host_ptr(c,a);
    if(p){ memcpy(lo,p,8); memcpy(hi,p+8,8); return; }
  }
  *lo=recomp_load64(c,a); *hi=recomp_load64(c,a+8);
}
void recomp_stp64(GuestContext* c,uint64_t a,uint64_t v0,uint64_t v1){
  if(recomp_pair_same_page(c->host_mem,a,16)){
    unsigned char* p=recomp_host_ptr(c,a);
    if(p){ memcpy(p,&v0,8); memcpy(p+8,&v1,8); return; }
  }
  recomp_store64(c,a,v0); recomp_store64(c,a+8,v1);
}
void recomp_ldp32(GuestContext* c,uint64_t a,uint64_t* lo,uint64_t* hi){
  if(recomp_pair_same_page(c->host_mem,a,8)){
    unsigned char* p=recomp_host_ptr(c,a);
    if(p){ uint32_t x,y; memcpy(&x,p,4); memcpy(&y,p+4,4);
           *lo=(uint64_t)x; *hi=(uint64_t)y; return; }
  }
  *lo=recomp_load32(c,a); *hi=recomp_load32(c,a+4);
}
void recomp_stp32(GuestContext* c,uint64_t a,uint64_t v0,uint64_t v1){
  if(recomp_pair_same_page(c->host_mem,a,8)){
    unsigned char* p=recomp_host_ptr(c,a);
    if(p){ uint32_t x=(uint32_t)v0,y=(uint32_t)v1;
           memcpy(p,&x,4); memcpy(p+4,&y,4); return; }
  }
  recomp_store32(c,a,v0); recomp_store32(c,a+4,v1);
}

#ifndef RECOMP_STATIC_HOST
/* Owned by the runtime in the single-module shapes (standalone exe, loadable
   shared image). When several modules are linked statically into one host this
   file is compiled once for all of them, so each module defines its own
   (renamed) copy in recomp_export.c instead. */
uint64_t g_module_base = 0;
#endif

uint64_t recomp_umulh(uint64_t a,uint64_t b){
  /* Portable 64x64->high64: split into 32-bit halves. Avoids depending on
     __int128 or MSVC intrinsics so the generated project stays plain C11. */
  uint64_t al=a&0xFFFFFFFFULL, ah=a>>32, bl=b&0xFFFFFFFFULL, bh=b>>32;
  uint64_t ll=al*bl, lh=al*bh, hl=ah*bl, hh=ah*bh;
  uint64_t mid=(ll>>32)+(lh&0xFFFFFFFFULL)+(hl&0xFFFFFFFFULL);
  return hh+(lh>>32)+(hl>>32)+(mid>>32);
}
uint64_t recomp_smulh(uint64_t a,uint64_t b){
  uint64_t hi=recomp_umulh(a,b);
  /* Convert the unsigned high half to the signed one. */
  if((int64_t)a<0) hi-=b;
  if((int64_t)b<0) hi-=a;
  return hi;
}
void recomp_set_flags(GuestContext* c,int is_sub,uint64_t a,uint64_t b,uint64_t r,int is64){
  uint64_t m=is64?~0ULL:0xFFFFFFFFULL; r&=m;a&=m;b&=m;
  uint64_t s=is64?0x8000000000000000ULL:0x80000000ULL;
  c->z=(r==0); c->n=(r&s)?1:0;
  if(is_sub){ c->c=(a>=b); c->v=(((a^b)&(a^r))&s)?1:0; }
  else { c->c=(r<a); c->v=((~(a^b)&(a^r))&s)?1:0; }
}

int recomp_cond(GuestContext* c,unsigned cond){
  int n=c->n,z=c->z,cc=c->c,v=c->v,res;
  switch(cond>>1){case 0:res=z;break;case 1:res=cc;break;case 2:res=n;break;case 3:res=v;break;
   case 4:res=cc&&!z;break;case 5:res=(n==v);break;case 6:res=(n==v)&&!z;break;default:res=1;}
  return ((cond&1)&&cond!=15)? !res:res;
}

/* ── Save-data filesystem ── */

static void mkpath(const char* path) {
  char tmp[512]; size_t len;
  snprintf(tmp,sizeof tmp,"%s",path); len=strlen(tmp);
  for(size_t i=1;i<len;i++){
    if(tmp[i]==PATH_SEP||tmp[i]=='/'){tmp[i]=0; MKDIR(tmp); tmp[i]=PATH_SEP;}
  }
  MKDIR(tmp);
}

int recomp_save_init(GuestContext* c, const char* exe_path) {
  char dir[512];
  /* Put save_data/ next to the executable */
  snprintf(dir,sizeof dir,"%s",exe_path);
  char* sl=strrchr(dir,PATH_SEP);
  if(!sl) sl=strrchr(dir,'/');
  if(sl) *(sl+1)=0; else dir[0]=0;
  snprintf(c->save_dir,sizeof c->save_dir,"%ssave_data",dir);
  mkpath(c->save_dir);
  printf("[recomp] Save directory: %s\n",c->save_dir);
  return 1;
}

int recomp_save_write(GuestContext* c, const char* name, const void* data, uint64_t size) {
  char path[1024];
  snprintf(path,sizeof path,"%s%c%s",c->save_dir,PATH_SEP,name);
  /* Ensure parent dirs exist */
  char parent[1024]; snprintf(parent,sizeof parent,"%s",path);
  char* sl=strrchr(parent,PATH_SEP); if(!sl) sl=strrchr(parent,'/'); if(sl)*sl=0;
)RT") + R"RT(  mkpath(parent);
  FILE* f=fopen(path,"wb");
  if(!f){fprintf(stderr,"[recomp] save write failed: %s\n",path); return 0;}
  fwrite(data,1,(size_t)size,f); fclose(f);
  printf("[recomp] Saved %llu bytes -> %s\n",(unsigned long long)size,path);
  return 1;
}

int recomp_save_read(GuestContext* c, const char* name, void* buf, uint64_t buf_size, uint64_t* out_size) {
  char path[1024];
  snprintf(path,sizeof path,"%s%c%s",c->save_dir,PATH_SEP,name);
  FILE* f=fopen(path,"rb");
  if(!f){if(out_size)*out_size=0; return 0;}
  fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
  uint64_t to_read=(uint64_t)sz<buf_size?(uint64_t)sz:buf_size;
  fread(buf,1,(size_t)to_read,f); fclose(f);
  if(out_size)*out_size=to_read;
  printf("[recomp] Loaded %llu bytes <- %s\n",(unsigned long long)to_read,path);
  return 1;
}

int recomp_save_delete(GuestContext* c, const char* name) {
  char path[1024];
  snprintf(path,sizeof path,"%s%c%s",c->save_dir,PATH_SEP,name);
  return remove(path)==0;
}

int recomp_save_exists(GuestContext* c, const char* name) {
  char path[1024];
  snprintf(path,sizeof path,"%s%c%s",c->save_dir,PATH_SEP,name);
  FILE* f=fopen(path,"rb");
  if(f){fclose(f); return 1;} return 0;
}

/* ── Segment loader: loads rodata.bin + data.bin from the data dir into guest memory ── */

int recomp_load_segments(GuestContext* c, const char* data_dir) {
  const char* names[]={"rodata.bin","data.bin","text.bin"};
  /* Corresponding offsets from segment info embedded in manifest — for now, load
     sequentially after .text in memory. The real offsets come from the blockmap. */
  for(int i=0;i<3;i++){
    char path[1024];
    snprintf(path,sizeof path,"%s%c%s",data_dir,PATH_SEP,names[i]);
    FILE* f=fopen(path,"rb");
    if(!f) continue;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if((uint64_t)sz<=c->mem_size){
      /* Load at the appropriate offset — text at base, others after */
      fread(c->mem,1,(size_t)sz,f);
    }
    fclose(f);
    printf("[recomp] Loaded segment %s (%ld bytes)\n",names[i],sz);
  }
  return 1;
}

/* ── SVC handler with HLE filesystem support ── */

#ifdef SUYU_HOSTED_RECOMP
/* In the suyu-hosted build the block already set pending_svc before calling
   this. Just return — arm_recomp.cpp's run loop detects pending_svc != ~0 and
   dispatches to the real HLE kernel. */
void recomp_svc(GuestContext* c,unsigned imm){ (void)c; (void)imm; }
#else
void recomp_svc(GuestContext* c,unsigned imm){
  /* Standalone runtime: this handler services the call itself, so clear the
     pending flag. The suyu-hosted build replaces this translation unit with a
     bridge that leaves pending_svc set and returns, handing the call to the
     real HLE kernel instead. */
  c->pending_svc = ~0ULL;
  switch(imm){
  case 0x1: /* SetHeapSize — x1 = requested size */
    if(c->heap_base==0){
      c->heap_base=c->mem_base_vaddr+c->mem_size/2;
      c->heap_cur=c->heap_base;
      c->heap_end=c->heap_base+c->mem_size/2;
    }
    c->x[0]=0; /* success */
    c->x[1]=c->heap_base;
    break;
  case 0x2: /* SetMemoryPermission — stub success */
    c->x[0]=0;
    break;
  case 0x3: /* SetMemoryAttribute — stub success */
    c->x[0]=0;
    break;
  case 0x6: /* QueryMemory — stub: report all memory as readable/writable */
    c->x[0]=0;
    c->x[1]=0; /* MemoryInfo written to [x0] — simplified */
    break;
  case 0x7: /* ExitProcess */
    printf("[recomp] ExitProcess called\n");
    c->halted=1;
    break;
  case 0x8: /* CreateThread — stub, return handle=1 */
    c->x[0]=0; c->x[1]=1;
    break;
  case 0xB: /* SleepThread — yield CPU to prevent spin-lock lag */
#ifdef _WIN32
    Sleep((DWORD)(c->x[0] / 1000000ULL)); /* ns to ms */
#else
    { struct timespec ts; ts.tv_sec=0; ts.tv_nsec=(long)(c->x[0]>0?c->x[0]:1000000);
      nanosleep(&ts,0); }
#endif
    c->x[0]=0;
    break;
  case 0x15: /* SendSyncRequest — IPC for fsp-srv / save data */
    /* Simplified HLE: check x[0] for handle, interpret IPC command buffer.
       For now, stub success so game save code paths don't crash. */
    c->x[0]=0;
    break;
  case 0x16: /* SendSyncRequestWithUserBuffer */
    c->x[0]=0;
    break;
  case 0x18: /* CloseHandle — stub */
    c->x[0]=0;
    break;
  case 0x1A: /* WaitSynchronization — stub immediate return */
    c->x[0]=0; c->x[1]=0;
    break;
  case 0x1F: /* ConnectToNamedPort — stub, return handle */
    c->x[0]=0; c->x[1]=0x100;
    break;
  case 0x21: /* SendSyncRequest (sm: variant) */
    c->x[0]=0;
    break;
  case 0x26: /* Break — debug break */
    printf("[recomp] Break SVC x0=%llu\n",(unsigned long long)c->x[0]);
    break;
  case 0x27: /* OutputDebugString */
    { uint8_t* p=memptr(c,c->x[0],(uint64_t)c->x[1]);
      if(p) printf("[guest] %.*s\n",(int)c->x[1],(char*)p);
      c->x[0]=0;
    }
    break;
  case 0x29: /* GetInfo — return stub values for system info queries */
    { uint32_t id=(uint32_t)c->x[1];
      switch(id){
      case 0: c->x[1]=0xFFFFFF; break; /* AllowedCPUCoreMask */
      case 1: c->x[1]=0xF; break; /* AllowedThreadPrioMask */
      case 2: c->x[1]=c->mem_base_vaddr; break; /* MapRegionBaseAddr */
      case 3: c->x[1]=c->mem_size; break; /* MapRegionSize */
      case 4: c->x[1]=c->heap_base; break; /* HeapRegionBaseAddr */
      case 5: c->x[1]=c->heap_end-c->heap_base; break; /* HeapRegionSize */
      case 6: c->x[1]=c->mem_size; break; /* TotalMemorySize */
      case 7: c->x[1]=c->mem_size/2; break; /* UsedMemorySize */
      case 12: c->x[1]=c->mem_base_vaddr+c->mem_size; break; /* AslrRegionBaseAddr */
      case 13: c->x[1]=0x1000000; break; /* AslrRegionSize */
      case 14: c->x[1]=c->mem_base_vaddr+c->mem_size; break; /* StackRegionBaseAddr */
      case 15: c->x[1]=0x100000; break; /* StackRegionSize */
      default: c->x[1]=0; break;
      }
      c->x[0]=0;
    }
    break;
  default:
    printf("[recomp] Unhandled SVC #0x%x x0=0x%llx x1=0x%llx\n",imm,
      (unsigned long long)c->x[0],(unsigned long long)c->x[1]);
    c->x[0]=0;
    break;
  }
}
#endif /* SUYU_HOSTED_RECOMP */

/* An opcode the decoder does not implement. Stubbing it out (the old
   behaviour: zero x0 and carry on) silently corrupts guest state - a
   function whose body is one unimplemented SIMD instruction returns a
   plausible-looking 0, and the caller then dereferences it. That is
   indistinguishable from a real null and shows up much later as a crash
   nowhere near the actual gap.

   Instead, park the context at this exact PC and halt with a distinct code.
   The host (ArmRecomp::RunThread) treats halted == RECOMP_HALT_UNHANDLED as
   "run this address on the interpreter fallback instead", so the instruction
   executes correctly and control returns to recompiled code as soon as the
   PC is covered again. Correct by construction whatever the decoder does or
   does not cover, and every remaining gap costs speed rather than
   correctness.

   Standalone builds have no fallback engine to hand off to, so they keep the
   old step-over behaviour - degraded, but still the best available there. */
/* DMB/DSB/ISB. Under the hosted backend the guest is genuinely multi-threaded,
   so these must be real fences: without one the host compiler is free to sink a
   pointer store past the field stores the barrier was there to publish. The
   standalone runtime drives one guest thread on one host thread and has nothing
   to order against, so it keeps the free no-op. */
void recomp_barrier(void){
#ifdef SUYU_HOSTED_RECOMP
#if defined(__cplusplus)
    atomic_thread_fence(memory_order_seq_cst);
#elif defined(_MSC_VER)
    MemoryBarrier();
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
#endif
}

void recomp_unhandled(GuestContext* c,uint32_t insn,uint64_t pc){
#ifdef SUYU_HOSTED_RECOMP
  (void)insn;
  c->pc=pc;
  c->halted=RECOMP_HALT_UNHANDLED;
#else
  fprintf(stderr,"[recomp] unhandled insn 0x%08x at 0x%llx\n",insn,(unsigned long long)pc);
  (void)c; /* stepping over is bad enough; do not clobber a register too */
#endif
}

#ifndef RECOMP_STATIC_HOST
/* Drives a single module's own dispatch table. Meaningless when the runtime is
   shared between several statically linked modules - the host dispatches
   across them instead - so it is compiled out there, where recomp_lookup has
   been renamed per module and would not resolve. */
void recomp_run(GuestContext* c){
  uint64_t g=0;
  while(!c->halted){
    BlockFn f=recomp_lookup(c->pc);
    if(!f){ fprintf(stderr,"[recomp] no block at 0x%llx\n",(unsigned long long)c->pc); break;}
    f(c);
    if(++g>100000000ULL){ fprintf(stderr,"[recomp] watchdog (100M iterations)\n"); break; }
    /* Yield every 4096 blocks to prevent 100% CPU spin on tight loops */
    if((g & 0xFFF)==0){
#ifdef _WIN32
      Sleep(0);
#else
      { struct timespec ts={0,0}; nanosleep(&ts,0); }
#endif
    }
  }
}
#endif /* !RECOMP_STATIC_HOST */
)RT";
    static const std::string with_estimates = text + EstimateRuntimeC();
    return with_estimates.c_str();
}

} // namespace suyu::recomp
