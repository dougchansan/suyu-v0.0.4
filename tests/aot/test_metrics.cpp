// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include "core/arm/recomp/aot_metrics.h"
#include "core/arm/recomp/aot_trace.h"

namespace {
int checks{};
void Check(bool value, const char* name) {
    ++checks;
    if (!value) {
        throw std::runtime_error(name);
    }
}
Core::Aot::Sample Active(bool jit_available = false, bool strict = true) {
    return {{0, 0, 0, 0, true, jit_available, strict}, {}, true};
}
}

int main(int argc, char** argv) {
    try {
        using namespace Core::Aot;
        auto sample = Active();
        Check(Classify(sample.live) == Backend::CompiledNoJit, "no-JIT classification");
        sample = Active(true, true);
        Check(Classify(sample.live) == Backend::RuntimeStrict, "runtime-strict classification");
        sample = Active(true, false);
        Check(Classify(sample.live) == Backend::HybridAllowed, "hybrid classification");
        sample.live.backend_active = false;
        Check(Classify(sample.live) == Backend::NonAot, "non-AOT is not assumed JIT");

        sample = Active();
        sample.execution = {1000, 100, 7, 2, 1};
        sample.live.jit_transitions = 3;
        Session session{sample};
        Check(!session.Current().StrictWorkloadObserved(), "empty run cannot pass");
        sample.execution.blocks += 50;
        sample.execution.svc_calls += 2;
        auto report = session.Observe(sample, true);
        Check(report.counters.blocks == 50 && report.counters.svc_calls == 2,
              "session baseline excludes previous runs");
        Check(report.jit_transitions == 0 && report.counters.lookup_misses == 0,
              "historical transitions are not this session");
        Check(report.StrictWorkloadObserved(), "guarded strict workload passes");
        sample.live.backend_active = false;
        Check(session.Observe(sample).StrictWorkloadObserved(), "shutdown preserves observed evidence");
        Check(!session.Observe(sample, true).StrictWorkloadObserved(), "non-AOT frame disqualifies candidate");

        for (int index = 0; index < 6; ++index) {
            auto start = Active();
            start.execution = {10, 10, 10, 10, 10};
            start.live.jit_transitions = 10;
            Session reset{start};
            auto next = start;
            switch (index) {
            case 0: next.execution.blocks = 9; break;
            case 1: next.execution.svc_calls = 9; break;
            case 2: next.execution.lookup_misses = 9; break;
            case 3: next.execution.unhandled = 9; break;
            case 4: next.execution.no_fallback = 9; break;
            case 5: next.live.jit_transitions = 9; break;
            }
            Check(!reset.Observe(next).counters_valid, "each counter reset is detected");
            start.execution.blocks = 100;
            Check(!reset.Observe(start).StrictWorkloadObserved(), "reset failure is latched");
        }
        for (int index = 0; index < 8; ++index) {
            auto next = Active();
            Session blocked{next};
            next.execution.blocks = 100;
            switch (index) {
            case 0: next.execution.lookup_misses = 1; break;
            case 1: next.execution.unhandled = 1; break;
            case 2: next.execution.no_fallback = 1; break;
            case 3: next.live.jit_transitions = 1; break;
            case 4: next.live.forced_cutoff_pc = 4; break;
            case 5: next.live.forced_cutoff_blocks = 1; break;
            case 6: next.code_guard_ready = false; break;
            case 7: next.live.jit_available = true; next.live.strict_mode = false; break;
            }
            Check(!blocked.Observe(next).StrictWorkloadObserved(), "unsafe workload cannot pass");
        }
        auto hybrid = Active(true, false);
        Session changed{hybrid};
        hybrid.execution.blocks = 1;
        changed.Observe(hybrid);
        hybrid.live.strict_mode = true;
        Check(!changed.Observe(hybrid).StrictWorkloadObserved(), "switching to strict cannot hide hybrid capability");

        Session initially_hybrid{Active(true, false)};
        auto later_strict = Active();
        later_strict.execution.blocks = 1;
        Check(!initially_hybrid.Observe(later_strict).StrictWorkloadObserved(),
              "initial hybrid state cannot be hidden by first observation");
        Check(CalculateFrameRates(1e-300, 1, 1, 1, 1e300).system_fps == 0,
              "finite inputs cannot overflow HUD rates");
        const auto rates = CalculateFrameRates(2.0, 120, 100, 1.2, 2.0);
        Check(rates.system_fps == 60 && rates.game_fps == 50 && rates.emulation_speed == 1,
              "frame rate denominator");
        Check(std::abs(rates.active_seconds_per_system_frame - .01) < 1e-12, "active frame timing");
        Check(CalculateFrameRates(1, 0, 0, 0, 0).active_seconds_per_system_frame == 0,
              "zero frames stays finite");
        for (double bad : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
            Check(CalculateFrameRates(bad, 1, 1, 1, 1).system_fps == 0, "invalid elapsed interval");
        }
        Check(CalculateFrameRates(1, 60, 60, 1, -1).emulation_speed == 0, "emulated time reset");
        std::ostringstream escaped;
        JsonString(escaped, std::string("quote\" slash\\ line\n byte\xff"));
        Check(escaped.str() == "\"quote\\\" slash\\\\ line\\u000a byte\\u00ff\"", "JSON byte escaping");

        if (argc != 2) {
            throw std::runtime_error("test output directory argument required");
        }
        const auto path = std::filesystem::path{argv[1]} / "metrics-trace.jsonl";
        std::filesystem::remove(path);
        TraceWriter writer{path, {1, "synthetic-build", "synthetic-fixture", "test-content", "test-settings", "test-host"}};
        writer.Frame(10, 16);
        writer.Frame(11, 17);
        auto strict = Active();
        Session traced{strict};
        strict.execution.blocks = 2;
        const auto final = traced.Observe(strict, true);
        writer.Finish(final);
        writer.Finish(final);
        writer.Frame(12, 18);
        Check(writer.Good(), "trace file writes");
        std::ifstream trace{path};
        std::string line;
        int lines{};
        while (std::getline(trace, line)) {
            ++lines;
        }
        Check(lines == 4, "exactly one footer; no frames after finish");
        std::cout << "aot_metrics: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
