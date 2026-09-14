// SPDX-License-Identifier: GPL-2.0-or-later
#include "core_probe.h"

#include <array>
#include <cstdio>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

#include "common/host_memory.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"

#ifndef SUYU_NO_JIT
#error "The core initialization probe requires SUYU_NO_JIT"
#endif

namespace {
std::string report;
std::string progress_file;

void Phase(const char* message) {
    report += message;
    report += '\n';
    std::ofstream(progress_file, std::ios::trunc) << report << std::flush;
    // Assertions and OS termination cannot be converted into a returned report.
    std::fprintf(stderr, "iHorizon core probe: %s\n", message);
    std::fflush(stderr);
}

struct LoggingSession {
    LoggingSession() {
        Common::Log::Initialize();
        Common::Log::Start();
    }
    ~LoggingSession() {
        Common::Log::Stop();
    }
};
} // namespace

extern "C" int ihorizon_core_initialize_probe(const char* progress_path) {
    report.clear();
    progress_file = progress_path;
    try {
        Phase("Initializing core logging.");
        LoggingSession logging;
        const long page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0) {
            throw std::runtime_error("Unable to determine host page size");
        }
        Phase(("Host page size: " + std::to_string(page_size) + " bytes.").c_str());
        Phase("Allocating 64 KiB HostMemory backing with 1 MiB virtual size.");
        {
            Common::HostMemory memory{64 * 1024, 1024 * 1024};
            if (!memory.BackingBasePointer()) {
                throw std::runtime_error("HostMemory backing is null");
            }
            if (page_size != 4096 && memory.VirtualBasePointer()) {
                throw std::runtime_error("Non-4K host unexpectedly received a fastmem arena");
            }
            constexpr std::array<std::size_t, 8> offsets{
                0, 4095, 4096, 16383, 16384, 32767, 32768, 65535};
            auto* backing = memory.BackingBasePointer();
            for (std::size_t i = 0; i < offsets.size(); ++i) {
                backing[offsets[i]] = static_cast<unsigned char>(0x31 + i);
            }
            for (std::size_t i = 0; i < offsets.size(); ++i) {
                if (backing[offsets[i]] != static_cast<unsigned char>(0x31 + i)) {
                    throw std::runtime_error("HostMemory backing read/write mismatch");
                }
            }
            Phase(memory.VirtualBasePointer() ? "Small backing read/write passed; fastmem arena present."
                                              : "Small backing read/write passed; software backing selected.");
        }
        Phase("Small HostMemory released. Testing timing initialize/reset cycles.");
        {
            Core::Timing::CoreTiming timing;
            timing.SetMulticore(true);
            for (int iteration = 0; iteration < 100; ++iteration) {
                timing.Initialize([] {});
                timing.Reset();
            }
        }
        Phase("100 timing initialize/reset cycles passed.");
        for (int iteration = 0; iteration < 2; ++iteration) {
            Phase(("Constructing Core::System cycle " + std::to_string(iteration + 1) + ".").c_str());
            Core::System system;
            Phase("Core::System constructed. Calling Initialize (full configured backing allocation).");
            system.Initialize();
            Phase("Core::System::Initialize returned. Destroying system and joining owned threads.");
        }
        Phase("PASS: two Core::System constructor, Initialize and destruction cycles completed. No process started; no game or guest HLE boot tested.");
        return 0;
    } catch (const std::exception& error) {
        Phase((std::string{"FAIL: "} + error.what()).c_str());
    } catch (...) {
        Phase("FAIL: unknown exception.");
    }
    return 1;
}

extern "C" const char* ihorizon_core_report(void) {
    return report.c_str();
}
