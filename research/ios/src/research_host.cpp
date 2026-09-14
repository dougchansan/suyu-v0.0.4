// SPDX-License-Identifier: GPL-2.0-or-later
#include "research_host.h"
#include "static_registry.h"
#include <sstream>
#include <stdexcept>
#include <string>
#ifdef SWITCH_AOT_SYNTHETIC
extern "C" {
#include "recomp_runtime.h"
}
#endif

namespace {
std::string report = "Not run. Suyu HLE/GPU/audio are not connected.";
void Check(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason); // Not disabled by NDEBUG.
}
}
extern "C" int switch_aot_self_test() {
    try {
        unsigned count = 0;
        const auto* modules = suyu_recomp_static_modules(&count);
        SwitchAOT::Registry registry(modules, count);
        Check(registry.Error().empty(), registry.Error().c_str());
#ifndef SWITCH_AOT_SYNTHETIC
        report = "PRIVATE AOT LINK PROBE: " + std::to_string(count) +
                 " module descriptors linked. No game code executed. "
                 "Suyu core, graphics, audio, input and content-identity integration pending.";
        return 2;
#else
        Check(count == 3, "Synthetic module count");
        Check(!registry.Lookup(0x100100), "Lookup must reject unsealed registry");
        for (unsigned i = 0; i < count; ++i)
            Check(registry.Bind(i, 0x100000ULL * (i+1)), "Module binding");
        Check(registry.Seal(), "Seal complete module set");
        for (unsigned i = 0; i < count; ++i) {
            const auto pc = 0x100000ULL * (i+1) + 0x100;
            auto block = registry.Lookup(pc);
            Check(block != nullptr, "Absolute PC lookup / single base subtraction");
            GuestContext context{}; // Full generated type, not just an ABI prefix.
            context.x[0] = 41;
            context.pc = pc;
            context.pending_svc = UINT64_MAX;
            context.chain_budget = 32;
            block(&context);
            Check(context.x[0] == 42, "Synthetic ADD x0,x0,#1");
            Check(context.pending_svc == 1, "Synthetic SVC #1 boundary");
            Check(context.pc == pc+8, "Synthetic SVC resume PC");
            Check(!registry.Lookup(pc+1), "Misaligned PC must not round down");
            Check(!registry.Lookup(pc+0x1000), "Missing block must not fall back");
        }
        Check(!registry.Lookup(0), "PC below all modules");
        Check(!registry.Lookup(UINT64_MAX), "Invalid high PC");
        Check(!registry.Bind(0, 0x400000), "Rebind must fail");
        Check(!registry.Lookup(0x100100), "Failed registry must fail closed");
        SwitchAOT::Registry wrong_name(modules, count);
        Check(!wrong_name.BindNamed(0, "main", 0x100000), "Wrong runtime name must fail");
        Check(!wrong_name.Seal(), "Name failure must remain fatal");
        SwitchAOT::Registry wrong_order(modules, count);
        Check(!wrong_order.BindNamed(0, modules[1].name, 0x100000), "Wrong name order");
        for (unsigned session = 0; session < 2; ++session) {
            SwitchAOT::Registry fresh(modules, count);
            const auto offset = 0x1000000ULL * (session + 1);
            for (unsigned i = 0; i < count; ++i)
                Check(fresh.BindNamed(i, modules[i].name, offset + 0x100000ULL * i),
                      "Fresh session binds all named modules");
            Check(fresh.Seal(), "Fresh session seal");
            Check(fresh.Lookup(offset + 0x100), "Fresh session lookup uses new base");
        }
        SwitchAOT::Registry empty(nullptr, 0);
        Check(!empty.Seal(), "Empty module set");
        SwitchAOT::Registry missing(modules, count);
        Check(missing.Bind(0, 0x100000), "Partial binding setup");
        Check(!missing.Seal(), "Partial binding must fail");
        SwitchAOT::Registry duplicate(modules, count);
        Check(duplicate.Bind(0, 0x100000), "Duplicate-base setup");
        Check(!duplicate.Bind(1, 0x100000), "Duplicate base must fail");
        SwitchAOT::Registry bad_index(modules, count);
        Check(!bad_index.Bind(count, 0x100000), "Unexpected load index");
        SwitchAOT::Registry unaligned(modules, count);
        Check(!unaligned.Bind(0, 1), "Unaligned base");
        SuyuRecompStaticModule repeated[2] = {modules[0], modules[0]};
        SwitchAOT::Registry names(repeated, 2);
        Check(!names.Error().empty(), "Duplicate descriptor names");
        SuyuRecompStaticModule invalid = modules[0]; invalid.lookup = nullptr;
        SwitchAOT::Registry null_callback(&invalid, 1);
        Check(!null_callback.Error().empty(), "Null callback");
        SwitchAOT::Registry reverse(modules, count);
        Check(reverse.Bind(0, 0x300000) && reverse.Bind(1, 0x200000) &&
              reverse.Bind(2, 0x100000), "Reverse binding setup");
        Check(!reverse.Seal(), "Wrong load order");
        report = "PASS: synthetic 3-module static link; ADD/SVC dispatch; "
                 "absolute PCs; alignment, missing-block, descriptor and lifecycle guards. "
                 "No JIT, no dynamic module loading. Not a game/runtime test.";
        return 0;
#endif
    } catch (const std::exception& e) {
        report = std::string("FAIL: ") + e.what();
        return 1;
    }
}
extern "C" const char* switch_aot_report() { return report.c_str(); }
