// SPDX-License-Identifier: GPL-2.0-or-later
// Full-checkout integration test: invoke the real header-only emitter.
#include "core/recompiler/arm64_to_c.h"
#include <array>
#include <fstream>
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    // Hand-authored AArch64: ADD x0,x0,#1; SVC #1. Not game-derived.
    constexpr std::array<std::uint32_t, 2> code{0x91000400U, 0xd4000021U};
    for (const std::string name : {"rtld", "main", "sdk"}) {
        const auto result = suyu::recomp::EmitProject(name,
            reinterpret_cast<const std::uint8_t*>(code.data()), sizeof(code),
            0x100, (output/name).string(), true, nullptr, 0, nullptr, 0, 0x100);
        if (!result.blocks || result.unhandled) {
            std::cerr << "Synthetic instruction export failed\n"; return 1;
        }
    }
    // Registration is supplied by make_fixture.registration() in the wrapper.
    std::ofstream(output/"SYNTHETIC_FIXTURE.txt") << "Real emitter: hand-authored ADD/SVC only\n";
    return 0;
}
