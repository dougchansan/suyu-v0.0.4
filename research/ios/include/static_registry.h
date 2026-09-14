// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Matches the generated registration table and src/suyu_cmd/suyu.cpp.
extern "C" {
using SwitchAOTBlock = void (*)(void*);
struct SuyuRecompStaticModule {
    const char* name;
    SwitchAOTBlock (*lookup)(std::uint64_t);
    void (*set_base)(std::uint64_t);
};
const SuyuRecompStaticModule* suyu_recomp_static_modules(unsigned* count);
}

namespace SwitchAOT {
// One registry per process session. Construct/bind/seal before starting ANY
// guest thread. Do not rebind, reset, or destroy until every guest thread stops.
// After Seal(), concurrent Lookup() is read-only. No dynamic code loading.
class Registry {
public:
    Registry(const SuyuRecompStaticModule* modules, unsigned count);
    bool Bind(std::size_t index, std::uint64_t base);
    bool BindNamed(std::size_t index, const char* name, std::uint64_t base);
    bool Seal();
    SwitchAOTBlock Lookup(std::uint64_t pc) const noexcept;
    bool Ready() const noexcept { return sealed_ && error_.empty(); }
    const std::string& Error() const noexcept { return error_; }
    std::size_t Count() const noexcept { return entries_.size(); }
private:
    struct Entry { SuyuRecompStaticModule module; std::uint64_t base{}; bool bound{}; };
    bool Fail(const char* message);
    std::vector<Entry> entries_;
    std::string error_;
    bool sealed_{};
};
} // namespace SwitchAOT
