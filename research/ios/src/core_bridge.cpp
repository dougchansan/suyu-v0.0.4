// SPDX-License-Identifier: GPL-2.0-or-later
// NOT part of the standalone diagnostic app. Attach only to a real Suyu core.
#include "static_registry.h"
#include "core/arm/recomp/arm_recomp.h"
#include <memory>
#ifndef SUYU_NO_JIT
#error "The iOS research core bridge requires SUYU_NO_JIT"
#endif
namespace SwitchAOT {
namespace { std::unique_ptr<Registry> active; }
extern "C" int switch_aot_enable_guards(void);
// All three operations require a stopped core, with ALL guest threads joined.
// Install BEFORE System::Load / KProcess::InitializeInterfaces.
bool InstallStaticImages() {
    if (active) return false; // Explicitly clear a stopped previous session first.
    unsigned count = 0;
    const auto* modules = suyu_recomp_static_modules(&count);
    auto candidate = std::make_unique<Registry>(modules, count);
    if (!candidate->Error().empty()) return false;
    if (!switch_aot_enable_guards()) return false;
    active = std::move(candidate);
    Core::SetRecompBaseSetter(nullptr);
    Core::SetRecompPrepareCallback([](const Core::RecompModules& inventory) {
        if (!active) return false;
        std::size_t index = 0;
        for (const auto& [base, name] : inventory) {
            if (!active->BindNamed(index++, name.c_str(), base)) return false;
        }
        return active->Seal();
    });
    Core::SetRecompLookup([](u64 pc) -> Core::RecompBlockFn {
        return active ? active->Lookup(pc) : nullptr;
    });
    return true;
}
// Requires explicit loader-time binding BEFORE process publication/guest startup.
// System::Load owns this barrier; callers can query completion afterward.
// A false result is a launch failure, never permission to start another engine.
bool FinalizeStaticImages() { return active && active->Ready(); }
const char* StaticImageError() {
    if (!active) return "Static image registry not installed";
    if (!active->Error().empty()) return active->Error().c_str();
    return active->Ready() ? "" : "Static image preparation has not completed";
}
void ClearStaticImages() {
    Core::SetRecompPrepareCallback(nullptr);
    Core::SetRecompLookup(nullptr);
    Core::SetRecompBaseSetter(nullptr);
    active.reset();
}
} // namespace SwitchAOT
