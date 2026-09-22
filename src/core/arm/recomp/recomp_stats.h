// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>

// The telemetry API has no ArmInterface or generated-image dependencies. This
// lets frontends and contract tests consume the real declarations directly.
namespace Core {

/// Existing process-wide runtime observations, not instruction/time coverage.
/// `jit_transitions` retains its legacy API name. The current producer counts
/// lookup-miss + unhandled-instruction fallback attempts, including attempts
/// that strict/no-JIT policy refuses; it is not retired JIT work.
struct RecompLiveStats {
    std::uint64_t static_blocks;      ///< blocks executed from recompiled images
    std::uint64_t jit_transitions;    ///< legacy fallback-attempt counter
    std::uint64_t forced_cutoff_pc;   ///< diagnostic static-block cutoff handoff PC
    std::uint64_t forced_cutoff_blocks;
    bool backend_active;    ///< ArmRecomp is the CPU for this process
    bool jit_available;     ///< false when built without a dynamic recompiler
    /// No JIT fallback is permitted: uncovered code stops execution rather than
    /// handing off. This is what separates a "suyu static AOT" run from a
    /// "Hybrid AOT + JIT" one - both execute recompiled code, but only the
    /// hybrid one is allowed to leave it - so the frontend cannot name the
    /// running backend without it.
    bool strict_mode;
};
RecompLiveStats GetRecompLiveStats();

// Process-wide monotonically accumulated diagnostic counters. Fields are sampled
// independently; callers may subtract a stopped-session baseline.
struct RecompExecutionStats {
    std::uint64_t blocks{};
    std::uint64_t svc_calls{};
    std::uint64_t lookup_misses{};
    std::uint64_t unhandled{};
    std::uint64_t no_fallback{};
};
RecompExecutionStats GetRecompExecutionStats();
std::array<std::uint64_t, 4> GetRecompCurrentPcs();

bool IsRecompCodeGuardReady();

} // namespace Core
