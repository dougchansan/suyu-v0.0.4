// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "common/settings.h"
#include "core/perf_stats.h"

namespace {
Core::Aot::Sample sample{{0, 0, 0, 0, true, false, true}, {}, true};
int checks{};
void Check(bool value, const char* message) {
    ++checks;
    if (!value) {
        throw std::runtime_error(message);
    }
}
void Env(const char* key, const char* value) {
#ifdef _WIN32
    if (_putenv_s(key, value ? value : "") != 0) {
        throw std::runtime_error("test environment setup failed");
    }
#else
    if ((value ? setenv(key, value, 1) : unsetenv(key)) != 0) {
        throw std::runtime_error("test environment setup failed");
    }
#endif
}
}

// These doubles implement the actual factored runtime telemetry declarations.
// They simulate counters, not execution of a guest or HLE services.
namespace Core {
RecompLiveStats GetRecompLiveStats() { return sample.live; }
RecompExecutionStats GetRecompExecutionStats() { return sample.execution; }
bool IsRecompCodeGuardReady() { return sample.code_guard_ready; }
}

int main(int argc, char** argv) {
    try {
        Env("SUYU_AOT_TRACE", nullptr);
        auto stats = std::make_unique<Core::PerfStats>(1);
        const auto empty = stats->GetAndResetStats(std::chrono::microseconds{0});
        Check(std::isfinite(empty.frametime) && empty.frametime == 0, "zero-frame query");
        Check(empty.game_frames == 0, "empty raw frame count");
        constexpr int Producers = 4;
        constexpr int PerProducer = 100000;
        std::atomic<int> remaining{Producers};
        std::vector<std::thread> workers;
        for (int i = 0; i < Producers; ++i) {
            workers.emplace_back([&] {
                for (int frame = 0; frame < PerProducer; ++frame) {
                    stats->EndGameFrame();
                }
                remaining.fetch_sub(1);
            });
        }
        std::uint64_t consumed{};
        while (remaining.load() != 0) {
            consumed += stats->GetAndResetStats(std::chrono::microseconds{0}).game_frames;
        }
        for (auto& worker : workers) {
            worker.join();
        }
        consumed += stats->GetAndResetStats(std::chrono::microseconds{0}).game_frames;
        Check(consumed == Producers * PerProducer, "no frame loss across concurrent resets");
        // Previously copying [begin+5, begin+0) in the destructor was invalid.
        Settings::values.record_frame_times.value = true;
        stats.reset();
        Check(true, "zero-frame CSV shutdown is safe");
        Settings::values.record_frame_times.value = false;

        if (argc != 2) {
            throw std::runtime_error("test output directory argument required");
        }
        const auto path = std::filesystem::path{argv[1]} / "perf-trace.jsonl";
        std::filesystem::remove(path);
        Env("SUYU_AOT_TRACE", path.string().c_str());
        Env("SUYU_AOT_BUILD_ID", "synthetic-build");
        Env("SUYU_AOT_WORKLOAD_ID", "synthetic-fixture");
        Env("SUYU_AOT_CONTENT_ID", "test-content");
        Env("SUYU_AOT_CONFIG_ID", "test-settings");
        Env("SUYU_AOT_MACHINE_ID", "test-host");
        sample.execution.blocks = 100;
        {
            Core::PerfStats traced{1};
            for (int i = 0; i < 10; ++i) {
                traced.BeginSystemFrame();
                ++sample.execution.blocks;
                traced.EndSystemFrame();
            }
            const auto result = traced.GetAndResetStats(std::chrono::microseconds{166667});
            Check(result.aot.counters.blocks == 10, "production bridge uses session baseline");
            Check(result.aot.StrictWorkloadObserved(), "production bridge exports strict gate");
            // Teardown must not relabel the observed AOT run as JIT.
            sample.live.backend_active = false;
        }
        Env("SUYU_AOT_TRACE", nullptr);
        std::ifstream input{path};
        std::string contents{std::istreambuf_iterator<char>{input}, {}};
        Check(contents.find("\"static_blocks\":10") != std::string::npos, "production trace has real API deltas");
        Check(contents.find("\"strict_workload_observed\":true") != std::string::npos, "teardown preserves trace gate");
        std::cout << "perf_stats component integration: " << checks << " checks passed; "
                  << consumed << " concurrent frames consumed exactly\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
