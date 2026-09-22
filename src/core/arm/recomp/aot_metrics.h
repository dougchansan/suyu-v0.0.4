// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>
#include <cstdint>
#include <string_view>
#include "core/arm/recomp/recomp_stats.h"

namespace Core::Aot {

struct Sample {
    RecompLiveStats live{};
    RecompExecutionStats execution{};
    bool code_guard_ready{};
};

// The producer samples atomics independently. This is an observational snapshot,
// not a synchronization primitive or proof of what unobserved code executed.
inline Sample CaptureSample() {
    return {GetRecompLiveStats(), GetRecompExecutionStats(), IsRecompCodeGuardReady()};
}

enum class Backend { NonAot, HybridAllowed, RuntimeStrict, CompiledNoJit };

constexpr Backend Classify(const RecompLiveStats& live) {
    if (!live.backend_active) {
        return Backend::NonAot; // Do not mislabel NCE/interpreters as JIT.
    }
    if (!live.jit_available) {
        return Backend::CompiledNoJit;
    }
    return live.strict_mode ? Backend::RuntimeStrict : Backend::HybridAllowed;
}

constexpr std::string_view Name(Backend backend) {
    switch (backend) {
    case Backend::NonAot:
        return "non_aot";
    case Backend::HybridAllowed:
        return "aot_hybrid_allowed";
    case Backend::RuntimeStrict:
        return "aot_runtime_strict";
    case Backend::CompiledNoJit:
        return "aot_no_jit_build";
    }
    return "unknown";
}

struct Report {
    RecompExecutionStats counters{}; // Delta from this PerfStats session's baseline.
    std::uint64_t jit_transitions{};
    Backend current_backend{Backend::NonAot};
    bool counters_valid{true};
    bool observed_aot{};
    bool observed_non_aot_frame{};
    bool all_aot_samples_strict{true};
    bool all_aot_samples_guarded{true};
    bool jit_compiled_out{};
    bool diagnostic_cutoff{};

    // This is a conservative workload gate, not an emulator correctness proof.
    // An empty run, hybrid-capable run, code miss, unsupported instruction,
    // diagnostic cutoff, counter reset or unguarded image cannot pass.
    bool StrictWorkloadObserved() const {
        return observed_aot && !observed_non_aot_frame && counters_valid &&
               all_aot_samples_strict && all_aot_samples_guarded && !diagnostic_cutoff &&
               counters.blocks != 0 && jit_transitions == 0 && counters.lookup_misses == 0 &&
               counters.unhandled == 0 && counters.no_fallback == 0;
    }
};

// Owned by a single session, and externally synchronized by PerfStats. Global
// counters are never reset: a HUD reader cannot consume another reader's data.
class Session {
public:
    explicit Session(const Sample& initial)
        : baseline{initial}, previous{initial} {
        report.jit_compiled_out = !initial.live.jit_available;
        Observe(initial); // Latch initial hybrid/unguarded/diagnostic state, too.
    }

    Report Observe(const Sample& now, bool completed_system_frame = false) {
        report.counters_valid &= Monotonic(previous, now);
        report.counters = Difference(baseline.execution, now.execution);
        report.jit_transitions = Delta(baseline.live.jit_transitions, now.live.jit_transitions);
        report.current_backend = Classify(now.live);
        report.jit_compiled_out &= !now.live.jit_available;
        report.diagnostic_cutoff |= now.live.forced_cutoff_pc != 0 ||
                                    now.live.forced_cutoff_blocks != 0;
        if (now.live.backend_active) {
            report.observed_aot = true;
            report.all_aot_samples_strict &= now.live.strict_mode || !now.live.jit_available;
            report.all_aot_samples_guarded &= now.code_guard_ready;
        } else if (completed_system_frame) {
            report.observed_non_aot_frame = true;
        }
        previous = now;
        return report;
    }

    const Report& Current() const {
        return report;
    }

private:
    static constexpr std::uint64_t Delta(std::uint64_t before, std::uint64_t after) {
        return after >= before ? after - before : 0;
    }

    static constexpr RecompExecutionStats Difference(const RecompExecutionStats& before,
                                                     const RecompExecutionStats& after) {
        return {Delta(before.blocks, after.blocks), Delta(before.svc_calls, after.svc_calls),
                Delta(before.lookup_misses, after.lookup_misses),
                Delta(before.unhandled, after.unhandled),
                Delta(before.no_fallback, after.no_fallback)};
    }

    static constexpr bool Monotonic(const Sample& before, const Sample& after) {
        return after.execution.blocks >= before.execution.blocks &&
               after.execution.svc_calls >= before.execution.svc_calls &&
               after.execution.lookup_misses >= before.execution.lookup_misses &&
               after.execution.unhandled >= before.execution.unhandled &&
               after.execution.no_fallback >= before.execution.no_fallback &&
               after.live.jit_transitions >= before.live.jit_transitions;
    }

    Sample baseline;
    Sample previous;
    Report report;
};

struct FrameRates {
    double system_fps{};
    double game_fps{};
    double active_seconds_per_system_frame{};
    double emulation_speed{};
};

// Shared by the production stats path and standalone tests. A zero-frame or
// zero-duration observation is not allowed to create NaN/Inf in HUDs or traces.
inline FrameRates CalculateFrameRates(double wall_seconds, std::uint64_t system_frames,
                                     std::uint64_t game_frames, double active_seconds,
                                     double emulated_seconds) {
    if (!std::isfinite(wall_seconds) || wall_seconds <= 0 ||
        !std::isfinite(active_seconds) || active_seconds < 0 ||
        !std::isfinite(emulated_seconds)) {
        return {};
    }
    const FrameRates result{static_cast<double>(system_frames) / wall_seconds,
            static_cast<double>(game_frames) / wall_seconds,
            system_frames == 0 ? 0 : active_seconds / static_cast<double>(system_frames),
            emulated_seconds < 0 ? 0 : emulated_seconds / wall_seconds};
    if (!std::isfinite(result.system_fps) || !std::isfinite(result.game_fps) ||
        !std::isfinite(result.active_seconds_per_system_frame) ||
        !std::isfinite(result.emulation_speed)) {
        return {};
    }
    return result;
}

} // namespace Core::Aot
