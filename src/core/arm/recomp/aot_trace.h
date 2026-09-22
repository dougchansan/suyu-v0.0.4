// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <ostream>
#include <string>
#include <string_view>
#include "core/arm/recomp/aot_metrics.h"

namespace Core::Aot {

struct TraceMetadata {
    std::uint64_t title_id{};
    std::string build_id;
    std::string workload_id;
    std::string content_id;
    std::string config_id; // Non-CPU settings; the CPU backend is the variable under test.
    std::string machine_id;
};

inline void JsonString(std::ostream& out, std::string_view value) {
    constexpr char Hex[] = "0123456789abcdef";
    out.put('"');
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') {
            out.put('\\');
            out.put(static_cast<char>(ch));
        } else if (ch < 0x20 || ch >= 0x7f) {
            // Metadata is an opaque byte identifier, not arbitrary Unicode text.
            // Escaping every non-ASCII byte also makes invalid UTF-8 valid JSON.
            out << "\\u00" << Hex[ch >> 4] << Hex[ch & 15];
        } else {
            out.put(static_cast<char>(ch));
        }
    }
    out.put('"');
}

constexpr std::string_view HostArchitecture() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "other";
#endif
}

// Optional diagnostic I/O only. The owner holds its session mutex. Appending
// avoids destroying an existing trace; the comparator rejects concatenated runs.
// No allocation of executable memory, code loading, or CPU fallback occurs here.
class TraceWriter {
public:
    TraceWriter(const std::filesystem::path& path, const TraceMetadata& metadata)
        : file{path, std::ios::out | std::ios::app} {
        file.imbue(std::locale::classic());
        file << std::setprecision(17) << std::boolalpha;
        file << "{\"type\":\"session\",\"schema\":1,\"title_id\":\"" << std::hex
             << std::setw(16) << std::setfill('0') << metadata.title_id << std::dec
             << "\",\"host_arch\":";
        JsonString(file, HostArchitecture());
        Field("build_id", metadata.build_id);
        Field("workload_id", metadata.workload_id);
        Field("content_id", metadata.content_id);
        Field("config_id", metadata.config_id);
        Field("machine_id", metadata.machine_id);
        file << ",\"timing_domain\":\"system_frame_active_ms\"}\n";
    }

    bool Good() const {
        return file.good();
    }

    void Frame(double active_ms, double period_ms) {
        if (finished) {
            return;
        }
        if (!std::isfinite(active_ms) || active_ms < 0 || !std::isfinite(period_ms) ||
            period_ms < 0) {
            ++invalid_frames;
            return;
        }
        file << "{\"type\":\"frame\",\"index\":" << ++frames
             << ",\"active_ms\":" << active_ms << ",\"period_ms\":" << period_ms << "}\n";
    }

    // Only an orderly owner shutdown writes a footer. A crash, I/O error or
    // abruptly killed process leaves incomplete evidence and fails comparison.
    void Finish(const Report& report) {
        if (finished) {
            return;
        }
        finished = true;
        file << "{\"type\":\"summary\",\"complete\":true,\"frames\":" << frames
             << ",\"invalid_frames\":" << invalid_frames << ",\"backend_at_shutdown\":";
        JsonString(file, Name(report.current_backend));
        file << ",\"static_blocks\":" << report.counters.blocks
             << ",\"svc_calls\":" << report.counters.svc_calls
             << ",\"lookup_misses\":" << report.counters.lookup_misses
             << ",\"unhandled\":" << report.counters.unhandled
             << ",\"no_fallback\":" << report.counters.no_fallback
             << ",\"jit_transitions\":" << report.jit_transitions
             << ",\"counters_valid\":" << report.counters_valid
             << ",\"observed_aot\":" << report.observed_aot
             << ",\"observed_non_aot_frame\":" << report.observed_non_aot_frame
             << ",\"all_aot_samples_strict\":" << report.all_aot_samples_strict
             << ",\"all_aot_samples_guarded\":" << report.all_aot_samples_guarded
             << ",\"jit_compiled_out\":" << report.jit_compiled_out
             << ",\"diagnostic_cutoff\":" << report.diagnostic_cutoff
             << ",\"strict_workload_observed\":" << report.StrictWorkloadObserved() << "}\n";
        file.flush();
    }

private:
    void Field(std::string_view name, std::string_view value) {
        file.put(',');
        JsonString(file, name);
        file.put(':');
        JsonString(file, value);
    }

    std::ofstream file;
    std::uint64_t frames{};
    std::uint64_t invalid_frames{};
    bool finished{};
};

} // namespace Core::Aot
