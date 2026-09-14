// Private owner-content staging utility; uses the existing Suyu NCA reader.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "common/logging.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"

static void Copy(const FileSys::VirtualFile& file, const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) throw std::runtime_error("Refusing to overwrite output");
    std::ofstream out(path, std::ios::binary);
    std::vector<u8> bytes(4 * 1024 * 1024);
    for (std::size_t offset = 0; offset < file->GetSize();) {
        const auto count = std::min(bytes.size(), file->GetSize() - offset);
        if (file->Read(bytes.data(), count, offset) != count) throw std::runtime_error("Short read");
        out.write(reinterpret_cast<const char*>(bytes.data()), count);
        if (!out) throw std::runtime_error("Write failed");
        offset += count;
    }
}

int main(int argc, char** argv) try {
    if (argc != 4 && argc != 5) throw std::runtime_error("usage: export_owner_content base.xci update.nsp output [--write]");
    Common::Log::Initialize();
    FileSys::RealVfsFilesystem vfs;
    FileSys::XCI base(vfs.OpenFile(argv[1]));
    auto base_nca = base.GetNCAByType(FileSys::NCAContentType::Program);
    if (!base_nca) throw std::runtime_error("Base program unavailable");
    FileSys::NSP update(vfs.OpenFile(argv[2]));
    std::shared_ptr<FileSys::NCA> patch;
    for (const auto& file : update.GetFiles()) {
        if (file->GetExtension() != "nca") continue;
        auto candidate = std::make_shared<FileSys::NCA>(file, base_nca.get());
        if (candidate->GetType() == FileSys::NCAContentType::Program &&
            candidate->GetTitleId() == (base_nca->GetTitleId() | 0x800)) patch = candidate;
    }
    if (!patch || patch->GetStatus() != Loader::ResultStatus::Success) throw std::runtime_error("Matching merged update unavailable");
    auto exefs = patch->GetExeFS();
    auto romfs = patch->GetRomFS();
    if (!exefs || !romfs) throw std::runtime_error("Missing merged sections");
    std::cout << "title=" << std::hex << base_nca->GetTitleId() << std::dec << "\n";
    for (const auto& file : exefs->GetFiles()) std::cout << file->GetName() << " " << file->GetSize() << "\n";
    std::cout << "romfs.bin " << romfs->GetSize() << std::endl;
    if (argc == 5) {
        if (std::string_view(argv[4]) != "--write") throw std::runtime_error("Unknown flag");
        std::filesystem::create_directories(argv[3]);
        for (const auto& file : exefs->GetFiles()) Copy(file, std::filesystem::path(argv[3]) / file->GetName());
        Copy(romfs, std::filesystem::path(argv[3]) / "romfs.bin");
    }
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
