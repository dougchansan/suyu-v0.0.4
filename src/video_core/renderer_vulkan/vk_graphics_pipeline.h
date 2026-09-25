// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>

#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

struct GraphicsPipelineCacheKey {
    std::array<u64, 6> unique_hashes;
    FixedPipelineState state;

    size_t Hash() const noexcept;

    bool operator==(const GraphicsPipelineCacheKey& rhs) const noexcept;

    bool operator!=(const GraphicsPipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }

    size_t Size() const noexcept {
        return sizeof(unique_hashes) + state.Size();
    }
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<GraphicsPipelineCacheKey>);

} // namespace Vulkan

namespace std {
template <>
struct hash<Vulkan::GraphicsPipelineCacheKey> {
    size_t operator()(const Vulkan::GraphicsPipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};
} // namespace std

namespace Vulkan {

struct GraphicsPipelineLibraryKey {
    GraphicsPipelineCacheKey pipeline;
    u32 part{};
    u32 variant{};
    u64 layout_signature{};

    bool operator==(const GraphicsPipelineLibraryKey& rhs) const noexcept {
        return part == rhs.part && variant == rhs.variant &&
               layout_signature == rhs.layout_signature && pipeline == rhs.pipeline;
    }
};

class GraphicsPipelineLibraryCache {
public:
    template <typename Create>
    std::shared_ptr<vk::Pipeline> GetOrCreate(const GraphicsPipelineLibraryKey& key,
                                              Create&& create, bool* cache_hit = nullptr) {
        {
            std::scoped_lock lock{mutex};
            if (const auto it = libraries.find(key); it != libraries.end()) {
                if (cache_hit) {
                    *cache_hit = true;
                }
                return it->second;
            }
        }
        // Compile outside the lock so unrelated boot-time shader workers can proceed.
        auto library = std::make_shared<vk::Pipeline>(create());
        if (!static_cast<bool>(*library)) {
            return library;
        }
        std::scoped_lock lock{mutex};
        const auto [it, inserted] = libraries.emplace(key, library);
        if (cache_hit) {
            *cache_hit = !inserted;
        }
        return it->second;
    }

private:
    struct Hash {
        size_t operator()(const GraphicsPipelineLibraryKey& key) const noexcept {
            return key.pipeline.Hash() ^ (static_cast<size_t>(key.part) << 1) ^
                   (static_cast<size_t>(key.variant) << 9) ^
                   static_cast<size_t>(key.layout_signature);
        }
    };
    std::mutex mutex;
    std::unordered_map<GraphicsPipelineLibraryKey, std::shared_ptr<vk::Pipeline>, Hash> libraries;
};

class Device;
class PipelineStatistics;
class RenderPassCache;
class RescalingPushConstant;
class RenderAreaPushConstant;
class Scheduler;

class GraphicsPipeline {
    static constexpr size_t NUM_STAGES = Tegra::Engines::Maxwell3D::Regs::MaxShaderStage;

public:
    explicit GraphicsPipeline(
        Scheduler& scheduler, BufferCache& buffer_cache, TextureCache& texture_cache,
        vk::PipelineCache& pipeline_cache, VideoCore::ShaderNotify* shader_notify,
        const Device& device, DescriptorPool& descriptor_pool,
        GuestDescriptorQueue& guest_descriptor_queue, Common::ThreadWorker* worker_thread,
        PipelineStatistics* pipeline_statistics, RenderPassCache& render_pass_cache,
        GraphicsPipelineLibraryCache& library_cache, Common::ThreadWorker& optimization_worker,
        const GraphicsPipelineCacheKey& key, bool precompile_only,
        std::array<vk::ShaderModule, NUM_STAGES> stages,
        std::array<u64, NUM_STAGES> code_hashes,
        const std::array<const Shader::Info*, NUM_STAGES>& infos);

    bool HasDynamicVertexInput() const noexcept { return key.state.dynamic_vertex_input; }
    bool SupportsAlphaToCoverage() const noexcept {
        return fragment_has_color0_output;
    }

    bool SupportsAlphaToOne() const noexcept {
        return fragment_has_color0_output;
    }

    bool UsesExtendedDynamicState() const noexcept {
        return key.state.extended_dynamic_state != 0;
    }
    GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept = delete;
    GraphicsPipeline(GraphicsPipeline&&) noexcept = delete;

    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(const GraphicsPipeline&) = delete;

    void AddTransition(GraphicsPipeline* transition);

    bool Configure(bool is_indexed) {
        return configure_func(this, is_indexed);
    }

    [[nodiscard]] GraphicsPipeline* Next(const GraphicsPipelineCacheKey& current_key) noexcept {
        if (key == current_key) {
            return this;
        }
        const auto it{std::find(transition_keys.begin(), transition_keys.end(), current_key)};
        return it != transition_keys.end() ? transitions[std::distance(transition_keys.begin(), it)]
                                           : nullptr;
    }

    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order::relaxed);
    }

    template <typename Spec>
    static auto MakeConfigureSpecFunc() {
        return [](GraphicsPipeline* pl, bool is_indexed) { return pl->ConfigureImpl<Spec>(is_indexed); };
    }

    void SetEngine(Tegra::Engines::Maxwell3D* maxwell3d_, Tegra::MemoryManager* gpu_memory_) {
        maxwell3d = maxwell3d_;
        gpu_memory = gpu_memory_;
    }

private:
    template <typename Spec>
    bool ConfigureImpl(bool is_indexed);

    bool ConfigureDraw(const RescalingPushConstant& rescaling,
                       const RenderAreaPushConstant& render_are);

    void MakePipeline(VkRenderPass render_pass);

    void OptimizePipeline();

    void Validate();

    const GraphicsPipelineCacheKey key;
    Tegra::Engines::Maxwell3D* maxwell3d;
    Tegra::MemoryManager* gpu_memory;
    const Device& device;
    TextureCache& texture_cache;
    BufferCache& buffer_cache;
    vk::PipelineCache& pipeline_cache;
    GraphicsPipelineLibraryCache& library_cache;
    Common::ThreadWorker& optimization_worker;
    Scheduler& scheduler;
    GuestDescriptorQueue& guest_descriptor_queue;

    bool (*configure_func)(GraphicsPipeline*, bool){};

    std::vector<GraphicsPipelineCacheKey> transition_keys;
    std::vector<GraphicsPipeline*> transitions;

    std::array<vk::ShaderModule, NUM_STAGES> spv_modules;
    std::array<u64, NUM_STAGES> code_hashes;

    std::array<Shader::Info, NUM_STAGES> stage_infos;
    std::array<u32, 5> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};
    u32 num_descriptor_entries{};
    size_t num_image_elements{};
    u32 num_textures{};
    bool fragment_has_color0_output{};
    u64 layout_signature{};
    bool precompile_only{};

    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    vk::Pipeline pipeline;
    std::array<std::shared_ptr<vk::Pipeline>, 4> pipeline_libraries;
    vk::Pipeline optimized_pipeline;
    VkRenderPass optimized_render_pass{};
    VkPipelineCreateFlags optimized_flags{};
    std::atomic_bool optimization_queued{false};
    std::atomic_bool optimized_ready{false};
    bool bound_optimized{};

    std::condition_variable build_condvar;
    std::mutex build_mutex;
    std::atomic_bool is_built{false};
    // Set when the worker could not produce a pipeline. is_built still becomes
    // true so nobody waits forever; draws using this pipeline are dropped.
    std::atomic_bool build_failed{false};
    bool uses_push_descriptor{false};
};

} // namespace Vulkan
