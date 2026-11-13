// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/pipeline_statistics.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

using namespace std::string_view_literals;

static u64 GetUint64(const VkPipelineExecutableStatisticKHR& statistic) {
    switch (statistic.format) {
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
        return static_cast<u64>(statistic.value.i64);
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
        return statistic.value.u64;
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
        return static_cast<u64>(statistic.value.f64);
    default:
        return 0;
    }
}

PipelineStatistics::PipelineStatistics(const Device& device_) {}

void PipelineStatistics::Collect(const Device& device, VkPipeline pipeline) {
    const std::vector properties{device.GetLogical().GetPipelineExecutablePropertiesKHR(pipeline)};
    const u32 num_executables{static_cast<u32>(properties.size())};
    for (u32 executable = 0; executable < num_executables; ++executable) {
        const auto statistics{device.GetLogical().GetPipelineExecutableStatisticsKHR(pipeline, executable)};
        if (statistics.empty()) {
            continue;
        }
        Stats stage_stats;
        for (const auto& statistic : statistics) {
            const char* const name{statistic.name};
            if (name == "Binary Size"sv || name == "Code size"sv || name == "Instruction Count"sv) {
                stage_stats.code_size = GetUint64(statistic);
            } else if (name == "Register Count"sv) {
                stage_stats.register_count = GetUint64(statistic);
            } else if (name == "SGPRs"sv || name == "numUsedSgprs"sv) {
                stage_stats.sgpr_count = GetUint64(statistic);
            } else if (name == "VGPRs"sv || name == "numUsedVgprs"sv) {
                stage_stats.vgpr_count = GetUint64(statistic);
            } else if (name == "Branches"sv) {
                stage_stats.branches_count = GetUint64(statistic);
            } else if (name == "Basic Block Count"sv) {
                stage_stats.basic_block_count = GetUint64(statistic);
            }
        }
        std::scoped_lock lock{mutex};
        collected_stats.push_back(stage_stats);
    }
}

void PipelineStatistics::Report() const {
    double num{};
    Stats total;
    {
        std::scoped_lock lock{mutex};
        for (const Stats& stats : collected_stats) {
            total.code_size += stats.code_size;
            total.register_count += stats.register_count;
            total.sgpr_count += stats.sgpr_count;
            total.vgpr_count += stats.vgpr_count;
            total.branches_count += stats.branches_count;
            total.basic_block_count += stats.basic_block_count;
        }
        num = static_cast<double>(collected_stats.size());
    }
    std::string report;
    const auto add = [&](const char* fmt, u64 value) {
        if (value > 0) {
            report += fmt::format(fmt::runtime(fmt), static_cast<double>(value) / num);
        }
    };
    add("Code size:      {:9.03f}\n", total.code_size);
    add("Register count: {:9.03f}\n", total.register_count);
    add("SGPRs:          {:9.03f}\n", total.sgpr_count);
    add("VGPRs:          {:9.03f}\n", total.vgpr_count);
    add("Branches count: {:9.03f}\n", total.branches_count);
    add("Basic blocks:   {:9.03f}\n", total.basic_block_count);

    LOG_INFO(Render_Vulkan,
             "\nAverage pipeline statistics\n"
             "==========================================\n"
             "{}\n",
             report);
}

namespace PipelineExecutableDump {
namespace {

struct Dump {
    std::filesystem::path directory;
    std::ofstream lines;
    bool representations{};
    std::mutex mutex;
    /// Under mutex: the representation files written, each by the first pipeline that names it
    std::unordered_set<std::string> written;
};

/// Null without EDEN_NX2_PIPELINE_STATS, or when its file cannot be opened
Dump* GetDump() {
    static Dump* const dump = []() -> Dump* {
        const char* const directory = std::getenv("EDEN_NX2_PIPELINE_STATS");
        if (directory == nullptr || *directory == '\0') {
            return nullptr;
        }
        try {
            auto opened = std::make_unique<Dump>();
            opened->directory = std::filesystem::path{directory};
            std::error_code error;
            std::filesystem::create_directories(opened->directory, error);
            opened->lines.open(opened->directory / "pipeline_stats.jsonl",
                               std::ios::app | std::ios::binary);
            if (!opened->lines) {
                LOG_ERROR(Render_Vulkan, "EDEN_NX2_PIPELINE_STATS: cannot write to {}", directory);
                return nullptr;
            }
            const char* const ir = std::getenv("EDEN_NX2_PIPELINE_STATS_IR");
            opened->representations = ir != nullptr && *ir != '\0' && *ir != '0';
            LOG_INFO(Render_Vulkan,
                     "EDEN_NX2_PIPELINE_STATS: appending to {}/pipeline_stats.jsonl{}", directory,
                     opened->representations ? ", with internal representations" : "");
            // Kept until the process ends, like the pipeline build trace's file
            return opened.release();
        } catch (const std::exception& exception) {
            LOG_ERROR(Render_Vulkan, "EDEN_NX2_PIPELINE_STATS: {}", exception.what());
            return nullptr;
        }
    }();
    return dump;
}

void LogFailure(const char* what) {
    static std::atomic<bool> logged{};
    if (!logged.exchange(true)) {
        LOG_ERROR(Render_Vulkan,
                  "EDEN_NX2_PIPELINE_STATS: a pipeline's executables were not written (logged "
                  "once): {}",
                  what);
    }
}

/// The driver's strings are null-terminated arrays of VK_MAX_DESCRIPTION_SIZE
std::string_view DriverText(const char* text) {
    return {text,
            static_cast<size_t>(std::find(text, text + VK_MAX_DESCRIPTION_SIZE, '\0') - text)};
}

/// Bytes outside ASCII pass as they are
std::string JsonString(std::string_view text) {
    std::string result{"\""};
    for (const char character : text) {
        switch (character) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                result += fmt::format("\\u{:04x}",
                                      static_cast<u32>(static_cast<unsigned char>(character)));
            } else {
                result += character;
            }
            break;
        }
    }
    result += '"';
    return result;
}

/// A driver's name as part of a file name
std::string FileNamePart(std::string_view name) {
    std::string part;
    for (const char character : name.substr(0, 64)) {
        const bool keep = (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') || character == '-' ||
                          character == '.';
        part += keep ? character : '_';
    }
    return part.empty() ? std::string{"ir"} : part;
}

std::string StatisticJson(const VkPipelineExecutableStatisticKHR& statistic) {
    std::string_view format_name{"unknown"};
    std::string value{"null"};
    switch (statistic.format) {
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
        format_name = "bool32";
        value = statistic.value.b32 != VK_FALSE ? "true" : "false";
        break;
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
        format_name = "int64";
        value = fmt::format("{}", statistic.value.i64);
        break;
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
        format_name = "uint64";
        value = fmt::format("{}", statistic.value.u64);
        break;
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR:
        format_name = "float64";
        // JSON has no NaN or infinity: those stay null
        if (std::isfinite(statistic.value.f64)) {
            value = fmt::format("{}", statistic.value.f64);
        }
        break;
    default:
        break;
    }
    return fmt::format(R"({{"name":{},"description":{},"format":"{}","value":{}}})",
                       JsonString(DriverText(statistic.name)),
                       JsonString(DriverText(statistic.description)), format_name, value);
}

} // Anonymous namespace

bool Enabled(const Device& device) {
    return device.IsKhrPipelineExecutablePropertiesEnabled() && GetDump() != nullptr;
}

VkPipelineCreateFlags CaptureFlags(const Device& device) {
    if (!Enabled(device)) {
        return 0;
    }
    VkPipelineCreateFlags flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    if (GetDump()->representations) {
        flags |= VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
    }
    return flags;
}

void Record(const Device& device, VkPipeline pipeline, const BuiltPipeline& built) {
    if (!Enabled(device)) {
        return;
    }
    Dump& dump = *GetDump();
    try {
        const vk::Device& logical = device.GetLogical();
        std::string hashes;
        for (const u64 hash : built.stage_hashes) {
            hashes += fmt::format("{}\"{:016x}\"", hashes.empty() ? "" : ",", hash);
        }
        struct TextFile {
            std::string name;
            std::vector<char> text;
        };
        std::vector<TextFile> text_files;
        std::string executables;
        const std::vector properties{logical.GetPipelineExecutablePropertiesKHR(pipeline)};
        const u32 num_executables{static_cast<u32>(properties.size())};
        for (u32 executable = 0; executable < num_executables; ++executable) {
            std::string statistics;
            for (const VkPipelineExecutableStatisticKHR& statistic :
                 logical.GetPipelineExecutableStatisticsKHR(pipeline, executable)) {
                statistics += statistics.empty() ? "" : ",";
                statistics += StatisticJson(statistic);
            }
            executables += fmt::format(
                R"({}{{"index":{},"name":{},"description":{},"stages":{},"subgroup_size":{},"statistics":[{}])",
                executables.empty() ? "" : ",", executable,
                JsonString(DriverText(properties[executable].name)),
                JsonString(DriverText(properties[executable].description)),
                static_cast<u32>(properties[executable].stages),
                properties[executable].subgroupSize, statistics);
            if (dump.representations) {
                std::string listed;
                std::vector<std::vector<char>> storage;
                const std::vector found{logical.GetPipelineExecutableInternalRepresentationsKHR(
                    pipeline, executable, storage)};
                for (size_t index = 0; index < found.size(); ++index) {
                    const bool is_text = found[index].isText != VK_FALSE;
                    std::string file_name;
                    if (is_text) {
                        file_name =
                            fmt::format("{:016x}_{}_{}.txt", built.shader_hash, executable,
                                        FileNamePart(DriverText(found[index].name)));
                        // dataSize counts the text's terminating null
                        std::vector<char>& text = storage[index];
                        text.resize((std::min)(text.size(), found[index].dataSize));
                        while (!text.empty() && text.back() == '\0') {
                            text.pop_back();
                        }
                        text_files.push_back({file_name, std::move(text)});
                    }
                    listed += fmt::format(
                        R"({}{{"name":{},"description":{},"text":{},"bytes":{},"file":{}}})",
                        listed.empty() ? "" : ",", JsonString(DriverText(found[index].name)),
                        JsonString(DriverText(found[index].description)), is_text,
                        found[index].dataSize,
                        is_text ? JsonString(file_name) : std::string{"null"});
                }
                executables += fmt::format(R"(,"representations":[{}])", listed);
            }
            executables += '}';
        }
        std::string line = fmt::format(
            R"({{"kind":"{}","pipeline":"{:016x}","hashes":[{}],"shader":"{:016x}","mesh":{},"flags":{},"executables":[{}]}})",
            built.kind, built.key_hash, hashes, built.shader_hash, built.is_mesh,
            static_cast<u32>(built.flags), executables);
        line += '\n';

        std::scoped_lock lock{dump.mutex};
        for (const TextFile& text_file : text_files) {
            if (!dump.written.insert(text_file.name).second) {
                continue;
            }
            std::ofstream file{dump.directory / text_file.name, std::ios::binary};
            file.write(text_file.text.data(),
                       static_cast<std::streamsize>(text_file.text.size()));
        }
        dump.lines.write(line.data(), static_cast<std::streamsize>(line.size()));
        dump.lines.flush();
    } catch (const std::exception& exception) {
        LogFailure(exception.what());
    } catch (...) {
        LogFailure("unknown exception");
    }
}

} // namespace PipelineExecutableDump

} // namespace Vulkan
