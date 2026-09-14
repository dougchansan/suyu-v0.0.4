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
// All three operations require a stopped core, with ALL guest threads joined.
// Install BEFORE System::Load / KProcess::InitializeInterfaces.
bool InstallStaticImages() {
    if (active) return false; // Explicitly clear a stopped previous session first.
    unsigned count = 0;
    const auto* modules = suyu_recomp_static_modules(&count);
    auto candidate = std::make_unique<Registry>(modules, count);
    if (!candidate->Error().empty()) return false;
    active = std::move(candidate);
    Core::SetRecompBaseSetter([](std::size_t index, const char*, u64 base) {
        if (active) active->Bind(index, base);
    });
    Core::SetRecompLookup([](u64 pc) -> Core::RecompBlockFn {
        return active ? active->Lookup(pc) : nullptr;
    });
    return true;
}
// After System::Load has assigned every module base, BEFORE any guest runs.
// A false result is a launch failure, never permission to start another engine.
bool FinalizeStaticImages() { return active && active->Seal(); }
const char* StaticImageError() {
    return active ? active->Error().c_str() : "Static image registry not installed";
}
void ClearStaticImages() {
    Core::SetRecompLookup(nullptr);
    Core::SetRecompBaseSetter(nullptr);
    active.reset();
}
} // namespace SwitchAOT
