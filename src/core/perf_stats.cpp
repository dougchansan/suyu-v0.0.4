// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>
#include <fmt/chrono.h>
#include <fmt/ranges.h>
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/arm/recomp/aot_trace.h"
#include "core/perf_stats.h"

using namespace std::chrono_literals;
using DoubleSecs = std::chrono::duration<double, std::chrono::seconds::period>;
using std::chrono::duration_cast;
using std::chrono::microseconds;

// Purposefully ignore the first five frames, as there's a significant amount of overhead in
// booting that we shouldn't account for
constexpr std::size_t IgnoreFrames = 5;

namespace Core {

PerfStats::PerfStats(u64 title_id_)
    : aot_session{Aot::CaptureSample()}, title_id{title_id_} {
    // Opt-in: normal runs perform no per-frame trace I/O or counter sampling.
    const auto* trace_path = std::getenv("SUYU_AOT_TRACE");
    if (!trace_path || !*trace_path || title_id == 0) {
        return;
    }
    const auto env = [](const char* name) -> std::string {
        const auto* value = std::getenv(name);
        return value ? value : "";
    };
    aot_trace = std::make_unique<Aot::TraceWriter>(trace_path, Aot::TraceMetadata{
        title_id, env("SUYU_AOT_BUILD_ID"), env("SUYU_AOT_WORKLOAD_ID"),
        env("SUYU_AOT_CONTENT_ID"), env("SUYU_AOT_CONFIG_ID"), env("SUYU_AOT_MACHINE_ID")});
    if (!aot_trace->Good()) {
        std::fputs("AOT trace could not be opened; no benchmark evidence will be recorded.\n",
                   stderr);
        aot_trace.reset();
    }
}

PerfStats::~PerfStats() {
    if (aot_trace) {
        aot_trace->Finish(aot_session.Observe(Aot::CaptureSample()));
        if (!aot_trace->Good()) {
            std::fputs("AOT trace write failed; discard this benchmark trace.\n", stderr);
        }
    }
    if (!Settings::values.record_frame_times || title_id == 0 || current_index <= IgnoreFrames) {
        return;
    }

    const std::time_t t = std::time(nullptr);
    std::ostringstream stream;
    std::copy(perf_history.begin() + IgnoreFrames, perf_history.begin() + current_index,
              std::ostream_iterator<double>(stream, "\n"));

    const auto path = Common::FS::GetSuyuPath(Common::FS::SuyuPath::LogDir);
    // %F Date format expanded is "%Y-%m-%d"
    const auto filename = fmt::format("{}_{:016X}.csv",
        [&] {
            std::ostringstream oss;
            oss << std::put_time(std::localtime(&t), "%F-%H-%M");
            return oss.str();
        }(),
        title_id);

    const auto filepath = path / filename;

    if (Common::FS::CreateParentDir(filepath)) {
        Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Write,
                                Common::FS::FileType::TextFile);
        void(file.WriteString(stream.str()));
    }
}

void PerfStats::BeginSystemFrame() {
    std::scoped_lock lock{object_mutex};

    frame_begin = Clock::now();
}

void PerfStats::EndSystemFrame() {
    std::scoped_lock lock{object_mutex};

    auto frame_end = Clock::now();
    const auto frame_time = frame_end - frame_begin;
    if (current_index < perf_history.size()) {
        perf_history[current_index++] =
            std::chrono::duration<double, std::milli>(frame_time).count();
    }
    accumulated_frametime += frame_time;
    system_frames += 1;

    previous_frame_length = frame_end - previous_frame_end;
    previous_frame_end = frame_end;
    if (aot_trace) {
        aot_session.Observe(Aot::CaptureSample(), true);
        aot_trace->Frame(std::chrono::duration<double, std::milli>(frame_time).count(),
                         std::chrono::duration<double, std::milli>(previous_frame_length).count());
    }
}

void PerfStats::EndGameFrame() {
    game_frames.fetch_add(1, std::memory_order_relaxed);
}

double PerfStats::GetMeanFrametime() const {
    std::scoped_lock lock{object_mutex};

    if (current_index <= IgnoreFrames) {
        return 0;
    }

    const double sum = std::accumulate(perf_history.begin() + IgnoreFrames,
                                       perf_history.begin() + current_index, 0.0);
    return sum / static_cast<double>(current_index - IgnoreFrames);
}

PerfStatsResults PerfStats::GetAndResetStats(microseconds current_system_time_us) {
    std::scoped_lock lock{object_mutex};

    const auto now = Clock::now();
    // Walltime elapsed since stats were reset
    const auto interval = duration_cast<DoubleSecs>(now - reset_point).count();

    // An exchange cannot discard a frame increment racing with the reset.
    const auto current_frames = game_frames.exchange(0, std::memory_order_relaxed);
    const auto rates = Aot::CalculateFrameRates(
        interval, system_frames, current_frames,
        duration_cast<DoubleSecs>(accumulated_frametime).count(),
        duration_cast<DoubleSecs>(current_system_time_us - reset_point_system_us).count());
    const auto current_fps = rates.game_fps;
    const PerfStatsResults results{
        .system_fps = rates.system_fps,
        .average_game_fps = (current_fps + previous_fps) / 2.0,
        .frametime = rates.active_seconds_per_system_frame,
        .emulation_speed = rates.emulation_speed,
        .game_frames = current_frames,
        .aot = aot_session.Observe(Aot::CaptureSample()),
    };

    // Reset counters
    reset_point = now;
    reset_point_system_us = current_system_time_us;
    accumulated_frametime = Clock::duration::zero();
    system_frames = 0;
    previous_fps = current_fps;

    return results;
}

double PerfStats::GetLastFrameTimeScale() const {
    std::scoped_lock lock{object_mutex};

    constexpr double FRAME_LENGTH = 1.0 / 60;
    return duration_cast<DoubleSecs>(previous_frame_length).count() / FRAME_LENGTH;
}

void SpeedLimiter::DoSpeedLimiting(microseconds current_system_time_us) {
    if (Settings::values.use_multi_core.GetValue() ||
        !Settings::values.use_speed_limit.GetValue()) {
        return;
    }

    auto now = Clock::now();

    const double sleep_scale = Settings::SpeedLimit() / 100.0;

    // Max lag caused by slow frames. Shouldn't be more than the length of a frame at the current
    // speed percent or it will clamp too much and prevent this from properly limiting to that
    // percent. High values means it'll take longer after a slow frame to recover and start
    // limiting
    const microseconds max_lag_time_us = duration_cast<microseconds>(
        std::chrono::duration<double, std::chrono::microseconds::period>(25ms / sleep_scale));
    speed_limiting_delta_err += duration_cast<microseconds>(
        std::chrono::duration<double, std::chrono::microseconds::period>(
            (current_system_time_us - previous_system_time_us) / sleep_scale));
    speed_limiting_delta_err -= duration_cast<microseconds>(now - previous_walltime);
    speed_limiting_delta_err =
        std::clamp(speed_limiting_delta_err, -max_lag_time_us, max_lag_time_us);

    if (speed_limiting_delta_err > microseconds::zero()) {
        std::this_thread::sleep_for(speed_limiting_delta_err);
        auto now_after_sleep = Clock::now();
        speed_limiting_delta_err -= duration_cast<microseconds>(now_after_sleep - now);
        now = now_after_sleep;
    }

    previous_system_time_us = current_system_time_us;
    previous_walltime = now;
}

} // namespace Core
