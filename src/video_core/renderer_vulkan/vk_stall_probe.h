// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>

#include "common/common_types.h"
#include "common/logging/log.h"

namespace Vulkan::StallProbe {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SUYU_STALL_PROBE");
        return value && *value && *value != '0';
    }();
    return enabled;
}

inline u64 Now() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline std::atomic<u64> build_wait_ns{0}, build_wait_count{0};
inline std::atomic<u64> acquire_ns{0}, present_ns{0}, frame_wait_ns{0};
inline std::atomic<u64> gpu_idle_ns{0}, draw_ns{0}, draw_count{0}, sched_wait_ns{0};
inline std::atomic<u64> shader_translate_ns{0}, shader_emit_ns{0}, shader_count{0};
inline std::atomic<u64> pipeline_build_ns{0}, pipeline_build_count{0};
inline std::atomic<u64> draw_flush_ns{0}, pipeline_lookup_ns{0}, pipeline_configure_ns{0},
    draw_record_ns{0};

// The disabled path performs neither clock reads nor atomic counter updates.
// Counts and durations are published together on scope completion. These are
// process-wide sums from multiple threads; they can overlap and a scope can
// straddle a reporting interval. They are NOT additive frame wall-time buckets.
class Accum {
public:
    explicit Accum(std::atomic<u64>& sink, std::atomic<u64>* count = nullptr)
        : sink_{sink}, count_{count}, enabled_{Enabled()}, start_{enabled_ ? Now() : 0} {}
    ~Accum() {
        if (!enabled_) return;
        sink_.fetch_add(Now() - start_, std::memory_order_relaxed);
        if (count_) count_->fetch_add(1, std::memory_order_relaxed);
    }
    Accum(const Accum&) = delete;
    Accum& operator=(const Accum&) = delete;
private:
    std::atomic<u64>& sink_;
    std::atomic<u64>* count_;
    bool enabled_;
    u64 start_;
};

inline void ReportFrame() {
    if (!Enabled()) return;
    // Present normally has one caller; TLS also avoids a plain-data race if
    // separate presentation threads report concurrently.
    static thread_local u64 last_present = 0;
    const u64 now = Now();
    const u64 build = build_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 builds = build_wait_count.exchange(0, std::memory_order_relaxed);
    const u64 acquire = acquire_ns.exchange(0, std::memory_order_relaxed);
    const u64 present = present_ns.exchange(0, std::memory_order_relaxed);
    const u64 framew = frame_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 idle = gpu_idle_ns.exchange(0, std::memory_order_relaxed);
    const u64 draw = draw_ns.exchange(0, std::memory_order_relaxed);
    const u64 draws = draw_count.exchange(0, std::memory_order_relaxed);
    const u64 schedw = sched_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 translate = shader_translate_ns.exchange(0, std::memory_order_relaxed);
    const u64 emit = shader_emit_ns.exchange(0, std::memory_order_relaxed);
    const u64 shaders = shader_count.exchange(0, std::memory_order_relaxed);
    const u64 pipeline_build = pipeline_build_ns.exchange(0, std::memory_order_relaxed);
    const u64 pipeline_builds = pipeline_build_count.exchange(0, std::memory_order_relaxed);
    const u64 flush = draw_flush_ns.exchange(0, std::memory_order_relaxed);
    const u64 lookup = pipeline_lookup_ns.exchange(0, std::memory_order_relaxed);
    const u64 configure = pipeline_configure_ns.exchange(0, std::memory_order_relaxed);
    const u64 record = draw_record_ns.exchange(0, std::memory_order_relaxed);
    if (last_present && now - last_present > 100'000'000ULL) {
        const auto milliseconds = [](u64 ns) { return static_cast<double>(ns) * 1.0e-6; };
        LOG_INFO(Render_Vulkan,
                 "STALL present_interval={:.1f}ms completed_scope_totals(overlap_possible): "
                 "gpu_idle={:.1f}ms draw={:.1f}ms(n={}) sched_wait={:.1f}ms "
                 "build_wait={:.1f}ms(n={}) shader_translate={:.1f}ms shader_emit={:.1f}ms(n={}) "
                 "pipeline_build={:.1f}ms(n={}) draw_flush={:.1f}ms lookup={:.1f}ms "
                 "configure={:.1f}ms record={:.1f}ms acquire={:.1f}ms present={:.1f}ms "
                 "frame_wait={:.1f}ms",
                 milliseconds(now - last_present), milliseconds(idle), milliseconds(draw), draws,
                 milliseconds(schedw), milliseconds(build), builds, milliseconds(translate),
                 milliseconds(emit), shaders, milliseconds(pipeline_build), pipeline_builds,
                 milliseconds(flush), milliseconds(lookup), milliseconds(configure),
                 milliseconds(record), milliseconds(acquire), milliseconds(present),
                 milliseconds(framew));
    }
    last_present = now;
}

} // namespace Vulkan::StallProbe
