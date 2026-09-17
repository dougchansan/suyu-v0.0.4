// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <array>
#include <chrono>
#include <exception>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

#include <fmt/ostream.h>

#include "common/detached_tasks.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "common/fs/path_util.h"
#include "common/nvidia_flags.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/arm/recomp/arm_recomp.h"
#include "core/core.h"
#include "core/perf_stats.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/service/library_applet_creator.h"
#include "core/hle/service/filesystem/filesystem.h"
#include <map>
#include "common/lz4_compression.h"
#include "core/loader/nso.h"
#include "core/recompiler/arm64_to_c.h"
#include "core/loader/loader.h"
#include "frontend_common/config.h"
#include "input_common/main.h"
#include "network/network.h"
#include "sdl_config.h"
#include "suyu_cmd/emu_window/emu_window_sdl2.h"
#include "suyu_cmd/emu_window/emu_window_sdl2_gl.h"
#ifdef __APPLE__
#include "suyu_cmd/emu_window/emu_window_sdl2_mtl.h"
#endif
#include "suyu_cmd/emu_window/emu_window_sdl2_null.h"
#include "suyu_cmd/emu_window/emu_window_sdl2_vk.h"
#include "video_core/renderer_base.h"

#ifdef _WIN32
// windows.h needs to be included before shellapi.h
#include <windows.h>

#include <shellapi.h>

#include "common/windows/timer_resolution.h"
#endif

#undef _UNICODE
#include <getopt.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif

#ifdef _WIN32
extern "C" {
// tells Nvidia and AMD drivers to use the dedicated GPU by default on laptops with switchable
// graphics
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

#ifdef __unix__
#include "common/linux/gamemode.h"
#endif

// Statically linked recompiled CPU modules.
//
// A per-game build of this executable (see SUYU_CMD_RECOMP_DIR in
// src/suyu_cmd/CMakeLists.txt) compiles in a generated recomp_registration.c
// that lists the game's modules in NSO load order. That makes the exported
// game a single self-contained binary - there is nothing to LoadLibrary and no
// recompiled_*.dll to ship beside it. The layout below must match the struct
// the generator emits.
extern "C" {
struct SuyuRecompStaticModule {
    const char* name;
    void (*(*lookup)(u64))(void*);
    void (*set_base)(u64);
};
#ifdef SUYU_CMD_STATIC_RECOMP
const SuyuRecompStaticModule* suyu_recomp_static_modules(unsigned* count);
#ifdef SUYU_RECOMP_GUARD_V2
int suyu_recomp_static_guard_v2(unsigned version);
#endif
#endif
}

static void PrintHelp(const char* argv0) {
    std::cout << "Usage: " << argv0
              << " [options]\n"
                 "-c, --config          Load the specified configuration file\n"
                 "-f, --fullscreen      Start in fullscreen mode\n"
                 "-g, --game            File path of the game to load\n"
                 "-h, --help            Display this help and exit\n"
                 "-m, --multiplayer=nick:password@address:port"
                 " Nickname, password, address and port for multiplayer\n"
                 "-p, --program         Pass following string as arguments to executable\n"
                 "-t, --tas             Replay the TAS script from the user tas directory,\n"
                 "                      starting at the first displayed frame and exiting\n"
                 "                      when the script runs out\n"
                 "-u, --user            Select a specific user profile from 0 to 7\n"
                 "-V, --app-version=<n>[:<display>]\n"
                 "                      Report <n> as the application version and <display>\n"
                 "                      as its version string, for content that carries no\n"
                 "                      control data of its own. Without this a deconstructed\n"
                 "                      ROM directory always reports 1.0.0. Persisted as\n"
                 "                      application_version_override and\n"
                 "                      application_display_version_override in the config.\n"
                 "-v, --version         Output version information and exit\n"
                 "-l, "
                 "--applet-params="
                 "\"program_id,applet_id,applet_type,launch_type,prog_index,prev_prog_index\"\n"
                 "                      Numerical parameters for launching an applet. If no\n"
                 "                      game is provided, then the applet will launch off of\n"
                 "                      the applet_id.\n";
}

static void PrintVersion() {
    std::cout << "suyu" << Common::g_scm_branch << " " << Common::g_scm_desc << std::endl;
}

static void OnStateChanged(const Network::RoomMember::State& state) {
    switch (state) {
    case Network::RoomMember::State::Idle:
        LOG_DEBUG(Network, "Network is idle");
        break;
    case Network::RoomMember::State::Joining:
        LOG_DEBUG(Network, "Connection sequence to room started");
        break;
    case Network::RoomMember::State::Joined:
        LOG_DEBUG(Network, "Successfully joined to the room");
        break;
    case Network::RoomMember::State::Moderator:
        LOG_DEBUG(Network, "Successfully joined the room as a moderator");
        break;
    default:
        break;
    }
}

static void OnNetworkError(const Network::RoomMember::Error& error) {
    switch (error) {
    case Network::RoomMember::Error::LostConnection:
        LOG_DEBUG(Network, "Lost connection to the room");
        break;
    case Network::RoomMember::Error::CouldNotConnect:
        LOG_ERROR(Network, "Error: Could not connect");
        exit(1);
        break;
    case Network::RoomMember::Error::NameCollision:
        LOG_ERROR(
            Network,
            "You tried to use the same nickname as another user that is connected to the Room");
        exit(1);
        break;
    case Network::RoomMember::Error::IpCollision:
        LOG_ERROR(Network, "You tried to use the same fake IP-Address as another user that is "
                           "connected to the Room");
        exit(1);
        break;
    case Network::RoomMember::Error::WrongPassword:
        LOG_ERROR(Network, "Room replied with: Wrong password");
        exit(1);
        break;
    case Network::RoomMember::Error::WrongVersion:
        LOG_ERROR(Network,
                  "You are using a different version than the room you are trying to connect to");
        exit(1);
        break;
    case Network::RoomMember::Error::RoomIsFull:
        LOG_ERROR(Network, "The room is full");
        exit(1);
        break;
    case Network::RoomMember::Error::HostKicked:
        LOG_ERROR(Network, "You have been kicked by the host");
        break;
    case Network::RoomMember::Error::HostBanned:
        LOG_ERROR(Network, "You have been banned by the host");
        break;
    case Network::RoomMember::Error::UnknownError:
        LOG_ERROR(Network, "UnknownError");
        break;
    case Network::RoomMember::Error::PermissionDenied:
        LOG_ERROR(Network, "PermissionDenied");
        break;
    case Network::RoomMember::Error::NoSuchUser:
        LOG_ERROR(Network, "NoSuchUser");
        break;
    }
}

static void OnMessageReceived(const Network::ChatEntry& msg) {
    std::cout << std::endl << msg.nickname << ": " << msg.message << std::endl << std::endl;
}

static void OnStatusMessageReceived(const Network::StatusMessageEntry& msg) {
    std::string message;
    switch (msg.type) {
    case Network::IdMemberJoin:
        message = fmt::format("{} has joined", msg.nickname);
        break;
    case Network::IdMemberLeave:
        message = fmt::format("{} has left", msg.nickname);
        break;
    case Network::IdMemberKicked:
        message = fmt::format("{} has been kicked", msg.nickname);
        break;
    case Network::IdMemberBanned:
        message = fmt::format("{} has been banned", msg.nickname);
        break;
    case Network::IdAddressUnbanned:
        message = fmt::format("{} has been unbanned", msg.nickname);
        break;
    }
    if (!message.empty())
        std::cout << std::endl << "* " << message << std::endl << std::endl;
}

/// True once native recompiled CPU modules are registered — the running
/// process is a standalone game export, not the suyu dev frontend.
bool g_native_export_mode = false;

/// Application entry point
/// mk8-recomp: report each title's CPU architecture without booting it.
///
/// The static recompiler is AArch64-only, so whether a title is A32 or A64
/// decides whether it can be targeted at all. Nothing in suyu answers that
/// without a full boot, which spins up Vulkan and crashes outright on some
/// titles. This walks the same path the exporter does
/// (XCI -> secure NSP -> Program NCA -> ExeFS -> main.npdm) and reports the
/// NPDM flags.
///
/// Results go to a file rather than stdout because suyu-cmd is linked
/// /SUBSYSTEM:WINDOWS (suyu_cmd/CMakeLists.txt:142), so std::cout is discarded
/// even when redirected. A whole library is processed in one invocation,
/// which also avoids paying process startup 100+ times.
static int ProbeIsaList(const std::string& list_path, const std::string& out_path) {
    std::ifstream list{list_path};
    if (!list) {
        return 1;
    }
    std::ofstream out{out_path, std::ios::trunc};
    if (!out) {
        return 1;
    }

    static const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();

    const auto exefs_from_nsp =
        [](const std::shared_ptr<FileSys::NSP>& nsp) -> FileSys::VirtualDir {
        if (!nsp || nsp->GetStatus() != Loader::ResultStatus::Success) {
            return nullptr;
        }
        if (auto pre_extracted = nsp->GetExeFS()) {
            return pre_extracted;
        }
        const auto tid = nsp->GetProgramTitleID();
        if (const auto nca = nsp->GetNCA(tid, FileSys::ContentRecordType::Program)) {
            if (auto exefs = nca->GetExeFS()) {
                return exefs;
            }
        }
        // An update-only NSP holds its Program NCA under TitleType::Update, and
        // the Base lookup above finds nothing at all. That matters here rather
        // than being a curiosity: where a title ships a 32-bit base, the whole
        // question is whether a later update rebuilt it as AArch64,
        // which cannot be answered without reading the update's own NPDM.
        if (const auto nca = nsp->GetNCA(tid, FileSys::ContentRecordType::Program,
                                         FileSys::TitleType::Update)) {
            return nca->GetExeFS();
        }
        return nullptr;
    };

    std::string rom_path;
    while (std::getline(list, rom_path)) {
        while (!rom_path.empty() && (rom_path.back() == '\r' || rom_path.back() == '\n')) {
            rom_path.pop_back();
        }
        if (rom_path.empty()) {
            continue;
        }

        const auto emit = [&out, &rom_path](std::string_view isa, u64 tid,
                                            std::string_view note) {
            out << isa << '\t' << fmt::format("{:016X}", tid) << '\t' << note << '\t'
                << rom_path << '\n';
            out.flush();
        };

        auto file = vfs->OpenFile(rom_path, FileSys::OpenMode::Read);
        if (!file) {
            emit("ERROR", 0, "open failed");
            continue;
        }

        std::string name = file->GetName();
        std::string ext;
        if (const auto pos = name.rfind('.'); pos != std::string::npos) {
            ext = name.substr(pos);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        }

        FileSys::VirtualDir exefs;
        u64 title_id = 0;
        try {
            if (ext == ".nsp") {
                auto nsp = std::make_shared<FileSys::NSP>(file);
                title_id = nsp->GetProgramTitleID();
                exefs = exefs_from_nsp(nsp);
            } else if (ext == ".xci") {
                auto xci = std::make_shared<FileSys::XCI>(file);
                if (xci->GetStatus() != Loader::ResultStatus::Success) {
                    emit("ERROR", 0,
                         fmt::format("xci: {}", Loader::GetResultStatusString(xci->GetStatus())));
                    continue;
                }
                auto secure = xci->GetSecurePartitionNSP();
                if (!secure) {
                    emit("ERROR", 0, "xci: no secure partition");
                    continue;
                }
                title_id = secure->GetProgramTitleID();
                if (secure->GetStatus() != Loader::ResultStatus::Success) {
                    emit("ERROR", title_id,
                         fmt::format("nsp: {}", Loader::GetResultStatusString(secure->GetStatus())));
                    continue;
                }
                exefs = exefs_from_nsp(secure);
                if (!exefs) {
                    // Distinguish "no Program NCA at all" from "found it but it
                    // would not decrypt", which is almost always a missing title
                    // key. Both surface as a null ExeFS otherwise.
                    const auto nca = secure->GetNCA(title_id, FileSys::ContentRecordType::Program);
                    if (!nca) {
                        emit("ERROR", title_id, "no program nca for title id");
                    } else {
                        emit("ERROR", title_id,
                             fmt::format("program nca: {}",
                                         Loader::GetResultStatusString(nca->GetStatus())));
                    }
                    continue;
                }
            } else if (ext == ".nca") {
                auto nca = std::make_shared<FileSys::NCA>(file);
                if (nca->GetStatus() == Loader::ResultStatus::Success) {
                    title_id = nca->GetTitleId();
                    exefs = nca->GetExeFS();
                }
            }
        } catch (const std::exception& e) {
            emit("ERROR", title_id, fmt::format("exception: {}", e.what()));
            continue;
        }

        if (!exefs) {
            emit("ERROR", title_id, "no exefs");
            continue;
        }
        const auto npdm_file = exefs->GetFile("main.npdm");
        if (!npdm_file) {
            emit("ERROR", title_id, "no main.npdm");
            continue;
        }

        FileSys::ProgramMetadata metadata;
        if (metadata.Load(npdm_file) != Loader::ResultStatus::Success) {
            emit("ERROR", title_id, "npdm parse failed");
            continue;
        }

        emit(metadata.Is64BitProgram() ? "ARM64" : "ARM32", title_id,
             fmt::format("flags=0x{:02X}", metadata.Is64BitProgram() ? 1 : 0));
    }
    return 0;
}


// Decode coverage for a whole library, without exporting anything.
//
// Whether a title can run without a JIT has two halves: does the emitter
// understand every instruction in its image, and is every block that executes
// actually emitted. The second needs the title to run. This answers the first,
// which gates the second, and needs no boot, no input and no disk - the .text is
// decompressed in memory and every word is run through the emitter.
//
// Zero unhandled instructions makes a title a candidate for a JIT-free build.
// Any at all rules it out, and the signature says what is missing.
static int ProbeDecodeList(const std::string& list_path, const std::string& out_path) {
    std::ifstream list{list_path};
    std::ofstream out{out_path, std::ios::trunc};
    std::ofstream encodings{out_path + ".encodings.tsv", std::ios::trunc};
    if (!list || !out || !encodings) {
        return 1;
    }

    // The configuration a JIT-free export uses. Two instruction families are off
    // by default because the JIT runs those blocks faster; a build with no JIT
    // has nothing to hand them to, so asking the default question would report a
    // gap in every title that is not a gap for the case being measured.
    suyu::recomp::g_translate_all = true;

    static const auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();

    const auto exefs_from_nsp =
        [](const std::shared_ptr<FileSys::NSP>& nsp) -> FileSys::VirtualDir {
        if (!nsp || nsp->GetStatus() != Loader::ResultStatus::Success) {
            return nullptr;
        }
        if (auto pre_extracted = nsp->GetExeFS()) {
            return pre_extracted;
        }
        const auto tid = nsp->GetProgramTitleID();
        if (const auto nca = nsp->GetNCA(tid, FileSys::ContentRecordType::Program)) {
            if (auto exefs = nca->GetExeFS()) {
                return exefs;
            }
        }
        if (const auto nca = nsp->GetNCA(tid, FileSys::ContentRecordType::Program,
                                         FileSys::TitleType::Update)) {
            return nca->GetExeFS();
        }
        return nullptr;
    };

    std::string rom_path;
    while (std::getline(list, rom_path)) {
        while (!rom_path.empty() && (rom_path.back() == '\r' || rom_path.back() == '\n')) {
            rom_path.pop_back();
        }
        if (rom_path.empty()) {
            continue;
        }

        u64 zero_words = 0, udf_words = 0, reserved_low_words = 0;
        const auto emit = [&out, &rom_path, &zero_words, &udf_words, &reserved_low_words](std::string_view status, u64 tid, u64 total,
                                            u64 unhandled, std::string_view note) {
            out << status << '\t' << fmt::format("{:016X}", tid) << '\t' << total << '\t'
                << unhandled << '\t' << note << '\t' << rom_path << '\t' << zero_words
                << '\t' << udf_words << '\t' << reserved_low_words << '\n';
            out.flush();
        };

        FileSys::VirtualDir exefs;
        u64 title_id = 0;
        try {
            auto file = vfs->OpenFile(rom_path, FileSys::OpenMode::Read);
            if (!file) {
                emit("ERROR", 0, 0, 0, "open failed");
                continue;
            }
            // These probes open encrypted archives. Report the prerequisite
            // before the storage layer attempts to use an uninitialized cipher.
            if (!Core::Crypto::KeyManager::Instance().HasKey(Core::Crypto::S256KeyType::Header)) {
                emit("ERROR", 0, 0, 0,
                     fmt::format("{}", fmt::streamed(Loader::ResultStatus::ErrorMissingHeaderKey)));
                return 1;
            }
            std::string name = file->GetName();
            std::string ext;
            if (const auto pos = name.rfind('.'); pos != std::string::npos) {
                ext = name.substr(pos);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            }
            if (ext == ".nsp") {
                auto nsp = std::make_shared<FileSys::NSP>(file);
                title_id = nsp->GetProgramTitleID();
                exefs = exefs_from_nsp(nsp);
            } else if (ext == ".xci") {
                auto xci = std::make_shared<FileSys::XCI>(file);
                if (xci->GetStatus() != Loader::ResultStatus::Success) {
                    emit("ERROR", 0, 0, 0, "xci header");
                    continue;
                }
                auto secure = xci->GetSecurePartitionNSP();
                if (secure) {
                    title_id = secure->GetProgramTitleID();
                    exefs = exefs_from_nsp(secure);
                }
            }
        } catch (const std::exception& e) {
            emit("ERROR", title_id, 0, 0, fmt::format("exception: {}", e.what()));
            continue;
        }

        if (!exefs) {
            emit("ERROR", title_id, 0, 0, "no exefs");
            continue;
        }
        if (const auto npdm_file = exefs->GetFile("main.npdm")) {
            FileSys::ProgramMetadata metadata;
            if (metadata.Load(npdm_file) == Loader::ResultStatus::Success &&
                !metadata.Is64BitProgram()) {
                emit("ARM32", title_id, 0, 0, "not translatable");
                continue;
            }
        }

        u64 total = 0;
        u64 unhandled_count = 0;
        std::map<u32, u64> unhandled_sig;
        std::map<u32, u32> unhandled_example;
        std::map<u32, u64> unhandled_encodings;
        try {
            for (const auto& nso_file : exefs->GetFiles()) {
                if (!nso_file || nso_file->GetSize() < sizeof(Loader::NSOHeader)) {
                    continue;
                }
                Loader::NSOHeader header{};
                if (nso_file->ReadObject(&header) != sizeof(Loader::NSOHeader)) {
                    continue;
                }
                if (header.magic != Common::MakeMagic('N', 'S', 'O', '0')) {
                    continue;   // main.npdm and friends live here too
                }
                std::vector<u8> text = nso_file->ReadBytes(header.segments_compressed_size[0],
                                                           header.segments[0].offset);
                if (text.empty()) {
                    continue;
                }
                if (header.IsSegmentCompressed(0)) {
                    text = Common::Compression::DecompressDataLZ4(text, header.segments[0].size);
                    if (text.empty()) {
                        continue;
                    }
                }
                // Discovered blocks, not a linear sweep of .text. A linear
                // sweep also decodes literal pools and alignment padding, which
                // are not instructions and never will be - it would report a gap
                // in every binary ever built. This is the same denominator the
                // exporter uses, so the number is comparable to its coverage.
                const u64 base = header.segments[0].location;
                const auto blocks = suyu::recomp::DiscoverBlocks(text.data(), text.size(), base);
                std::string sink;
                for (const auto& block : blocks) {
                    for (u32 k = 0; k < block.count; ++k) {
                        const u64 pc = block.vaddr + static_cast<u64>(k) * 4;
                        const size_t off = static_cast<size_t>(pc - base);
                        if (off + 4 > text.size()) {
                            break;
                        }
                        u32 insn = 0;
                        std::memcpy(&insn, text.data() + off, sizeof(insn));
                        sink.clear();
                        bool miss = false;
                        suyu::recomp::Translate(insn, pc, sink, &miss);
                        ++total;
                        if (miss) {
                            ++unhandled_count;
                            ++unhandled_sig[insn & 0xFFC00000u];
                            unhandled_example.try_emplace(insn & 0xFFC00000u, insn);
                            ++unhandled_encodings[insn];
                            // Signature zero is wider than UDF's imm16. Keep
                            // its parts separate before drawing padding conclusions.
                            if (insn == 0) ++zero_words;
                            else if ((insn & 0xFFFF0000u) == 0) ++udf_words;
                            else if ((insn & 0xFFC00000u) == 0) ++reserved_low_words;
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            emit("ERROR", title_id, total, unhandled_count, fmt::format("decode: {}", e.what()));
            continue;
        }

        if (total == 0) {
            emit("ERROR", title_id, 0, 0, "no nso text");
            continue;
        }

        // Exact encodings permit mnemonic-level ranking and fast decoder-only
        // rechecks without reopening archives. Keep this artifact with the survey.
        for (const auto& [insn, count] : unhandled_encodings) {
            encodings << fmt::format("{:016X}\t{:08X}\t{}\t{}\n", title_id, insn, count, rom_path);
        }
        encodings.flush();
        if (!encodings) return 1;

        // Ranked, so the note says what is missing and not only how much.
        std::vector<std::pair<u32, u64>> ranked{unhandled_sig.begin(), unhandled_sig.end()};
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) {
                      return a.second != b.second ? a.second > b.second : a.first < b.first;
                  });
        std::string note;
        for (size_t i = 0; i < ranked.size(); ++i) {
            note += fmt::format("{}{:08X}:{}:{:08X}", i ? " " : "", ranked[i].first,
                                ranked[i].second, unhandled_example.at(ranked[i].first));
        }
        if (note.empty()) {
            note = "-";
        }
        emit(unhandled_count == 0 ? "CLEAN" : "GAPS", title_id, total, unhandled_count, note);
    }
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "wb", stdout);
        freopen("CONOUT$", "wb", stderr);
    }
#endif

    try {

    // ── Portable user data for standalone game exports ──────────────────────
    // An exported game is a self-contained folder the user can move or delete
    // as a unit, so its config/saves/NAND/logs/screenshots live in <exe>/user
    // rather than %APPDATA%\suyu. KeysDir must be explicitly pinned at
    // %APPDATA%\suyu\keys (not left to derive on its own): the presence of a
    // sibling "user" folder next to the exe makes the FS layer's own
    // portable-mode auto-detection kick in first and silently rederive
    // KeysDir under <exe>/user/keys instead, so prod.keys/title.keys are
    // never bundled with a distributed export.
    // Must run before Log::Initialize(), which opens a file under LogDir.
#ifdef SUYU_CMD_STATIC_RECOMP
    {
        namespace FS = Common::FS;
        // Captured before the portable overrides below take effect: keys
        // belong to the user's installed suyu rather than to the export,
        // so this is where they still are once the rest has been
        // repointed into the export's own user directory.
        const std::filesystem::path installed_keys =
            FS::GetSuyuPath(FS::SuyuPath::KeysDir);
#ifdef _WIN32
        wchar_t exe_w[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_w, MAX_PATH);
        const std::filesystem::path user_root =
            std::filesystem::path(exe_w).parent_path() / L"user";
#else
        const std::filesystem::path user_root =
            std::filesystem::path(argv[0]).parent_path() / "user";
#endif
        std::filesystem::create_directories(user_root);
        // SetSuyuPath (path_util.cpp) fails with "is not a directory" if the
        // path doesn't exist yet - most of these get created lazily by
        // whatever subsystem first writes into them, but LoadDir/TASDir are
        // read from (mod scan, TAS script lookup) before anything writes to
        // them, so create every subdir up front instead of relying on that.
        for (const char* sub : {"config", "cache", "cache/shader", "log", "nand", "sdmc", "dump",
                                 "load", "screenshots", "play_time", "crash_dumps", "amiibo", "tas",
                                 "icons", "themes"}) {
            std::filesystem::create_directories(user_root / sub);
        }
        FS::SetSuyuPath(FS::SuyuPath::EdenDir, user_root);
        FS::SetSuyuPath(FS::SuyuPath::ConfigDir, user_root / "config");
        FS::SetSuyuPath(FS::SuyuPath::CacheDir, user_root / "cache");
        FS::SetSuyuPath(FS::SuyuPath::ShaderDir, user_root / "cache" / "shader");
        FS::SetSuyuPath(FS::SuyuPath::LogDir, user_root / "log");
        FS::SetSuyuPath(FS::SuyuPath::NANDDir, user_root / "nand");
        FS::SetSuyuPath(FS::SuyuPath::SaveDir, user_root / "nand");
        FS::SetSuyuPath(FS::SuyuPath::SDMCDir, user_root / "sdmc");
        FS::SetSuyuPath(FS::SuyuPath::DumpDir, user_root / "dump");
        FS::SetSuyuPath(FS::SuyuPath::LoadDir, user_root / "load");
        FS::SetSuyuPath(FS::SuyuPath::ScreenshotsDir, user_root / "screenshots");
        FS::SetSuyuPath(FS::SuyuPath::PlayTimeDir, user_root / "play_time");
        FS::SetSuyuPath(FS::SuyuPath::CrashDumpsDir, user_root / "crash_dumps");
        FS::SetSuyuPath(FS::SuyuPath::AmiiboDir, user_root / "amiibo");
        FS::SetSuyuPath(FS::SuyuPath::TASDir, user_root / "tas");
        FS::SetSuyuPath(FS::SuyuPath::IconsDir, user_root / "icons");
        FS::SetSuyuPath(FS::SuyuPath::ThemesDir, user_root / "themes");
#ifdef _WIN32
        FS::SetSuyuPath(FS::SuyuPath::KeysDir, FS::GetAppDataRoamingDirectory() / "suyu" / "keys");
#else
        // No roaming-appdata equivalent here, and the default already
        // points at the installed location on these platforms.
        FS::SetSuyuPath(FS::SuyuPath::KeysDir, installed_keys);
#endif
    }
#endif

    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Start();

    // mk8-recomp: --probe-isa <rom> prints the title's CPU architecture and
    // exits, before any config, window or emulation setup.
    for (int i = 1; i + 2 < argc; ++i) {
        if (std::string_view{argv[i]} == "--probe-isa-list") {
            return ProbeIsaList(argv[i + 1], argv[i + 2]);
        }
        if (std::string_view{argv[i]} == "--probe-decode-list") {
            return ProbeDecodeList(argv[i + 1], argv[i + 2]);
        }
    }

    LOG_INFO(Frontend, "suyu-cmd starting up...");
    Common::DetachedTasks detached_tasks;

    int option_index = 0;
#ifdef _WIN32
    int argc_w;
    auto argv_w = CommandLineToArgvW(GetCommandLineW(), &argc_w);

    if (argv_w == nullptr) {
        LOG_CRITICAL(Frontend, "Failed to get command line arguments");
        return -1;
    }
#endif
    std::string filepath;
    std::optional<std::string> config_path;
    std::string program_args;
    std::optional<int> selected_user;

    bool use_multiplayer = false;
    bool fullscreen = false;
    bool tas_playback = false;
    std::optional<u32> app_version_override;
    std::string app_display_version_override;
    Service::AM::FrontendAppletParameters load_parameters{};
    std::string nickname{};
    std::string password{};
    std::string address{};
    u16 port = Network::DefaultRoomPort;

    static struct option long_options[] = {
        // clang-format off
        {"config", required_argument, 0, 'c'},
        {"fullscreen", no_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {"game", required_argument, 0, 'g'},
        {"applet-params", optional_argument, 0, 'l'},
        {"multiplayer", required_argument, 0, 'm'},
        {"program", optional_argument, 0, 'p'},
        {"tas", no_argument, 0, 't'},
        {"user", required_argument, 0, 'u'},
        {"version", no_argument, 0, 'v'},
        {"app-version", required_argument, 0, 'V'},
        {0, 0, 0, 0},
        // clang-format on
    };

    while (optind < argc) {
        int arg = getopt_long(argc, argv, "g:fhvp::c:u:l::tV:", long_options, &option_index);
        if (arg != -1) {
            switch (static_cast<char>(arg)) {
            case 'c':
                config_path = optarg;
                break;
            case 'f':
                fullscreen = true;
                LOG_INFO(Frontend, "Starting in fullscreen mode...");
                break;
            case 'h':
                PrintHelp(argv[0]);
                return 0;
            case 'g': {
                const std::string str_arg(optarg);
                filepath = str_arg;
                break;
            }
            case 'l': {
                std::string str_arg(argv[optind++]);
                str_arg.append(",0"); // FALLBACK: if string is partially completed ("1234,3")
                                      // this will set all those unset to 0. otherwise we get
                                      // all 3s.
                std::stringstream stream(str_arg);
                std::string sub;
                std::getline(stream, sub, ',');
                load_parameters.program_id = std::stoull(sub);
                std::getline(stream, sub, ',');
                load_parameters.applet_id = static_cast<Service::AM::AppletId>(std::stoul(sub));
                std::getline(stream, sub, ',');
                load_parameters.applet_type = static_cast<Service::AM::AppletType>(std::stoi(sub));
                std::getline(stream, sub, ',');
                load_parameters.launch_type = static_cast<Service::AM::LaunchType>(std::stoi(sub));
                std::getline(stream, sub, ',');
                load_parameters.program_index = std::stoi(sub);
                std::getline(stream, sub, ',');
                load_parameters.previous_program_index = std::stoi(sub);
                break;
            }
            case 'm': {
                use_multiplayer = true;
                const std::string str_arg(optarg);
                // regex to check if the format is nickname:password@ip:port
                // with optional :password
                const std::regex re("^([^:]+)(?::(.+))?@([^:]+)(?::([0-9]+))?$");
                if (!std::regex_match(str_arg, re)) {
                    std::cout << "Wrong format for option --multiplayer\n";
                    PrintHelp(argv[0]);
                    return 0;
                }

                std::smatch match;
                std::regex_search(str_arg, match, re);
                ASSERT(match.size() == 5);
                nickname = match[1];
                password = match[2];
                address = match[3];
                if (!match[4].str().empty()) {
                    port = static_cast<u16>(std::strtoul(match[4].str().c_str(), nullptr, 0));
                }
                std::regex nickname_re("^[a-zA-Z0-9._\\- ]+$");
                if (!std::regex_match(nickname, nickname_re)) {
                    std::cout
                        << "Nickname is not valid. Must be 4 to 20 alphanumeric characters.\n";
                    return 0;
                }
                if (address.empty()) {
                    std::cout << "Address to room must not be empty.\n";
                    return 0;
                }
                break;
            }
            case 't':
                tas_playback = true;
                break;
            case 'p':
                program_args = argv[optind];
                ++optind;
                break;
            case 'u':
                selected_user = atoi(optarg);
                break;
            case 'v':
                PrintVersion();
                return 0;
            case 'V': {
                // <numeric>[:<display>]. The numeric part is what the guest sees through
                // the application version, the display part is the string a title prints
                // for itself.
                const std::string str_arg(optarg);
                const auto colon = str_arg.find(':');
                const std::string numeric = str_arg.substr(0, colon);
                try {
                    app_version_override = static_cast<u32>(std::stoul(numeric));
                } catch (const std::exception&) {
                    std::cout << "Invalid --app-version: " << str_arg
                              << " (expected <number>[:<display>])\n";
                    return 0;
                }
                if (colon != std::string::npos) {
                    app_display_version_override = str_arg.substr(colon + 1);
                }
                break;
            }
            }
        } else {
#ifdef _WIN32
            filepath = Common::UTF16ToUTF8(argv_w[optind]);
#else
            filepath = argv[optind];
#endif
            optind++;
        }
    }

    SdlConfig config{config_path};

    // apply the log_filter setting
    // the logger was initialized before and doesn't pick up the filter on its own
    Common::Log::Filter filter;
    filter.ParseFilterString(Settings::values.log_filter.GetValue());
    Common::Log::SetGlobalFilter(filter);

    if (!program_args.empty()) {
        Settings::values.program_args = program_args;
    }

    if (selected_user.has_value()) {
        Settings::values.current_user = std::clamp(*selected_user, 0, 7);
    }

    if (tas_playback) {
        // Must be set before the input subsystem is constructed: the TAS driver
        // only reads the scripts out of the TAS directory when it sees this
        // enabled, and it is applied here so the config file cannot clear it.
        Settings::values.tas_enable.SetValue(true);
    }

#ifdef _WIN32
    LocalFree(argv_w);
#endif

    MicroProfileOnThreadCreate("EmuThread");
    SCOPE_EXIT {
        MicroProfileShutdown();
    };

    Common::ConfigureNvidiaEnvironmentFlags();

    // Auto-detect ROM / exefs alongside the executable when no -g flag is given
    if (filepath.empty() && !static_cast<u32>(load_parameters.applet_id)) {
#ifdef _WIN32
        wchar_t exe_path_w[MAX_PATH];
        GetModuleFileNameW(nullptr, exe_path_w, MAX_PATH);
        const std::filesystem::path exe_dir = std::filesystem::path(exe_path_w).parent_path();
#else
        const std::filesystem::path exe_dir =
            std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
        // Prefer deconstructed exefs dir (Switch ROM viewer structure: exefs/main)
        const std::filesystem::path exefs_main = exe_dir / "exefs" / "main";
        if (std::filesystem::exists(exefs_main)) {
#ifdef _WIN32
            filepath = Common::UTF16ToUTF8(exefs_main.wstring());
#else
            filepath = exefs_main.string();
#endif
            LOG_INFO(Frontend, "Auto-detected exefs/main: {}", filepath);
            goto rom_found;
        }
        // Fall back to packed ROM files (XCI/NSP/NCA)
        static constexpr std::array<std::string_view, 3> exts{".xci", ".nsp", ".nca"};
        for (const auto& entry : std::filesystem::directory_iterator(exe_dir)) {
            const auto ext = Common::ToLower(entry.path().extension().string());
            for (const auto e : exts) {
                if (ext == e) {
#ifdef _WIN32
                    filepath = Common::UTF16ToUTF8(entry.path().wstring());
#else
                    filepath = entry.path().string();
#endif
                    LOG_INFO(Frontend, "Auto-detected ROM: {}", filepath);
                    goto rom_found;
                }
            }
        }
        LOG_CRITICAL(Frontend, "No ROM specified, no exefs/main found, and no XCI/NSP/NCA next to exe");
        return -1;
        rom_found:;
    }

    // Native recompiled CPU modules, in NSO load order: rtld(0), main(1),
    // subsdk0-N(2..N+1), sdk(last). Whichever way they arrive, registering any
    // of them makes ArmRecomp run the game's CPU natively instead of dynarmic.
    struct RecompModule {
        Core::RecompBlockFn (*lookup)(u64){};
        void (*set_base)(u64){};
        unsigned (*guard_v2)(unsigned){};
    };
    static std::vector<RecompModule> s_recomp_modules;
    bool recomp_guard_ready = false;

    // Preferred path: modules compiled straight into this executable. Nothing
    // to find on disk, nothing to load, and no version skew between the exe and
    // its modules.
#ifdef SUYU_CMD_STATIC_RECOMP
    {
        unsigned count = 0;
        const SuyuRecompStaticModule* mods = suyu_recomp_static_modules(&count);
        for (unsigned i = 0; i < count; ++i) {
            s_recomp_modules.push_back({mods[i].lookup, mods[i].set_base});
            LOG_INFO(Frontend, "Static recompiled module [{}] {} — ArmRecomp active", i,
                     mods[i].name ? mods[i].name : "?");
        }
#ifdef SUYU_RECOMP_GUARD_V2
        recomp_guard_ready = suyu_recomp_static_guard_v2(2) != 0;
#endif
    }
#endif

    // Compatibility path for exports that ship recompiled_*.dll beside the exe:
    // recompiled_rtld.dll, recompiled_image.dll (main), recompiled_subsdk0.dll,
    // recompiled_sdk.dll. Skipped entirely when modules are already linked in.
#ifdef _WIN32
    if (s_recomp_modules.empty()) {
        wchar_t _exe_w[MAX_PATH]{};
        GetModuleFileNameW(nullptr, _exe_w, MAX_PATH);
        const auto _exe_dir = std::filesystem::path(_exe_w).parent_path();

        // Load in standard NSO load order: rtld, main (recompiled_image), subsdk0..9, sdk
        std::vector<std::wstring> dll_order = {
            L"recompiled_rtld.dll",
            L"recompiled_image.dll",  // main
            L"recompiled_subsdk0.dll", L"recompiled_subsdk1.dll", L"recompiled_subsdk2.dll",
            L"recompiled_subsdk3.dll", L"recompiled_subsdk4.dll", L"recompiled_subsdk5.dll",
            L"recompiled_subsdk6.dll", L"recompiled_subsdk7.dll", L"recompiled_subsdk8.dll",
            L"recompiled_subsdk9.dll",
            L"recompiled_sdk.dll",
        };

        for (const auto& dll_name : dll_order) {
            const auto p = _exe_dir / dll_name;
            if (!std::filesystem::exists(p)) continue;
            HMODULE h = LoadLibraryW(p.wstring().c_str());
            if (!h) {
                LOG_WARNING(Frontend, "Found {} but LoadLibrary failed (err={})",
                            Common::UTF16ToUTF8(dll_name), GetLastError());
                continue;
            }
            using LookupFn = Core::RecompBlockFn (*)(u64);
            using SetBaseFn = void (*)(u64);
            auto lkp = reinterpret_cast<LookupFn>(GetProcAddress(h, "recomp_image_lookup"));
            auto sbf = reinterpret_cast<SetBaseFn>(GetProcAddress(h, "recomp_image_set_base"));
            if (lkp) {
                auto guard = reinterpret_cast<unsigned (*)(unsigned)>(GetProcAddress(h, "recomp_image_guard_v2"));
                s_recomp_modules.push_back({lkp, sbf, guard});
                LOG_INFO(Frontend, "Native recompiled module [{}] loaded from {} — ArmRecomp active",
                         s_recomp_modules.size() - 1, Common::UTF16ToUTF8(dll_name));
            }
        }
        recomp_guard_ready = !s_recomp_modules.empty();
        for (const auto& module : s_recomp_modules) {
            if (!module.guard_v2 || module.guard_v2(0) != 2) recomp_guard_ready = false;
        }
        for (const auto& module : s_recomp_modules) {
            if (module.guard_v2) module.guard_v2(recomp_guard_ready ? 2 : 0);
        }
    }
#endif

    if (!s_recomp_modules.empty()) {
        // Combined lookup: try each module's lookup until one returns non-null.
        Core::SetRecompLookup([](u64 pc) -> Core::RecompBlockFn {
            for (const auto& m : s_recomp_modules) {
                if (auto fn = m.lookup(pc)) return fn;
            }
            return nullptr;
        });
        Core::SetRecompCodeGuardReady(recomp_guard_ready);
        LOG_INFO(Frontend, "Recompiled instruction guard-v2: {}",
                 recomp_guard_ready ? "ready" : "not negotiated");
        // Route base to the module at the same index in load order.
        // rtld=index0, main=index1, subsdk0=index2, ..., sdk=last.
        Core::SetRecompBaseSetter([](size_t index, const char*, u64 base) {
            if (index < s_recomp_modules.size() && s_recomp_modules[index].set_base) {
                s_recomp_modules[index].set_base(base);
            }
        });
        // A window running native recompiled code is a standalone game export,
        // not the suyu dev frontend — the window chrome (title/icon) should
        // read as the game, not the emulator.
        g_native_export_mode = true;

        // Decode video on the CPU unless the user has chosen otherwise.
        // Handing the guest's VP9 streams to a hardware decoder (d3d11va on
        // Windows) deadlocks partway through the first movie on at least Intel
        // integrated graphics: the process stays alive and the log stops
        // mid-line, which presents as a permanently black window right after
        // boot. An export is something a player double-clicks with no
        // settings UI in front of them, so it defaults to the path that always
        // finishes over the one that is faster when it works.
        if (Settings::values.nvdec_emulation.UsingGlobal() &&
            Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu) {
            Settings::values.nvdec_emulation.SetValue(Settings::NvdecEmulation::Cpu);
            LOG_INFO(Frontend, "Native export: using CPU video decoding");
        }
    }

    // Mods/patches: a standalone export is a self-contained folder, so a "mods"
    // directory beside the executable is where users will drop things. Point
    // the existing load directory at it and the normal PatchManager path
    // (LayeredFS, IPS/pchtxt patches, cheats) picks it up unchanged - same
    // <title_id>/<mod name>/ layout the Qt frontend uses.
    {
#ifdef _WIN32
        wchar_t exe_w[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_w, MAX_PATH);
        const auto exe_dir = std::filesystem::path(exe_w).parent_path();
#else
        const auto exe_dir = std::filesystem::path(argv[0]).parent_path();
#endif
        const auto local_mods = exe_dir / "mods";
        std::error_code ec;
        std::filesystem::create_directories(local_mods, ec);
        if (std::filesystem::is_directory(local_mods)) {
            Common::FS::SetSuyuPath(Common::FS::SuyuPath::LoadDir, local_mods);
            LOG_INFO(Frontend, "Using local mod directory: {}", local_mods.string());
        }
        LOG_INFO(Frontend, "Keys directory (never bundled): {}",
                 Common::FS::GetSuyuPathString(Common::FS::SuyuPath::KeysDir));
    }

    LOG_INFO(Frontend, "suyu-cmd: Initializing system...");
    Core::System system{};
    system.Initialize();
    LOG_INFO(Frontend, "suyu-cmd: System initialized.");

    InputCommon::InputSubsystem input_subsystem{};

    // Apply the command line arguments
    system.ApplySettings();

    std::unique_ptr<EmuWindow_SDL2> emu_window;
    switch (Settings::values.renderer_backend.GetValue()) {
    case Settings::RendererBackend::OpenGL_GLSL:
    case Settings::RendererBackend::OpenGL_GLASM:
    case Settings::RendererBackend::OpenGL_SPIRV:
        emu_window = std::make_unique<EmuWindow_SDL2_GL>(&input_subsystem, system, fullscreen);
        break;
    case Settings::RendererBackend::Vulkan:
        emu_window = std::make_unique<EmuWindow_SDL2_VK>(&input_subsystem, system, fullscreen);
        break;
    case Settings::RendererBackend::Null:
        emu_window = std::make_unique<EmuWindow_SDL2_Null>(&input_subsystem, system, fullscreen);
        break;
    default:
        emu_window = std::make_unique<EmuWindow_SDL2_VK>(&input_subsystem, system, fullscreen);
        break;
    }

    if (tas_playback) {
        emu_window->EnableTasPlayback();
    }

#ifdef _WIN32
    Common::Windows::SetCurrentTimerResolutionToMaximum();
    system.CoreTiming().SetTimerResolutionNs(Common::Windows::GetCurrentTimerResolution());
#endif

    LOG_INFO(Frontend, "suyu-cmd: Window created, loading game...");
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    // The command line wins over the configuration file, so a one-off run can differ
    // from the persisted setting without editing it.
    if (!app_version_override) {
        const u32 configured = Settings::values.application_version_override.GetValue();
        const std::string& configured_display =
            Settings::values.application_display_version_override.GetValue();
        if (configured != 0 || !configured_display.empty()) {
            app_version_override = configured;
            if (app_display_version_override.empty()) {
                app_display_version_override = configured_display;
            }
        }
    }

    if (app_version_override) {
        // Deconstructed ROM directories carry no control data, so GetDisplayVersion has
        // nothing to read and falls back to a hard-coded 1.0.0. Titles that report their
        // own version, and anything that checks version compatibility, then see a value
        // that does not match the code actually loaded.
        LOG_INFO(Frontend, "suyu-cmd: reporting application version {} ({})",
                 *app_version_override,
                 app_display_version_override.empty() ? "no display version"
                                                      : app_display_version_override);
        system.SetApplicationVersionOverride(*app_version_override,
                                             app_display_version_override);
    }
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    system.GetUserChannel().clear();

    if (static_cast<u32>(load_parameters.applet_id)) {
        // code below based off of suyu/main.cpp : GMainWindow::OnHomeMenu()
        // Inline minimal mapping (AppletIdToProgramId is in an anonymous namespace)
        const auto applet_id_to_prog_id = [](Service::AM::AppletId id) -> Service::AM::AppletProgramId {
            using namespace Service::AM;
            switch (id) {
            case AppletId::QLaunch:        return AppletProgramId::QLaunch;
            case AppletId::Starter:        return AppletProgramId::Starter;
            case AppletId::Auth:           return AppletProgramId::Auth;
            case AppletId::OverlayDisplay: return AppletProgramId::OverlayDisplay;
            default:                       return static_cast<AppletProgramId>(0);
            }
        };
        Service::AM::AppletProgramId applet_prog_id = applet_id_to_prog_id(load_parameters.applet_id);
        auto sysnand = system.GetFileSystemController().GetSystemNANDContents();
        if (!sysnand) {
            LOG_CRITICAL(Frontend, "Failed to load applet: Firmware not installed.");
            return -1;
        }

        auto user_applet_nca = sysnand->GetEntry(static_cast<u64>(applet_prog_id),
                                                 FileSys::ContentRecordType::Program);
        if (!user_applet_nca) {
            LOG_CRITICAL(Frontend, "Failed to load applet: applet cannot be found.");
            return -1;
        }
        if (filepath.empty())
            filepath = user_applet_nca->GetFullPath();
    } else {
        load_parameters.applet_id = Service::AM::AppletId::Application;
    }
    LOG_INFO(Frontend, "suyu-cmd: Calling system.Load for '{}'...", filepath);
    const Core::SystemResultStatus load_result{system.Load(*emu_window, filepath, load_parameters)};
    LOG_INFO(Frontend, "suyu-cmd: system.Load returned: {}", static_cast<int>(load_result));

    switch (load_result) {
    case Core::SystemResultStatus::ErrorGetLoader:
        LOG_CRITICAL(Frontend, "Failed to obtain loader for {}!", filepath);
        return -1;
    case Core::SystemResultStatus::ErrorLoader:
        LOG_CRITICAL(Frontend, "Failed to load ROM!");
        return -1;
    case Core::SystemResultStatus::ErrorNotInitialized:
        LOG_CRITICAL(Frontend, "CPUCore not initialized");
        return -1;
    case Core::SystemResultStatus::ErrorVideoCore:
        LOG_CRITICAL(Frontend, "Failed to initialize VideoCore!");
        return -1;
    case Core::SystemResultStatus::Success:
        break; // Expected case
    default:
        if (static_cast<u32>(load_result) >
            static_cast<u32>(Core::SystemResultStatus::ErrorLoader)) {
            const u16 loader_id = static_cast<u16>(Core::SystemResultStatus::ErrorLoader);
            const u16 error_id = static_cast<u16>(load_result) - loader_id;
            LOG_CRITICAL(Frontend,
                         "While attempting to load the ROM requested, an error occurred. Please "
                         "refer to the suyu wiki for more information or the suyu discord for "
                         "additional help.\n\nError Code: {:04X}-{:04X}\nError Description: {}",
                         loader_id, error_id, static_cast<Loader::ResultStatus>(error_id));
        }
        break;
    }

    if (use_multiplayer) {
        if (auto member = system.GetRoomNetwork().GetRoomMember().lock()) {
            member->BindOnChatMessageReceived(OnMessageReceived);
            member->BindOnStatusMessageReceived(OnStatusMessageReceived);
            member->BindOnStateChanged(OnStateChanged);
            member->BindOnError(OnNetworkError);
            LOG_DEBUG(Network, "Start connection to {}:{} with nickname {}", address, port,
                      nickname);
            member->Join(nickname, address.c_str(), port, 0, Network::NoPreferredIP, password);
        } else {
            LOG_ERROR(Network, "Could not access RoomMember");
            return 0;
        }
    }

    // Core is loaded, start the GPU (makes the GPU contexts current to this thread)
    system.GPU().Start();
    system.GetCpuManager().OnGpuReady();

    // A game export is launched over and over by a player, always for the same
    // title, so the disk shader cache is the difference between a long black
    // screen on every single run and one slow first run. Keep it for exports
    // and keep the old blanket disable for the plain dev frontend, where the
    // startup instability it works around was originally seen.
    if (!g_native_export_mode && Settings::values.use_disk_shader_cache.GetValue()) {
        LOG_WARNING(Frontend,
                    "suyu-cmd: disabling disk shader cache for this run to avoid known startup instability");
        Settings::values.use_disk_shader_cache.SetValue(false);
    }

    if (Settings::values.use_disk_shader_cache.GetValue()) {
        try {
            system.Renderer().ReadRasterizer()->LoadDiskResources(
                system.GetApplicationProcessProgramID(), std::stop_token{},
                [](VideoCore::LoadCallbackStage, size_t value, size_t total) {});
        } catch (const std::exception& e) {
            LOG_ERROR(Frontend, "Failed to load disk shader cache: {}", e.what());
        } catch (...) {
            LOG_ERROR(Frontend, "Failed to load disk shader cache due to unknown exception");
        }
    }

    system.RegisterExitCallback([&] {
        // Just exit right away.
        exit(0);
    });

#ifdef __unix__
    Common::Linux::StartGamemode();
#endif

    void(system.Run());
    if (system.DebuggerEnabled()) {
        system.InitializeDebugger();
    }

    // Periodic performance samples for benchmarking.
    //
    // Timing a replay end to end says little when a run can stall partway
    // and still finish: the stall is averaged in invisibly, and a run that
    // never finishes yields no number at all. A series lets a measurement
    // pick a window, and a stall shows up in it as a gap.
    //
    // On a thread of its own because the loop below blocks in WaitEvent:
    // samples driven from there would stop arriving exactly when the
    // emulator stops making progress, which is the case worth seeing. The
    // window-title refresh gives up the counters while this is enabled, so
    // there is still only one reader of them.
    const bool perf_sampling = std::getenv("SUYU_CMD_PERF_SAMPLE") != nullptr;
    std::atomic<bool> perf_sampling_run{perf_sampling};
    std::thread perf_sampler;
    if (perf_sampling) {
        perf_sampler = std::thread([&system, &perf_sampling_run] {
            while (perf_sampling_run.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::seconds{1});
                if (!perf_sampling_run.load(std::memory_order_relaxed)) {
                    break;
                }
                const auto r = system.GetAndResetPerfStats();
                LOG_INFO(Frontend,
                         "PERF game_fps={:.3f} system_fps={:.3f} frametime_ms={:.3f} "
                         "speed={:.4f}",
                         r.average_game_fps, r.system_fps, r.frametime * 1000.0,
                         r.emulation_speed);
            }
        });
    }

    while (emu_window->IsOpen()) {
        emu_window->WaitEvent();
    }

    perf_sampling_run.store(false, std::memory_order_relaxed);
    if (perf_sampler.joinable()) {
        perf_sampler.join();
    }

    system.DetachDebugger();
    void(system.Pause());
    system.ShutdownMainProcess();

#ifdef __unix__
    Common::Linux::StopGamemode();
#endif

    detached_tasks.WaitForAllTasks();
    return 0;
    } catch (const std::exception& e) {
        LOG_CRITICAL(Frontend, "Unhandled fatal exception in suyu-cmd: {}", e.what());
        return -1;
    } catch (...) {
        LOG_CRITICAL(Frontend, "Unhandled unknown fatal exception in suyu-cmd");
        return -1;
    }
}
