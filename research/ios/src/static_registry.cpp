// SPDX-License-Identifier: GPL-2.0-or-later
#include "static_registry.h"
#include <cstring>

namespace SwitchAOT {
bool Registry::Fail(const char* message) {
    if (error_.empty()) error_ = message;
    return false;
}
Registry::Registry(const SuyuRecompStaticModule* modules, unsigned count) {
    if (!modules || count == 0 || count > 64) {
        Fail("Expected 1..64 statically linked modules");
        return;
    }
    entries_.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        const auto& m = modules[i];
        if (!m.name || !*m.name || !m.lookup || !m.set_base) {
            Fail("Incomplete static-module descriptor");
            return;
        }
        for (const auto& e : entries_) {
            if (std::strcmp(e.module.name, m.name) == 0) {
                Fail("Duplicate static-module name");
                return;
            }
        }
        entries_.push_back({m, 0, false});
    }
}
bool Registry::BindNamed(std::size_t index, const char* name, std::uint64_t base) {
    if (index >= entries_.size()) return Fail("Unexpected runtime module index");
    if (!name || std::strcmp(name, entries_[index].module.name) != 0)
        return Fail("Runtime module name disagrees with registration load order");
    return Bind(index, base);
}
bool Registry::Bind(std::size_t index, std::uint64_t base) {
    if (!error_.empty()) return false;
    if (sealed_) return Fail("Cannot rebind a sealed registry");
    if (index >= entries_.size()) return Fail("Unexpected runtime module index");
    if (base & 3U) return Fail("Unaligned module base");
    auto& e = entries_[index];
    if (e.bound) return Fail("Module was bound twice");
    for (const auto& other : entries_) {
        if (other.bound && other.base == base) return Fail("Duplicate module load base");
    }
    e.module.set_base(base); // Generated setter also builds the data-only index.
    e.base = base;
    e.bound = true;
    return true;
}
bool Registry::Seal() {
    if (!error_.empty()) return false;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (!entries_[i].bound) return Fail("Missing runtime module binding");
        if (i && entries_[i].base <= entries_[i-1].base)
            return Fail("Runtime bases disagree with registration load order");
    }
    sealed_ = true;
    return true;
}
SwitchAOTBlock Registry::Lookup(std::uint64_t pc) const noexcept {
    if (!Ready() || (pc & 3U)) return nullptr;
    // Match by load index, never by NSO's embedded display name. The image
    // lookup receives an ABSOLUTE guest PC and subtracts its own base once.
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (pc >= it->base) {
            return it->module.lookup(pc);
        }
    }
    return nullptr;
}
} // namespace SwitchAOT
