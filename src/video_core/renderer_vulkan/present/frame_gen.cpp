// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/frame_budget.h"
#include "video_core/frame_gen_choice.h"
#include "video_core/renderer_vulkan/present/frame_gen.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"
#include "video_core/vulkan_common/vulkan_device.h"
#ifdef EGLANTINE_STREAMLINE
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/streamline_frame_gen.h"
#endif

namespace Vulkan {

namespace {

constexpr size_t COLOR_CHANNELS = 4;
constexpr u64 LSFG_REQUIRED_FRAMES = 2;
constexpr u32 LSFG_RECURRENCE_FRAMES = 2;
constexpr u64 EXTRAPOLATION_LOG_INTERVAL = 600;

/// EDEN_NX2_CADENCE: with extrapolated frames, hold real frames to an even share of the target
/// rate so that every shown frame lasts the same time. 1 chooses the share (60, 30, 20 or 15 real
/// frames for a target of 60) from what the game sustains; 2 to 4 fix the divisor.
[[nodiscard]] u32 CadenceSetting() {
    static const u32 setting = [] {
        const char* value = std::getenv("EDEN_NX2_CADENCE");
        return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0U;
    }();
    return setting;
}

constexpr u32 CADENCE_MAX_DIVISOR = 4;

[[nodiscard]] f32 CadenceTargetRate() {
    const u32 rate = Settings::values.frame_gen_target_rate.GetValue();
    return rate != 0 ? static_cast<f32>(rate) : 60.0f;
}

constexpr auto ACTIVITY_LOG_INTERVAL = std::chrono::seconds(10);

[[nodiscard]] std::string ModeText(Settings::FrameGenType active) {
    const char* type = active == Settings::FrameGenType::Fsr3 ? "FSR3" : "LSFG";
    const u32 target_rate = Settings::values.frame_gen_target_rate.GetValue();
    if (target_rate != 0) {
        return fmt::format("{} up to {}x towards {} FPS", type, Settings::MAX_FRAME_GEN_MULTIPLIER,
                           target_rate);
    }
    return fmt::format("{} {}x", type, Settings::FrameGenMultiplier());
}

/// Why NVIDIA DLSS Frame Generation isn't this session's frame generator, if it isn't. Streamline
/// decided that when the game started.
[[nodiscard]] VideoCore::DlssgBlock DlssgBlocked() {
#ifdef EGLANTINE_STREAMLINE
    if (Streamline::FrameGenSession()) {
        return VideoCore::DlssgBlock::None;
    }
    return VideoCore::BlockedDlssg({
        .streamline = true,
        .dlss = Settings::values.nvidia_dlss.GetValue(),
        .program_id = Settings::GetCurrentProgramID(),
        .loaded = false,
    });
#else
    return VideoCore::DlssgBlock::NoStreamline;
#endif
}

[[nodiscard]] std::string DlssgBlockedText(VideoCore::DlssgBlock block) {
    switch (block) {
    case VideoCore::DlssgBlock::None:
        break;
    case VideoCore::DlssgBlock::NoStreamline:
        return "this build has no NVIDIA Streamline";
    case VideoCore::DlssgBlock::DlssOff:
        return "\"Use NVIDIA DLSS\" is off, and DLSS-G takes its inputs from DLSS";
    case VideoCore::DlssgBlock::NoGameInputs:
        return fmt::format("no game motion vectors for {:016X}", Settings::GetCurrentProgramID());
    case VideoCore::DlssgBlock::NotLoaded:
#ifdef EGLANTINE_STREAMLINE
        if (std::string reason = Streamline::FrameGenUnavailableReason(); !reason.empty()) {
            return reason;
        }
#endif
        return "NVIDIA Streamline did not start";
    }
    return {};
}

[[nodiscard]] f32 ManualFlowScale() {
    return static_cast<f32>(Settings::values.frame_gen_flow_scale.GetValue()) / 100.0f;
}

[[nodiscard]] f32 ConfiguredFlowScale(VkExtent2D guest_extent, VkExtent2D presented_extent) {
    if (!Settings::values.frame_gen_flow_scale_auto.GetValue()) {
        return ManualFlowScale();
    }
    if (guest_extent.width == 0 || presented_extent.width == 0) {
        return 1.0f;
    }

    const f32 rendered_width = static_cast<f32>(guest_extent.width) *
                               Settings::values.resolution_info.up_factor;
    const f32 ratio = rendered_width / static_cast<f32>(presented_extent.width);

    constexpr f32 FLOW_SCALE_STEPS = 20.0f;
    const f32 stepped = std::ceil(ratio * FLOW_SCALE_STEPS) / FLOW_SCALE_STEPS;
    return std::clamp(stepped, 0.25f, 1.0f);
}

bool IsBlueFirst(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

VkDeviceSize BytesPerTexel(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM:
        return 1;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 8;
    default:
        return COLOR_CHANNELS;
    }
}

void WritePortablePixmap(const std::filesystem::path& path, const std::string& magic,
                         VkExtent2D extent, std::span<const u8> pixels) {
    Common::FS::IOFile file{path, Common::FS::FileAccessMode::Write,
                            Common::FS::FileType::BinaryFile};
    if (!file.IsOpen()) {
        return;
    }

    const std::string header = magic + "\n" + std::to_string(extent.width) + " " +
                               std::to_string(extent.height) + "\n255\n";
    if (file.Write(header) != header.size()) {
        return;
    }

    void(file.Write(pixels));
    void(file.Flush());
}

void WriteGrayscalePgm(const std::filesystem::path& path, VkExtent2D extent,
                       std::span<const u8> pixels) {
    const size_t expected = static_cast<size_t>(extent.width) * extent.height;
    WritePortablePixmap(path, "P5", extent, pixels.subspan(0, (std::min)(expected, pixels.size())));
}

void WriteRaw(const std::filesystem::path& path, std::span<const u8> pixels) {
    Common::FS::IOFile file{path, Common::FS::FileAccessMode::Write,
                            Common::FS::FileType::BinaryFile};
    if (!file.IsOpen()) {
        return;
    }
    void(file.Write(pixels));
    void(file.Flush());
}

void WriteColorPpm(const std::filesystem::path& path, VkExtent2D extent,
                   std::span<const u8> pixels, bool blue_first) {
    const size_t pixel_count = static_cast<size_t>(extent.width) * extent.height;
    if (pixels.size() < pixel_count * COLOR_CHANNELS) {
        return;
    }

    std::vector<u8> rgb(pixel_count * 3);
    for (size_t i = 0; i < pixel_count; ++i) {
        const u8 first = pixels[i * COLOR_CHANNELS];
        const u8 green = pixels[i * COLOR_CHANNELS + 1];
        const u8 third = pixels[i * COLOR_CHANNELS + 2];
        rgb[i * 3] = blue_first ? third : first;
        rgb[i * 3 + 1] = green;
        rgb[i * 3 + 2] = blue_first ? first : third;
    }

    WritePortablePixmap(path, "P6", extent, rgb);
}

VkImageMemoryBarrier MakeTransitionBarrier(VkImage image, VkAccessFlags src_access,
                                           VkAccessFlags dst_access, VkImageLayout old_layout,
                                           VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
}

VkImageCopy MakeCopyRegion(VkExtent2D extent) {
    return VkImageCopy{
        .srcSubresource{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .srcOffset = {},
        .dstSubresource{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .dstOffset = {},
        .extent = {.width = extent.width, .height = extent.height, .depth = 1},
    };
}

void CopyPresentedFrame(vk::CommandBuffer cmdbuf, VkImage source, LsfgImage& destination,
                        VkExtent2D extent) {
    const auto make_barrier = MakeTransitionBarrier;

    const std::array before{
        make_barrier(source, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        make_barrier(destination.Handle(), VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     destination.Layout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);

    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.Handle(),
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, MakeCopyRegion(extent));

    const std::array after{
        make_barrier(source, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        make_barrier(destination.Handle(), VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, {}, {}, after);

    destination.SetLayout(VK_IMAGE_LAYOUT_GENERAL);
}

} // Anonymous namespace

FrameGen::FrameGen(MemoryAllocator& memory_allocator_, Scheduler& scheduler_)
    : memory_allocator{memory_allocator_}, scheduler{scheduler_} {}

FrameGen::~FrameGen() {
    ReleaseCadence();
}

void FrameGen::SetExtrapolationSource(const std::optional<ExtrapolationSource>& source,
                                      VkRect2D screen) {
    extrapolation_source = source;
    extrapolation_screen = screen;
    if (source && !extrapolating) {
        LOG_INFO(Render_Vulkan, "Frame generation: extrapolating from the game's motion vectors");
        extrapolating = true;
    }
}

void FrameGen::ProcessExtrapolated(const Device& device, Frame* frame, VkFormat format) {
    if (chain || fsr3_chain) {
        scheduler.Finish();
        chain.reset();
        fsr3_chain.reset();
    }
    frame_count = 0;
    warm_streak = 0;
    extrapolation_frame = frame;

    if (!frame->storage_view) {
        ReleaseCadence();
        SetState(VideoCore::FrameGenState::UnsupportedSurface, format);
        return;
    }
    SetState(VideoCore::FrameGenState::Running, format);

    if (!extrapolation) {
        extrapolation.emplace(device);
    }

    // Without motion or across a camera cut the real frame is shown alone.
    const bool usable = extrapolation_source && !extrapolation_source->cut &&
                        extrapolation_screen.extent.width != 0 &&
                        extrapolation_screen.extent.height != 0;
    extrapolation_missing += extrapolation_source ? 0 : 1;
    extrapolation_cuts += extrapolation_source && extrapolation_source->cut ? 1 : 0;
    // A cadence holds real frames only while frames are predicted: menus and cut scenes without
    // motion data run at the game's own rate.
    unusable_streak = usable ? 0 : unusable_streak + 1;
    if (usable) {
        UpdateCadence();
    } else if (unusable_streak > 30) {
        ReleaseCadence();
    }

    last_generations = plan.generations;
    generated = usable && plan.warm && plan.generations > 0;
    generated_frames.fetch_add(GeneratedFrameCount(), std::memory_order_relaxed);

    if (++extrapolation_frames % EXTRAPOLATION_LOG_INTERVAL == 0) {
        LOG_INFO(Render_Vulkan,
                 "Frame extrapolation: {} frames, {} without motion, {} camera cuts, {} planned "
                 "per frame, HUD kept {}",
                 EXTRAPOLATION_LOG_INTERVAL, extrapolation_missing, extrapolation_cuts,
                 plan.generations,
                 extrapolation_source && extrapolation_source->hud_reference != VK_NULL_HANDLE);
        extrapolation_missing = 0;
        extrapolation_cuts = 0;
    }
}

void FrameGen::Process(const Device& device, Frame* frame, VkFormat format,
                       VkExtent2D guest_extent) {
    generated = false;

    const Settings::FrameGenType chosen = Settings::values.frame_gen_type.GetValue();
    const Settings::FrameGenType type =
        chosen == Settings::FrameGenType::NvidiaDlssG
            ? VideoCore::ActiveFrameGenType(chosen, DlssgBlocked())
            : chosen;
    if (type == Settings::FrameGenType::Lsfg && !shaders) {
        shaders.emplace(device);
    }
    unavailable = type == Settings::FrameGenType::Lsfg && !shaders->IsValid();

    if (extrapolating && Settings::values.frame_gen.GetValue()) {
        // Extrapolation needs neither Lossless Scaling shaders nor an interpolation chain.
        unavailable = false;
        ProcessExtrapolated(device, frame, format);
        return;
    }

    const bool nvidia_frame_gen = type == Settings::FrameGenType::NvidiaDlssG;
    if (unavailable || !Settings::values.frame_gen.GetValue() || nvidia_frame_gen) {
        if (!Settings::values.frame_gen.GetValue()) {
            SetState(VideoCore::FrameGenState::Pending, format);
        } else if (nvidia_frame_gen) {
            SetState(VideoCore::FrameGenState::NvidiaFrameGen, format);
        }
        if (chain || fsr3_chain) {
            scheduler.Finish();
            chain.reset();
            fsr3_chain.reset();
        }
        warm_streak = 0;
        if (unavailable && Settings::values.frame_gen.GetValue()) {
            SetState(shaders->IsDeviceSupported() ? VideoCore::FrameGenState::MissingLosslessDll
                                                  : VideoCore::FrameGenState::UnsupportedGpu,
                     format);
        }
        ReleaseCadence();
        return;
    }

    if (!frame->storage_view) {
        SetState(VideoCore::FrameGenState::UnsupportedSurface, format);
        warm_streak = 0;
        return;
    }
    SetState(VideoCore::FrameGenState::Running, format);

    peak_guest_extent.width = (std::max)(peak_guest_extent.width, guest_extent.width);
    peak_guest_extent.height = (std::max)(peak_guest_extent.height, guest_extent.height);

    const VkExtent2D extent{.width = frame->width, .height = frame->height};
    const f32 flow_scale = ConfiguredFlowScale(peak_guest_extent, extent);
    if ((!chain && !fsr3_chain) || built_type != type || built_extent.width != extent.width ||
        built_extent.height != extent.height || built_format != format ||
        built_flow_scale != flow_scale) {
        Rebuild(device, extent, format, flow_scale, type);
    }

    const u64 count = frame_count++;
    last_count = count;
    last_generations = plan.generations;

    const bool warm = plan.warm && count + 1 >= LSFG_REQUIRED_FRAMES;
    warm_streak = warm ? warm_streak + 1 : 0;
    generated = warm && warm_streak >= LSFG_RECURRENCE_FRAMES && plan.generations > 0;
    generated_frames.fetch_add(GeneratedFrameCount(), std::memory_order_relaxed);
    LogActivity();

    const bool use_fsr3 = type == Settings::FrameGenType::Fsr3;

    if (use_fsr3 && fsr3_chain) {
        scheduler.RequestOutsideRenderPassOperationContext();
        fsr3_chain->Process(device, scheduler, *frame->image, *frame->storage_view, count);
    } else if (chain) {
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([this, source = *frame->image, extent, count,
                          dispatch = warm](vk::CommandBuffer cmdbuf) {
            CopyPresentedFrame(cmdbuf, source, chain->Input(count), extent);
            if (dispatch) {
                chain->DispatchShared(cmdbuf, count);
            }
        });

        const bool dump_requested = generated && Settings::values.frame_gen_dump_flow.GetValue();
        if (!dump_requested) {
            dumped = false;
        } else if (!dumped) {
            DumpDebugImages(count);
            dumped = true;
        }
    }
}

void FrameGen::Pause() {
    pacer.Reset();
    plan = {};
    frame_count = 0;
    last_count = 0;
    last_generations = 0;
    warm_streak = 0;
    generated = false;
    extrapolation_source.reset();
    ReleaseCadence();
}

void FrameGen::SetState(VideoCore::FrameGenState new_state, VkFormat format) {
    if (state.exchange(new_state, std::memory_order_relaxed) == new_state) {
        return;
    }
    switch (new_state) {
    case VideoCore::FrameGenState::Running:
        if (Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::NvidiaDlssG) {
            LOG_INFO(Render_Vulkan,
                     "Frame generation: NVIDIA DLSS-G unavailable ({}), using {}, presenting {}",
                     DlssgBlockedText(DlssgBlocked()), ModeText(Settings::FrameGenType::Fsr3),
                     string_VkFormat(format));
            break;
        }
        LOG_INFO(Render_Vulkan, "Frame generation on: {}, presenting {}",
                 ModeText(Settings::values.frame_gen_type.GetValue()), string_VkFormat(format));
        break;
#ifdef EGLANTINE_STREAMLINE
    case VideoCore::FrameGenState::NvidiaFrameGen:
        LOG_INFO(Render_Vulkan,
                 "Frame generation on: NVIDIA DLSS-G {}x (numFramesToGenerateMax {}), presenting "
                 "{}",
                 Streamline::FrameGenGenerated() + 1, Streamline::FrameGenMaxGenerated(),
                 string_VkFormat(format));
        break;
#endif
    case VideoCore::FrameGenState::UnsupportedSurface:
        LOG_WARNING(Render_Vulkan,
                    "Frame generation is on but not running: the GPU can't write {} from a compute "
                    "shader, so generated frames can't be presented",
                    string_VkFormat(format));
        break;
    case VideoCore::FrameGenState::MissingLosslessDll:
        LOG_WARNING(Render_Vulkan,
                    "Frame generation is on but not running: LSFG needs Lossless.dll in the "
                    "lossless folder of the user directory. FSR3 needs nothing");
        break;
    case VideoCore::FrameGenState::UnsupportedGpu:
        LOG_WARNING(Render_Vulkan,
                    "Frame generation is on but not running: this GPU lacks Vulkan features LSFG "
                    "needs. FSR3 may still work");
        break;
    default:
        break;
    }
    activity = {};
}

void FrameGen::LogActivity() {
    const auto now = std::chrono::steady_clock::now();
    ++activity.frames;
    activity.generated += GeneratedFrameCount();
    if (activity.since == std::chrono::steady_clock::time_point{}) {
        activity.since = now;
        return;
    }
    if (now - activity.since < ACTIVITY_LOG_INTERVAL) {
        return;
    }
    const f32 per_frame =
        std::round(10.0f * static_cast<f32>(activity.generated) / static_cast<f32>(activity.frames)) /
        10.0f;
    if (per_frame != activity.logged_per_frame) {
        const f32 seconds = std::chrono::duration<f32>(now - activity.since).count();
        if (activity.generated == 0) {
            LOG_INFO(Render_Vulkan,
                     "Frame generation ({}) idle: {:.0f} game FPS, no frames generated. It waits "
                     "while the game is below 10 FPS, after hitches and once the target is met",
                     ModeText(built_type), static_cast<f32>(activity.frames) / seconds);
        } else {
            LOG_INFO(Render_Vulkan,
                     "Frame generation ({}) generating: {:.0f} game FPS + {:.0f} generated, {:.1f} "
                     "per game frame",
                     ModeText(built_type), static_cast<f32>(activity.frames) / seconds,
                     static_cast<f32>(activity.generated) / seconds, per_frame);
        }
        activity.logged_per_frame = per_frame;
    }
    activity.since = now;
    activity.frames = 0;
    activity.generated = 0;
}

size_t FrameGen::WantedGenerations(size_t capacity,
                                   std::chrono::steady_clock::duration blocked) {
    if (unavailable) {
        plan = {};
        return 0;
    }
    plan = pacer.Plan(capacity, blocked);
    if (extrapolating && cadence_divisor != 0) {
        // Extrapolated frames cost next to nothing: always fill the cadence.
        plan = {.generations = std::min<size_t>(capacity, cadence_divisor - 1), .warm = true};
    }
    return plan.generations;
}

std::chrono::nanoseconds FrameGen::CadenceSpacing() const {
    if (!extrapolating || !generated || last_generations == 0 || cadence_divisor == 0) {
        return {};
    }
    return std::chrono::nanoseconds{static_cast<s64>(1.0e9 / CadenceTargetRate())};
}

void FrameGen::ReleaseCadence() {
    if (cadence_divisor == 0) {
        return;
    }
    LOG_INFO(Render_Vulkan, "Frame cadence: off, frame budget target back to {:.0f} FPS",
             cadence_saved_target);
    cadence_divisor = 0;
    cadence_slow_checks = 0;
    cadence_fast_checks = 0;
    VideoCore::FrameBudget::SetTargetFps(cadence_saved_target);
}

void FrameGen::UpdateCadence() {
    const u32 setting = CadenceSetting();
    if (setting == 0) {
        return;
    }
    const f32 target = CadenceTargetRate();
    const auto apply = [this, target](u32 divisor, const char* reason) {
        if (divisor == cadence_divisor) {
            return;
        }
        if (cadence_divisor == 0) {
            cadence_saved_target = VideoCore::FrameBudget::TargetFps();
        }
        LOG_INFO(Render_Vulkan,
                 "Frame cadence: {:.0f} real frames shown as {:.0f} ({}), was {:.0f}",
                 target / static_cast<f32>(divisor), target, reason,
                 cadence_divisor != 0 ? target / static_cast<f32>(cadence_divisor) : 0.0f);
        cadence_divisor = divisor;
        cadence_slow_checks = 0;
        cadence_fast_checks = 0;
        VideoCore::FrameBudget::SetTargetFps(target / static_cast<f32>(divisor));
    };
    if (setting >= 2) {
        apply(std::min(setting, CADENCE_MAX_DIVISOR), "fixed");
        return;
    }
    if (cadence_divisor == 0) {
        apply(3, "start");
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - cadence_checked < std::chrono::milliseconds{500}) {
        return;
    }
    cadence_checked = now;
    // What four in five frames took without the pacing wait, 0 while unknown.
    const f64 work_ms = VideoCore::FrameBudget::RecentWorkMs();
    if (work_ms <= 0.0) {
        return;
    }
    const f64 budget_ms = 1000.0 * cadence_divisor / target;
    const f64 faster_ms = 1000.0 * (cadence_divisor - 1) / target;
    cadence_slow_checks = work_ms > budget_ms * 1.03 ? cadence_slow_checks + 1 : 0;
    cadence_fast_checks =
        cadence_divisor > 1 && work_ms < faster_ms * 0.90 ? cadence_fast_checks + 1 : 0;
    if (cadence_slow_checks >= 3 && cadence_divisor < CADENCE_MAX_DIVISOR) {
        apply(cadence_divisor + 1, "the frames take longer");
    } else if (cadence_fast_checks >= 8) {
        apply(cadence_divisor - 1, "the frames leave room");
    }
}

size_t FrameGen::GeneratedFrameCount() const {
    return generated ? last_generations : 0;
}

void FrameGen::GenerateInto(const Device& device, Frame* destination, size_t generation) {
    if (extrapolating) {
        if (!extrapolation || !extrapolation_source || !extrapolation_frame ||
            !destination->storage_view) {
            return;
        }
        const f32 advance =
            static_cast<f32>(generation + 1) / static_cast<f32>(last_generations + 1);
        const VkExtent2D extent{.width = destination->width, .height = destination->height};

        scheduler.RequestOutsideRenderPassOperationContext();
        // The storage view has the image's own format, so reads and writes encode alike.
        extrapolation->Generate(device, scheduler, *extrapolation_source,
                                *extrapolation_frame->storage_view, *destination->image,
                                *destination->storage_view, extent, extrapolation_screen, advance);
        return;
    }

    const bool use_fsr3 = built_type == Settings::FrameGenType::Fsr3;

    if (use_fsr3 && fsr3_chain) {
        const f32 blend = static_cast<f32>(generation + 1) / static_cast<f32>(last_generations + 1);
        const f32 flow_scale = ConfiguredFlowScale(peak_guest_extent, built_extent);

        scheduler.RequestOutsideRenderPassOperationContext();
        fsr3_chain->Generate(device, scheduler, *destination->image, *destination->storage_view,
                             blend, flow_scale);
    } else if (chain) {
        chain->SetTarget(device, last_generations, generation, destination->index,
                         *destination->storage_view);

        const VkExtent2D extent{.width = destination->width, .height = destination->height};

        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([this, count = last_count, generation_count = last_generations, generation,
                          target = destination->index, image = *destination->image,
                          extent](vk::CommandBuffer cmdbuf) {
            chain->DispatchGeneration(cmdbuf, count, generation_count, generation, target, image,
                                      extent);
        });
    }
}

void FrameGen::Rebuild(const Device& device, VkExtent2D extent, VkFormat format, f32 flow_scale,
                       Settings::FrameGenType type) {
    scheduler.Finish();
    chain.reset();
    fsr3_chain.reset();

    built_flow_scale = flow_scale;
    built_type = type;

    if (built_type == Settings::FrameGenType::Fsr3) {
        fsr3_chain.emplace(device, memory_allocator, extent, format);
    } else {
        chain.emplace(device, memory_allocator, *shaders, extent, format, built_flow_scale);
    }
    built_extent = extent;
    built_format = format;
    frame_count = 0;
    warm_streak = 0;
    generated = false;
}

void FrameGen::DumpDebugImages(u64 count) {
    const std::filesystem::path directory =
        Common::FS::GetEdenPath(Common::FS::EdenPath::LosslessDir) / "debug";
    if (!Common::FS::CreateDirs(directory)) {
        return;
    }

    const auto dump = [&](const std::string& name, LsfgImage& image) {
        const VkExtent2D extent = image.Extent();
        const VkFormat format = image.Format();
        const VkDeviceSize texel_size = BytesPerTexel(format);
        const VkDeviceSize size =
            static_cast<VkDeviceSize>(extent.width) * extent.height * texel_size;

        vk::Buffer buffer = CreateWrappedBuffer(memory_allocator, size, MemoryUsage::Download);

        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record(
            [handle = image.Handle(), dst = *buffer, extent](vk::CommandBuffer cmdbuf) {
                DownloadColorImage(
                    cmdbuf, handle, dst,
                    VkExtent3D{.width = extent.width, .height = extent.height, .depth = 1});
            });
        scheduler.Finish();

        buffer.Invalidate();
        const std::span<u8> mapped = buffer.Mapped();

        if (format == LSFG_FLOW_FORMAT) {
            WriteGrayscalePgm(directory / (name + ".pgm"), extent, mapped);
        } else if (texel_size == COLOR_CHANNELS) {
            WriteColorPpm(directory / (name + ".ppm"), extent, mapped, IsBlueFirst(format));
        } else {
            WriteRaw(directory / (name + "_" + std::to_string(extent.width) + "x" +
                                  std::to_string(extent.height) + ".f16"),
                     mapped.subspan(0, std::min<size_t>(size, mapped.size())));
        }
    };

    dump("in0", chain->Input(0));
    dump("in1", chain->Input(1));

    for (size_t level = 0; level < LSFG_MIP_LEVELS; ++level) {
        dump("flow_mip" + std::to_string(level), chain->FlowLevel(level));
    }
    for (size_t index = 0; index < 2; ++index) {
        dump("alpha0_" + std::to_string(index), chain->AlphaOutput(0, count, index));
        dump("alpha6_" + std::to_string(index), chain->AlphaOutput(LSFG_MIP_LEVELS - 1, count,
                                                                  index));
    }
    for (size_t level = 0; level < LSFG_BETA_OUTPUTS; ++level) {
        dump("beta_" + std::to_string(level), chain->BetaOutput(level));
    }

    dump("gamma0", chain->GammaOutput(0));
    dump("gamma6", chain->GammaOutput(LSFG_MIP_LEVELS - 1));
    dump("delta2_out1", chain->DeltaOutput1(LSFG_DELTA_INSTANCES - 1));
    dump("delta2_out2", chain->DeltaOutput2(LSFG_DELTA_INSTANCES - 1));
}

} // namespace Vulkan
