// Emit one dispatcher entry for a guest PC discovered by a no-JIT runtime run.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/recompiler/arm64_to_c.h"

int main(int argc, char** argv) try {
    if (argc != 4) {
        throw std::runtime_error("usage: emit_runtime_root text.bin module offset_hex");
    }
    const std::string module = argv[2];
    const auto offset = std::strtoull(argv[3], nullptr, 0);
    if ((offset & 3) != 0) {
        throw std::runtime_error("root offset must be instruction-aligned");
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open text segment");
    }
    std::vector<suyu::recomp::u8> text{std::istreambuf_iterator<char>(input), {}};
    if (offset >= text.size()) {
        throw std::runtime_error("could not read root from text segment");
    }

    const auto name = suyu::recomp::FuncName(module, offset);
    std::cout << "void " << name << "(GuestContext* c){\n";
    bool terminated = false;
    for (auto pc = offset; pc + 4 <= text.size(); pc += 4) {
        suyu::recomp::u32 instruction{};
        std::memcpy(&instruction, text.data() + pc, sizeof(instruction));
        std::string body;
        terminated = !suyu::recomp::Translate(instruction, pc, body);
        std::cout << body;
        if (terminated) {
            break;
        }
    }
    if (!terminated) {
        throw std::runtime_error("root did not reach a block terminator");
    }
    std::cout << "}\n\n";
    std::cerr << "  {0x" << std::hex << offset << "ULL, " << name << "},\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
