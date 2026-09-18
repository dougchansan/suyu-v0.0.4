// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sub-second attribution for frame stalls.
//
// A 1 Hz fps counter says a second went missing; a 3 s `sample` is too coarse
// to say where. This times the specific places a frame can block - waiting on a
// pipeline build, acquiring a swapchain image, the present call itself, waiting
// for a frame's resources - and prints the split for any frame that overran, so
// a freeze can be attributed instead of guessed at.
//
// Off unless SUYU_STALL_PROBE is set, so a normal run pays nothing but an
// already-predicted branch.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>

#include "common/common_types.h"
#include "common/logging/log.h"

namespace Vulkan::StallProbe {

inline bool Enabled() {
    static const bool enabled = std::getenv("SUYU_STALL_PROBE") != nullptr;
    return enabled;
}

inline u64 Now() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

inline std::atomic<u64> build_wait_ns{0};
inline std::atomic<u64> build_wait_count{0};
inline std::atomic<u64> acquire_ns{0};
inline std::atomic<u64> present_ns{0};
inline std::atomic<u64> frame_wait_ns{0};

// Round 2: the first pass showed ~100% of stall time outside every GPU-side
// wait, so these split what is left. gpu_idle is the GPU thread blocked with an
// empty command queue - if that owns a stall, the guest never submitted the
// work and the wall is upstream of video_core entirely.
inline std::atomic<u64> gpu_idle_ns{0};
inline std::atomic<u64> draw_ns{0};
inline std::atomic<u64> draw_count{0};
inline std::atomic<u64> sched_wait_ns{0};

// Adds the lifetime of the scope to a counter. Counters are summed across
// threads, so a total can exceed the frame it is reported against - what
// matters is which bucket is large, not that they add up.
class Accum {
public:
    explicit Accum(std::atomic<u64>& sink_) : sink{sink_}, start{Enabled() ? Now() : 0} {}
    ~Accum() {
        if (start != 0) {
            sink.fetch_add(Now() - start, std::memory_order_relaxed);
        }
    }
    Accum(const Accum&) = delete;
    Accum& operator=(const Accum&) = delete;

private:
    std::atomic<u64>& sink;
    u64 start;
};

// Called once per present. Reports only frames that overran, so the log holds
// the freezes rather than 60 lines a second of healthy frames.
inline void ReportFrame() {
    if (!Enabled()) {
        return;
    }
    static u64 last_present = 0;
    const u64 now = Now();

    const u64 build = build_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 builds = build_wait_count.exchange(0, std::memory_order_relaxed);
    const u64 acquire = acquire_ns.exchange(0, std::memory_order_relaxed);
    const u64 present = present_ns.exchange(0, std::memory_order_relaxed);
    const u64 framew = frame_wait_ns.exchange(0, std::memory_order_relaxed);
    const u64 gpuidle = gpu_idle_ns.exchange(0, std::memory_order_relaxed);
    const u64 draw = draw_ns.exchange(0, std::memory_order_relaxed);
    const u64 draws = draw_count.exchange(0, std::memory_order_relaxed);
    const u64 schedw = sched_wait_ns.exchange(0, std::memory_order_relaxed);

    if (last_present != 0) {
        const u64 frame = now - last_present;
        if (frame > 100'000'000ULL) { // only frames over 100 ms
            const double ms = 1.0e-6;
            const u64 known = build + acquire + present + framew;
            LOG_INFO(Render_Vulkan,
                     "STALL frame={:.1f}ms gpu_idle={:.1f}ms draw={:.1f}ms(n={}) "
                     "sched_wait={:.1f}ms build_wait={:.1f}ms(n={}) acquire={:.1f}ms "
                     "present={:.1f}ms frame_wait={:.1f}ms unattributed={:.1f}ms",
                     frame * ms, gpuidle * ms, draw * ms, draws, schedw * ms, build * ms, builds,
                     acquire * ms, present * ms, framew * ms,
                     (frame > known ? frame - known : 0) * ms);
        }
    }
    last_present = now;
}

} // namespace Vulkan::StallProbe
