// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;

class PipelineStatistics {
public:
    explicit PipelineStatistics(const Device& device_);
    void Collect(const Device& device, VkPipeline pipeline);
    void Report() const;

private:
    struct Stats {
        u64 code_size{};
        u64 register_count{};
        u64 sgpr_count{};
        u64 vgpr_count{};
        u64 branches_count{};
        u64 basic_block_count{};
    };
    mutable std::mutex mutex;
    std::vector<Stats> collected_stats;
};

/// EDEN_NX2_PIPELINE_STATS=<dir>: guest pipelines are built with
/// VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR and each one appends a line to
/// <dir>/pipeline_stats.jsonl with what the driver's compiler reports for its executables
/// (VK_KHR_pipeline_executable_properties). EDEN_NX2_PIPELINE_STATS_IR=1 also captures their
/// internal representations, the text ones written as
/// <dir>/<fragment or compute hash>_<executable index>_<name>.txt.
namespace PipelineExecutableDump {

/// Whether the variable is set, its file opened and the device has the extension enabled
[[nodiscard]] bool Enabled(const Device& device);

/// The capture bits a guest pipeline's create flags take, none when not enabled
[[nodiscard]] VkPipelineCreateFlags CaptureFlags(const Device& device);

struct BuiltPipeline {
    std::string_view kind;             ///< "graphics" or "compute"
    u64 key_hash{};                    ///< The pipeline key's hash
    std::span<const u64> stage_hashes; ///< A graphics key's unique_hashes, or the compute hash
    u64 shader_hash{};                 ///< Fragment or compute hash: names the representations
    bool is_mesh{};
    VkPipelineCreateFlags flags{};
};

/// Appends the line of a pipeline built with CaptureFlags. A failure is logged once, not thrown.
void Record(const Device& device, VkPipeline pipeline, const BuiltPipeline& built);

} // namespace PipelineExecutableDump

} // namespace Vulkan
