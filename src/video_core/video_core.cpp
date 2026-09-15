// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#endif
#include <memory>

#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/host1x/host1x.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_null/renderer_null.h"
#ifdef HAS_OPENGL
#include "video_core/renderer_opengl/renderer_opengl.h"
#endif
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/video_core.h"

namespace {

std::unique_ptr<VideoCore::RendererBase> CreateRenderer(Core::System& system, Core::Frontend::EmuWindow& emu_window, Tegra::GPU& gpu, std::unique_ptr<Core::Frontend::GraphicsContext> context) {
    [[maybe_unused]] auto& device_memory = system.Host1x().MemoryManager();
    switch (Settings::values.renderer_backend.GetValue()) {
#ifdef HAS_OPENGL
    case Settings::RendererBackend::OpenGL_GLSL:
    case Settings::RendererBackend::OpenGL_GLASM:
    case Settings::RendererBackend::OpenGL_SPIRV:
        return std::make_unique<OpenGL::RendererOpenGL>(emu_window, device_memory, gpu, std::move(context));
#endif
    case Settings::RendererBackend::Vulkan:
        return std::make_unique<Vulkan::RendererVulkan>(emu_window, device_memory, gpu, std::move(context));
    case Settings::RendererBackend::Null:
        return std::make_unique<Null::RendererNull>(emu_window, gpu, std::move(context));
    default:
        return nullptr;
    }
}

} // Anonymous namespace

namespace VideoCore {

/// @brief Creates an emulated GPU instance using the given system context.
void CreateGPU(std::optional<Tegra::GPU>& gpu, Core::Frontend::EmuWindow& emu_window, Core::System& system) {
    Settings::UpdateRescalingInfo();

    const auto nvdec_value = Settings::values.nvdec_emulation.GetValue();
    const bool use_nvdec = nvdec_value != Settings::NvdecEmulation::Off;
    const bool use_async = Settings::values.use_asynchronous_gpu_emulation.GetValue();
    gpu.emplace(system, use_async, use_nvdec);
    auto context = emu_window.CreateSharedContext();
    auto scope = context->Acquire();
    try {
        auto renderer = CreateRenderer(system, emu_window, *gpu, std::move(context));
        gpu->BindRenderer(std::move(renderer));
    } catch (const std::exception& exception) {
        scope.Cancel();
        LOG_ERROR(HW_GPU, "Failed to initialize GPU: {}", exception.what());
        gpu.reset();
    } catch (...) {
        // CreateRenderer owns the context once it is called, and the unwind that got us here has
        // already destroyed it, so the guard must be cancelled before it can reach DoneCurrent()
        // through the dead reference. Types that are not std::exception - a vk::Exception escaping
        // a renderer's member initialisers, or an Objective-C exception out of MoltenVK, for
        // instance - used to skip the handler above entirely and leave the guard active.
        scope.Cancel();
#if __has_include(<cxxabi.h>)
        const std::type_info* const type = abi::__cxa_current_exception_type();
        LOG_ERROR(HW_GPU, "Failed to initialize GPU: unhandled exception of type {}",
                  type ? type->name() : "unknown");
#else
        LOG_ERROR(HW_GPU, "Failed to initialize GPU: unhandled exception of unknown type");
#endif
        gpu.reset();
    }
}

} // namespace VideoCore
