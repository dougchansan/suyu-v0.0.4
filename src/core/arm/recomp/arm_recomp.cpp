// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "common/string_util.h"
#include "common/fs/path_util.h"
#include "core/arm/recomp/arm_recomp.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_process.h"
#include "core/hardware_properties.h"
#include "core/arm/debug.h"
#ifndef SUYU_NO_JIT
#include "core/arm/dynarmic/arm_dynarmic_64.h"
#include "core/arm/dynarmic/dynarmic_exclusive_monitor.h"
#endif
#include "core/arm/exclusive_monitor.h"
#include "core/memory.h"

namespace Core {

namespace {
// Mirrors the prefix of the GuestContext the recompiler emits. Only the fields
// the emulator needs to observe or mutate are modelled; the generated struct
// carries additional members after these (heap bookkeeping, save-data handles)
// which the recompiled code manages itself and we never touch.
struct GuestContextView {
    u64 x[32];
    u64 pc;
    u8 n, z, c, v;
    u8* mem;
    u64 mem_size;
    u64 mem_base_vaddr;
    int halted;
    u64 pending_svc;
    // The SIMD/FP register file and thread pointer sit immediately after
    // pending_svc in the emitted struct. They have to be modelled here rather
    // than left off the end: the recompiler emits SIMD code, so a context
    // switch that did not carry these would silently lose every floating-point
    // and vector register the guest had live.
    u64 vreg[32][2];
    u64 tpidr_el0;
    const void* host_mem;
    // TPIDRRO_EL0 - the kernel-published thread-local region, whose first
    // 0x100 bytes are the IPC message buffer KServerSession reads a
    // SendSyncRequest out of. Kept apart from tpidr_el0 (the guest's own
    // thread pointer) because the two have different owners and different
    // lifetimes; see the note on GuestContext in core/recompiler/arm64_to_c.h.
    u64 tpidrro_el0;
    // FP control and status. Modelled here so they survive the marshal to and
    // from the fallback JIT - without these fields the guest's rounding mode
    // was silently reset to the host default on every engine transition.
    u64 fpcr;
    u64 fpsr;
    // Must sit here rather than at the end: the emitted struct continues with
    // heap bookkeeping this view does not model, so a field appended after that
    // point would be at a different offset on each side.
    int chain_budget;
};

// Matches RecompHostMem in the generated runtime. The recompiled code calls
// through this for every guest access, so that it reads and writes the
// emulator's address space rather than the flat buffer the standalone runtime
// would otherwise own - without it the recompiled code and the HLE kernel
// would be looking at two different memories.
struct RecompHostMem {
    void* user;
    u64 (*load)(void* user, u64 va, u32 size);
    void (*store)(void* user, u64 va, u32 size, u64 value);
    // Exclusive access, routed at the kernel's own monitor so recompiled code
    // and the fallback JIT contend correctly against each other.
    u64 (*excl_load)(void* user, u64 va, u32 size);
    u32 (*excl_store)(void* user, u64 va, u32 size, u64 value);
    void (*clear_excl)(void* user);
    // The physical counter, read from the emulator's timing source so the
    // recompiled code and the JIT agree about time.
    u64 (*read_cntpct)(void* user);
    // Exclusive pair forms; `size` is the width of one register, 4 or 8.
    void (*excl_load_pair)(void* user, u64 va, u32 size, u64* lo, u64* hi);
    // Page table, so generated code can resolve a mapped address inline instead
    // of calling out for every load and store. Mirrors GetPointerImpl's fast
    // path; a null backing pointer (unmapped, debug, or GPU-tracked memory)
    // falls through to `load`/`store` so rasterizer invalidation still happens.
    u32 (*excl_store_pair)(void* user, u64 va, u32 size, u64 lo, u64 hi);
    const void* page_entries;
    u64 page_entry_stride;
    u64 page_bits;
    u64 pointer_mask;
    u64 address_space_max;
    // Points at g_guard_generation. Generated blocks re-verify their bytes only
    // when this moves, rather than on every entry. Shared by all cores, so it is
    // a pointer: one core invalidating code has to be visible to blocks running
    // on the others.
    const u64* guard_generation;
};

// Bumped whenever guest code may have changed - IC IVAU, a cache-range
// invalidation, a full icache flush, or a new page table. Generated code reads
// it through the pointer above, so it must be a plain 64-bit object in memory.
static std::atomic<u64> g_guard_generation{0};
static_assert(std::atomic<u64>::is_always_lock_free);
static_assert(sizeof(std::atomic<u64>) == sizeof(u64));

/// Force every compiled block to re-verify its bytes before it next runs.
static void BumpGuardGeneration() {
    g_guard_generation.fetch_add(1, std::memory_order_release);
}

// This struct is duplicated by hand in the emitter (arm64_to_c.h, RuntimeH's
// RecompHostMem) because the generated project is plain C and shares no headers
// with the emulator. Nothing links the two, so a field added in the middle of
// one and at the end of the other compiles cleanly on both sides and hands the
// generated code a function pointer where it expects data.
//
// That is not hypothetical: inserting the page-table fields after
// excl_load_pair here, while the emitter appended them after excl_store_pair,
// made recompiled code dereference excl_store_pair's code pointer as a page
// table. suyu died during boot with no diagnostic. Hence these.
static_assert(offsetof(RecompHostMem, excl_load_pair) == 56);
static_assert(offsetof(RecompHostMem, excl_store_pair) == 64);
static_assert(offsetof(RecompHostMem, page_entries) == 72);
static_assert(offsetof(RecompHostMem, page_entry_stride) == 80);
static_assert(offsetof(RecompHostMem, page_bits) == 88);
static_assert(offsetof(RecompHostMem, pointer_mask) == 96);
static_assert(offsetof(RecompHostMem, address_space_max) == 104);
static_assert(offsetof(RecompHostMem, guard_generation) == 112);
static_assert(sizeof(RecompHostMem) == 120);

// Nothing links these two builds together, so the shared layout is pinned on
// both sides: the generated runtime asserts the same four offsets against its
// own GuestContext. If a field is ever inserted rather than appended, one of
// the two fails to compile instead of the emulator silently reading the wrong
// registers.
static_assert(offsetof(GuestContextView, pc) == 256);
static_assert(offsetof(GuestContextView, pending_svc) == 304);
static_assert(offsetof(GuestContextView, vreg) == 312);
static_assert(offsetof(GuestContextView, tpidr_el0) == 824);
static_assert(offsetof(GuestContextView, chain_budget) == 864);

// Blocks a chain of direct calls may run before returning here. Only this side
// sets it - the generated code just decrements - so the emitter does not need
// to agree on the value.
//
// It bounds two things. How long a guest loop can run without the interrupt and
// SVC checks below getting a look in; and, because the generated calls are not
// guaranteed to be tail calls, how deep the host stack goes. Guest threads run
// on 512 KB fibers (common/fiber.cpp) and a block frame carrying SIMD locals is
// not small, so this has to stay well under what that stack can hold. 256
// overflowed it and crashed on boot.
// Overridable so the depth can be measured rather than guessed, but only
// upwards from a value known to be safe, and only when the generated code was
// built with -foptimize-sibling-calls - without real tail calls a large budget
// is a stack overflow, which is exactly how 256 crashed on boot.
// Refuse the JIT entirely. Without this, "the JIT was never reached" is an
// observation about one run; with it, reaching the JIT is a loud, fatal failure
// that names the address, which is the difference between evidence and proof.
const bool kStrictNoFallback = [] {
    const char* e = std::getenv("SUYU_RECOMP_STRICT");
    return e && *e && *e != '0';
}();

const int kChainBudget = [] {
    const char* e = std::getenv("SUYU_RECOMP_CHAIN_BUDGET");
    if (!e) {
        return 32;
    }
    const int v = std::atoi(e);
    return (v >= 1 && v <= 8192) ? v : 32;
}();

const bool kSamplePc = [] {
    const char* e = std::getenv("SUYU_RECOMP_SAMPLE_PC");
    return e && *e && *e != '0';
}();
static_assert(offsetof(GuestContextView, host_mem) == 832);
static_assert(offsetof(GuestContextView, tpidrro_el0) == 840);
static_assert(offsetof(GuestContextView, fpcr) == 848);
static_assert(offsetof(GuestContextView, fpsr) == 856);

// The generated code signals an SVC by parking with this set. Kept in sync
// with the emitted recomp_svc contract in core/recompiler/arm64_to_c.h.
constexpr u64 kNoPendingSvc = ~0ULL;

// Mirrors RECOMP_HALT_UNHANDLED in the generated runtime: a block that halts
// with this parked its PC on an instruction the decoder cannot translate and
// is asking for that address to be executed by the interpreter fallback.
constexpr int kHaltUnhandled = 2;
// Mirrors RECOMP_HALT_BREAKPOINT. PC already names the faulting instruction.
constexpr int kHaltBreakpoint = 3;
constexpr int kHaltIcIvau = 4;
std::atomic<bool> g_code_guard_ready{false};

// An unresolved GOT/JUMP_SLOT relocation used to be left untouched, which
// means a call through it branches to whatever the raw NSO file already had
// sitting in that GOT slot - typically a small placeholder/addend value the
// static linker left for a symbol it expected the *dynamic* linker to fill
// in later (lazy-binding stub offset, or just zero-adjacent garbage), not a
// real address. The guest's BLR then lands on that small value directly -
// e.g. 0xe7ff0 - which is unmapped, and the JIT fallback that "No recompiled
// block" hands off to can't execute there either, so the whole thread dies.
// Every unresolved slot is patched to this fixed, recognizable sentinel
// instead: the dispatch loop below special-cases it as an immediate "return
// to caller" (PC = LR) rather than a real guest address, so a genuinely
// call-but-never-actually-invoked unresolved import (the common case - most
// entries in a large import table exist for code paths a given boot never
// takes) fails soft instead of crashing the thread outright.
constexpr u64 kUnresolvedImportTrap = 0xFFFF'FFFF'0000'0000ULL;
} // namespace

namespace {
std::atomic<RecompLookupFn> g_recomp_lookup{nullptr};
std::atomic<RecompBaseFn> g_recomp_base_setter{nullptr};
std::atomic<RecompPrepareFn> g_recomp_prepare{nullptr};

/// Execution coverage for the AOT path (mk8-recomp #13).
///
/// The exporter's static coverage says what fraction of the *image* translates.
/// It cannot say what fraction of *execution* stays on the recompiled path,
/// and those differ by orders of magnitude: one untranslated instruction inside
/// a hot loop costs a full engine transition on every iteration, while a
/// thousand untranslated instructions in code that never runs cost nothing.
///
/// Only this decides whether the AOT path is worth anything, and only this can
/// rank the missing opcodes by what actually executes.
struct RecompCounters {
    std::atomic<u64> svc_calls{0};
    std::atomic<u64> fallback_from_miss{0};
    std::atomic<u64> fallback_from_unhandled{0};
    std::atomic<u64> jit_to_static{0};
    std::atomic<u64> unresolved_import_traps{0};
    std::atomic<u64> no_fallback_available{0};

    // Guarded rather than atomic: these are touched only on a transition, which
    // is by definition already the slow path.
    std::mutex hist_lock;
    std::map<u32, u64> unhandled_insn;  ///< guest encoding -> times it forced a fallback
    std::map<u64, u64> miss_pc;         ///< PC with no block -> times it forced a fallback
    std::map<u32, u64> svc_numbers;     ///< SVC imm -> times the guest issued it
    std::map<u64, u64> sampled_pc;      ///< sparse samples for zero-transition stalls
    /// Load address -> module name, so a PC in this report can be resolved to
    /// module+offset. Without it the addresses mean nothing except beside the
    /// matching boot log, and a report read against another run's log resolves
    /// to the wrong place without saying so.
    std::map<u64, std::string> modules;

    void RecordModules(const std::map<u64, std::string>& m) {
        std::scoped_lock lk{hist_lock};
        modules = m;
    }

    void RecordSvc(u32 num) {
        std::scoped_lock lk{hist_lock};
        ++svc_numbers[num];
    }

    void RecordUnhandled(u32 insn) {
        std::scoped_lock lk{hist_lock};
        ++unhandled_insn[insn];
    }
    void RecordMiss(u64 pc) {
        std::scoped_lock lk{hist_lock};
        ++miss_pc[pc];
    }
    void RecordSample(u64 pc) {
        std::scoped_lock lk{hist_lock};
        ++sampled_pc[pc];
    }
};

RecompCounters g_counters;
std::array<std::atomic<u64>, 4> g_current_pcs{};
std::atomic<int> g_live_instances{0};

/// The block tally is incremented once per executed block by every guest
/// thread. As one shared atomic that is a contended cache line on the hottest
/// path there is: it was 23% of the dispatch loop's own cycles. Each thread
/// counts into its own line instead, and the report sums them.
struct alignas(64) ThreadBlocks {
    std::atomic<u64> n{0};
    char pad[64 - sizeof(std::atomic<u64>)];
};
std::mutex g_tally_lock;
std::vector<ThreadBlocks*> g_tallies;
std::atomic<u64> g_retired_blocks{0};

struct ThreadBlockSlot {
    ThreadBlocks* slot = new ThreadBlocks{};
    ThreadBlockSlot() {
        std::scoped_lock lk{g_tally_lock};
        g_tallies.push_back(slot);
    }
    // The slot outlives the thread: the reporter may be summing while a guest
    // thread exits, and the count still belongs in the total.
};
thread_local ThreadBlockSlot t_blocks;

u64 TotalStaticBlocks() {
    std::scoped_lock lk{g_tally_lock};
    u64 sum = g_retired_blocks.load(std::memory_order_relaxed);
    for (const ThreadBlocks* s : g_tallies) {
        sum += s->n.load(std::memory_order_relaxed);
    }
    return sum;
}

template <typename Map>
auto TopN(const Map& m, size_t n) {
    // Not Map::value_type: that has a const key and so is not assignable, which
    // partial_sort requires.
    using Entry = std::pair<typename Map::key_type, typename Map::mapped_type>;
    std::vector<Entry> v(m.begin(), m.end());
    std::partial_sort(v.begin(), v.begin() + std::min(n, v.size()), v.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
    if (v.size() > n) {
        v.resize(n);
    }
    return v;
}

/// Formats the run's execution-coverage report.
///
/// Built as a string rather than logged line by line so the same text can go to
/// both the log and a file. The file matters: the emulated process is not
/// always torn down at shutdown, so ~ArmRecomp may never run and the whole
/// run's measurement is lost with it. Writing periodically means a report
/// always exists for the last completed interval however the process ends.
std::string FormatRecompCoverage() {
    const u64 blocks = TotalStaticBlocks();
    const u64 miss = g_counters.fallback_from_miss.load();
    const u64 unh = g_counters.fallback_from_unhandled.load();
    const u64 transitions = miss + unh;

    if (blocks == 0 && transitions == 0) {
        return {};  // backend never ran; saying nothing is better than printing zeros
    }

    std::string o = "=== RECOMP EXECUTION COVERAGE ===\n";
    o += fmt::format("  static blocks executed : {}\n", blocks);
    o += fmt::format("  SVCs to HLE            : {}\n", g_counters.svc_calls.load());
    o += fmt::format("  static -> JIT          : {} ({} lookup miss, {} unimplemented opcode)\n",
                     transitions, miss, unh);
    o += fmt::format("  JIT -> static          : {}\n", g_counters.jit_to_static.load());
    o += fmt::format("  unresolved import traps: {}\n", g_counters.unresolved_import_traps.load());
    if (const u64 nofb = g_counters.no_fallback_available.load(); nofb) {
        o += fmt::format("  threads killed with no JIT fallback: {}\n", nofb);
    }

    // Blocks per transition is the number that matters. Each transition costs a
    // 32-GPR + 32-vector marshal in each direction, so a high static block count
    // next to a comparable transition count is worse than it looks.
    if (transitions) {
        o += fmt::format("  blocks per transition  : {:.1f}\n",
                         double(blocks) / double(transitions));
    } else {
        o += fmt::format("  blocks per transition  : no transitions - fully static\n");
    }

    std::scoped_lock lk{g_counters.hist_lock};

    if (!g_counters.unhandled_insn.empty()) {
        o += "  --- unimplemented opcodes by execution count ---\n";
        for (const auto& [insn, count] : TopN(g_counters.unhandled_insn, 24)) {
            o += fmt::format("    {:08X}  {:>10}  {:5.2f}%  (sig {:08X})\n", insn, count,
                             unh ? 100.0 * double(count) / double(unh) : 0.0, insn & 0xFFC00000u);
        }
        o += fmt::format("    {} distinct encodings\n", g_counters.unhandled_insn.size());
    }

    if (!g_counters.svc_numbers.empty()) {
        // What the guest actually asks the kernel for. A boot that stops making
        // system calls while still executing millions of blocks is spinning on
        // something, and this says on what.
        o += "  --- SVCs by call count ---\n";
        for (const auto& [num, count] : TopN(g_counters.svc_numbers, 16)) {
            o += fmt::format("    svc 0x{:02X}  {:>10}\n", num, count);
        }
        o += fmt::format("    {} distinct SVCs\n", g_counters.svc_numbers.size());
    }

    if (!g_counters.modules.empty()) {
        o += "  --- loaded modules ---\n";
        for (const auto& [base, name] : g_counters.modules) {
            o += fmt::format("    {:#018x}  {}\n", base, name);
        }
    }

    if (!g_counters.sampled_pc.empty()) {
        const auto resolve = [](u64 pc) -> std::string {
            u64 best = 0;
            const std::string* name = nullptr;
            for (const auto& [base, module_name] : g_counters.modules) {
                if (pc >= base && base >= best) {
                    best = base;
                    name = &module_name;
                }
            }
            return name ? fmt::format("  {}+{:#x}", *name, pc - best) : std::string{};
        };
        o += "  --- sampled PCs by count ---\n";
        for (const auto& [pc, count] : TopN(g_counters.sampled_pc, 16)) {
            o += fmt::format("    {:#018x}  {:>10}{}\n", pc, count, resolve(pc));
        }
        o += fmt::format("    {} distinct sampled PCs\n", g_counters.sampled_pc.size());
    }

    if (!g_counters.miss_pc.empty()) {
        // Resolved here rather than left to the reader: a bare guest PC
        // needs this run's load addresses to mean anything, and pairing a
        // report with another run's log gives a confident wrong answer.
        const auto resolve = [](u64 pc) -> std::string {
            u64 best = 0;
            const std::string* name = nullptr;
            for (const auto& [base, module_name] : g_counters.modules) {
                if (pc >= base && base >= best) {
                    best = base;
                    name = &module_name;
                }
            }
            return name ? fmt::format("  {}+{:#x}", *name, pc - best) : std::string{};
        };
        o += "  --- uncovered PCs by execution count ---\n";
        for (const auto& [pc, count] : TopN(g_counters.miss_pc, 16)) {
            o += fmt::format("    {:#018x}  {:>10}{}\n", pc, count, resolve(pc));
        }
        o += fmt::format("    {} distinct PCs\n", g_counters.miss_pc.size());
    }
    o += "=== END RECOMP EXECUTION COVERAGE ===\n";
    return o;
}

void WriteRecompCoverageFile(const std::string& text) {
    const auto path = Common::FS::GetSuyuPath(Common::FS::SuyuPath::LogDir) / "recomp_coverage.txt";
    std::ofstream out(path, std::ios::trunc);
    if (out) {
        out << text;
    }
}

void ReportRecompCoverage() {
    const std::string report = FormatRecompCoverage();
    if (report.empty()) {
        return;
    }
    WriteRecompCoverageFile(report);
    // Split by hand: the report is already newline-delimited and the logger
    // takes one line at a time.
    size_t pos = 0;
    while (pos < report.size()) {
        const size_t nl = report.find('\n', pos);
        const std::string_view line{report.data() + pos,
                                    (nl == std::string::npos ? report.size() : nl) - pos};
        if (!line.empty()) {
            LOG_INFO(Core_ARM, "{}", line);
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
}

} // namespace

RecompExecutionStats GetRecompExecutionStats() {
    return {
        TotalStaticBlocks(),
        g_counters.svc_calls.load(std::memory_order_relaxed),
        g_counters.fallback_from_miss.load(std::memory_order_relaxed),
        g_counters.fallback_from_unhandled.load(std::memory_order_relaxed),
        g_counters.no_fallback_available.load(std::memory_order_relaxed),
    };
}

std::array<u64, 4> GetRecompCurrentPcs() {
    std::array<u64, 4> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = g_current_pcs[index].load(std::memory_order_relaxed);
    }
    return result;
}

void SetRecompLookup(RecompLookupFn lookup) {
    g_code_guard_ready.store(false, std::memory_order_release);
    g_recomp_lookup.store(lookup, std::memory_order_release);
}

void SetRecompCodeGuardReady(bool ready) {
    g_code_guard_ready.store(ready, std::memory_order_release);
}

bool IsRecompCodeGuardReady() {
    return g_code_guard_ready.load(std::memory_order_acquire);
}

void SetRecompBaseSetter(RecompBaseFn setter) {
    g_recomp_base_setter.store(setter, std::memory_order_release);
}

RecompLiveStats GetRecompLiveStats() {
    return RecompLiveStats{
        TotalStaticBlocks(),
        g_counters.fallback_from_miss.load(std::memory_order_relaxed) +
            g_counters.fallback_from_unhandled.load(std::memory_order_relaxed),
        g_live_instances.load(std::memory_order_relaxed) > 0,
#ifdef SUYU_NO_JIT
        false,
#else
        true,
#endif
    };
}

void SetRecompPrepareCallback(RecompPrepareFn callback) {
    g_recomp_prepare.store(callback, std::memory_order_release);
}

bool HasRecompPrepareCallback() {
    return g_recomp_prepare.load(std::memory_order_acquire) != nullptr;
}

RecompLookupFn GetRecompLookup() {
    return g_recomp_lookup.load(std::memory_order_acquire);
}

struct ArmRecomp::Impl {
    Impl(System& system_, RecompLookupFn lookup_) : system{system_}, lookup{lookup_} {
        std::memset(&ctx, 0, sizeof(ctx));
        ctx.pending_svc = kNoPendingSvc;
        // Point the recompiled code at the emulator's address space.
        bridge.user = this;
        bridge.load = &Impl::HostLoad;
        bridge.store = &Impl::HostStore;
        bridge.excl_load = &Impl::HostExclusiveLoad;
        bridge.excl_store = &Impl::HostExclusiveStore;
        bridge.clear_excl = &Impl::HostClearExclusive;
        bridge.read_cntpct = &Impl::HostReadCntpct;
        bridge.excl_load_pair = &Impl::HostExclusiveLoadPair;
        bridge.excl_store_pair = &Impl::HostExclusiveStorePair;
        // Filled in by RefreshPageTable once a process exists; until then the
        // fields stay null and every access takes the callback path.
        bridge.page_entries = nullptr;
        bridge.guard_generation = reinterpret_cast<const u64*>(&g_guard_generation);
        ctx.host_mem = &bridge;
    }

    // The monitor is per-core, and core_index is what distinguishes one guest
    // thread's reservation from another's. Without a monitor (no owning process,
    // so no fallback either) these degrade to plain accesses with STXR always
    // succeeding - the old behaviour, and wrong under threads, but that
    // configuration cannot run a real title anyway.
    static u64 HostExclusiveLoad(void* user, u64 va, u32 size) {
        auto* self = static_cast<Impl*>(user);
        if (!self->exclusive_monitor) {
            return HostLoad(user, va, size);
        }
        const auto core = self->core_index;
        switch (size) {
        case 1: return self->exclusive_monitor->ExclusiveRead8(core, va);
        case 2: return self->exclusive_monitor->ExclusiveRead16(core, va);
        case 4: return self->exclusive_monitor->ExclusiveRead32(core, va);
        case 8: return self->exclusive_monitor->ExclusiveRead64(core, va);
        default: return HostLoad(user, va, size);
        }
    }

    /// Returns 0 on success, 1 when the reservation was lost - the sense of the
    /// status register STXR writes.
    static u32 HostExclusiveStore(void* user, u64 va, u32 size, u64 value) {
        auto* self = static_cast<Impl*>(user);
        if (!self->exclusive_monitor) {
            HostStore(user, va, size, value);
            return 0;
        }
        const auto core = self->core_index;
        bool ok = false;
        switch (size) {
        case 1: ok = self->exclusive_monitor->ExclusiveWrite8(core, va, static_cast<u8>(value)); break;
        case 2: ok = self->exclusive_monitor->ExclusiveWrite16(core, va, static_cast<u16>(value)); break;
        case 4: ok = self->exclusive_monitor->ExclusiveWrite32(core, va, static_cast<u32>(value)); break;
        case 8: ok = self->exclusive_monitor->ExclusiveWrite64(core, va, value); break;
        default: HostStore(user, va, size, value); return 0;
        }
        return ok ? 0u : 1u;
    }

    /// LDXP/LDAXP. The 64-bit pair takes a real 128-bit reservation; the
    /// 32-bit pair is a 64-bit reservation whose two words are the registers,
    /// which is what the architecture specifies rather than a shortcut.
    static void HostExclusiveLoadPair(void* user, u64 va, u32 size, u64* lo, u64* hi) {
        auto* self = static_cast<Impl*>(user);
        if (!self->exclusive_monitor) {
            *lo = HostLoad(user, va, size);
            *hi = HostLoad(user, va + size, size);
            return;
        }
        const auto core = self->core_index;
        if (size == 8) {
            const u128 v = self->exclusive_monitor->ExclusiveRead128(core, va);
            *lo = v[0];
            *hi = v[1];
        } else {
            const u64 v = self->exclusive_monitor->ExclusiveRead64(core, va);
            *lo = static_cast<u32>(v);
            *hi = static_cast<u32>(v >> 32);
        }
    }

    /// STXP/STLXP. Returns 0 on success, matching STXR's status sense.
    static u32 HostExclusiveStorePair(void* user, u64 va, u32 size, u64 lo, u64 hi) {
        auto* self = static_cast<Impl*>(user);
        if (!self->exclusive_monitor) {
            HostStore(user, va, size, lo);
            HostStore(user, va + size, size, hi);
            return 0;
        }
        const auto core = self->core_index;
        bool ok = false;
        if (size == 8) {
            ok = self->exclusive_monitor->ExclusiveWrite128(core, va, u128{lo, hi});
        } else {
            ok = self->exclusive_monitor->ExclusiveWrite64(
                core, va, static_cast<u32>(lo) | (static_cast<u64>(static_cast<u32>(hi)) << 32));
        }
        return ok ? 0u : 1u;
    }

    /// Re-read the page table description into the bridge.
    ///
    /// Called on entry to a run rather than once at construction: the table
    /// belongs to the process, and the pointer is not valid until one exists.
    /// A stale pointer here would have generated code reading another address
    /// space, so it is refreshed rather than cached forever.
    void RefreshPageTable() {
        const auto view = system.ApplicationMemory().GetPageTableView();
        // A different table can mean different bytes behind the same guest
        // address, so nothing verified against the old one still counts.
        if (bridge.page_entries != view.entries) {
            BumpGuardGeneration();
        }
        bridge.page_entries = view.entries;
        bridge.page_entry_stride = view.entry_stride;
        bridge.page_bits = view.page_bits;
        bridge.pointer_mask = view.pointer_mask;
        bridge.address_space_max = view.address_space_max;
    }

    /// The same source DynarmicCallbacks64::GetCNTPCT uses, so a guest thread
    /// that migrates between engines sees one monotonic clock.
    static u64 HostReadCntpct(void* user) {
        return static_cast<Impl*>(user)->system.CoreTiming().GetClockTicks();
    }

    static void HostClearExclusive(void* user) {
        auto* self = static_cast<Impl*>(user);
        if (self->exclusive_monitor) {
            self->exclusive_monitor->ClearExclusive(self->core_index);
        }
    }

    static u64 HostLoad(void* user, u64 va, u32 size) {
        va &= 0xffffffffffffULL;
        auto& memory = static_cast<Impl*>(user)->system.ApplicationMemory();
        if (size == 0) {
            u64 instruction = 0;
            for (u32 offset = 0; offset < sizeof(u32); ++offset) {
                if (!memory.IsValidVirtualAddress(va + offset)) return 0;
                instruction |= static_cast<u64>(memory.Read8(va + offset)) << (offset * 8);
            }
            return (u64{1} << 32) | instruction;
        }
        if ((size != 1 && size != 2 && size != 4 && size != 8) ||
            size > 0x1000000000000ULL - va) {
            return 0;
        }
        u64 value = 0;
        for (u32 offset = 0; offset < size; ++offset) {
            value |= static_cast<u64>(memory.Read8(va + offset)) << (offset * 8);
        }
        return value;
    }

    static void HostStore(void* user, u64 va, u32 size, u64 value) {
        va &= 0xffffffffffffULL;
        if ((size != 1 && size != 2 && size != 4 && size != 8) ||
            size > 0x1000000000000ULL - va) {
            return;
        }
        auto& memory = static_cast<Impl*>(user)->system.ApplicationMemory();
        for (u32 i = 0; i < size; ++i) {
            memory.Write8(va + i, static_cast<u8>(value >> (i * 8)));
        }
    }

    /// Base address of the module containing `pc`, so an address can be turned
    /// into the module-relative offset a recompiled image is keyed by. The
    /// module list is fixed once the process is running, so it is read once.
    u64 ModuleBaseFor(Kernel::KThread* thread, u64 pc) {
        if (!modules_read) {
            modules_read = true;
            if (auto* process = thread->GetOwnerProcess()) {
                modules = FindModules(process);
                g_counters.RecordModules(modules);
                // Now that the loader has placed everything, tell each image
                // where its own module went.
                if (const auto setter = g_recomp_base_setter.load(std::memory_order_acquire)) {
                    // modules is keyed by base, so iteration is load order.
                    size_t index = 0;
                    for (const auto& [module_base, name] : modules) {
                        setter(index++, name.c_str(), module_base);
                    }
                }
            }
        }
        u64 base = 0;
        for (const auto& [module_base, name] : modules) {
            if (pc >= module_base && module_base >= base) {
                base = module_base;
            }
        }
        return base;
    }

    struct DynInfo {
        u64 mod_base = 0;
        u64 rela_va = 0, rela_sz = 0, rela_ent = 24, rela_sz_va = 0;
        u64 jmprel_va = 0, jmprel_sz = 0, jmprel_ent = 24, jmprel_sz_va = 0;
        u64 symtab_va = 0, strtab_va = 0;
        // Address of a harmless "return 0" stub inside this module's own text,
        // used as the target for every import that could not be resolved.
        u64 trap_va = 0;
    };

    // Find an existing `mov x0, #0; ret` (or failing that, a bare `ret`) in a
    // module's text and hand back its address.
    //
    // Unresolved imports have to point somewhere, and the address has to be one
    // BOTH execution engines can survive: the recompiled dispatcher can
    // special-case a magic sentinel, but the dynarmic fallback cannot - it just
    // tries to translate the address and dies with "cannot execute instruction
    // at unmapped address". Reusing a real instruction pair the module already
    // contains sidesteps that entirely: it is mapped, executable, and returns to
    // the caller under any engine, with no per-engine handling and nothing
    // written into guest memory. Most entries in a large import table exist for
    // code paths a given boot never takes, so failing soft here is what keeps a
    // partially-resolvable module bootable at all.
    u64 FindReturnStub(u64 mod_base) {
        auto& mem = system.ApplicationMemory();
        constexpr u32 kMovX0Zero = 0xD2800000, kRet = 0xD65F03C0;
        // Text sits at the front of every NSO; a module without a matching pair
        // in its first megabyte does not have one worth scanning further for.
        constexpr u64 kScanLimit = 0x100000;
        u64 bare_ret = 0;
        for (u64 off = 0; off < kScanLimit; off += 4) {
            const u32 insn = mem.Read32(mod_base + off);
            if (insn == kRet) {
                if (!bare_ret) bare_ret = mod_base + off;
            } else if (insn == kMovX0Zero && mem.Read32(mod_base + off + 4) == kRet) {
                return mod_base + off;
            }
        }
        return bare_ret;
    }

    // Locate a module's MOD0 header and parse its .dynamic section. Returns
    // false if this module has no MOD0 (nothing to relocate).
    bool ParseDynamic(u64 mod_base, DynInfo& out) {
        auto& mem = system.ApplicationMemory();
        // MOD0 magic "MOD0" = 0x30444F4D. It sits at the start of rodata
        // (typically mod+0x2000 for rtld), but the actual location is pointed
        // to by a 4-byte offset at mod+4 (per NSO ABI). Scan the first few KB.
        u64 mod0_va = 0;
        for (u64 off = 0; off < 0x4000; off += 4) {
            if (mem.Read32(mod_base + off) == 0x30444F4Du) {
                mod0_va = mod_base + off;
                break;
            }
        }
        if (!mod0_va) return false;

        // MOD0 layout: magic(4), dyn_offset(4), bss_start(4), bss_end(4)
        // dyn_offset is relative to the MOD0 header itself.
        const u32 dyn_rel_off = mem.Read32(mod0_va + 4);
        const u64 dyn_va = mod0_va + dyn_rel_off;

        constexpr u32 DT_NULL = 0, DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7,
                       DT_RELASZ = 8, DT_RELAENT = 9, DT_PLTREL = 20, DT_JMPREL = 23,
                       DT_REL_TAG = 17;
        out.mod_base = mod_base;
        u64 pltrel_kind = DT_RELA; // default per AArch64 ABI (RELA, not REL)
        for (u64 p = dyn_va; ; p += 16) {
            const u64 tag = mem.Read64(p);
            const u64 val = mem.Read64(p + 8);
            if (tag == DT_NULL) break;
            if (tag == DT_RELA)     out.rela_va    = mod_base + val;
            if (tag == DT_RELASZ)   { out.rela_sz = val; out.rela_sz_va = p + 8; }
            if (tag == DT_RELAENT)  out.rela_ent   = val;
            if (tag == DT_JMPREL)   out.jmprel_va  = mod_base + val;
            if (tag == DT_PLTRELSZ) { out.jmprel_sz = val; out.jmprel_sz_va = p + 8; }
            if (tag == DT_PLTREL)   pltrel_kind    = val;
            if (tag == DT_SYMTAB)   out.symtab_va  = mod_base + val;
            if (tag == DT_STRTAB)   out.strtab_va  = mod_base + val;
            if (p - dyn_va > 0x1000) break; // safety
        }
        // DT_PLTREL says whether JMPREL uses 16-byte REL entries (no addend)
        // instead of 24-byte RELA - vanishingly rare on AArch64, but assuming
        // RELA unconditionally would silently misalign every read if a
        // module did use it. Checked after the loop since DT_PLTREL can
        // appear either before or after the entries it describes.
        out.jmprel_ent = (pltrel_kind == DT_REL_TAG) ? 16 : 24;
        return true;
    }

    // Read a symbol's name (from .dynstr) and value for GLOB_DAT/JUMP_SLOT
    // resolution. Elf64_Sym: st_name(4) st_info(1) st_other(1) st_shndx(2)
    // st_value(8) st_size(8) = 24 bytes.
    struct SymInfo {
        std::string name;
        u64 value = 0;
        bool defined = false;
    };
    SymInfo ReadSymbol(const DynInfo& d, u32 index) {
        auto& mem = system.ApplicationMemory();
        SymInfo s;
        if (!d.symtab_va) return s;
        const u64 sym_va = d.symtab_va + static_cast<u64>(index) * 24;
        const u32 name_off = mem.Read32(sym_va);
        // st_shndx is a 2-byte field at offset 6 (st_name(4) st_info(1)
        // st_other(1) st_shndx(2) st_value(8) st_size(8)) - reading 4 bytes
        // here previously spilled into st_value's low bytes, corrupting the
        // defined/undefined check for essentially every symbol whose value
        // had nonzero low 16 bits.
        const u16 shndx = mem.Read16(sym_va + 6);
        s.value = mem.Read64(sym_va + 8);
        s.defined = shndx != 0; // SHN_UNDEF == 0
        if (d.strtab_va) {
            std::string name;
            for (u64 i = 0; i < 512; ++i) {
                const u8 c = static_cast<u8>(mem.Read8(d.strtab_va + name_off + i));
                if (!c) break;
                name.push_back(static_cast<char>(c));
            }
            s.name = std::move(name);
        }
        return s;
    }

    // Every module's exported (defined) symbols, keyed by name, so
    // GLOB_DAT/JUMP_SLOT relocations that reference another module's symbol
    // (e.g. main calling into sdk, or rtld exporting to everything) can be
    // resolved. Built once, across every module, before any relocation
    // actually writes anything - a relocation processed before its target
    // module's exports are indexed would silently resolve to nothing.
    void IndexExports(const DynInfo& d, std::unordered_map<std::string, u64>& out) {
        if (!d.symtab_va || !d.strtab_va) return;
        // No count is stored in .dynamic for a plain DT_SYMTAB (that's normally
        // DT_HASH/DT_GNU_HASH territory), but .dynsym and .dynstr are laid out
        // back to back in every Switch module observed so far, so the gap
        // between them is a reliable entry count - far more so than guessing
        // from name-offset values, which was cutting exports short before
        // rtld's own required symbols were reached (18 unresolved externals
        // for rtld itself were enough to trigger its self-abort).
        u32 max_index = 8192;
        if (d.strtab_va > d.symtab_va) {
            const u64 span = d.strtab_va - d.symtab_va;
            max_index = static_cast<u32>(std::min<u64>(span / 24, 65536));
        }
        for (u32 i = 1; i < max_index; ++i) { // index 0 is always the null symbol
            const auto sym = ReadSymbol(d, i);
            if (sym.defined && !sym.name.empty()) {
                out.emplace(sym.name, d.mod_base + sym.value);
            }
        }
    }

    void ApplyRelocTable(const DynInfo& d, u64 table_va, u64 table_sz, u64 entry_sz,
                          const std::unordered_map<std::string, u64>& exports, u32& applied,
                          u32& unresolved) {
        auto& mem = system.ApplicationMemory();
        constexpr u32 R_AARCH64_ABS64 = 0x101, R_AARCH64_RELATIVE = 0x403,
                       R_AARCH64_GLOB_DAT = 0x401, R_AARCH64_JUMP_SLOT = 0x402,
                       R_AARCH64_IRELATIVE = 0x408;
        for (u64 p = table_va; p < table_va + table_sz; p += entry_sz) {
            const u64 r_offset = mem.Read64(p);
            const u64 r_info   = mem.Read64(p + 8);
            const u64 r_addend = mem.Read64(p + 16);
            const u32 r_type = static_cast<u32>(r_info & 0xFFFFFFFF);
            const u32 r_sym  = static_cast<u32>(r_info >> 32);
            if (r_type == R_AARCH64_RELATIVE) {
                mem.Write64(d.mod_base + r_offset, d.mod_base + r_addend);
                ++applied;
            } else if (r_type == R_AARCH64_IRELATIVE) {
                // IRELATIVE (ifunc): r_addend is a RESOLVER function's address,
                // not the final target - the correct behaviour is to call it
                // (no args, AAPCS64) and store whatever it returns. Actually
                // invoking guest code from inside relocation application would
                // need a full nested-call machinery this backend doesn't have,
                // so - same as a genuinely-unresolved import - patch to the
                // trap sentinel instead of leaving the GOT slot as raw
                // pre-relocation file content. Previously this relocation type
                // matched none of the branches below and was silently skipped
                // entirely, which is exactly how a BLR through this slot ended
                // up jumping to a small leftover file value (e.g. 0xe7ff0)
                // instead of either a real function or a diagnosable trap.
                LOG_ERROR(Core_ARM,
                          "recomp: IRELATIVE relocation at module base={:#x} offset={:#x} not "
                          "invoked (resolver call unsupported); trapped instead",
                          d.mod_base, r_offset);
                mem.Write64(d.mod_base + r_offset, d.trap_va ? d.trap_va : kUnresolvedImportTrap);
            } else if (r_type == R_AARCH64_GLOB_DAT || r_type == R_AARCH64_JUMP_SLOT ||
                       r_type == R_AARCH64_ABS64) {
                const auto sym = ReadSymbol(d, r_sym);
                // ABS64 is S + A, unlike GLOB_DAT/JUMP_SLOT which are plain S.
                // It is by far the most common relocation in a C++ module's
                // .data.rel.ro - every vtable slot, every typeinfo pointer, every
                // static function-pointer table is one - and skipping it left
                // those slots holding the raw module-relative symbol value the
                // linker wrote. A virtual call through such a vtable branches to
                // that small offset instead of base+offset, which is unmapped:
                // that is the whole "cannot execute instruction at unmapped
                // address 0xe7ff0" family of boot crashes.
                const u64 addend = (r_type == R_AARCH64_ABS64) ? r_addend : 0;
                // Linker-synthesized section-boundary symbols (__got_start,
                // __rela_dyn_end, __tbss_align_abs, __EX_start, etc.) describe
                // the CURRENT module's own layout - they're self-referential,
                // not imports - but the minimal Switch toolchain often leaves
                // them marked SHN_UNDEF anyway despite carrying a correct
                // st_value. A nonzero value on an otherwise-"undefined"
                // symbol is a strong signal it's one of these, not a genuine
                // external import (those are left at value 0 with nothing to
                // point to), so trust it ahead of both the defined check and
                // cross-module export lookup.
                u64 synthetic = 0;
                bool is_synthetic = true;
                // These describe THIS module's own relocation sections - the
                // linker leaves them SHN_UNDEF/value-0 in the dynamic symbol
                // table expecting the loader to patch them in directly from
                // its own knowledge of where it placed .rela.dyn/.rela.plt,
                // rather than resolving them like a normal import. We already
                // parsed those bounds for our own use.
                if (sym.name == "__rela_dyn_start" || sym.name == "__rel_dyn_start") {
                    synthetic = d.rela_va;
                } else if (sym.name == "__rela_dyn_end" || sym.name == "__rel_dyn_end") {
                    synthetic = d.rela_va + d.rela_sz;
                } else if (sym.name == "__rela_plt_start" || sym.name == "__rel_plt_start") {
                    synthetic = d.jmprel_va;
                } else if (sym.name == "__rela_plt_end" || sym.name == "__rel_plt_end") {
                    synthetic = d.jmprel_va + d.jmprel_sz;
                } else {
                    is_synthetic = false;
                }
                if (is_synthetic && synthetic) {
                    mem.Write64(d.mod_base + r_offset, synthetic + addend);
                    ++applied;
                } else if (sym.defined || sym.value != 0) {
                    mem.Write64(d.mod_base + r_offset, d.mod_base + sym.value + addend);
                    ++applied;
                } else if (auto it = exports.find(sym.name); it != exports.end()) {
                    mem.Write64(d.mod_base + r_offset, it->second + addend);
                    ++applied;
                } else if (r_type == R_AARCH64_ABS64 && r_sym == 0) {
                    // STN_UNDEF ABS64: S is 0 by definition, so the result is
                    // the addend alone - a plain absolute constant, not a
                    // failed import. Never trap these.
                    mem.Write64(d.mod_base + r_offset, addend);
                    ++applied;
                } else {
                    ++unresolved;
                    if (unresolved <= 30) {
                        LOG_ERROR(Core_ARM, "recomp: unresolved GOT/PLT symbol '{}' for module base={:#x}",
                                  sym.name.empty() ? "<no name>" : sym.name, d.mod_base);
                    }
                    // Patch to the trap sentinel rather than leaving the slot
                    // as whatever the raw file had - see kUnresolvedImportTrap.
                    mem.Write64(d.mod_base + r_offset, d.trap_va ? d.trap_va : kUnresolvedImportTrap);
                }
            }
        }
    }

    // Pre-apply relocations for every loaded module. Under dynarmic, rtld
    // runs its own self-relocation loop correctly; under ArmRecomp the
    // recompiled loop exits early, leaving most relocations un-applied and
    // corrupting both data reads (R_AARCH64_RELATIVE, e.g. vtables, GOT
    // pointers to local data) and indirect calls through the GOT/PLT
    // (R_AARCH64_GLOB_DAT / R_AARCH64_JUMP_SLOT - unresolved, these are the
    // null/garbage function pointers that were previously observed crashing
    // rtld's module bootstrap). All module bases are already known by the
    // time this runs (the loader maps every NSO up front), so cross-module
    // symbol resolution just needs every module's exports indexed first.
    void ApplyAllRelocations(const std::map<u64, std::string>& all_modules) {
        auto& mem = system.ApplicationMemory();
        std::vector<DynInfo> dyns;
        std::unordered_map<std::string, u64> exports;
        for (const auto& [module_base, name] : all_modules) {
            DynInfo d;
            if (ParseDynamic(module_base, d)) {
                d.trap_va = FindReturnStub(module_base);
                IndexExports(d, exports);
                dyns.push_back(d);
            }
        }
        for (const auto& d : dyns) {
            u32 applied = 0, unresolved = 0;
            if (d.rela_va && d.rela_sz) {
                ApplyRelocTable(d, d.rela_va, d.rela_sz, d.rela_ent, exports, applied, unresolved);
            }
            if (d.jmprel_va && d.jmprel_sz) {
                ApplyRelocTable(d, d.jmprel_va, d.jmprel_sz, d.jmprel_ent, exports, applied,
                                 unresolved);
            }
            // Zero DT_RELASZ/DT_PLTRELSZ so rtld's own self-relocator sees
            // nothing left to do and skips both tables - it runs its own
            // GLOB_DAT/JUMP_SLOT resolution loop with a load-bias consistency
            // check that assumes it's relocating fresh, unresolved entries;
            // finding them already resolved by us trips that check and it
            // calls svcBreak, which is what was hanging every recompiled
            // game at boot despite relocations succeeding.
            if (d.rela_sz_va) mem.Write64(d.rela_sz_va, 0);
            if (d.jmprel_sz_va) mem.Write64(d.jmprel_sz_va, 0);
            LOG_INFO(Core_ARM,
                     "recomp: pre-applied {} relocations ({} unresolved external symbols) for "
                     "module base={:#x}",
                     applied, unresolved, d.mod_base);
        }
    }

    System& system;
    RecompLookupFn lookup{};
    GuestContextView ctx{};
    RecompHostMem bridge{};
    std::atomic<bool> interrupted{false};
    Loader::AppLoader::Modules modules{};
    bool modules_read{false};
    bool rela_applied{false};
    bool explicitly_prepared{false};
    static constexpr size_t kTrail = 32;
    u64 trail[kTrail]{};
    size_t trail_pos{0};

    // Interpreter fallback for PCs the static pass never covered. Built on the
    // first miss rather than up front: most runs never need it, and a JIT per
    // core costs a code cache each.
    Kernel::KProcess* owner_process{};
    // The base interface, not dynarmic's implementation of it: the recompiled
    // path only ever calls the virtual methods, and holding the concrete type
    // here is what forced the library into a build that never runs a JIT.
    ExclusiveMonitor* exclusive_monitor{};
    // Last guest thread run on this core, so a reservation is voided on a real
    // context switch and left alone on an ordinary return to the host.
    Kernel::KThread* last_thread{};
    std::size_t core_index{};
    bool uses_wall_clock{};
#ifndef SUYU_NO_JIT
    std::unique_ptr<ArmDynarmic64> fallback{};
#endif
    bool in_fallback{false};
    bool fallback_unavailable{false};
};

ArmRecomp::ArmRecomp(System& system, bool uses_wall_clock, RecompLookupFn lookup,
                     Kernel::KProcess* process, ExclusiveMonitor* exclusive_monitor,
                     std::size_t core_index)
    : ArmInterface{uses_wall_clock}, impl{std::make_unique<Impl>(system, lookup)} {
    impl->owner_process = process;
    impl->exclusive_monitor = exclusive_monitor;
    impl->core_index = core_index;
    impl->uses_wall_clock = uses_wall_clock;
    // One instance is built per core; the last one torn down prints the run's
    // execution-coverage report.
    g_live_instances.fetch_add(1, std::memory_order_relaxed);
}

ArmRecomp::~ArmRecomp() {
    if (impl->core_index < g_current_pcs.size()) {
        g_current_pcs[impl->core_index].store(0, std::memory_order_relaxed);
    }
    // Report on the *first* instance torn down, not the last. Waiting for the
    // last one means the report is lost whenever anything still holds a
    // reference at shutdown - which happens, and silently costs the whole run's
    // measurement. All per-core instances go down together, so the first is
    // just as complete.
    g_live_instances.fetch_sub(1, std::memory_order_acq_rel);
    static std::once_flag reported;
    std::call_once(reported, [] { ReportRecompCoverage(); });
}

bool PrepareRecompProcess(Kernel::KProcess& process, const RecompModules& modules) {
    const auto prepare = g_recomp_prepare.load(std::memory_order_acquire);
    if (!prepare) return true;
    // KProcess::InitializeInterfaces chooses ArmRecomp for every application
    // core while this lookup is installed. RTTI is disabled in core builds.
    if (!GetRecompLookup() || !process.IsApplication() || !process.Is64Bit() ||
        modules.empty() || modules.begin()->second != "rtld" ||
        modules.begin()->first != GetInteger(process.GetEntryPoint())) {
        LOG_ERROR(Core_ARM, "Static preparation requires an AArch64 rtld entry point");
        return false;
    }
    for (size_t i = 0; i < Hardware::NUM_CPU_CORES; ++i) {
        const auto* cpu = static_cast<ArmRecomp*>(process.GetArmInterface(i));
        if (!cpu || cpu->impl->explicitly_prepared) return false;
    }
    if (!prepare(modules)) return false;
    for (size_t i = 0; i < Hardware::NUM_CPU_CORES; ++i) {
        auto& state = *static_cast<ArmRecomp*>(process.GetArmInterface(i))->impl;
        state.modules = modules;
        state.modules_read = true;
        // Run real rtld relocation. The desktop host pre-relocator substitutes
        // unresolved imports and must never modify this strict session's memory.
        state.rela_applied = true;
        state.explicitly_prepared = true;
    }
    g_counters.RecordModules(modules);
    return true;
}

bool ArmRecomp::EnterFallback() {
    if (impl->fallback_unavailable) {
        return false;
    }
    if (kStrictNoFallback) {
        // Latched, so the caller's own critical log naming the PC is what gets
        // read, and the second thread to arrive does not repeat this one.
        impl->fallback_unavailable = true;
        LOG_CRITICAL(Core_ARM, "recomp: strict mode - refusing to fall back to the JIT");
        return false;
    }
#ifdef SUYU_NO_JIT
    // Built without a dynamic recompiler at all. There is nothing to fall back
    // to, by construction rather than by configuration.
    impl->fallback_unavailable = true;
    LOG_CRITICAL(Core_ARM, "recomp: built without a JIT; uncovered code cannot run");
    return false;
#else
    if (!impl->fallback) {
        if (!impl->owner_process || !impl->exclusive_monitor) {
            impl->fallback_unavailable = true;
            return false;
        }
        impl->fallback = std::make_unique<ArmDynarmic64>(
            impl->system, impl->uses_wall_clock, impl->owner_process,
            static_cast<DynarmicExclusiveMonitor&>(*impl->exclusive_monitor), impl->core_index);
        LOG_WARNING(Core_ARM, "recomp: created JIT fallback for uncovered code");
    }
    impl->in_fallback = true;
    return true;
#endif
}

HaltReason ArmRecomp::RunFallback(Kernel::KThread* thread) {
#ifdef SUYU_NO_JIT
    // Unreachable: EnterFallback never succeeds in this build. Kept so the one
    // call site needs no guard of its own.
    (void)thread;
    return HaltReason::PrefetchAbort;
#else
    // The recompiled context is the single source of truth; the JIT is loaded
    // from it on the way in and drained back on the way out, so every accessor
    // on this interface (SVC arguments, thread context save/restore) keeps
    // working unchanged no matter which engine actually ran.
    impl->ctx.pending_svc = kNoPendingSvc;
    impl->ctx.halted = 0;
    impl->interrupted.store(false, std::memory_order_relaxed);

    Kernel::Svc::ThreadContext tctx{};
    this->GetContext(tctx);
    impl->fallback->SetContext(tctx);
    impl->fallback->SetTpidrroEl0(impl->ctx.tpidrro_el0);

    const HaltReason hr = impl->fallback->RunThread(thread);

    impl->fallback->GetContext(tctx);
    this->SetContext(tctx);
    if (True(hr & HaltReason::SupervisorCall)) {
        impl->ctx.pending_svc = impl->fallback->GetSvcNumber();
    }

    if (True(hr & HaltReason::SupervisorCall)) {
        g_counters.svc_calls.fetch_add(1, std::memory_order_relaxed);
    }

    // Return to recompiled execution as soon as the PC is covered again, so a
    // single uncovered function costs only the time spent inside it.
    if (impl->lookup && impl->lookup(impl->ctx.pc)) {
        impl->in_fallback = false;
        g_counters.jit_to_static.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
#endif
}

HaltReason ArmRecomp::RunThread(Kernel::KThread* thread) {
    // A context switch voids any outstanding reservation. A thread preempted
    // between its LDXR and STXR would otherwise have the STXR succeed against a
    // word another thread changed on this core meanwhile, silently losing an
    // update to a mutex or condition variable and deadlocking the guest.
    //
    // Only on an actual switch: this backend returns to the host constantly
    // (chain budget, SVCs), so clearing on every entry would void reservations
    // the same thread is still in the middle of.
    if (impl->last_thread != thread) {
        impl->last_thread = thread;
        if (impl->exclusive_monitor) {
            impl->exclusive_monitor->ClearExclusive(impl->core_index);
        }
    }
    // Logged once so it is obvious from a log whether the backend was ever
    // entered at all. A run with no errors is otherwise indistinguishable from
    // a run where the guest thread was never scheduled onto it.
    static std::atomic_bool announced{false};
    bool first_run = !announced.exchange(true, std::memory_order_relaxed);
    if (first_run) {
        LOG_INFO(Core_ARM, "ArmRecomp::RunThread entered, pc={:#x}", impl->ctx.pc);
    }
    if (!impl->lookup) {
        LOG_ERROR(Core_ARM, "No recompiled code registered; cannot run thread");
        return HaltReason::BreakLoop;
    }

    impl->RefreshPageTable();
    if (first_run) {
        LOG_INFO(Core_ARM, "ArmRecomp page table ready: entries={} stride={} page_bits={} max={:#x}",
                 impl->bridge.page_entries != nullptr, impl->bridge.page_entry_stride,
                 impl->bridge.page_bits, impl->bridge.address_space_max);
    }

    // Registering every loaded image's base with the host dispatcher is a
    // side effect of this call, not something its return value is used for
    // here - the dispatcher needs it done once before the first lookup, or
    // every image's base stays 0 and every lookup misses.
    if (HasRecompPrepareCallback() && !impl->explicitly_prepared) {
        LOG_ERROR(Core_ARM, "Refusing unprepared static guest execution");
        return HaltReason::BreakLoop;
    }
    // Keep the legacy desktop path isolated. Explicit sessions never enter
    // this process-global lazy protocol or its heuristic relocation path.
    if (!impl->explicitly_prepared) {
        static bool bases_registered = false;
        if (!bases_registered) {
            bases_registered = true;
            impl->ModuleBaseFor(thread, impl->ctx.pc);
        }
    }

    if (!impl->rela_applied) {
        impl->rela_applied = true;
        impl->ApplyAllRelocations(impl->modules);
    }

    // A previous miss handed this thread to the JIT; keep running there until
    // the PC lands back inside recompiled code. The trap sentinel has to be
    // caught before that hand-off as well as inside the dispatch loop below -
    // a thread already in the JIT that calls an unresolved import would
    // otherwise be handed the sentinel address to execute, which is unmapped.
    if (impl->ctx.pc == kUnresolvedImportTrap) {
        impl->ctx.x[0] = 0;
        impl->ctx.pc = impl->ctx.x[30];
        impl->in_fallback = false;
    }
    if (impl->in_fallback) {
        return RunFallback(thread);
    }

    impl->interrupted.store(false, std::memory_order_relaxed);
    impl->ctx.halted = 0;

    while (!impl->ctx.halted) {
        if (impl->core_index < g_current_pcs.size()) {
            g_current_pcs[impl->core_index].store(impl->ctx.pc, std::memory_order_relaxed);
        }
        if (impl->interrupted.load(std::memory_order_relaxed)) {
            return HaltReason::BreakLoop;
        }

        // An SVC parked us last time round; the kernel has now serviced it and
        // resumed, so clear it before continuing.
        if (impl->ctx.pending_svc != kNoPendingSvc) {
            impl->ctx.pending_svc = kNoPendingSvc;
        }

        // A recompiled image is keyed by each block's offset within its own
        // module, because that is all the static pass can know: an NSO's
        // segment header carries the offset inside the module, not the address
        // the loader will map it to, and that address changes per run anyway.
        // The host-side dispatcher (suyu's chained lookup) owns picking which
        // image the PC belongs to and reducing to that image's offset before
        // calling into it - a second offset-based retry here used to guess
        // which image based only on pc-base, but two images can both define a
        // block at the same offset (every module has one at offset 0), so a
        // guess made without knowing which image owns the address silently
        // ran the wrong module's code with no error. Ask with the absolute PC
        // and let the dispatcher own the reduction.
        // Rolling trail of the last few PCs. A wild indirect branch reports
        // only the address it landed on, which says nothing about which block
        // computed it; without the predecessors there is no way to tell a bad
        // GOT read from a bad emitted branch.
        impl->trail[impl->trail_pos++ & (Impl::kTrail - 1)] = impl->ctx.pc;

        if (impl->ctx.pc == kUnresolvedImportTrap) {
            // Landed here via a BLR through a GOT/JUMP_SLOT slot patched by
            // ApplyRelocTable because no definition was found anywhere - most
            // such imports exist for code paths this particular boot never
            // takes, so treat the call as a no-op: return to the caller with
            // a zeroed result register rather than crash the thread. x30 is
            // the guest's own return address (BLR sets it before the branch),
            // exactly as if this were a real function that did nothing.
            static std::atomic<int> trap_count{0};
            if (trap_count.fetch_add(1, std::memory_order_relaxed) < 16) {
                g_counters.unresolved_import_traps.fetch_add(1, std::memory_order_relaxed);
                LOG_ERROR(Core_ARM, "recomp: called through unresolved import (returning to caller {:#x})",
                          impl->ctx.x[30]);
            }
            impl->ctx.x[0] = 0;
            impl->ctx.pc = impl->ctx.x[30];
            continue;
        }

        static std::atomic<u64> early_dispatches{0};
        const u64 early_dispatch = early_dispatches.fetch_add(1, std::memory_order_relaxed);
        if (early_dispatch < 64) {
            LOG_INFO(Core_ARM, "Early static dispatch #{}: pc={:#x}", early_dispatch,
                     impl->ctx.pc);
        }
        RecompBlockFn block = impl->lookup(impl->ctx.pc);
        if (first_run) {
            LOG_INFO(Core_ARM, "First static lookup: pc={:#x}, covered={}", impl->ctx.pc,
                     block != nullptr);
        }
        // Test hook: forces every lookup past the Nth to miss, so the JIT
        // fallback below can be exercised on a title that would otherwise never
        // hit a gap. Unset in normal runs.
        {
            static const char* const force_miss = std::getenv("SUYU_RECOMP_FORCE_MISS_AFTER");
            static std::atomic<int> blocks_run{0};
            if (force_miss) {
                const int n = blocks_run.fetch_add(1, std::memory_order_relaxed);
                const int limit = std::atoi(force_miss);
                // Name the blocks either side of the cutoff. Bisecting on the
                // cutoff tells you which index first breaks the run; this turns
                // that index into the actual guest address to look at.
                if (n >= limit - 4 && n <= limit + 4) {
                    LOG_ERROR(Core_ARM, "recomp: block #{} pc={:#x}", n, impl->ctx.pc);
                }
                if (n >= limit) {
                    block = nullptr;
                }
            }
        }
        // A miss is now recoverable, so it can happen many times per second;
        // the full diagnostic dump is kept for the first few only, where it is
        // still useful for finding which indirect call went uncovered.
        static std::atomic<int> miss_count{0};
        const int miss_index = block ? 0 : miss_count.fetch_add(1, std::memory_order_relaxed);
        if (!block && miss_index < 8) {
            std::string trail;
            const size_t count = std::min<size_t>(impl->trail_pos, Impl::kTrail);
            for (size_t i = 0; i < count; ++i) {
                const u64 p = impl->trail[(impl->trail_pos - count + i) & (Impl::kTrail - 1)];
                trail += fmt::format("{:#x} ", p);
            }
            LOG_ERROR(Core_ARM, "recomp PC trail (oldest first): {}", trail);
            LOG_ERROR(Core_ARM, "recomp regs x16={:#x} x17={:#x} x30={:#x} sp={:#x}",
                      impl->ctx.x[16], impl->ctx.x[17], impl->ctx.x[30], impl->ctx.x[31]);
            LOG_ERROR(Core_ARM, "recomp regs x0={:#x} x15={:#x} x18={:#x} x19={:#x}",
                      impl->ctx.x[0], impl->ctx.x[15], impl->ctx.x[18], impl->ctx.x[19]);
            // The whole file, four per line. A miss is almost always a bad value
            // in some register the previous block computed, and guessing which
            // one to print in advance costs a rebuild per guess.
            for (size_t r = 0; r < 32; r += 4) {
                LOG_ERROR(Core_ARM, "recomp regs x{:<2}={:#018x} x{:<2}={:#018x} x{:<2}={:#018x} x{:<2}={:#018x}",
                          r, impl->ctx.x[r], r + 1, impl->ctx.x[r + 1], r + 2, impl->ctx.x[r + 2],
                          r + 3, impl->ctx.x[r + 3]);
            }
            // Dump guest memory around the registers that look like pointers.
            // A miss caused by a bad *value* and one caused by the wrong data
            // being mapped at the right address look identical from the
            // register file alone.
            for (u32 r : {8u, 22u, 25u}) {
                const u64 p = impl->ctx.x[r];
                if (p < 0x1000 || p > 0x0000'FFFF'FFFF'FFFFULL) {
                    continue;
                }
                std::string dump;
                for (s64 d = -0x20; d < 0x30; d += 4) {
                    dump += fmt::format("{:08x} ", (u32)Impl::HostLoad(impl.get(), p + d, 4));
                }
                LOG_ERROR(Core_ARM, "recomp mem @x{} ({:#x}) [-0x20..+0x30): {}", r, p, dump);
            }
            {
                const u64 mbase = impl->modules.empty() ? 0 : impl->modules.begin()->first;
                // A wide window through .rodata, to diff against the exporter's
                // extracted copy: if the two disagree, the recompiled code is
                // computing correct addresses into memory that holds something
                // other than what was recompiled against.
                for (u64 w = 0x3c00; w < 0x3d80; w += 0x40) {
                    std::string dump;
                    for (u64 i = 0; i < 0x40; i += 4) {
                        dump += fmt::format("{:08x} ", (u32)Impl::HostLoad(impl.get(), mbase + w + i, 4));
                    }
                    LOG_ERROR(Core_ARM, "recomp rodata mod+{:#x}: {}", w, dump);
                }
                for (u64 seg : {0x0ULL, 0x2000ULL, 0x3000ULL}) {
                    std::string dump;
                    for (u64 i = 0; i < 0x40; i += 4) {
                        dump += fmt::format("{:08x} ", Impl::HostLoad(impl.get(), mbase + seg + i, 4));
                    }
                    LOG_ERROR(Core_ARM, "recomp mem mod+{:#x} (base {:#x}): {}", seg, mbase, dump);
                }
            }
        }
        if (!block) {
            // No recompiled block covers this address: an indirect branch into
            // code the static pass never reached. The guest's own instructions
            // are still mapped in guest memory, so hand the thread to a JIT and
            // keep going instead of returning PrefetchAbort - that halt reason
            // makes the kernel suspend the thread for a debugger that is not
            // attached, which is a permanent, silent black-screen hang.
            if (miss_index < 64) {
                LOG_ERROR(Core_ARM, "No recompiled block at PC {:#x}; falling back to JIT",
                          impl->ctx.pc);
            } else {
                LOG_DEBUG(Core_ARM, "No recompiled block at PC {:#x}; falling back to JIT",
                          impl->ctx.pc);
            }
            g_counters.fallback_from_miss.fetch_add(1, std::memory_order_relaxed);
            g_counters.RecordMiss(impl->ctx.pc);
            if (!EnterFallback()) {
                g_counters.no_fallback_available.fetch_add(1, std::memory_order_relaxed);
                LOG_CRITICAL(Core_ARM,
                             "recomp: no JIT fallback available at PC {:#x}; thread cannot "
                             "continue",
                             impl->ctx.pc);
                return HaltReason::PrefetchAbort;
            }
            return RunFallback(thread);
        }

        // Dump periodically: teardown is not guaranteed to run (the emulated
        // process can outlive shutdown), and a run with no report is a run with
        // no measurement. One compare per block against a power-of-two mask.
        const u64 seen = t_blocks.slot->n.load(std::memory_order_relaxed) + 1;
        t_blocks.slot->n.store(seen, std::memory_order_relaxed);
        if ((seen & 0x3FFFFULL) == 0x3FFFFULL) {
            WriteRecompCoverageFile(FormatRecompCoverage());
        }
        // Generated code calls a direct branch's target itself rather than
        // coming back here, so one call below can run a whole chain of blocks.
        // The budget bounds that chain, and what is left of it afterwards says
        // how many blocks actually ran - without which every count here would
        // report chains rather than blocks.
        impl->ctx.chain_budget = kChainBudget;
        if (first_run) {
            LOG_INFO(Core_ARM, "Calling first signed static block at pc={:#x}", impl->ctx.pc);
        }
        block(&impl->ctx);
        if (early_dispatch < 64) {
            LOG_INFO(Core_ARM, "Early static return #{}: pc={:#x}, svc={}, halted={}",
                     early_dispatch, impl->ctx.pc, impl->ctx.pending_svc, impl->ctx.halted);
        }
        if (first_run) {
            LOG_INFO(Core_ARM, "First signed static block returned: pc={:#x}, svc={}, halted={}",
                     impl->ctx.pc, impl->ctx.pending_svc, impl->ctx.halted);
            first_run = false;
        }
        {
            const int spent = kChainBudget - impl->ctx.chain_budget;
            u64 after = seen;
            if (spent > 1) {
                after += static_cast<u64>(spent - 1);
                t_blocks.slot->n.store(after, std::memory_order_relaxed);
            }
            if (kSamplePc && (seen >> 18) != (after >> 18)) {
                g_counters.RecordSample(impl->ctx.pc);
            }
        }

        if (impl->ctx.halted == kHaltIcIvau) {
            if (!g_code_guard_ready.load(std::memory_order_acquire)) {
                LOG_CRITICAL(Core_ARM, "recomp: IC IVAU requires guard-v2 host and all guarded modules");
                std::abort();
            }
            const u64 address = impl->ctx.pending_svc;
            impl->ctx.pending_svc = kNoPendingSvc;
            // The guest just synchronized new instructions, so nothing a block
            // verified before this point can still be assumed. Bumped here as
            // well as in InvalidateCacheRange so the guarantee does not depend
            // on which engine GetArmInterface hands back.
            BumpGuardGeneration();
            for (size_t core = 0; core < Hardware::NUM_CPU_CORES; ++core) {
                if (auto* cpu = thread->GetOwnerProcess()->GetArmInterface(core)) cpu->InvalidateCacheRange(address, 64);
            }
            impl->ctx.pc += 4;
            impl->ctx.halted = 0;
            continue;
        }
        if (impl->ctx.halted == kHaltBreakpoint) {
            return HaltReason::InstructionBreakpoint;
        }

        // The block stopped on an instruction the decoder has no translation
        // for, having parked the PC on that instruction. Running it on the JIT
        // instead keeps guest state exact: the alternative the generated code
        // used to take - step over it and zero x0 - silently produced a
        // plausible-looking null that only surfaced as a crash much later, in
        // whatever code eventually dereferenced it.
        if (impl->ctx.halted == kHaltUnhandled) {
            if ((static_cast<u32>(Impl::HostLoad(impl.get(), impl->ctx.pc, 4)) & 0xffffffe0U) == 0xd50b7520U) {
                LOG_CRITICAL(Core_ARM, "recomp: legacy unguarded IC IVAU refused at {:#x}", impl->ctx.pc);
                std::abort();
            }
            impl->ctx.halted = 0;
            // The generated code knows the encoding but cannot pass it back
            // through the halt contract, so read it out of guest memory - the
            // PC is parked exactly on the offending instruction. This is what
            // ranks the missing opcodes by execution rather than by how often
            // they appear in the image.
            g_counters.fallback_from_unhandled.fetch_add(1, std::memory_order_relaxed);
            g_counters.RecordUnhandled(
                static_cast<u32>(Impl::HostLoad(impl.get(), impl->ctx.pc, 4)));
            static std::atomic<int> unhandled_count{0};
            if (unhandled_count.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_WARNING(Core_ARM, "recomp: unimplemented opcode at {:#x}; running on JIT",
                            impl->ctx.pc);
            }
            if (!EnterFallback()) {
                g_counters.no_fallback_available.fetch_add(1, std::memory_order_relaxed);
                LOG_CRITICAL(Core_ARM, "recomp: unimplemented opcode at {:#x} and no JIT fallback",
                             impl->ctx.pc);
                return HaltReason::PrefetchAbort;
            }
            return RunFallback(thread);
        }

        if (impl->ctx.pending_svc != kNoPendingSvc) {
            // Log every SVC call from rtld (first few hundred only to avoid spam)
            LOG_TRACE(Core_ARM, "recomp SVC {} at pc={:#x} x0={:#x} x1={:#x} x2={:#x} x3={:#x}",
                      impl->ctx.pending_svc, impl->ctx.pc, impl->ctx.x[0], impl->ctx.x[1],
                      impl->ctx.x[2], impl->ctx.x[3]);
            g_counters.svc_calls.fetch_add(1, std::memory_order_relaxed);
            g_counters.RecordSvc(static_cast<u32>(impl->ctx.pending_svc));
            return HaltReason::SupervisorCall;
        }
    }

    return HaltReason::BreakLoop;
}

HaltReason ArmRecomp::StepThread(Kernel::KThread* thread) {
    // A context switch voids any outstanding reservation. A thread preempted
    // between its LDXR and STXR would otherwise have the STXR succeed against a
    // word another thread changed on this core meanwhile, silently losing an
    // update to a mutex or condition variable and deadlocking the guest.
    //
    // Only on an actual switch: this backend returns to the host constantly
    // (chain budget, SVCs), so clearing on every entry would void reservations
    // the same thread is still in the middle of.
    if (impl->last_thread != thread) {
        impl->last_thread = thread;
        if (impl->exclusive_monitor) {
            impl->exclusive_monitor->ClearExclusive(impl->core_index);
        }
    }
    // Block granularity is the finest this backend can step: recompiled blocks
    // are straight-line C with no per-instruction re-entry point.
    if (!impl->lookup) {
        return HaltReason::BreakLoop;
    }
    const RecompBlockFn block = impl->lookup(impl->ctx.pc);
    if (!block) {
        return HaltReason::PrefetchAbort;
    }
    impl->ctx.halted = 0;
    impl->ctx.pending_svc = kNoPendingSvc;
    impl->ctx.chain_budget = 1;
    block(&impl->ctx);
    if (impl->ctx.halted == kHaltIcIvau) {
        if (!g_code_guard_ready.load(std::memory_order_acquire)) {
            LOG_CRITICAL(Core_ARM, "recomp: IC IVAU requires guard-v2 host and all guarded modules");
            std::abort();
        }
        const u64 address = impl->ctx.pending_svc;
        impl->ctx.pending_svc = kNoPendingSvc;
        BumpGuardGeneration();
        for (size_t core = 0; core < Hardware::NUM_CPU_CORES; ++core) {
            if (auto* cpu = thread->GetOwnerProcess()->GetArmInterface(core)) cpu->InvalidateCacheRange(address, 64);
        }
        impl->ctx.pc += 4;
        impl->ctx.halted = 0;
        return HaltReason::StepThread;
    }
    if (impl->ctx.halted == kHaltBreakpoint) {
        return HaltReason::InstructionBreakpoint;
    }
    if (impl->ctx.halted == kHaltUnhandled &&
        (static_cast<u32>(Impl::HostLoad(impl.get(), impl->ctx.pc, 4)) & 0xffffffe0U) == 0xd50b7520U) {
        LOG_CRITICAL(Core_ARM, "recomp: legacy unguarded IC IVAU refused while stepping at {:#x}", impl->ctx.pc);
        std::abort();
    }
    if (impl->ctx.pending_svc != kNoPendingSvc) {
        return HaltReason::SupervisorCall;
    }
    return HaltReason::StepThread;
}

void ArmRecomp::ClearInstructionCache() {
#ifndef SUYU_NO_JIT
    if (impl->fallback) impl->fallback->ClearInstructionCache();
#endif
    // A whole-icache flush is the broadest statement that guest code may have
    // changed, so it retires every block's cached verification. Blocks used to
    // re-read their bytes on every entry, which made this a no-op for them.
    BumpGuardGeneration();
}

void ArmRecomp::InvalidateCacheRange(u64 addr, std::size_t size) {
#ifndef SUYU_NO_JIT
    if (impl->fallback) impl->fallback->InvalidateCacheRange(addr, size);
#endif
    // Whoever called this is telling us the bytes behind some guest address may
    // have changed. Blocks cache the generation they last verified at, so moving
    // it is what makes them look again. Deliberately global rather than ranged:
    // the counter is read once per block entry, and narrowing it to the affected
    // range would cost a lookup on the hot path to save re-verifying blocks in a
    // situation that is rare to begin with.
    BumpGuardGeneration();
}

void ArmRecomp::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    std::memset(&ctx, 0, sizeof(ctx));
    for (size_t i = 0; i < 29; ++i) {
        ctx.r[i] = impl->ctx.x[i];
    }
    ctx.fp = impl->ctx.x[29];
    ctx.lr = impl->ctx.x[30];
    ctx.sp = impl->ctx.x[31];
    ctx.pc = impl->ctx.pc;
    ctx.pstate = (static_cast<u32>(impl->ctx.n) << 31) |
                 (static_cast<u32>(impl->ctx.z) << 30) |
                 (static_cast<u32>(impl->ctx.c) << 29) |
                 (static_cast<u32>(impl->ctx.v) << 28);
    // u128 here is a pair of 64-bit halves, matching how the generated
    // context stores each vector register.
    for (size_t i = 0; i < 32; ++i) {
        ctx.v[i][0] = impl->ctx.vreg[i][0];
        ctx.v[i][1] = impl->ctx.vreg[i][1];
    }
    ctx.fpcr = static_cast<u32>(impl->ctx.fpcr);
    ctx.fpsr = static_cast<u32>(impl->ctx.fpsr);
    ctx.tpidr = impl->ctx.tpidr_el0;
}

void ArmRecomp::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    for (size_t i = 0; i < 29; ++i) {
        impl->ctx.x[i] = ctx.r[i];
    }
    impl->ctx.x[29] = ctx.fp;
    impl->ctx.x[30] = ctx.lr;
    impl->ctx.x[31] = ctx.sp;
    impl->ctx.pc = ctx.pc;
    impl->ctx.n = static_cast<u8>((ctx.pstate >> 31) & 1);
    impl->ctx.z = static_cast<u8>((ctx.pstate >> 30) & 1);
    impl->ctx.c = static_cast<u8>((ctx.pstate >> 29) & 1);
    impl->ctx.v = static_cast<u8>((ctx.pstate >> 28) & 1);
    for (size_t i = 0; i < 32; ++i) {
        impl->ctx.vreg[i][0] = ctx.v[i][0];
        impl->ctx.vreg[i][1] = ctx.v[i][1];
    }
    impl->ctx.fpcr = ctx.fpcr;
    impl->ctx.fpsr = ctx.fpsr;
    // Only the guest-owned thread pointer travels in ThreadContext. The
    // read-only one is republished separately by PhysicalCore::LoadContext
    // on every switch-in, so writing it from here would overwrite the
    // kernel's TLS pointer with the guest's.
    impl->ctx.tpidr_el0 = ctx.tpidr;
}

void ArmRecomp::SetTpidrroEl0(u64 value) {
    // The emitted MRS handler for TPIDRRO_EL0 reads this out of the guest
    // context, so it has to land there and nowhere else: writing it into
    // tpidr_el0 (as this used to) destroyed the guest's own thread pointer on
    // every context switch and made the guest emit its IPC header outside the
    // TLS region the kernel parses it from.
    impl->ctx.tpidrro_el0 = value;
}

void ArmRecomp::GetSvcArguments(std::span<uint64_t, 8> args) const {
    for (size_t i = 0; i < 8; ++i) {
        args[i] = impl->ctx.x[i];
    }
}

void ArmRecomp::SetSvcArguments(std::span<const uint64_t, 8> args) {
    for (size_t i = 0; i < 8; ++i) {
        impl->ctx.x[i] = args[i];
    }
}

u32 ArmRecomp::GetSvcNumber() const {
    return static_cast<u32>(impl->ctx.pending_svc);
}

void ArmRecomp::SignalInterrupt(Kernel::KThread* thread) {
    impl->interrupted.store(true, std::memory_order_relaxed);
    // While the JIT is running this thread it is the one that has to be woken;
    // the flag above is only read by the recompiled dispatch loop.
#ifndef SUYU_NO_JIT
    if (impl->fallback) {
        impl->fallback->SignalInterrupt(thread);
    }
#else
    (void)thread;
#endif
}

const Kernel::DebugWatchpoint* ArmRecomp::HaltedWatchpoint() const {
    return nullptr;
}

void ArmRecomp::RewindBreakpointInstruction() {
    // No breakpoint patching in statically recompiled code.
}

} // namespace Core
