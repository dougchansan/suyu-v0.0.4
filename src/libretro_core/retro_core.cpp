// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Real libretro core entry points for suyu (RetroArch loads this .dll as a
// core, in contrast to core/libretro_wrapper.cpp which is suyu acting as a
// libretro *frontend* loading other cores - the two are separate features).
//
// STATUS: boots and runs a game headlessly via Core::System, exactly like
// suyu_cmd (src/suyu_cmd/suyu.cpp) does without Qt. System info, environment
// negotiation, load/unload/reset, and serialize size are real and correct.
//
// WIRED AND VERIFIED:
//   - Video: Vulkan renderer runs headless, rendering to a CPU buffer via
//     RenderToBuffer which retro_run reads back (with a B8G8R8A8 -> XRGB8888
//     channel swap). Falls back to a black frame before the first one lands.
//   - Audio: by default suyu opens a host audio device and plays directly,
//     which is what sounds correct. An "Audio Output" core option can instead
//     route samples through retro_audio_sample_batch via the
//     AudioEngine::Libretro sink - tidier in principle, but nothing paces the
//     emulated renderer here so it delivers in bursts.
//   - Input: retro_input_state_cb is bridged into InputCommon's
//     VirtualGamepad (16 buttons + both analog sticks).
//   - Keys: prod/title/console.keys are picked up from the frontend's
//     system directory (<system>/suyu/keys) if not already installed.
//   - Online: suyu's own room-based multiplayer is initialised here, so
//     online play works in the core the same way it does under Qt.
//
// NOT WIRED (deliberately, not silently faked):
//   - Save states: retro_serialize/unserialize return false and
//     retro_serialize_size() returns 0 - see the comment there. This also
//     rules out RetroArch netplay and rerecording, which are defined in
//     libretro.h as depending on serialization; the core declares
//     RETRO_SERIALIZATION_QUIRK_INCOMPLETE so the frontend reports them as
//     unavailable rather than offering them and failing later.

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
#include <string>
#include <filesystem>
#include <vector>
#ifdef SUYU_ANDROID_LIBRETRO
#include <sys/system_properties.h>
#endif
#include "audio_core/sink/libretro_sink.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/crypto/key_manager.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "core/perf_stats.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/drivers/tas_input.h"
#include "input_common/main.h"
#include "network/network.h"
#include "libretro_core/libretro.h"
#include "libretro_core/retro_emu_window.h"
#include "suyu_cmd/explicit_update.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/shader_notify.h"

namespace {

std::string GetRuntimeOption(const char* environment_key,
                             [[maybe_unused]] const char* android_property) {
    if (const char* value = std::getenv(environment_key)) {
        return value;
    }
#ifdef SUYU_ANDROID_LIBRETRO
    char value[PROP_VALUE_MAX]{};
    const int length = __system_property_get(android_property, value);
    if (length > 0 && length < PROP_VALUE_MAX) {
        return std::string(value, static_cast<size_t>(length));
    }
#endif
    return {};
}

bool HasUsableNcaHeaderKey() {
    const auto key = Core::Crypto::KeyManager::Instance().GetKey(Core::Crypto::S256KeyType::Header);
    constexpr size_t half_size = 16;
    // OpenSSL XTS rejects a missing key and equal key halves. Check the
    // already-loaded key material without attempting to decrypt content.
    return std::any_of(key.begin(), key.end(), [](u8 byte) { return byte != 0; }) &&
           !std::equal(key.begin(), key.begin() + half_size, key.begin() + half_size);
}


std::unique_ptr<Core::System> g_system;
std::unique_ptr<SuyuCli::ExplicitUpdateProvider> g_explicit_provider;
std::unique_ptr<LibretroCore::RetroEmuWindow> g_emu_window;
std::shared_ptr<InputCommon::InputSubsystem> g_input_subsystem;
std::string g_game_path;
bool g_game_loaded = false;
bool g_tas_playback = false;
bool g_previous_tas_enable = false;
bool g_previous_tas_loop = false;
std::filesystem::path g_previous_tas_directory;
bool g_sample_perf = false;
std::chrono::steady_clock::time_point g_perf_start;
std::chrono::steady_clock::time_point g_perf_last_sample;
// On-screen game performance readout (core option). It shares the per-second
// sampler above, because GetAndResetPerfStats may only be read once per interval.
bool g_show_perf = true;
bool g_message_ext = false;
bool g_can_dupe = false;
// FramesDisplayed() value of the frame last uploaded; equal means nothing new to show.
u64 g_presented_frame = 0;
bool g_have_presented = false;
unsigned g_perf_frontend_frames = 0;
unsigned g_perf_unique_frames = 0;

unsigned g_output_scale = 1;
bool g_geometry_dirty = false;

// False: suyu drives a host audio device directly (default, sounds correct).
// True: samples are handed to the frontend via retro_audio_sample_batch.
bool g_use_frontend_audio = false;
// Set once RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK registration succeeds (see
// retro_load_game()). When true, retro_run() leaves audio delivery to
// FrontendAudioCallback() instead of doing it inline, since both draining the
// same queue would just race pointlessly.
bool g_audio_callback_registered = false;

retro_environment_t g_environ_cb;
retro_video_refresh_t g_video_cb;
retro_audio_sample_t g_audio_sample_cb;
retro_audio_sample_batch_t g_audio_batch_cb;
retro_input_poll_t g_input_poll_cb;
retro_input_state_t g_input_state_cb;

constexpr unsigned kFrameWidth = 1280;
constexpr unsigned kFrameHeight = 720;

// System::Initialize() may decrypt firmware metadata. Make the frontend's
// existing keys visible before that call, not after it has already tried to
// create crypto contexts with a missing key.
void PrepareLibretroKeys() {
    const auto keys_dir = Common::FS::GetSuyuPath(Common::FS::SuyuPath::KeysDir);
    const auto roaming = keys_dir.parent_path().parent_path();
    for (const auto& emu : {"suyu", "yuzu", "sudachi", "citron", "Ryujinx"}) {
        const auto src_dir = roaming / emu / "keys";
        std::error_code ec;
        if (!std::filesystem::exists(src_dir, ec)) {
            continue;
        }
        LOG_INFO(Frontend, "libretro: found {} key directory at {}", emu, src_dir.string());
        if (!Common::FS::CreateDirs(keys_dir)) {
            LOG_WARNING(Frontend, "libretro: could not prepare local key directory");
            continue;
        }
        for (const auto& name : {"prod.keys", "title.keys", "console.keys"}) {
            const auto src = src_dir / name;
            const auto dst = keys_dir / name;
            if (std::filesystem::exists(src, ec) && !std::filesystem::exists(dst, ec)) {
                std::filesystem::copy_file(src, dst, ec);
                if (!ec) {
                    LOG_INFO(Frontend, "libretro: adopted {} from {}", name, emu);
                }
            }
        }
    }

    if (g_environ_cb) {
        const char* system_dir = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir) {
            const auto src_dir = std::filesystem::path(system_dir) / "suyu" / "keys";
            LOG_INFO(Frontend, "libretro: checking for keys in: {}", src_dir.string());
            if (std::filesystem::exists(src_dir)) {
                if (!Common::FS::CreateDirs(keys_dir)) {
                    LOG_WARNING(Frontend, "libretro: could not prepare local key directory");
                } else {
                    for (const auto& name : {"prod.keys", "title.keys", "console.keys"}) {
                        const auto src = src_dir / name;
                        const auto dst = keys_dir / name;
                        if (std::filesystem::exists(src) && !std::filesystem::exists(dst)) {
                            std::error_code ec;
                            std::filesystem::copy_file(src, dst, ec);
                            if (!ec) {
                                LOG_INFO(Frontend, "libretro: copied {} from RetroArch system dir", name);
                            }
                        }
                    }
                }
            }
        }
    }
}

bool ReadShowPerfOption() {
    retro_variable var{"suyu_show_perf", nullptr};
    return !(g_environ_cb && g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
             std::string(var.value) == "Off");
}

void ShowPerfMessage(std::string text) {
#ifdef __APPLE__
    // RetroArch on macOS sizes the right-anchored status box about a quarter
    // narrower than the text it draws, clipping the end of the readout at the
    // window edge. Trailing spaces draw as nothing but widen the box; half the
    // character count was enough in every form on RetroArch 1.22.2.
    std::size_t code_points = 0;
    for (const unsigned char c : text) {
        code_points += (c & 0xC0) != 0x80;
    }
    text.append((code_points + 1) / 2, ' ');
#endif
    // Refreshed every second; the duration overlaps the next update so it never
    // blinks, and a status message replaces the previous one instead of queueing.
    if (g_message_ext) {
        retro_message_ext message{};
        message.msg = text.c_str();
        message.duration = 1500;
        message.priority = 1;
        message.level = RETRO_LOG_INFO;
        message.target = RETRO_MESSAGE_TARGET_OSD;
        message.type = RETRO_MESSAGE_TYPE_STATUS;
        message.progress = -1;
        g_environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &message);
    } else {
        retro_message message{text.c_str(), 90};
        g_environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
    }
}
// Points a single suyu data directory at a subfolder of a frontend-provided
// base directory, creating it first since Common::FS::SetSuyuPath silently
// refuses to point at a path that doesn't exist yet (or isn't a directory).
bool RedirectSuyuDir(Common::FS::SuyuPath suyu_path, const std::filesystem::path& base,
                      const char* sub_dir) {
    if (base.empty()) {
        return false;
    }
    const auto target = base / sub_dir;
    std::error_code ec;
    std::filesystem::create_directories(target, ec);
    if (ec) {
        LOG_ERROR(Frontend, "libretro: failed to create {} ({})", target.string(), ec.message());
        return false;
    }
    Common::FS::SetSuyuPath(suyu_path, target);
    return true;
}

// Redirects suyu's persistent data out of %APPDATA%\suyu (or <retroarch>\user
// in portable mode) and into the directories RetroArch itself hands the core:
//   - RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY for config/keys/cache/logs
//   - RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY for NAND/SDMC/saves/screenshots,
//     so RetroArch's own save handling (per-core folders, backups, cloud
//     sync, etc.) covers suyu's data the same way it covers any other core
// Each is placed directly in the frontend-provided directory (e.g.
// <system_directory>\config, <save_directory>\nand) with no extra "suyu"
// subfolder, so per-core RetroArch overrides that already point
// system_directory/save_directory somewhere suyu-specific (as set up in a
// core override .cfg) land exactly where they say, with nothing extra
// appended. If you share that base directory with another emulator, point
// the override somewhere suyu-specific instead of relying on this code to
// namespace it for you.
// Controlled by the "suyu_use_frontend_dirs" core option, enabled by default.
// Must run before anything (including keys import, below) touches
// Common::FS::GetSuyuPath, or those uses will already have grabbed the old,
// non-redirected paths.
void RedirectSuyuPathsToFrontend() {
    if (!g_environ_cb) {
        return;
    }

    struct retro_variable var {
        "suyu_use_frontend_dirs", nullptr
    };
    // Frontends make the declared default available as soon as
    // RETRO_ENVIRONMENT_SET_VARIABLES has run, i.e. before retro_init() is
    // even called, so this reads correctly on a completely fresh install too.
    bool use_frontend_dirs = true;
    if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        use_frontend_dirs = std::strcmp(var.value, "Disabled") != 0;
    }
    if (!use_frontend_dirs) {
        LOG_INFO(Frontend, "libretro: suyu_use_frontend_dirs disabled, keeping default data location");
        return;
    }

    const char* system_dir = nullptr;
    const char* save_dir = nullptr;
    g_environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
    g_environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir);

    const std::filesystem::path system_base =
        (system_dir && *system_dir) ? std::filesystem::path(system_dir) : std::filesystem::path{};
    // Some setups (e.g. RetroArch's "Save files in content directory") can
    // report an empty save directory even though a system directory exists.
    // Fall back to the system directory as the root for save-type data too,
    // rather than silently reverting to %APPDATA% for just those folders.
    const std::filesystem::path save_base =
        (save_dir && *save_dir) ? std::filesystem::path(save_dir) : system_base;

    if (system_base.empty()) {
        LOG_WARNING(Frontend, "libretro: frontend gave no system directory; "
                              "keeping suyu's default (%APPDATA%/portable) data location");
        return;
    }

    using Common::FS::SuyuPath;

    // Save-type data: goes under RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY.
    RedirectSuyuDir(SuyuPath::NANDDir, save_base, "nand");
    RedirectSuyuDir(SuyuPath::SaveDir, save_base, "nand"); // suyu aliases SaveDir to the NAND dir
    RedirectSuyuDir(SuyuPath::SDMCDir, save_base, "sdmc");
    RedirectSuyuDir(SuyuPath::ScreenshotsDir, save_base, "screenshots");
    RedirectSuyuDir(SuyuPath::TASDir, save_base, "tas");
    RedirectSuyuDir(SuyuPath::PlayTimeDir, save_base, "play_time");
    RedirectSuyuDir(SuyuPath::AmiiboDir, save_base, "amiibo");
    RedirectSuyuDir(SuyuPath::LoadDir, save_base, "load");
    RedirectSuyuDir(SuyuPath::DumpDir, save_base, "dump");

    // Fixed/system-type data: goes under RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY.
    RedirectSuyuDir(SuyuPath::ConfigDir, system_base, "config");
    RedirectSuyuDir(SuyuPath::KeysDir, system_base, "keys");
    RedirectSuyuDir(SuyuPath::CacheDir, system_base, "cache");
    RedirectSuyuDir(SuyuPath::ShaderDir, system_base, "cache/shader");
    RedirectSuyuDir(SuyuPath::LogDir, system_base, "log");
    RedirectSuyuDir(SuyuPath::CrashDumpsDir, system_base, "crash_dumps");

    // Root path that a couple of rarely-used secondary subsystems read
    // directly (a nested-libretro-core loader's default firmware directory,
    // and the hactool integration helper's temp-extraction and tool-search
    // paths). Pointed at the system directory itself, with no subfolder, so
    // nothing is ever written under %APPDATA% even for these.
    {
        std::error_code ec;
        std::filesystem::create_directories(system_base, ec);
        Common::FS::SetSuyuPath(SuyuPath::EdenDir, system_base);
    }

    LOG_INFO(Frontend, "libretro: redirected suyu data - system={} save={}", system_base.string(),
              save_base.string());
}

} // namespace

extern "C" {

RETRO_API void retro_set_environment(retro_environment_t cb) {
    g_environ_cb = cb;

    bool no_content = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

    static const struct retro_variable vars[] = {
        // This headless readback window only implements Vulkan. Advertising
        // other renderers would claim an option that cannot take effect.
        {"suyu_resolution", "Internal Resolution (reload content); 1x|2x|3x|4x"},
        {"suyu_scaling_filter", "Window Adapting Filter (reload content); Bilinear|Bicubic|Lanczos|ScaleForce|FSR|NearestNeighbor"},
        {"suyu_anti_aliasing", "Anti-Aliasing (reload content); None|FXAA|SMAA"},
        {"suyu_cpu_accuracy", "CPU Accuracy (reload content); Auto|Accurate|Unsafe"},
        {"suyu_use_docked", "Docked Mode (reload content); Yes|No"},
        {"suyu_fastmem", "Fastmem (reload content); Enabled|Disabled"},
        // Redirects suyu's data (NAND/SDMC/saves/config/keys/cache/logs) into
        // RetroArch's own system/save directories instead of suyu's normal
        // %APPDATA%\suyu (or <retroarch>\user in portable mode) location.
        // Default is Enabled so the core behaves like other libretro cores
        // out of the box; switch to Disabled to keep using a standalone
        // suyu install's existing data instead.
        {"suyu_use_frontend_dirs", "Use RetroArch System/Save Directories; Enabled|Disabled"},
        {"suyu_audio_output", "Audio Output (reload content); Host (direct)|Frontend (libretro)"},
        // suyu's own online play. RetroArch's netplay can't drive this core
        // (see retro_serialize_size), but suyu's room system tunnels the
        // game's own LAN multiplayer between peers and doesn't need frame
        // sync, so it works here - it just needs somewhere to be configured,
        // which is what these are.
        {"suyu_online_enable", "suyu Online Play (reload content); Disabled|Enabled"},
        // The frontend's FPS counter counts every presented frame, including
        // repeats while the game has not drawn a new one; this shows the game's own rate.
        {"suyu_show_perf", "Show Game Performance; On|Off"},
        // Legacy variables cannot accept free-form strings. Online endpoint
        // and nickname are supplied through environment variables instead.
        {nullptr, nullptr},
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);

    // Tell the frontend up front that state serialization is not usable for
    // frame-sensitive features. RetroArch keys netplay and rerecording off
    // this, so declaring it means those are cleanly reported as unavailable
    // instead of being offered and then failing mid-session.
    uint64_t quirks = RETRO_SERIALIZATION_QUIRK_INCOMPLETE;
    cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &quirks);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) {
    g_video_cb = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) {
    g_audio_sample_cb = cb;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    g_audio_batch_cb = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb) {
    g_input_poll_cb = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb) {
    g_input_state_cb = cb;
}

RETRO_API void retro_init() {
#ifdef SUYU_ANDROID_LIBRETRO
    // RetroArch owns Android storage; establish a writable core directory before
    // logging and firmware/key initialization. Content must use ordinary paths.
    const char* system_directory = nullptr;
    if (!g_environ_cb ||
        !g_environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_directory) ||
        !system_directory || !*system_directory) {
        return;
    }
    const auto user_directory = std::filesystem::path{system_directory} / "suyu";
    std::error_code directory_error;
    std::filesystem::create_directories(user_directory, directory_error);
    if (directory_error) {
        return;
    }
    Common::FS::SetAppDirectory(user_directory.string());
    Common::FS::CreateSuyuPaths();
#endif
    // Must happen before any Common::FS::GetSuyuPath() call (including the
    // key-import block just below), or those calls will already have latched
    // onto the old %APPDATA%/portable paths.
    RedirectSuyuPathsToFrontend();
  
    Common::Log::Initialize();
    Common::Log::Start();

    LOG_INFO(Frontend, "libretro core: retro_init() starting");

    PrepareLibretroKeys();
    g_system = std::make_unique<Core::System>();
    g_emu_window = std::make_unique<LibretroCore::RetroEmuWindow>();
    g_input_subsystem = std::make_shared<InputCommon::InputSubsystem>();
    g_input_subsystem->Initialize();

    g_system->Initialize();
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    // Audio output path. Default is "host": suyu opens its own audio device
    // and plays directly, which is how this core behaved before the libretro
    // route existed and is what actually sounds correct today.
    //
    // The libretro route hands samples to the frontend instead, which is
    // tidier in principle (frontend volume, recording, per-core mixing) but
    // sounds worse in practice: nothing throttles the emulated renderer here
    // the way a real device's buffer does, so it produces audio in bursts
    // rather than at a steady rate. It's offered as an option rather than
    // imposed, and the choice is re-read in retro_load_game.
    g_use_frontend_audio = false;
    Settings::values.sink_id.SetValue(Settings::AudioEngine::Auto);
    Settings::values.cpuopt_fastmem.SetValue(true);
    Settings::values.cpuopt_fastmem_exclusives.SetValue(true);
    Settings::values.log_flush_line.SetValue(true);
    Settings::values.log_filter.SetValue("*:Info Service.VI:Debug Service.AM:Debug Service.Nvnflinger:Debug");
    g_system->ApplySettings();
    g_system->SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    g_system->SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    g_system->GetFileSystemController().CreateFactories(*g_system->GetFilesystem());
    g_system->GetUserChannel().clear();

    // Bring up suyu's own room-based multiplayer. RetroArch's netplay can't
    // drive this core (see retro_serialize_size() for why), but suyu's online
    // play is peer-to-peer at the emulated-console level and doesn't depend on
    // frontend savestates, so it works here exactly as it does in the Qt
    // frontend once the user joins a room.
    if (!Network::Init()) {
        LOG_WARNING(Frontend, "libretro: could not initialise the network layer; "
                              "suyu online play will be unavailable in this session");
    } else {
        LOG_INFO(Frontend, "libretro: suyu room networking initialised");
    }

    g_system->HIDCore().ReloadInputDevices();
    LOG_INFO(Frontend, "libretro core: retro_init() complete");
}

RETRO_API void retro_deinit() {
    retro_unload_game();
    Network::Shutdown();
    g_system.reset();
    g_explicit_provider.reset();
    g_emu_window.reset();
    if (g_input_subsystem) { g_input_subsystem->Shutdown(); g_input_subsystem.reset(); }
    Common::Log::Stop();
}

RETRO_API unsigned retro_api_version() {
    return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info* info) {
    std::memset(info, 0, sizeof(*info));
    // RetroArch shows "<name> <version>" in its core information. Take the
    // version from the build ("suyu v0.0.11 (mk8-recomp)" -> "v0.0.11
    // (mk8-recomp)") and add the commit when the build knows it, so the core
    // can be matched to the release it came from.
    static const std::string version = [] {
        std::string text = Common::g_build_fullname;
        const std::string prefix = std::string(Common::g_build_name) + " ";
        if (text.starts_with(prefix)) {
            text.erase(0, prefix.size());
        }
        const std::string_view rev = Common::g_scm_rev;
        if (rev.size() >= 10 && rev.find_first_not_of("0123456789abcdef") == std::string_view::npos) {
            text += " ";
            text += rev.substr(0, 10);
        }
        return text;
    }();
    info->library_name = "suyu";
    info->library_version = version.c_str();
    info->valid_extensions = "nsp|xci|nca|nro";
    info->need_fullpath = true;
    info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info) {
    info->geometry.base_width = kFrameWidth;
    info->geometry.base_height = kFrameHeight;
    // Allow up to 4x so raising the internal resolution option doesn't get
    // clamped back down to 720p by the frontend. RetroArch sizes its texture
    // from max_*, and a core may deliver anything up to it.
    info->geometry.max_width = kFrameWidth * 4;
    info->geometry.max_height = kFrameHeight * 4;
    info->geometry.aspect_ratio = static_cast<float>(kFrameWidth) / static_cast<float>(kFrameHeight);
    info->timing.fps = 60.0;
    info->timing.sample_rate = 48000.0;
}

RETRO_API void retro_set_controller_port_device(unsigned /*port*/, unsigned /*device*/) {}

RETRO_API void retro_reset() {
    LOG_WARNING(Frontend, "libretro core: retro_reset() requested but not yet implemented "
                          "(would need Core::System restart without a full unload/reload)");
}

namespace {
using VB = InputCommon::VirtualGamepad::VirtualButton;
struct RetroToVirtual {
    unsigned retro_id;
    VB virtual_button;
};
constexpr RetroToVirtual kButtonMap[] = {
    {RETRO_DEVICE_ID_JOYPAD_A, VB::ButtonA},
    {RETRO_DEVICE_ID_JOYPAD_B, VB::ButtonB},
    {RETRO_DEVICE_ID_JOYPAD_X, VB::ButtonX},
    {RETRO_DEVICE_ID_JOYPAD_Y, VB::ButtonY},
    {RETRO_DEVICE_ID_JOYPAD_L, VB::TriggerL},
    {RETRO_DEVICE_ID_JOYPAD_R, VB::TriggerR},
    {RETRO_DEVICE_ID_JOYPAD_L2, VB::TriggerZL},
    {RETRO_DEVICE_ID_JOYPAD_R2, VB::TriggerZR},
    {RETRO_DEVICE_ID_JOYPAD_L3, VB::StickL},
    {RETRO_DEVICE_ID_JOYPAD_R3, VB::StickR},
    {RETRO_DEVICE_ID_JOYPAD_START, VB::ButtonPlus},
    {RETRO_DEVICE_ID_JOYPAD_SELECT, VB::ButtonMinus},
    {RETRO_DEVICE_ID_JOYPAD_UP, VB::ButtonUp},
    {RETRO_DEVICE_ID_JOYPAD_DOWN, VB::ButtonDown},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, VB::ButtonLeft},
    {RETRO_DEVICE_ID_JOYPAD_RIGHT, VB::ButtonRight},
};
bool g_prev_buttons[20] = {};
} // namespace

namespace {

// Drains whatever the emulated audio renderer produced since the last call
// and hands it to the frontend. upload_batch takes frames (L+R pairs), not
// individual samples. Feed the frontend a steady ~1 frame of audio per call
// rather than whatever has piled up: RetroArch resamples against its own
// clock and expects roughly sample_rate/fps frames each call, and handing it
// a quarter second in one lump and nothing for the next 15 calls is what made
// the output screech. Anything beyond a small backlog is dropped so latency
// can't creep up instead.
//
// Called either from retro_run() (frontends too old to support
// RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK) or from FrontendAudioCallback()
// below, never both - see g_audio_callback_registered.
void DeliverPendingAudio() {
    if (!g_use_frontend_audio || !g_audio_batch_cb || !g_game_loaded) {
        return;
    }

    constexpr size_t kFramesPerCall = 48000 / 60; // stereo frames
    constexpr size_t kMaxBacklogFrames = kFramesPerCall * 6;

    static std::vector<s16> pending; // interleaved L,R awaiting delivery
    std::vector<s16> drained;
    AudioCore::Sink::LibretroSampleQueue::Instance().Drain(drained);
    if (!drained.empty()) {
        pending.insert(pending.end(), drained.begin(), drained.end());
    }

    // Trim from the front if we've fallen behind; stale audio is worse than a
    // short gap.
    if (pending.size() > kMaxBacklogFrames * 2) {
        const size_t excess = pending.size() - kMaxBacklogFrames * 2;
        pending.erase(pending.begin(), pending.begin() + static_cast<ptrdiff_t>(excess));
    }

    const size_t frames = std::min(kFramesPerCall, pending.size() / 2);
    if (frames > 0) {
        g_audio_batch_cb(pending.data(), frames);
        pending.erase(pending.begin(), pending.begin() + static_cast<ptrdiff_t>(frames * 2));
    }
}

// Called by the frontend's own audio thread once RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK
// is registered, independently of whether retro_run() is being called at all.
void RETRO_CALLCONV FrontendAudioCallback() {
    DeliverPendingAudio();
}

// The actual reason to register SET_AUDIO_CALLBACK at all: its set_state
// half is the one libretro hook that still fires while the frontend has
// stopped calling retro_run() entirely (RetroArch's menu open, content
// paused, rewind, etc.). Without this, suyu's own CPU/GPU threads - kicked
// off once by g_system->Run() in retro_load_game() and never revisited -
// keep running regardless of whether the frontend is still ticking us, which
// is why the game previously kept advancing behind the RetroArch menu.
// enabled=false means "the frontend has gone quiet"; enabled=true means
// "resume normal operation".
void RETRO_CALLCONV FrontendAudioSetState(bool enabled) {
    if (!g_system || !g_game_loaded) {
        return;
    }
    if (enabled) {
        if (g_system->IsPaused()) {
            g_system->Run();
        }
    } else {
        if (!g_system->IsPaused()) {
            g_system->Pause();
        }
    }
}

} // namespace

RETRO_API void retro_run() {
    // This frontend is the sole periodic consumer of the destructive stats read.
    // Sample guest rendering/timing, independently of retro_run's frontend FPS.
    if (g_system && g_game_loaded) {
        const auto now = std::chrono::steady_clock::now();
        if (now - g_perf_last_sample >= std::chrono::seconds{1}) {
            bool discard_stale_stats = false;
            bool options_updated = false;
            if (g_environ_cb &&
                g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &options_updated) &&
                options_updated) {
                const bool was_shown = g_show_perf;
                g_show_perf = ReadShowPerfOption();
                // Nothing read the counters while the readout was off, so the next read
                // would average that whole period; drop it and report from the next one.
                discard_stale_stats = g_show_perf && !was_shown && !g_sample_perf;
            }
            if (discard_stale_stats) {
                (void)g_system->GetAndResetPerfStats();
            } else if (g_sample_perf || g_show_perf) {
                const auto stats = g_system->GetAndResetPerfStats();
                if (g_sample_perf) {
                    const double interval =
                        std::chrono::duration<double>(now - g_perf_last_sample).count();
                    const double elapsed =
                        std::chrono::duration<double>(now - g_perf_start).count();
                    const auto unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count();
                    const auto metric = [](double value) {
                        return std::isfinite(value) && value >= 0.0
                                   ? fmt::format("{:.3f}", value)
                                   : std::string{"unavailable"};
                    };
                    const bool has_system_frames =
                        std::isfinite(stats.system_fps) && stats.system_fps > 0.0;
                    LOG_INFO(Frontend,
                             "libretro PERF unix_ms={} elapsed_s={:.3f} interval_s={:.3f} "
                             "average_game_fps={} system_fps={} emulation_speed={} "
                             "frametime_ms={} has_system_frames={} frontend_frames={} "
                             "unique_frames={}",
                             unix_ms, elapsed, interval, metric(stats.average_game_fps),
                             metric(stats.system_fps), metric(stats.emulation_speed),
                             has_system_frames ? metric(stats.frametime * 1000.0) : "unavailable",
                             has_system_frames, g_perf_frontend_frames, g_perf_unique_frames);
                }
                if (g_show_perf && g_environ_cb) {
                    const auto finite = [](double value) {
                        return std::isfinite(value) && value >= 0.0 ? value : 0.0;
                    };
                    // RetroArch's status box is narrow, so the readout carries one detail after
                    // the frame rate: the shaders being built while there are any, since that is
                    // what a stutter needs explained; else, with multicore on (the default), the
                    // frame time.
                    // Speed is shown only in single-core mode: with multicore, guest time is
                    // the host wall clock, so emulation_speed reads ~100% however slow it runs.
                    std::string text = fmt::format("Game {:.1f} FPS", finite(stats.average_game_fps));
                    if (const int building = g_system->GPU().ShaderNotify().ShadersBuilding();
                        building > 0) {
                        text += fmt::format(" · {} shader{}", building, building == 1 ? "" : "s");
                    } else if (!Settings::values.use_multi_core.GetValue()) {
                        text += fmt::format(" · {:.0f}%", finite(stats.emulation_speed) * 100.0);
                    } else if (std::isfinite(stats.system_fps) && stats.system_fps > 0.0) {
                        text += fmt::format(" · {:.1f} ms", finite(stats.frametime) * 1000.0);
                    }
                    ShowPerfMessage(std::move(text));
                }
            }
            g_perf_last_sample = now;
            g_perf_frontend_frames = 0;
            g_perf_unique_frames = 0;
        }
    if (g_geometry_dirty && g_environ_cb) {
        retro_game_geometry geom{};
        geom.base_width = kFrameWidth * g_output_scale;
        geom.base_height = kFrameHeight * g_output_scale;
        geom.max_width = kFrameWidth * 4;
        geom.max_height = kFrameHeight * 4;
        geom.aspect_ratio = (float)kFrameWidth / (float)kFrameHeight;
        g_environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
        g_geometry_dirty = false;
    }
    if (g_input_poll_cb) {
        g_input_poll_cb();
    }

    // Bridge libretro input to suyu HID via VirtualGamepad.
    if (g_input_state_cb && g_input_subsystem && g_game_loaded) {
        auto* vgp = g_input_subsystem->GetVirtualGamepad();
        if (vgp) {
            for (const auto& m : kButtonMap) {
                const bool pressed = g_input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, m.retro_id) != 0;
                const int idx = static_cast<int>(m.virtual_button);
                if (pressed != g_prev_buttons[idx]) {
                    g_prev_buttons[idx] = pressed;
                    vgp->SetButtonState(0, m.virtual_button, pressed);
                }
            }
            // Left analog stick
            const float lx = g_input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) / 32768.0f;
            const float ly = g_input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y) / -32768.0f;
            vgp->SetStickPosition(0, InputCommon::VirtualGamepad::VirtualStick::Left, lx, ly);
            // Right analog stick
            const float rx = g_input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X) / 32768.0f;
            const float ry = g_input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y) / -32768.0f;
            vgp->SetStickPosition(0, InputCommon::VirtualGamepad::VirtualStick::Right, rx, ry);
        }
    }

    static unsigned frame_counter = 0;
    ++frame_counter;

    if (frame_counter <= 3 || (frame_counter % 600) == 0) {
        LOG_INFO(Frontend, "libretro: retro_run frame {}, game_loaded={}", frame_counter, g_game_loaded);
        fprintf(stderr, "[suyu-libretro] retro_run frame %u, game_loaded=%d\n", frame_counter, g_game_loaded);
        fflush(stderr);
    }

    // Only handle audio here ourselves if no async audio callback took over
    // in retro_load_game() - otherwise FrontendAudioCallback() is already
    // draining the same queue from the frontend's audio thread, and doing it
    // in both places would just race for no benefit.
    if (!g_audio_callback_registered) {
        DeliverPendingAudio();
    }

    if (g_video_cb && g_system && g_game_loaded) {
        auto& renderer = g_system->Renderer();
        if (renderer.IsHeadless()) {
            // Read before the upload: a frame composited during it is shown next call.
            const u64 displayed = g_emu_window ? g_emu_window->FramesDisplayed() : 0;
            ++g_perf_frontend_frames;
            if (g_can_dupe && g_have_presented && displayed == g_presented_frame) {
                // Nothing new since the last upload; tell the frontend it is a repeat.
                g_video_cb(nullptr, renderer.GetHeadlessWidth(), renderer.GetHeadlessHeight(), 0);
                return;
            }
            const auto& frame = renderer.GetLastRenderedFrame();
            if (!frame.empty()) {
                if (!g_have_presented || displayed != g_presented_frame) {
                    ++g_perf_unique_frames;
                }
                g_presented_frame = displayed;
                g_have_presented = true;
                // No channel swap: the renderer produces VK_FORMAT_B8G8R8A8,
                // i.e. B,G,R,A in ascending byte order, and libretro's
                // XRGB8888 is the 32-bit word 0xXXRRGGBB, which on a
                // little-endian host is that same B,G,R,X byte order. Passing
                // the buffer straight through is correct; an earlier R<->B
                // swap here was the cause of red rendering as blue.
                g_video_cb(frame.data(), renderer.GetHeadlessWidth(),
                           renderer.GetHeadlessHeight(),
                           renderer.GetHeadlessWidth() * 4);
                return;
            }
        }
        if (frame_counter <= 5 || (frame_counter % 300) == 0) {
            LOG_WARNING(Frontend, "libretro: frame {} - no rendered frame available, sending black",
                        frame_counter);
        }
        static const std::vector<u32> black_frame(
            static_cast<size_t>(kFrameWidth) * kFrameHeight, 0xFF000000);
        g_video_cb(black_frame.data(), kFrameWidth, kFrameHeight, kFrameWidth * sizeof(u32));
    }
}

RETRO_API size_t retro_serialize_size() {
    // Returning 0 disables savestates (and, through them, RetroArch netplay
    // and rerecording) in the frontend UI.
    //
    // This isn't a shortcut in the libretro layer: suyu has no state
    // serialization at all, and neither does any other yuzu-derived emulator.
    // A Switch savestate has to capture several GB of guest RAM plus dynarmic
    // CPU state for every guest thread, the whole GPU pipeline (Maxwell
    // registers, in-flight command buffers, texture/shader caches), the audio
    // renderer, and the entire HLE kernel object graph - threads, mutexes,
    // events, shared memory, open filesystem handles and live IPC sessions.
    // Until that exists in core/, there is nothing here to hand back, and
    // faking a partial state would corrupt saves rather than fail cleanly.
    return 0;
}

RETRO_API bool retro_serialize(void* /*data*/, size_t /*size*/) {
    return false;
}

RETRO_API bool retro_unserialize(const void* /*data*/, size_t /*size*/) {
    return false;
}

RETRO_API void retro_cheat_reset() {}

RETRO_API void retro_cheat_set(unsigned /*index*/, bool /*enabled*/, const char* /*code*/) {}

RETRO_API bool retro_load_game(const struct retro_game_info* game) {
    if (!g_system || !g_emu_window || !game || !game->path) {
        LOG_CRITICAL(Frontend, "libretro core: retro_load_game null check failed "
                               "(system={} window={} game={} path={})",
                     !!g_system, !!g_emu_window, !!game, game ? !!game->path : false);
        return false;
    }

    g_game_path = game->path;
    // need_fullpath makes content availability the core's responsibility. Avoid
    // initializing guest kernel state for a path the frontend cannot open.
    std::error_code path_error;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(g_game_path), path_error)) {
        LOG_ERROR(Frontend, "libretro: content is not an accessible file");
        g_game_path.clear();
        return false;
    }
    // Loader::IdentifyFile probes encrypted NCAs even for a malformed NRO.
    // With no keys, that probe reaches AES-XTS with an uninitialized cipher.
    // Check only the unencrypted executable signatures here; normal loader
    // validation still decides whether a recognized file can actually boot.
    const auto filename_type =
        Loader::GuessFromFilename(std::filesystem::u8path(g_game_path).filename().string());
    if (filename_type == Loader::FileType::NSO) {
        // A standalone NSO has no NPDM to initialize its process page table.
        // The supported deconstructed layout is booted through exefs/main.
        LOG_ERROR(Frontend, "libretro: standalone NSO requires a deconstructed ExeFS main");
        g_game_path.clear();
        return false;
    }
    if (filename_type == Loader::FileType::NRO || filename_type == Loader::FileType::KIP ||
        filename_type == Loader::FileType::DeconstructedRomDirectory) {
        std::array<char, 0x14> header{};
        std::ifstream content{std::filesystem::u8path(g_game_path), std::ios::binary};
        content.read(header.data(), header.size());
        const auto magic_offset = filename_type == Loader::FileType::NRO ? 0x10 : 0;
        const char* magic = filename_type == Loader::FileType::NRO ? "NRO0" :
                            filename_type == Loader::FileType::KIP ? "KIP1" : "NSO0";
        if (content.gcount() < magic_offset + 4 ||
            std::memcmp(header.data() + magic_offset, magic, 4) != 0) {
            LOG_ERROR(Frontend, "libretro: executable header is not recognized");
            g_game_path.clear();
            return false;
        }
    } else if ((filename_type == Loader::FileType::NSP || filename_type == Loader::FileType::XCI ||
                filename_type == Loader::FileType::NCA || filename_type == Loader::FileType::NAX) &&
               !HasUsableNcaHeaderKey()) {
        LOG_ERROR(Frontend, "libretro: encrypted content requires a usable NCA header key");
        g_game_path.clear();
        return false;
    }
    LOG_INFO(Frontend, "libretro core: loading game: {}", g_game_path);

    // RetroArch passes one content path. When an update is requested, require
    // that path to be the base XCI/NSP and verify the supplied update with the
    // same read-only provider and module/RomFS checks as suyu-cmd. The provider
    // is registered only for this content session; no NAND install occurs.
    if (const auto update_path = GetRuntimeOption("SUYU_LIBRETRO_UPDATE_PATH",
                                                 "debug.suyu.libretro.update");
        !update_path.empty()) {
        g_explicit_provider = std::make_unique<SuyuCli::ExplicitUpdateProvider>();
        if (!SuyuCli::ConfigureExplicitUpdate(*g_system, *g_explicit_provider,
                                              g_game_path, update_path)) {
            retro_unload_game();
            return false;
        }
        LOG_INFO(Frontend, "libretro: explicit base/update provider verified for this session");
    }

    // A verified deconstructed ExeFS/RomFS pair has no CNMT to report the
    // update version. Allow an explicit version only for that layout; packed
    // base+update sessions get their version from the verified provider.
    g_system->SetApplicationVersionOverride(0, {});
    if (const char* raw_version = std::getenv("SUYU_LIBRETRO_APP_VERSION");
        raw_version && *raw_version) {
        const char* display_version = std::getenv("SUYU_LIBRETRO_DISPLAY_VERSION");
        const auto main_path = std::filesystem::u8path(g_game_path);
        const auto exefs_dir = main_path.parent_path();
        u32 version{};
        const auto* end = raw_version + std::strlen(raw_version);
        const auto [parsed_end, parse_error] = std::from_chars(raw_version, end, version);
        std::array<char, 0x10> npdm_header{};
        std::ifstream npdm{exefs_dir / "main.npdm", std::ios::binary};
        npdm.read(npdm_header.data(), npdm_header.size());
        const bool valid_arm64 = npdm.gcount() ==
                                     static_cast<std::streamsize>(npdm_header.size()) &&
                                 std::memcmp(npdm_header.data(), "META", 4) == 0 &&
                                 (npdm_header[0x0C] & 1) != 0;
        if (g_explicit_provider || main_path.filename() != "main" ||
            !std::filesystem::is_regular_file(exefs_dir / "romfs.bin") || !valid_arm64 ||
            !display_version || !*display_version || parse_error != std::errc{} ||
            parsed_end != end || version == 0) {
            LOG_ERROR(Frontend, "libretro: invalid deconstructed application version override");
            retro_unload_game();
            return false;
        }
        g_system->SetApplicationVersionOverride(version, display_version);
        LOG_INFO(Frontend, "libretro: using explicit deconstructed application version {} ({})",
                 version, display_version);
    }

    // Apply core options before loading the game
    if (g_environ_cb) {
        struct retro_variable var;
        var.key = "suyu_use_docked";
        var.value = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            Settings::values.use_docked_mode.SetValue(
                std::string(var.value) == "Yes" ? Settings::ConsoleMode::Docked
                                                : Settings::ConsoleMode::Handheld);
        }
        var.key = "suyu_cpu_accuracy";
        var.value = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            std::string v(var.value);
            if (v == "Accurate")
                Settings::values.cpu_accuracy.SetValue(Settings::CpuAccuracy::Accurate);
            else if (v == "Unsafe")
                Settings::values.cpu_accuracy.SetValue(Settings::CpuAccuracy::Unsafe);
            else
                Settings::values.cpu_accuracy.SetValue(Settings::CpuAccuracy::Auto);
        }
        var.key = "suyu_resolution";
        var.value = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            const std::string v(var.value);
            auto res = Settings::ResolutionSetup::Res1X;
            if (v == "2x") {
                res = Settings::ResolutionSetup::Res2X;
                g_output_scale = 2;
            } else if (v == "3x") {
                res = Settings::ResolutionSetup::Res3X;
                g_output_scale = 3;
            }
            else if (v == "4x") {
                res = Settings::ResolutionSetup::Res4X;
                g_output_scale = 4;
            }
            Settings::values.resolution_setup.SetValue(res);
        }
        var.key = "suyu_scaling_filter";
        var.value = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            const std::string v(var.value);
            auto f = Settings::ScalingFilter::Bilinear;
            if (v == "Bicubic") f = Settings::ScalingFilter::Bicubic;
            else if (v == "Lanczos") f = Settings::ScalingFilter::Lanczos;
            else if (v == "ScaleForce") f = Settings::ScalingFilter::ScaleForce;
            else if (v == "FSR") f = Settings::ScalingFilter::Fsr;
            else if (v == "NearestNeighbor") f = Settings::ScalingFilter::NearestNeighbor;
            Settings::values.scaling_filter.SetValue(f);
        }
        var.key = "suyu_anti_aliasing";
        var.value = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            const std::string v(var.value);
            auto aa = Settings::AntiAliasing::None;
            if (v == "FXAA") aa = Settings::AntiAliasing::Fxaa;
            else if (v == "SMAA") aa = Settings::AntiAliasing::Smaa;
            Settings::values.anti_aliasing.SetValue(aa);
        }
        var.key = "suyu_audio_output";
        var.value = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            g_use_frontend_audio = std::string(var.value).rfind("Frontend", 0) == 0;
            Settings::values.sink_id.SetValue(g_use_frontend_audio
                                                  ? Settings::AudioEngine::Libretro
                                                  : Settings::AudioEngine::Auto);
            LOG_INFO(Frontend, "libretro: audio output = {}",
                     g_use_frontend_audio ? "frontend" : "host");
        }
        var.key = "suyu_fastmem";
        var.value = nullptr;
        if (g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            const bool enabled = std::string(var.value) == "Enabled";
            Settings::values.cpuopt_fastmem.SetValue(enabled);
            Settings::values.cpuopt_fastmem_exclusives.SetValue(enabled);
        }
        g_system->ApplySettings();

        g_emu_window->UpdateCurrentFramebufferLayout(kFrameWidth * g_output_scale, kFrameHeight * g_output_scale);

        // Join a suyu room if the user configured one. Done here rather than
        // in retro_init so the options the frontend collected are already
        // available, and so a failed join can't stop the game from booting
        // single-player.
        // If the frontend has a netplay session running, treat that as a
        // request for online play even when the core option is off. RetroArch's
        // own netplay can't synchronise this core (see retro_serialize_size),
        // but a user who started one has clearly asked to play with someone,
        // and suyu's room system can carry that - so the session is used as
        // the trigger and the client index decides who hosts.
        unsigned netplay_index = 0;
        const bool frontend_netplay =
            g_environ_cb(RETRO_ENVIRONMENT_GET_NETPLAY_CLIENT_INDEX, &netplay_index);
        if (frontend_netplay) {
            LOG_INFO(Frontend, "libretro: frontend netplay active (client index {}); "
                               "bringing up suyu online play", netplay_index);
        }

        var.key = "suyu_online_enable";
        var.value = nullptr;
        const bool option_enabled =
            g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value &&
            std::string(var.value) == "Enabled";
        if (option_enabled || frontend_netplay) {
            std::string server = "127.0.0.1";
            std::string nickname = "Player";
            u16 port = 24872;

            if (const char* configured = std::getenv("SUYU_LIBRETRO_ROOM_SERVER");
                configured && *configured) {
                server = configured;
            }
            if (const char* configured = std::getenv("SUYU_LIBRETRO_ROOM_NICKNAME");
                configured && *configured) {
                nickname = configured;
            }
            if (const char* configured = std::getenv("SUYU_LIBRETRO_ROOM_PORT");
                configured && *configured) {
                char* end = nullptr;
                const unsigned long parsed = std::strtoul(configured, &end, 10);
                if (*end == '\0' && parsed > 0 && parsed <= 65535) {
                    port = static_cast<u16>(parsed);
                } else {
                    LOG_WARNING(Frontend, "libretro: invalid SUYU_LIBRETRO_ROOM_PORT; using 24872");
                }
            }

            // Keep peers in one netplay session from colliding on nickname,
            // which the room rejects as a duplicate.
            if (frontend_netplay && netplay_index > 0) {
                nickname += "_" + std::to_string(netplay_index);
            }

            if (auto member = Network::GetRoomMember().lock()) {
                LOG_INFO(Frontend, "libretro: joining suyu room {}:{} as '{}'", server, port,
                         nickname);
                member->Join(nickname, server.c_str(), port);
            } else {
                LOG_WARNING(Frontend, "libretro: online play requested but the room member is "
                                      "unavailable; continuing single-player");
            }
        }
    }

    g_tas_playback = false;
    if (const auto tas_dir = GetRuntimeOption("SUYU_LIBRETRO_TAS_DIR", "debug.suyu.libretro.tas");
        !tas_dir.empty()) {
        const auto directory = std::filesystem::u8path(tas_dir);
        std::error_code error;
        if (!std::filesystem::is_regular_file(directory / "script0-1.txt", error)) {
            LOG_ERROR(Frontend, "libretro: requested TAS script0-1.txt is unavailable");
            retro_unload_game();
            return false;
        }
        g_previous_tas_enable = Settings::values.tas_enable.GetValue();
        g_previous_tas_loop = Settings::values.tas_loop.GetValue();
        g_previous_tas_directory = Common::FS::GetSuyuPath(Common::FS::SuyuPath::TASDir);
        Common::FS::SetSuyuPath(Common::FS::SuyuPath::TASDir, directory);
        Settings::values.tas_enable.SetValue(true);
        Settings::values.tas_loop.SetValue(false);
        g_tas_playback = true;
    }

    Service::AM::FrontendAppletParameters load_parameters{};
    load_parameters.applet_id = Service::AM::AppletId::Application;

    const Core::SystemResultStatus result =
        g_system->Load(*g_emu_window, g_game_path, load_parameters);
    if (result != Core::SystemResultStatus::Success) {
        LOG_CRITICAL(Frontend, "libretro core: Load() failed with status {}",
                     static_cast<u32>(result));
        // Libretro does not require an unload call after a failed load. Restore
        // the per-game TAS overrides now so a later load starts cleanly.
        retro_unload_game();
        return false;
    }

    // The GUI's EmuThread does these steps between Load and Run; otherwise the GPU thread
    // never starts and the CPU manager doesn't know the GPU is ready, causing the game to stall
    // before it ever reaches display setup.
    if (g_tas_playback) {
        auto* tas = g_input_subsystem->GetTas();
        tas->BeginBootSession(InputCommon::TasInput::TasBootMode::Playback);
        g_emu_window->SetTasPlayback(tas);
        LOG_INFO(Frontend, "libretro: boot TAS playback armed, {} commands",
                 std::get<2>(tas->GetStatus())[0]);
    }
    auto& gpu = g_system->GPU();
    gpu.ObtainContext();
    gpu.ReleaseContext();
    gpu.Start();
    g_system->GetCpuManager().OnGpuReady();

    const auto perf_setting = GetRuntimeOption("SUYU_LIBRETRO_PERF", "debug.suyu.libretro.perf");
    g_sample_perf = perf_setting == "1";
    g_show_perf = ReadShowPerfOption();
    unsigned message_version = 0;
    g_message_ext = g_environ_cb &&
                    g_environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION,
                                 &message_version) &&
                    message_version >= 1;
    g_can_dupe = false;
    if (g_environ_cb && !g_environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &g_can_dupe)) {
        g_can_dupe = false;
    }
    g_have_presented = false;
    g_perf_frontend_frames = 0;
    g_perf_unique_frames = 0;
    (void)g_system->GetAndResetPerfStats();
    g_perf_start = g_perf_last_sample = std::chrono::steady_clock::now();
    g_system->Run();
    g_game_loaded = true;

    // Register for the frontend's audio-pause notification (see
    // FrontendAudioSetState's comment) so opening the RetroArch menu, or
    // pausing content, actually pauses suyu's own CPU/GPU threads instead of
    // letting them run on unattended in the background.
    struct retro_audio_callback audio_cb {};
    audio_cb.callback = FrontendAudioCallback;
    audio_cb.set_state = FrontendAudioSetState;
    g_audio_callback_registered = g_environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &audio_cb);
    if (g_audio_callback_registered) {
        LOG_INFO(Frontend, "libretro core: registered async audio callback - "
                            "frontend pause/menu will now pause emulation");
    } else {
        LOG_WARNING(Frontend, "libretro core: frontend doesn't support "
                               "RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK - opening the menu or "
                               "pausing content won't pause suyu's own emulation");
    }

    LOG_INFO(Frontend, "libretro core: game loaded and running");
    return true;
}

RETRO_API bool retro_load_game_special(unsigned /*game_type*/, const struct retro_game_info* /*info*/,
                                       size_t /*num_info*/) {
    return false;
}

RETRO_API void retro_unload_game() {
    g_sample_perf = false;
    g_have_presented = false;
    if (g_system && g_game_loaded) {
        g_output_scale = 1;
        g_geometry_dirty = false;
        g_system->ShutdownMainProcess();
    }
    if (g_emu_window) {
        g_emu_window->SetTasPlayback(nullptr);
    }
    if (g_tas_playback && g_input_subsystem) {
        g_input_subsystem->GetTas()->BeginBootSession(InputCommon::TasInput::TasBootMode::None);
        Settings::values.tas_enable.SetValue(g_previous_tas_enable);
        Settings::values.tas_loop.SetValue(g_previous_tas_loop);
        Common::FS::SetSuyuPath(Common::FS::SuyuPath::TASDir, g_previous_tas_directory);
        g_previous_tas_directory.clear();
    }
    g_tas_playback = false;
    g_game_loaded = false;
    g_game_path.clear();
    if (g_explicit_provider) {
        g_system->RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                          nullptr);
        g_explicit_provider.reset();
    }
    // Leave any room we joined for this game; the next one loaded in this
    // session gets to make its own decision from its own core options.
    if (auto member = Network::GetRoomMember().lock()) {
        if (member->IsConnected()) {
            member->Leave();
        }
    }
    // Drop any audio still queued from the game that just went away, so it
    // can't leak into the next one loaded in this session.
    AudioCore::Sink::LibretroSampleQueue::Instance().Clear();
}

RETRO_API unsigned retro_get_region() {
    return RETRO_REGION_NTSC;
}

RETRO_API void* retro_get_memory_data(unsigned /*id*/) {
    return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned /*id*/) {
    return 0;
}

} // extern "C"
