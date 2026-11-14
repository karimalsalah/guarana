// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <numeric>
#include <optional>
#include <thread>

#include "common/fs/path_util.h"
#include "common/boot_timing.h"
#include "common/cpu_features.h"
#include "common/host_memory.h"
#include "common/memory_detect.h"
#include "common/scope_exit.h"
#include "common/logging.h"
#include "common/nx2_sampler.h"
#include "common/power_meter.h"
#include "common/power_policy.h"
#include "common/power_tuning.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/stb.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/debugger/nx2_bug_bundle.h"
#include "core/debugger/nx2_debug_server.h"
#include "core/debugger/nx2_debug_tools.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hle/kernel/board/nintendo/nx/k_system_control.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/nx2_thread_stats.h"
#include "core/memory.h"
#include "core/nx2_autopilot_session.h"
#include "core/nx2_power_session.h"
#include "hid_core/hid_core.h"
#include "video_core/auto_shading_rate.h"
#include "video_core/frame_budget.h"
#include "video_core/gpu.h"
#include "video_core/gpu_trace/recorder.h"
#include "video_core/nx2_async_readback.h"
#include "video_core/nx2_cache_stats.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_frame_predictor.h"
#include "video_core/nx2_shown_frames.h"
#include "video_core/renderer_base.h"

namespace Core::Nx2Debug {

using nlohmann::json;
using namespace std::chrono_literals;

/// Lines the log ring keeps for log_tail.
constexpr size_t RING_LINES = 20000;
/// Bytes one read_memory or read_gpu_memory call returns at most.
constexpr u64 MAX_READ = 64 * 1024;
/// Presented frames one input or input_sequence call holds at most (10 minutes at 60 fps).
constexpr u64 MAX_INPUT_FRAMES = 36000;
constexpr size_t MAX_INPUT_STEPS = 1000;

constexpr std::array<std::pair<std::string_view, HID::NpadButton>, 16> INPUT_BUTTONS{{
    {"A", HID::NpadButton::A},         {"B", HID::NpadButton::B},
    {"X", HID::NpadButton::X},         {"Y", HID::NpadButton::Y},
    {"L", HID::NpadButton::L},         {"R", HID::NpadButton::R},
    {"ZL", HID::NpadButton::ZL},       {"ZR", HID::NpadButton::ZR},
    {"Plus", HID::NpadButton::Plus},   {"Minus", HID::NpadButton::Minus},
    {"Up", HID::NpadButton::Up},       {"Down", HID::NpadButton::Down},
    {"Left", HID::NpadButton::Left},   {"Right", HID::NpadButton::Right},
    {"StickL", HID::NpadButton::StickL}, {"StickR", HID::NpadButton::StickR},
}};

struct ToolState {
    explicit ToolState(System& system_, std::string title_, u64 program_id_,
                       std::filesystem::path dump_directory_)
        : system{system_}, title{std::move(title_)}, program_id{program_id_},
          dump_directory{std::move(dump_directory_)} {}

    System& system;
    std::string title;
    u64 program_id;
    std::filesystem::path dump_directory;
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    /// Names this session's captures apart from earlier ones in the same dump directory.
    s64 session{std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()};
    std::string log_filter{Settings::values.log_filter.GetValue()};
    u64 captures{};
    u64 screenshots{};
    u64 shown_captures{};
    u64 samples{};
    u64 traces{};
    u64 gpu_reads{};
    /// Process CPU time and when it was read, at the previous power call.
    double power_cpu_seconds{};
    std::chrono::steady_clock::time_point power_time{};
    std::atomic<bool> stopping{};
};

namespace {

std::string Hex(u64 value) {
    return fmt::format("{:016x}", value);
}

json HexList(const std::vector<u64>& values) {
    json list = json::array();
    for (const u64 value : values) {
        list.push_back(Hex(value));
    }
    return list;
}

template <typename T>
T Param(const json& params, const char* name, T fallback, T low, T high) {
    const T value = params.value(name, fallback);
    return (std::clamp)(value, low, high);
}

std::string HexAddress(u64 value) {
    return fmt::format("{:#x}", value);
}

/// An address as a JSON number or a hex string (0x optional).
u64 AddressParam(const json& params, const char* name) {
    const json& value = params.at(name);
    if (value.is_number_unsigned()) {
        return value.get<u64>();
    }
    if (!value.is_string()) {
        throw std::runtime_error{fmt::format("{} must be a hex string or a non-negative number", name)};
    }
    const std::string text = value.get<std::string>();
    const std::string_view view{text};
    const std::string_view digits =
        view.starts_with("0x") || view.starts_with("0X") ? view.substr(2) : view;
    u64 address = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), address, 16);
    if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size()) {
        throw std::runtime_error{fmt::format("bad {} \"{}\": want hex, e.g. 0x8012345000", name, text)};
    }
    return address;
}

/// The size of a read: 1 to MAX_READ bytes, not wrapping past the end of the address space.
u64 ReadSize(const json& params, u64 address) {
    const json& value = params.at("size");
    if (!value.is_number_integer() || value.get<s64>() < 1 || value.get<u64>() > MAX_READ) {
        throw std::runtime_error{fmt::format("size must be 1 to {} bytes", MAX_READ)};
    }
    const u64 size = value.get<u64>();
    if (address + size < address) {
        throw std::runtime_error{"address + size wraps around"};
    }
    return size;
}

Kernel::KProcess& Application(ToolState& state) {
    Kernel::KProcess* const process = state.system.ApplicationProcess();
    if (process == nullptr) {
        throw std::runtime_error{"no application process is running"};
    }
    return *process;
}

/// A host pointer to the byte at address, or null when its page is not mapped. GetPointerSilent
/// reads the page entry as guest cores do, with no commit, log or GPU download; it masks the top
/// 16 bits, so addresses there would alias and are unmapped instead.
const u8* GuestByte(Memory::Memory& memory, u64 address) {
    return (address >> 48) == 0 ? memory.GetPointerSilent(address) : nullptr;
}

bool IsVulkan() {
    return Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
}

/// Waits for a future in short slices so a stopping server does not wait out the timeout.
template <typename T>
bool WaitReady(const std::atomic<bool>& stop, std::future<T>& future,
               std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (future.wait_for(50ms) != std::future_status::ready) {
        if (stop.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }
    return true;
}

void AppendPng(void* context, void* data, int size) {
    auto& png = *static_cast<std::vector<u8>*>(context);
    const u8* const bytes = static_cast<const u8*>(data);
    png.insert(png.end(), bytes, bytes + size);
}

json AutopilotState();

/// EDEN_NX2_PREDICT: the frame predictor's model in use, its interval errors and the slot
/// outcomes, since the game started ("total", no p90s) and over the last 10 s ("last_window").
json FramePredictorState() {
    namespace Predict = VideoCore::Nx2Predict;
    const Predict::Snapshot snapshot = Predict::FramePredictor::Instance().GetSnapshot();
    const auto window = [](const Predict::Window& w) {
        return json{{"samples", w.samples},
                    {"interval_mae_ms", {{"in_use", w.mae_active_ms},
                                         {"rls", w.mae_rls_ms},
                                         {"ewma", w.mae_ewma_ms}}},
                    {"interval_p90_error_ms", {{"in_use", w.p90_active_ms},
                                               {"rls", w.p90_rls_ms},
                                               {"ewma", w.p90_ewma_ms}}},
                    {"p90_coverage_percent", w.coverage_percent},
                    {"slots", w.slots},
                    {"slots_no_frame", w.misses},
                    {"slots_avoided", w.avoided},
                    {"misses_avoidable", w.avoidable},
                    {"holds", w.holds},
                    {"holds_landed", w.landed},
                    {"holds_timed_out", w.timeouts},
                    {"hold_wait_ms", w.waited_ms}};
    };
    return {{"mode", snapshot.mode},
            {"model", snapshot.using_rls ? "rls" : "ewma"},
            {"samples", snapshot.samples},
            {"switches", snapshot.switches},
            {"resets", snapshot.resets},
            {"mean_interval_ms", snapshot.mean_interval_ms},
            {"next_interval_ms", snapshot.next_mean_ms},
            {"next_interval_p90_ms", snapshot.next_p90_ms},
            {"hold_cap_ms", snapshot.hold_cap_ms},
            {"total", window(snapshot.total)},
            {"last_window", window(snapshot.last)}};
}

json Status(ToolState& state, const json& params) {
    const auto window = std::chrono::milliseconds{Param<int>(params, "fps_window_ms", 500, 0, 5000)};
    const u64 before = g_nx2_present_count.load(std::memory_order_relaxed);
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - begin < window &&
           !state.stopping.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(20ms);
    }
    const u64 after = g_nx2_present_count.load(std::memory_order_relaxed);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    return {
        {"title", state.title},
        {"program_id", fmt::format("{:016X}", state.program_id)},
        {"build", std::string{Common::g_build_fullname}},
        {"scm_rev", std::string{Common::g_scm_rev}},
        {"switch2", Settings::CurrentProgramIsSwitch2()},
        {"renderer", IsVulkan() ? "vulkan" : "other"},
        {"powered_on", state.system.IsPoweredOn()},
        {"paused", state.system.IsPaused()},
        {"presents", after},
        {"fps", seconds > 0.0 ? static_cast<double>(after - before) / seconds : 0.0},
        {"uptime_s", std::chrono::duration<double>(std::chrono::steady_clock::now() - state.start)
                         .count()},
        {"dump_dir", state.dump_directory.generic_string()},
        {"log_filter", state.log_filter},
        {"rejected", HexList(VideoCore::Nx2Debug::RejectedHashes())},
        {"autopilot", AutopilotState()},
        {"frame_predictor", FramePredictorState()},
    };
}

json FrameTimes(ToolState& state, const json& params) {
    const double seconds = params.value("seconds", 5.0);
    const double refresh_hz = params.value("refresh_hz", 60.0);
    if (!std::isfinite(seconds) || seconds <= 0.0 || seconds > 60.0 ||
        !std::isfinite(refresh_hz) || refresh_hz <= 0.0) {
        throw std::runtime_error{"seconds must be > 0 and <= 60; refresh_hz must be > 0"};
    }
    const u64 before = g_nx2_present_count.load(std::memory_order_relaxed);
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() <
               seconds &&
           !state.stopping.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(20ms);
    }
    const auto end = std::chrono::steady_clock::now();
    const u64 after = g_nx2_present_count.load(std::memory_order_relaxed);
    const u64 begin_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(begin.time_since_epoch()).count();
    const u64 end_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count();
    return ReadFrameTimes(before, after, begin_ns, end_ns, refresh_hz);
}

} // Anonymous namespace

json ReadFrameTimes(u64 before, u64 after, u64 begin_ns, u64 end_ns, double refresh_hz,
                    size_t recent_limit) {
    const u64 capacity = g_nx2_present_times_ns.size();
    bool truncated = after < before || after - before > capacity;
    const u64 first = (std::max)(before + 1, after > capacity ? after - capacity + 1 : u64{1});
    std::vector<double> intervals;
    u64 previous = 0;
    for (u64 frame = after; frame >= first; --frame) {
        const u64 timestamp =
            g_nx2_present_times_ns[frame % capacity].load(std::memory_order_relaxed);
        // The counter advances before its store; the slot can also be overwritten during a read.
        if (timestamp < begin_ns || timestamp > end_ns) {
            truncated |= timestamp > end_ns;
            previous = 0;
            continue;
        }
        if (previous != 0 && timestamp > previous) {
            intervals.clear();
        } else if (previous != 0) {
            intervals.push_back(static_cast<double>(previous - timestamp) / 1'000'000.0);
        }
        previous = timestamp;
    }

    std::ranges::reverse(intervals);
    json recent = json::array();
    for (size_t i = intervals.size() > recent_limit ? intervals.size() - recent_limit : 0;
         i < intervals.size(); ++i) {
        recent.push_back(std::round(intervals[i] * 10.0) / 10.0);
    }
    std::ranges::sort(intervals);
    const size_t frames = intervals.size();
    const auto percentile = [&](double fraction) {
        if (frames == 0) {
            return 0.0;
        }
        const double index = fraction * static_cast<double>(frames - 1);
        const size_t low = static_cast<size_t>(index);
        const size_t high = (std::min)(low + 1, frames - 1);
        return intervals[low] +
               (intervals[high] - intervals[low]) * (index - static_cast<double>(low));
    };
    const double mean = frames == 0 ? 0.0 : std::accumulate(intervals.begin(), intervals.end(), 0.0) /
                                              static_cast<double>(frames);
    const double median = percentile(0.5);
    std::array<size_t, 4> buckets{};
    size_t stutters = 0;
    for (const double interval : intervals) {
        const double periods = interval * refresh_hz / 1000.0;
        ++buckets[periods < 1.5 ? 0 : periods < 2.5 ? 1 : periods < 3.5 ? 2 : 3];
        stutters += interval > 2.0 * median;
    }
    const auto share = [&](size_t bucket) {
        return frames == 0 ? 0.0 : static_cast<double>(buckets[bucket]) / static_cast<double>(frames);
    };
    return {{"frames", frames},
            {"mean_ms", mean},
            {"fps", mean > 0.0 ? 1000.0 / mean : 0.0},
            {"p50_ms", median},
            {"p90_ms", percentile(0.9)},
            {"p99_ms", percentile(0.99)},
            {"max_ms", frames == 0 ? 0.0 : intervals.back()},
            {"min_ms", frames == 0 ? 0.0 : intervals.front()},
            {"vsync_buckets", {{"1", share(0)}, {"2", share(1)}, {"3", share(2)}, {"4+", share(3)}}},
            {"stutters", stutters},
            {"longest_ms", json(std::vector<double>(intervals.rbegin(),
                                                    intervals.rbegin() +
                                                        static_cast<std::ptrdiff_t>(
                                                            (std::min<size_t>)(frames, 10))))},
            {"intervals_ms", std::move(recent)},
            {"truncated", truncated}};
}


Png CaptureScreenshot(System& system, u32 height, std::chrono::seconds timeout,
                      const std::atomic<bool>& stop) {
    auto& renderer = system.GPU().Renderer();
    if (renderer.IsScreenshotPending()) {
        throw std::runtime_error{"a screenshot is already pending: no frame was presented since"};
    }
    const auto layout = Layout::DefaultFrameLayout(height * 16 / 9, height);
    const auto pixels = std::make_shared<std::vector<u8>>(size_t{layout.width} * layout.height * 4);
    const auto done = std::make_shared<std::promise<bool>>();
    auto future = done->get_future();
    renderer.RequestScreenshot(
        pixels->data(), [pixels, done](bool invert_y) { done->set_value(invert_y); }, layout);
    if (!WaitReady(stop, future, timeout)) {
        throw std::runtime_error{fmt::format(
            "no frame was presented within {} s (paused or loading?)", timeout.count())};
    }
    const bool invert_y = future.get();
    std::vector<u8> rgb(size_t{layout.width} * layout.height * 3);
    for (u32 y = 0; y < layout.height; ++y) {
        const u8* source = pixels->data() + size_t{invert_y ? layout.height - 1 - y : y} * layout.width * 4;
        u8* target = rgb.data() + size_t{y} * layout.width * 3;
        for (u32 x = 0; x < layout.width; ++x, source += 4, target += 3) {
            target[0] = source[2];
            target[1] = source[1];
            target[2] = source[0];
        }
    }
    Png png{.bytes = {}, .width = layout.width, .height = layout.height};
    if (!stbi_write_png_to_func(AppendPng, &png.bytes, static_cast<int>(layout.width),
                                static_cast<int>(layout.height), 3, rgb.data(),
                                static_cast<int>(layout.width * 3))) {
        throw std::runtime_error{"PNG encoding failed"};
    }
    return png;
}

namespace {

json Screenshot(ToolState& state, const json& params) {
    const Png shot = CaptureScreenshot(
        state.system, Param<u32>(params, "height", 720, 144, 2160),
        std::chrono::seconds{Param<int>(params, "timeout_s", 5, 1, 30)}, state.stopping);
    const std::vector<u8>& png = shot.bytes;
    json result = {{"width", shot.width}, {"height", shot.height}};
    if (params.value("save", false)) {
        std::filesystem::create_directories(state.dump_directory);
        const auto path =
            state.dump_directory / fmt::format("screenshot-{}-{:04}.png", state.session, ++state.screenshots);
        std::ofstream file{path, std::ios::binary};
        file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!file) {
            throw std::runtime_error{fmt::format("cannot write {}", path.generic_string())};
        }
        result["path"] = path.generic_string();
    }
    if (params.value("inline", true)) {
        result["png_base64"] = Base64(png);
    }
    return result;
}

std::vector<u8> EncodePng(const std::vector<u8>& rgb, u32 width, u32 height) {
    std::vector<u8> png;
    if (!stbi_write_png_to_func(AppendPng, &png, static_cast<int>(width), static_cast<int>(height),
                                3, rgb.data(), static_cast<int>(width * 3))) {
        throw std::runtime_error{"PNG encoding failed"};
    }
    return png;
}

/// The next frames the presenter shows, generated ones included, with their timing and a contact
/// sheet of their pictures (video_core/nx2_shown_frames.h, docs/nx2/debug-mcp.md).
json ShownFrames(ToolState& state, const json& params) {
    namespace Shown = VideoCore::Nx2ShownFrames;
    if (!IsVulkan()) {
        throw std::runtime_error{"shown_frames needs the Vulkan renderer"};
    }
    const u32 count = Param<u32>(params, "count", 12, 1, 32);
    const bool pictures = params.value("pictures", true);
    const u32 height = pictures ? Param<u32>(params, "height", 270, 144, 540) : 0;
    const int timeout = Param<int>(params, "timeout_s", 10, 1, 60);
    const u64 id = ++state.shown_captures;
    if (!Shown::RequestCapture({.id = id, .count = count, .height = height})) {
        throw std::runtime_error{"another shown_frames capture is still running"};
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{timeout};
    std::optional<Shown::Result> captured;
    while (!(captured = Shown::WaitCapture(id, 50ms))) {
        if (state.stopping.load(std::memory_order_relaxed) ||
            std::chrono::steady_clock::now() >= deadline) {
            Shown::CancelCapture(id);
            throw std::runtime_error{fmt::format(
                "{} frames were not shown within {} s (paused or loading?)", count, timeout)};
        }
    }
    const Shown::Result& result = *captured;
    const u32 shown = static_cast<u32>(result.frames.size());
    const std::vector<Shown::Times> times = Shown::RelativeTimes(result.frames);
    const Shown::SheetLayout layout = Shown::LayoutSheet(shown, result.width, result.height);
    const bool each = params.value("each", false);
    const auto maybe = [](const std::optional<s64>& value) {
        return value ? json(*value) : json(nullptr);
    };
    const size_t picture_bytes = size_t{result.width} * result.height * 4;
    std::array<u64, 3> kinds{};
    std::string presenter = shown != 0 ? std::string{result.frames.front().presenter} : "";
    u32 with_picture = 0;
    json frames = json::array();
    for (u32 index = 0; index < shown; ++index) {
        const Shown::Shown& frame = result.frames[index];
        const Shown::Times& timing = times[index];
        ++kinds[static_cast<size_t>(frame.kind)];
        if (frame.presenter != presenter) {
            presenter = "mixed";
        }
        json entry = {
            {"index", index},
            {"kind", std::string{Shown::KindName(frame.kind)}},
            {"presenter", std::string{frame.presenter}},
            {"slot", frame.slot != 0 ? json(frame.slot) : json(nullptr)},
            {"batch", frame.batch != 0 ? json(frame.batch) : json(nullptr)},
            {"scheduled_us", maybe(timing.scheduled_us)},
            {"presented_us", timing.presented_us},
            {"late_us", maybe(timing.late_us)},
            {"interval_us", maybe(timing.interval_us)},
            {"age_us", maybe(timing.age_us)},
            {"deferred_present", frame.deferred},
            {"picture", false},
        };
        const size_t offset = static_cast<size_t>((std::max)(frame.picture, 0)) * picture_bytes;
        if (frame.picture >= 0 && layout.width != 0 && offset + picture_bytes <= result.pixels.size()) {
            ++with_picture;
            const Shown::SheetTile tile = Shown::TileAt(layout, index, result.width, result.height);
            entry["picture"] = true;
            entry["tile"] = {{"x", tile.x},
                             {"y", tile.y},
                             {"width", result.width},
                             {"height", result.height}};
            if (each) {
                std::vector<u8> rgb(size_t{result.width} * result.height * 3);
                const u8* source = result.pixels.data() + offset;
                for (size_t pixel = 0; pixel < rgb.size() / 3; ++pixel, source += 4) {
                    rgb[pixel * 3] = source[2];
                    rgb[pixel * 3 + 1] = source[1];
                    rgb[pixel * 3 + 2] = source[0];
                }
                entry["png_base64"] = Base64(EncodePng(rgb, result.width, result.height));
            }
        }
        frames.push_back(std::move(entry));
    }
    json answer = {
        {"count", shown},
        {"presenter", presenter},
        {"kinds", {{"real", kinds[0]}, {"predicted", kinds[1]}, {"interpolated", kinds[2]}}},
        {"span_us", times.empty() ? s64{0} : times.back().presented_us},
        {"pictures", with_picture},
        {"width", result.width},
        {"height", result.height},
        {"hdr", result.hdr},
        {"frames", std::move(frames)},
    };
    std::vector<std::string> notes;
    if (presenter != "fluid") {
        notes.emplace_back("some or all frames are not on the EDEN_NX2_FLUID vblank clock (off, or "
                           "its watchdog fell back): those are shown in arrival order, so their slot, "
                           "scheduled_us and late_us are null, and age_us is null too");
    } else if (height != 0) {
        notes.emplace_back("while pictures were taken every screen copy ran on the graphics queue, "
                           "also with EDEN_NX2_FLUID_QUEUE set: for timing exactly as played, ask "
                           "with pictures: false");
    }
    if (result.hdr) {
        notes.emplace_back("HDR output: the pictures hold the encoded values cut to 8 bits, not "
                           "tone-mapped");
    }
    if (!result.picture_note.empty()) {
        answer["picture_note"] = result.picture_note;
    }
    answer["notes"] = notes;
    if (with_picture != 0) {
        const std::vector<u8> sheet = Shown::ComposeSheet(result, layout);
        const std::vector<u8> png = EncodePng(sheet, layout.width, layout.height);
        answer["sheet"] = {{"width", layout.width},
                           {"height", layout.height},
                           {"columns", layout.columns},
                           {"rows", layout.rows},
                           {"legend", "frames in order, left to right then down; the strip above "
                                      "each: green real, orange predicted, blue interpolated; grey: "
                                      "no picture"}};
        if (params.value("save", false)) {
            std::filesystem::create_directories(state.dump_directory);
            const auto path =
                state.dump_directory / fmt::format("shown-{}-{:04}.png", state.session, id);
            std::ofstream file{path, std::ios::binary};
            file.write(reinterpret_cast<const char*>(png.data()),
                       static_cast<std::streamsize>(png.size()));
            if (!file) {
                throw std::runtime_error{fmt::format("cannot write {}", path.generic_string())};
            }
            answer["path"] = path.generic_string();
        }
        answer["sheet_png_base64"] = Base64(png);
    }
    return answer;
}

json LogTail(ToolState&, const json& params) {
    const u64 after = params.value("after", u64{0});
    const size_t limit = Param<size_t>(params, "limit", 200, 1, 5000);
    const std::string contains = params.value("contains", std::string{});
    const std::vector<Common::Log::RingLine> lines = Common::Log::ReadRing(after);
    std::vector<const Common::Log::RingLine*> matched;
    for (const auto& line : lines) {
        if (contains.empty() || line.text.find(contains) != std::string::npos) {
            matched.push_back(&line);
        }
    }
    // From the start the newest lines; after a sequence number the oldest ones, to page forward.
    const bool more = matched.size() > limit;
    if (more) {
        if (after == 0) {
            matched.erase(matched.begin(), matched.end() - static_cast<std::ptrdiff_t>(limit));
        } else {
            matched.resize(limit);
        }
    }
    json text = json::array();
    for (const auto* line : matched) {
        text.push_back(line->text);
    }
    const u64 next = more && after != 0 ? matched.back()->sequence
                     : lines.empty()    ? after
                                        : lines.back().sequence;
    return {{"lines", std::move(text)}, {"next", next}, {"more", more}};
}

json LogFilter(ToolState& state, const json& params) {
    const std::string filter = params.at("filter").get<std::string>();
    Common::Log::Filter parsed;
    parsed.ParseFilterString(filter);
    Common::Log::SetGlobalFilter(parsed);
    json result = {{"previous", state.log_filter}, {"filter", filter}};
    state.log_filter = filter;
    return result;
}

json CaptureFrame(ToolState& state, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"frame capture needs the Vulkan renderer"};
    }
    const u64 id = ++state.captures;
    const auto directory = state.dump_directory / fmt::format("capture-{}-{:03}", state.session, id);
    std::filesystem::create_directories(directory);
    const bool sampled = params.value("sampled", false);
    const bool buffers = params.value("buffers", false);
    const int timeout = Param<int>(params, "timeout_s", 120, 1, 600);
    VideoCore::Nx2Debug::RequestCapture(
        {.id = id, .directory = directory, .sampled = sampled, .buffers = buffers});
    const auto finished = VideoCore::Nx2Debug::WaitCapture(id, std::chrono::seconds{timeout});
    if (!finished) {
        VideoCore::Nx2Debug::CancelCapture(id);
        throw std::runtime_error{fmt::format(
            "the capture did not finish within {} s (paused or loading?); if it started, its "
            "files are still being written to {}",
            timeout, directory.generic_string())};
    }
    if (!finished->error.empty()) {
        throw std::runtime_error{finished->error};
    }
    const size_t keep = Param<size_t>(params, "manifest_lines", 200, 0, 5000);
    json entries = json::array();
    json kinds = json::object();
    json frame;
    json summary;
    size_t total = 0;
    std::ifstream manifest{directory / "manifest.jsonl"};
    for (std::string line; std::getline(manifest, line);) {
        json entry = json::parse(line, nullptr, false);
        if (entry.is_discarded()) {
            continue;
        }
        const std::string kind = entry.value("kind", std::string{});
        if (kind == "frame") {
            frame = std::move(entry);
        } else if (kind == "summary") {
            summary = std::move(entry);
        } else {
            kinds[kind] = kinds.value(kind, 0) + 1;
            if (total++ < keep) {
                entries.push_back(std::move(entry));
            }
        }
    }
    return {
        {"directory", directory.generic_string()},
        {"sampled", sampled},
        {"buffers", buffers},
        {"frame", std::move(frame)},
        {"summary", std::move(summary)},
        {"kinds", std::move(kinds)},
        {"entries_total", total},
        {"entries", std::move(entries)},
        {"index_command",
         fmt::format("python3 -I tools/nx2/frame_dump_index.py \"{}\"", directory.generic_string())},
    };
}

json CaptureBundle(ToolState& state, const json& params) {
    BugBundle bundle{
        state.system,
        {.trigger = "debug_server",
         .directory = state.dump_directory / "bug-reports",
         .size_limit = Param<u64>(params, "size_limit_mib", BugBundle::DEFAULT_SIZE_LIMIT >> 20, 1,
                                  100)
                       << 20,
         .capture_timeout = std::chrono::seconds{Param<int>(params, "timeout_s", 90, 5, 600)}}};
    bundle.Capture(state.stopping);
    const BugBundle::Result result = bundle.Write(params.value("description", std::string{}));
    return {
        {"path", result.path.generic_string()},
        {"bytes", result.bytes},
        {"dropped", result.dropped},
        {"frame_error", result.frame_error},
        {"manifest", result.manifest},
        {"index_command", fmt::format("python3 -I tools/nx2/bundle_index.py \"{}\"",
                                      result.path.generic_string())},
    };
}

json RejectShaders(ToolState&, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"runtime rejection needs the Vulkan renderer"};
    }
    std::vector<u64> hashes;
    for (const json& hash : params.value("hashes", json::array())) {
        hashes.push_back(ParseShaderHash(hash.get<std::string>()));
    }
    const bool reject = params.value("reject", true);
    const u64 generation = VideoCore::Nx2Debug::SetRejected(hashes, reject);
    const int timeout = Param<int>(params, "timeout_s", 5, 0, 60);
    const bool applied = hashes.empty() ||
                         VideoCore::Nx2Debug::WaitRejectApplied(generation, std::chrono::seconds{timeout});
    json result = {{"rejected", HexList(VideoCore::Nx2Debug::RejectedHashes())}, {"applied", applied}};
    if (!applied) {
        result["note"] = "no frame was presented yet; the change applies at the next frame end";
    }
    return result;
}

json SetShadingRate(ToolState& state, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"shading rates need the Vulkan renderer"};
    }
    namespace Auto = VideoCore::AutoShadingRate;
    const bool enabled = std::getenv("EDEN_NX2_VRS") != nullptr;
    auto& renderer = state.system.GPU().Renderer();
    if (params.contains("auto")) {
        const double fps = params.at("auto").get<double>();
        if (fps > 0.0 && !enabled) {
            throw std::runtime_error{"the governor needs EDEN_NX2_VRS at launch"};
        }
        if (fps > 0.0 && !renderer.RequestGpuCostSampling(true)) {
            throw std::runtime_error{"the governor needs cost sampling, which is unavailable"};
        }
        Auto::SetTargetFps(fps > 0.0 ? fps : 0.0);
    }
    for (const char* const key : {"auto_deny", "auto_allow"}) {
        std::vector<u64> listed;
        for (const json& hash : params.value(key, json::array())) {
            listed.push_back(ParseShaderHash(hash.get<std::string>()));
        }
        Auto::SetDenied(listed, std::string_view{key} == "auto_deny");
    }
    std::vector<u64> hashes;
    for (const json& hash : params.value("hashes", json::array())) {
        hashes.push_back(ParseShaderHash(hash.get<std::string>()));
    }
    const bool all = params.value("all", false);
    if (!hashes.empty() || all) {
        const std::string text = params.value("rate", std::string{"2x2"});
        const auto rate = VideoCore::Nx2Debug::ParseShadingRate(text);
        if (!rate) {
            throw std::runtime_error{fmt::format(
                "bad rate \"{}\": want 1x1, 2x1, 1x2, 2x2, 4x2, 2x4 or 4x4", text)};
        }
        VideoCore::Nx2Debug::SetShadingRates(hashes, *rate, all);
    }
    const auto size = [](VideoCore::Nx2Debug::ShadingRate rate) {
        return fmt::format("{}x{}", rate.width, rate.height);
    };
    const VideoCore::Nx2Debug::ShadingRateTable table = VideoCore::Nx2Debug::ShadingRates();
    json rates = json::object();
    for (const auto& [hash, rate] : table.rates) {
        rates[Hex(hash)] = size(rate);
    }
    json result = {{"enabled", enabled}, {"default", size(table.fallback)}, {"rates", std::move(rates)}};
    if (!enabled) {
        result["note"] = "EDEN_NX2_VRS is not set: the rates are kept but no draw uses them";
    }
    // The governor layer: used where the manual table above says nothing.
    const Auto::Status status = Auto::GetStatus();
    json auto_rates = json::object();
    for (const auto& [hash, rate] : table.auto_rates) {
        auto_rates[Hex(hash)] = size(rate);
    }
    json levels = json::object();
    for (const auto& [hash, level] : status.levels) {
        levels[Hex(hash)] = level;
    }
    const auto capped = [](std::vector<u64> hashes) {
        hashes.resize((std::min)(hashes.size(), size_t{64}));
        return HexList(hashes);
    };
    result["auto"] = {
        {"target_fps", status.target_fps},
        {"target_ms", status.target_ms},
        {"frame_ms", status.frame_ms},
        {"allowed", status.allowed},
        {"sampling", renderer.IsGpuCostSampling()},
        {"snapshots", status.snapshots},
        {"max_level", status.max_level},
        {"rates", std::move(auto_rates)},
        {"levels", std::move(levels)},
        {"denied",
         {{"user", HexList(status.denied_user)},
          {"small_draws_count", status.denied_small.size()},
          {"small_draws", capped(status.denied_small)},
          {"side_targets_count", status.denied_side.size()},
          {"side_targets", capped(status.denied_side)},
          {"depth_writers", status.depth_writers}}},
        {"last_action", status.last_action},
        {"hold_reason", status.hold_reason},
    };
    return result;
}

json SetPrepass(ToolState&, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"the prepass split needs the Vulkan renderer"};
    }
    std::vector<u64> hashes;
    for (const json& hash : params.value("hashes", json::array())) {
        hashes.push_back(ParseShaderHash(hash.get<std::string>()));
    }
    if (!hashes.empty()) {
        VideoCore::Nx2Debug::SetPrepassHashes(hashes, params.value("on", true));
    }
    const VideoCore::Nx2Debug::PrepassCounts counts = VideoCore::Nx2Debug::PrepassCountsSoFar();
    return {
        {"hashes", HexList(VideoCore::Nx2Debug::PrepassHashes().hashes)},
        {"pipelines", counts.pipelines},
        {"split_draws", counts.split_draws},
        {"plain_draws", counts.plain_draws},
    };
}

json TraceStatus(ToolState&, const json&) {
    const auto status = VideoCommon::GpuTrace::GetRecorderStatus();
    return {{"requested", status.requested},       {"recording", status.recording},
            {"presents", status.presents},         {"presents_wanted", status.presents_wanted},
            {"bytes_uncompressed", status.bytes},  {"path", status.path},
            {"last_trace", status.last_trace},     {"last_error", status.last_error}};
}

json TraceRecord(ToolState& state, const json& params) {
    VideoCommon::GpuTrace::RecordOptions options;
    const double seconds = Param<double>(params, "seconds", 3.0, 0.1, 60.0);
    options.presents = Param<u32>(params, "presents", static_cast<u32>(seconds * 60.0), 1, 3600);
    if (params.contains("path")) {
        options.path = params["path"].get<std::string>();
    } else {
        std::filesystem::create_directories(state.dump_directory);
        options.path = state.dump_directory /
                       fmt::format("trace-{}-{:03}.lxtrace", state.session, ++state.traces);
    }
    std::string error;
    if (!VideoCommon::GpuTrace::RequestRecording(options, error)) {
        throw std::runtime_error{error};
    }
    return TraceStatus(state, params);
}

json TraceStop(ToolState& state, const json& params) {
    VideoCommon::GpuTrace::StopRecording();
    return TraceStatus(state, params);
}

json SampleThreads(ToolState& state, const json& params) {
    const std::string threads = params.value("threads", std::string{"GPU,GPUBack"});
    const double seconds = Param<double>(params, "seconds", 10.0, 1.0, 60.0);
    std::filesystem::create_directories(state.dump_directory);
    const auto path =
        state.dump_directory / fmt::format("samples-{}-{:03}.txt", state.session, ++state.samples);
    std::string report = Nx2Sampler::Sample(threads.c_str(), seconds, path.string());
    report.resize((std::min)(report.size(), Param<size_t>(params, "max_chars", 60000, 0, 4000000)));
    return {{"file", path.generic_string()}, {"report", std::move(report)}};
}

// Lifts or restores the speed limit (test scripts boot through the logos and menus unlimited).
json SpeedLimit(ToolState&, const json& params) {
    auto& setting = Settings::values.use_speed_limit;
    const bool previous = setting.GetValue();
    if (params.contains("on")) {
        setting.SetValue(params.at("on").get<bool>());
    }
    return {{"previous", previous}, {"on", setting.GetValue()}};
}

json HostTiming(ToolState& state, const json& params) {
    auto& stats = state.system.CoreTiming().host_wait_stats;
    const bool previous = stats.load();
    if (params.contains("stats")) {
        stats = params.at("stats").get<bool>();
    }
    return {{"previous", previous}, {"stats", stats.load()}};
}

json PowerSwitches() {
    namespace Tuning = Common::PowerTuning;
    return {
        {"spin_scale", Tuning::spin_percent.load(std::memory_order_relaxed) / 100.0},
        {"eco_background", Tuning::eco_background.load(std::memory_order_relaxed)},
        {"idle_spin_us", Tuning::idle_spin_us.load(std::memory_order_relaxed)},
    };
}

// Host package, CPU core and integrated GPU watts and the cores this process kept busy, averaged
// over window_ms (0: since the previous call).
json Power(ToolState& state, const json& params) {
    const auto window = std::chrono::milliseconds{Param<int>(params, "window_ms", 1000, 0, 10000)};
    auto& meter = Common::PowerMeter::Instance();
    VideoCore::RendererBase* const renderer =
        state.system.IsPoweredOn() ? &state.system.GPU().Renderer() : nullptr;
    if (renderer != nullptr) {
        renderer->RequestGpuBusySample();
    }
    if (window.count() > 0) {
        static_cast<void>(meter.Read());
        state.power_cpu_seconds = Common::ProcessCpuSeconds();
        state.power_time = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - state.power_time < window &&
               !state.stopping.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(20ms);
        }
    }
    const Common::PowerMeter::Sample sample = meter.Read();
    const double cpu_seconds = Common::ProcessCpuSeconds();
    const auto now = std::chrono::steady_clock::now();
    const bool first = state.power_time == std::chrono::steady_clock::time_point{};
    const double seconds = std::chrono::duration<double>(now - state.power_time).count();
    const double cores =
        !first && seconds > 0.0 ? (cpu_seconds - state.power_cpu_seconds) / seconds : 0.0;
    state.power_cpu_seconds = cpu_seconds;
    state.power_time = now;
    json result = {
        {"available", meter.Available()},
        {"valid", sample.valid},
        {"package_w", sample.package_w},
        {"cpu_w", sample.cpu_w},
        {"gpu_w", sample.gpu_w},
        {"seconds", first ? 0.0 : seconds},
        {"process_cpu_cores", cores},
        {"gpu_busy_ms", nullptr},
        {"channels", meter.ChannelNames()},
    };
    if (renderer != nullptr) {
        if (const auto busy = renderer->GetGpuBusyMs()) {
            result["gpu_busy_ms"] = *busy;
        }
    }
    result.update(PowerSwitches());
    // EDEN_NX2_POWER_POLICY=igpu: the power scheme change of the session and the spin governor.
    const Common::PowerPolicy::Status policy = Common::PowerPolicy::GetStatus();
    json settings = json::object();
    for (const auto& setting : policy.settings) {
        settings[setting.name] = {{"saved_ac", setting.saved_ac},
                                  {"saved_dc", setting.saved_dc},
                                  {"applied_ac", setting.applied_ac},
                                  {"applied_dc", setting.applied_dc}};
    }
    result["policy"] = {{"requested", policy.requested},
                        {"active", policy.active},
                        {"scheme", policy.scheme},
                        {"settings", std::move(settings)},
                        {"message", policy.message}};
    const Core::Nx2PowerSession::GovernorState governor = Core::Nx2PowerSession::LastState();
    result["governor"] = {{"running", governor.running},
                          {"state", governor.state},
                          {"spin_scale", governor.spin_percent / 100.0},
                          {"ceiling", governor.ceiling / 100.0},
                          {"target_fps", governor.target_fps},
                          {"frame_ms", governor.frame_ms},
                          {"gpu_busy", governor.gpu_busy},
                          {"package_w", governor.package_w},
                          {"cpu_w", governor.cpu_w},
                          {"gpu_w", governor.gpu_w},
                          {"changes", governor.changes},
                          {"last_change", governor.last_change}};
    return result;
}

// Changes the power switches live. eco_background only reaches threads that start afterwards.
json PowerSet(ToolState&, const json& params) {
    namespace Tuning = Common::PowerTuning;
    if (params.contains("spin_scale")) {
        Tuning::spin_percent.store(Tuning::SpinPercent(params.at("spin_scale").get<double>()),
                                   std::memory_order_relaxed);
    }
    if (params.contains("eco_background")) {
        Tuning::eco_background.store(params.at("eco_background").get<bool>(),
                                     std::memory_order_relaxed);
    }
    if (params.contains("idle_spin_us")) {
        Tuning::idle_spin_us.store((std::clamp)(params.at("idle_spin_us").get<s64>(), s64{-1},
                                                s64{100000}),
                                   std::memory_order_relaxed);
    }
    return PowerSwitches();
}

// Host GPU time of the last sampled frame by shaders and by render target.
json GpuCosts(ToolState& state, const json& params) {
    auto& renderer = state.system.GPU().Renderer();
    const bool previous = renderer.IsGpuCostSampling();
    if (params.contains("enable")) {
        const bool enable = params.at("enable").get<bool>();
        if (!renderer.RequestGpuCostSampling(enable) && enable) {
            throw std::runtime_error{"cost sampling is unavailable: it needs the Vulkan renderer "
                                     "with timestamp queries"};
        }
    }
    const u32 top = Param<u32>(params, "top", 32, 0, 256);
    const VideoCore::GpuCostSnapshot costs = renderer.GetGpuCosts();
    json work = json::array();
    for (size_t i = 0; i < costs.work.size() && i < top; ++i) {
        const auto& entry = costs.work[i];
        json item = {
            {"vertex_hash", Hex(entry.vertex_hash)},
            {"fragment_hash", Hex(entry.fragment_hash)},
            {"kind", entry.kind == '\0' ? std::string{} : std::string(1, entry.kind)},
            {"compute", entry.IsCompute()},
            {"draws", entry.draws},
            {"small_draws", entry.small_draws},
            {"side_draws", entry.side_draws},
            {"total_ms", entry.total_ms},
            {"max_ms", entry.max_ms},
        };
        work.push_back(std::move(item));
    }
    json passes = json::array();
    for (size_t i = 0; i < costs.passes.size() && i < top; ++i) {
        const auto& pass = costs.passes[i];
        json item = {
            {"width", pass.width},
            {"height", pass.height},
            {"color_format", pass.color_format},
            {"num_colors", pass.num_colors},
            {"depth_format", pass.depth_format},
            {"samples", pass.samples},
            {"passes", pass.passes},
            {"total_ms", pass.total_ms},
            {"max_ms", pass.max_ms},
        };
        passes.push_back(std::move(item));
    }
    json emu = json::array();
    for (const auto& entry : costs.emu) {
        emu.push_back({{"category", entry.category},
                       {"count", entry.count},
                       {"total_ms", entry.total_ms},
                       {"max_ms", entry.max_ms}});
    }
    json age = nullptr;
    if (costs.frame != 0) {
        age = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                        costs.when)
                  .count();
    }
    return {{"previous", previous},
            {"sampling", renderer.IsGpuCostSampling()},
            {"frame", costs.frame},
            {"age_ms", std::move(age)},
            {"frame_span_ms", costs.frame_span_ms},
            {"has_main_target", costs.has_main_target},
            {"submission_sum_ms", costs.submission_sum_ms},
            {"timed_sum_ms", costs.timed_sum_ms},
            {"emu_sum_ms", costs.emu_sum_ms},
            {"emu", std::move(emu)},
            {"work_entries", costs.work.size()},
            {"work", std::move(work)},
            {"pass_entries", costs.passes.size()},
            {"passes", std::move(passes)}};
}

// EDEN_NX2_GPU_PERF: hardware GPU counters (VK_KHR_performance_query) averaged over a
// sampling window of submissions: busy, stall, occupancy and core frequency.
json GpuPerf(ToolState& state, const json& params) {
    auto& renderer = state.system.GPU().Renderer();
    if (!renderer.IsGpuPerfSupported()) {
        return {{"supported", false}, {"reason", renderer.GpuPerfReason()}};
    }
    if (params.contains("enable")) {
        const bool enable = params.at("enable").get<bool>();
        const u32 frames = Param<u32>(params, "frames", 30, 1, 600);
        if (!renderer.RequestGpuPerfSampling(enable ? frames : 0)) {
            return {{"supported", false}, {"reason", renderer.GpuPerfReason()}};
        }
    }
    const std::vector<VideoCore::RendererBase::GpuPerfSample> samples = renderer.ReadGpuPerf();
    json counters = json::array();
    json gpu_busy = nullptr;
    json stall = nullptr;
    json occupancy = nullptr;
    json freq = nullptr;
    for (const auto& sample : samples) {
        counters.push_back(
            {{"name", sample.name}, {"value", sample.value}, {"unit", sample.unit}});
        if (sample.name == "GpuBusy") {
            gpu_busy = sample.value;
        } else if (sample.name == "XveStall") {
            stall = sample.value;
        } else if (sample.name == "XveThreadOccupancy") {
            occupancy = sample.value;
        } else if (sample.name == "AvgGpuCoreFrequencyMHz") {
            freq = sample.value;
        }
    }
    const auto [pending, begin_calls, gpu_tick] = renderer.GpuPerfDiag();
    return {{"supported", true},
            {"sampling", renderer.IsGpuPerfSampling()},
            {"frames_sampled", renderer.GpuPerfFramesSampled()},
            {"diag", {{"pending", pending}, {"begin_calls", begin_calls}, {"gpu_tick", gpu_tick}}},
            {"counters", std::move(counters)},
            {"gpu_busy_pct", std::move(gpu_busy)},
            {"stall_pct", std::move(stall)},
            {"occupancy_pct", std::move(occupancy)},
            {"freq_mhz", std::move(freq)}};
}

// The frame budget controller (EDEN_NX2_TARGET_FPS): sets the target and the GPU time scale live
// and reports what the controller measured and decided at its last tick.
json FrameBudgetTool(ToolState&, const json& params) {
    namespace Budget = VideoCore::FrameBudget;
    if (params.contains("target_fps")) {
        const double fps = params.at("target_fps").get<double>();
        if (!std::isfinite(fps) || (fps != 0.0 && (fps < 1.0 || fps > 1000.0))) {
            throw std::runtime_error{"target_fps: want 0 (off) or 1 to 1000"};
        }
        Budget::SetTargetFps(fps);
    }
    if (params.contains("scale")) {
        const double scale = params.at("scale").get<double>();
        if (!std::isfinite(scale) || scale <= 0.0) {
            throw std::runtime_error{"scale: want a positive number"};
        }
        Budget::SetGpuTimeScale(scale);
    }
    const Budget::Status status = Budget::GetStatus();
    return {{"target_fps", status.target_fps},
            {"target_ms", status.target_ms},
            {"frame_ms", status.frame_ms},
            {"work_ms", status.work_ms},
            {"samples", status.samples},
            {"scale", status.scale},
            {"latched_scale", status.latched_scale},
            {"manual_scale", status.manual_scale},
            {"min_scale", status.min_scale},
            {"max_scale", status.max_scale},
            {"state", status.state},
            {"ticks", status.ticks},
            {"raises", status.raises},
            {"lowers", status.lowers},
            {"miss_scale", status.miss_scale},
            {"cooldown_ticks", status.cooldown_ticks},
            {"fast_streak", status.fast_streak},
            {"blind_lowers", status.blind_lowers},
            {"shading_rate_allowed", status.shading_allowed},
            {"last_change", status.last_change},
            {"unanswered_from", status.unanswered_from}};
}

// The autopilot (EDEN_NX2_AUTOPILOT=1): what its last 2 s window measured and what it holds.
json AutopilotState() {
    const Core::Nx2AutopilotSession::State state = Core::Nx2AutopilotSession::LastState();
    return {{"requested", Core::Nx2AutopilotSession::Requested()},
            {"running", state.running},
            {"windows", state.windows},
            {"changes", state.changes},
            {"real_fps", state.real_fps},
            {"shown_p50_ms", state.shown_p50_ms},
            {"shown_p95_ms", state.shown_p95_ms},
            {"shown_even_percent", state.shown_even_percent},
            {"shown_samples", state.shown_samples},
            {"refresh_hz", state.refresh_hz},
            {"refresh_source", state.refresh_source},
            {"gpu_busy", state.gpu_busy},
            {"power", state.power},
            {"frame_gen_on", state.frame_gen_on},
            {"frame_gen_target", state.frame_gen_target},
            {"frame_gen_multiplier", state.frame_gen_multiplier},
            {"gpu_time_scale", state.scale},
            {"gpu_time_scale_base", state.scale_base},
            {"resolution_available", state.resolution_available},
            {"power_policy_available", state.power_available},
            {"power_policy_applied", state.power_applied},
            {"last_change", state.last_change}};
}

json AutopilotTool(ToolState&, const json&) {
    return AutopilotState();
}

/// Host memory of the process and the system, and what the caches last published
/// (Common::g_memory_counters). All sizes in bytes.
json MemoryReport(ToolState&, const json&) {
    const Common::ProcessMemoryUsage usage = Common::GetProcessMemoryUsage();
    const Common::MemoryCounters& counters = Common::g_memory_counters;
    const auto get = [](const std::atomic<u64>& value) {
        return value.load(std::memory_order_relaxed);
    };
    return {
        {"low_memory_mode", Common::LowMemoryMode()},
        {"low_memory_reason", std::string{Common::LowMemoryModeReason()}},
        {"process",
         {
             {"working_set", usage.working_set},
             {"private_working_set", usage.private_working_set},
             {"peak_working_set", usage.peak_working_set},
             {"commit", usage.commit},
             {"peak_commit", usage.peak_commit},
         }},
        {"system",
         {
             {"commit", usage.system_commit},
             {"commit_limit", usage.system_commit_limit},
             {"physical_total", usage.physical_total},
             {"physical_available", usage.physical_available},
         }},
        {"guest",
         {
             {"size", u64{Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize()}},
             {"resident_after_trim", get(counters.guest_resident)},
             {"zero_trimmed_total", get(counters.guest_zero_trimmed)},
             {"zero_filled_total", Common::HostMemory::ZeroFilledBytes()},
             {"lazy_commit", get(counters.guest_lazy_commit)},
         }},
        {"renderer",
         {
             {"device_usage", get(counters.device_usage)},
             {"texture_expected", get(counters.texture_expected)},
             {"texture_critical", get(counters.texture_critical)},
             {"buffer_cache", get(counters.buffer_cache_bytes)},
             {"staging", get(counters.staging_bytes)},
             {"mesh_arenas", get(counters.mesh_arena_bytes)},
             {"graphics_pipelines", get(counters.graphics_pipelines)},
             {"built_pipelines", get(counters.built_pipelines)},
             {"kept_pipelines_limit", get(counters.kept_pipelines_limit)},
             {"kept_spirv_entries", get(counters.kept_spirv_entries)},
             {"kept_spirv", get(counters.kept_spirv_bytes)},
         }},
    };
}

json ReadMemory(ToolState& state, const json& params) {
    const u64 address = AddressParam(params, "address");
    const u64 size = ReadSize(params, address);
    const std::string as = params.value("as", std::string{"hex"});
    static_cast<void>(FormatMemory(std::span<const u8>{}, as));
    std::vector<u8> bytes(size);
    if (const auto unmapped = ReadGuestMemory(Application(state).GetMemory(), address, bytes)) {
        throw std::runtime_error{fmt::format(
            "guest address {:#x} is not mapped (reading {} bytes at {:#x})", *unmapped, size, address)};
    }
    json result = {{"address", HexAddress(address)}, {"size", size}};
    result.update(FormatMemory(bytes, as));
    return result;
}

json SearchMemory(ToolState& state, const json& params) {
    const std::vector<u8> pattern = ParseHexBytes(params.at("pattern").get<std::string>());
    if (pattern.size() > 256) {
        throw std::runtime_error{"the pattern is longer than 256 bytes"};
    }
    Kernel::KProcess& process = Application(state);
    const std::string regions = params.value("regions", std::string{"heap"});
    u64 region_start = 0;
    u64 region_size = 0;
    if (regions == "heap") {
        region_start = GetInteger(process.GetPageTable().GetHeapRegionStart());
        region_size = process.GetPageTable().GetHeapRegionSize();
    } else if (regions == "all") {
        region_start = GetInteger(process.GetPageTable().GetAddressSpaceStart());
        region_size = process.GetPageTable().GetAddressSpaceSize();
    } else {
        throw std::runtime_error{fmt::format("regions \"{}\": want heap or all", regions)};
    }
    const u64 start = params.contains("start") ? (std::max)(region_start, AddressParam(params, "start"))
                                               : region_start;
    const u64 end = params.contains("end")
                        ? (std::min)(region_start + region_size, AddressParam(params, "end"))
                        : region_start + region_size;
    const size_t max_hits = Param<size_t>(params, "max_hits", 64, 1, 256);
    const int budget = Param<int>(params, "budget_ms", 2000, 1, 10000);
    const auto begin = std::chrono::steady_clock::now();
    const GuestSearch search =
        SearchGuestMemory(process.GetMemory(), pattern, start, (std::max)(start, end), max_hits,
                          begin + std::chrono::milliseconds{budget});
    json hits = json::array();
    for (const u64 hit : search.hits) {
        hits.push_back(HexAddress(hit));
    }
    json result = {
        {"hits", std::move(hits)},
        {"complete", search.complete},
        {"start", HexAddress(start)},
        {"end", HexAddress((std::max)(start, end))},
        {"scanned_bytes", search.scanned},
        {"elapsed_ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                 begin)
                           .count()},
    };
    if (!search.complete) {
        result["next"] = HexAddress(search.next);
    }
    return result;
}

json ReadGpuMemory(ToolState& state, const json& params) {
    if (!IsVulkan()) {
        throw std::runtime_error{"GPU memory reads need the Vulkan renderer"};
    }
    const u64 address = AddressParam(params, "gpu_address");
    const u64 size = ReadSize(params, address);
    const std::string as = params.value("as", std::string{"hex"});
    static_cast<void>(FormatMemory(std::span<const u8>{}, as));
    const u64 id = ++state.gpu_reads;
    const int timeout = Param<int>(params, "timeout_s", 5, 1, 60);
    VideoCore::Nx2Debug::RequestGpuRead({.id = id, .gpu_address = address, .size = size});
    const auto read = VideoCore::Nx2Debug::WaitGpuRead(id, std::chrono::seconds{timeout});
    if (!read) {
        VideoCore::Nx2Debug::CancelGpuRead(id);
        throw std::runtime_error{
            fmt::format("no frame was presented within {} s (paused or loading?)", timeout)};
    }
    if (!read->error.empty()) {
        throw std::runtime_error{read->error};
    }
    json result = {{"gpu_address", HexAddress(address)},
                   {"size", size},
                   {"address_space", read->address_space}};
    result.update(FormatMemory(read->bytes, as));
    return result;
}

/// Holds the steps on player 1 (or the handheld controller when player 1 is not connected) from
/// the next present on, and returns once they are done. The injector stops by itself after the
/// last step; leaving this function by any path removes it.
json RunInput(ToolState& state, const std::vector<InputStep>& steps, const json& params,
              const ClientGone& client_gone) {
    u64 total = 0;
    for (const InputStep& step : steps) {
        total += step.frames;
    }
    if (total > MAX_INPUT_FRAMES) {
        throw std::runtime_error{
            fmt::format("{} frames in total; at most {} per call", total, MAX_INPUT_FRAMES)};
    }
    auto& hid = state.system.HIDCore();
    HID::EmulatedController* controller = hid.GetEmulatedController(HID::NpadIdType::Player1);
    std::string name = "player1";
    if (!controller->IsConnected()) {
        controller = hid.GetEmulatedController(HID::NpadIdType::Handheld);
        name = "handheld";
    }
    if (!controller->IsConnected()) {
        throw std::runtime_error{"neither player 1 nor the handheld controller is connected"};
    }
    const int timeout = Param<int>(params, "timeout_s", static_cast<int>(total / 20 + 10), 1, 900);
    const u64 start = g_nx2_present_count.load(std::memory_order_relaxed) + 1;
    const auto script = std::make_shared<const std::vector<InputStep>>(steps);
    controller->SetInputInjector([script, start] {
        return InputAt(*script, start, g_nx2_present_count.load(std::memory_order_relaxed));
    });
    SCOPE_EXIT {
        controller->SetInputInjector({});
    };
    const auto begin = std::chrono::steady_clock::now();
    const auto deadline = begin + std::chrono::seconds{timeout};
    for (;;) {
        const u64 now = g_nx2_present_count.load(std::memory_order_relaxed);
        if (now >= start + total) {
            break;
        }
        if (state.stopping.load(std::memory_order_relaxed)) {
            throw std::runtime_error{"the game is shutting down; input released"};
        }
        if (client_gone()) {
            LOG_INFO(Debug, "Debug server client left during input; input released");
            throw std::runtime_error{"the client disconnected; input released"};
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error{fmt::format(
                "{} of {} frames were presented within {} s (paused or loading?); input released",
                now >= start ? now - start : 0, total, timeout)};
        }
        std::this_thread::sleep_for(2ms);
    }
    return {
        {"controller", name},
        {"steps", steps.size()},
        {"frames", total},
        {"first_present", start},
        {"elapsed_s",
         std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count()},
    };
}

json Input(ToolState& state, const json& params, const ClientGone& client_gone) {
    json step = params;
    step.erase("timeout_s");
    return RunInput(state, {ParseInputStep(step)}, params, client_gone);
}

json InputSequence(ToolState& state, const json& params, const ClientGone& client_gone) {
    const json& list = params.at("steps");
    if (!list.is_array() || list.empty() || list.size() > MAX_INPUT_STEPS) {
        throw std::runtime_error{fmt::format("steps must be an array of 1 to {}", MAX_INPUT_STEPS)};
    }
    std::vector<InputStep> steps;
    for (size_t index = 0; index < list.size(); ++index) {
        try {
            steps.push_back(ParseInputStep(list[index]));
        } catch (const std::exception& error) {
            throw std::runtime_error{fmt::format("step {}: {}", index, error.what())};
        }
    }
    return RunInput(state, steps, params, client_gone);
}

/// Guest threads' CPU time and waits (EDEN_NX2_GUEST_THREAD_STATS): per thread, the time on a
/// core split into JIT work (game code, counter polling, hint spins, GPU-cached memory) and
/// kernel work, the time ready without a core, and the waits by kind. Totals since the thread
/// first ran, busiest first.
json ThreadStats(ToolState&, const json& params) {
    const size_t top = Param<size_t>(params, "top", 16, 1, 128);
    const std::vector<Kernel::Nx2ThreadStats::ThreadSnapshot> rows =
        Kernel::Nx2ThreadStats::Snapshot();
    const u64 freq = Common::g_wall_clock.GetGuestCNTFRQ();
    const auto ms = [freq](u64 ticks) {
        return freq != 0 ? static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq) : 0.0;
    };
    static constexpr std::array<const char*, Kernel::Nx2ThreadStats::Waits> wait_names{
        "ipc", "sync_objects", "address_arbiter", "condvar", "lock", "sleep", "other"};
    json threads = json::array();
    for (size_t i = 0; i < rows.size() && i < top; ++i) {
        const Kernel::Nx2ThreadStats::ThreadSnapshot& row = rows[i];
        const u64 accounted = row.poll_ticks + row.hint_spin_ticks + row.memory_ticks;
        json waits = json::object();
        for (size_t kind = 0; kind < Kernel::Nx2ThreadStats::Waits; ++kind) {
            waits[wait_names[kind]] = {{"ms", ms(row.wait_ticks[kind])},
                                       {"count", row.wait_counts[kind]}};
        }
        threads.push_back({
            {"thread_id", row.thread_id},
            {"process", row.process},
            {"priority", row.priority},
            {"affinity_mask", Hex(row.affinity)},
            {"on_core_ms", ms(row.on_core_ticks)},
            {"jit_ms", ms(row.jit_ticks)},
            {"game_code_ms", ms(row.jit_ticks > accounted ? row.jit_ticks - accounted : 0)},
            {"counter_poll_ms", ms(row.poll_ticks)},
            {"hint_spin_ms", ms(row.hint_spin_ticks)},
            {"cached_memory_ms", ms(row.memory_ticks)},
            {"read_back_ms", ms(row.read_back_ticks)},
            {"outside_jit_ms", ms(row.on_core_ticks > row.jit_ticks ? row.on_core_ticks - row.jit_ticks : 0)},
            {"ready_ms", ms(row.ready_ticks)},
            {"held_replies_ms", static_cast<double>(row.held_ns) / 1e6},
            {"presents", row.presents},
            {"waits", std::move(waits)},
        });
    }
    json result = {{"enabled", Kernel::Nx2ThreadStats::g_enabled},
                   {"cntfrq_hz", freq},
                   {"threads_total", rows.size()},
                   {"threads", std::move(threads)}};
    if (!Kernel::Nx2ThreadStats::g_enabled) {
        result["note"] = "EDEN_NX2_GUEST_THREAD_STATS is not set: the counters only fill with it";
    }
    return result;
}

/// The present thread's counters (the FLUID presenter's fluid_stats, EDEN_NX2_SHOWN_STATS),
/// the vblank clock it keeps, and the present queue's depth.
json PresentStatsTool(ToolState& state, const json&) {
    if (!state.system.IsPoweredOn()) {
        throw std::runtime_error{"the renderer is not running"};
    }
    const auto stats = state.system.GPU().Renderer().GetPresentStats();
    if (!stats) {
        throw std::runtime_error{"present stats need the Vulkan renderer"};
    }
    const auto mean = [](double sum, u64 samples) {
        return samples != 0 ? sum / static_cast<double>(samples) : 0.0;
    };
    json result = {
        {"fluid", stats->fluid},
        {"fallback", stats->fallback},
        {"shown_stats", stats->shown_stats},
        {"swapchain_images", stats->swapchain_images},
        {"present_frames", stats->frame_count},
        {"queued_frames", stats->queued_frames},
        {"free_frames", stats->free_frames},
        {"vblank",
         {{"refresh_hz", stats->refresh_hz},
          {"index", stats->vblank_index},
          {"divisor", stats->divisor},
          {"lead_ms", stats->lead_ms},
          {"latency_ms", stats->clock_latency_ms}}},
        {"slots", stats->slot_count},
        {"hits", stats->hits},
        {"missed", stats->no_frame + stats->queued + stats->overslept},
        {"no_frame", stats->no_frame},
        {"not_done", stats->not_done},
        {"uncovered", stats->uncovered},
        {"rolled", stats->rolled},
        {"spare", stats->spare},
        {"stale", stats->stale},
        {"queued", stats->queued},
        {"overslept", stats->overslept},
        {"in_order", stats->in_order},
        {"late", stats->late},
        {"dropped", stats->dropped},
        {"received", stats->received},
        {"held",
         {{"1", stats->held[0]},
          {"2", stats->held[1]},
          {"3", stats->held[2]},
          {"4+", stats->held[3]}}},
        {"age_ms", mean(stats->age_ms, stats->hits)},
        {"latency_ms", mean(stats->latency_ms, stats->latency_samples)},
        {"latency_samples", stats->latency_samples},
        {"present_thread",
         {{"presents", stats->presents},
          {"acquire_ms", mean(stats->acquire_ms, stats->presents)},
          {"acquire_max_ms", stats->acquire_max_ms},
          {"copy_wait_ms", mean(stats->copy_wait_ms, stats->copy_waits)},
          {"copy_wait_max_ms", stats->copy_wait_max_ms},
          {"copy_waits", stats->copy_waits},
          {"copy_wait_timeouts", stats->copy_wait_timeouts},
          {"lock_ms", mean(stats->lock_ms, stats->presents)},
          {"lock_max_ms", stats->lock_max_ms},
          {"call_ms", mean(stats->call_ms, stats->presents)},
          {"call_max_ms", stats->call_max_ms},
          {"margin_ms", mean(stats->margin_ms, stats->margin_samples)},
          {"margin_min_ms", stats->margin_min_ms},
          {"after_vblank", stats->after_vblank},
          {"copy_gpu_ms", mean(stats->copy_gpu_ms, stats->copy_gpu_samples)},
          {"copy_gpu_max_ms", stats->copy_gpu_max_ms},
          {"copy_gpu_samples", stats->copy_gpu_samples}}},
    };
    if (!stats->shown_stats) {
        result["note"] = "EDEN_NX2_SHOWN_STATS is not set: the slot counters stay zero; the "
                         "vblank clock and queue depths are live regardless";
    }
    return result;
}

/// Texture and buffer cache hit/miss/eviction counters since boot, with the memory the caches
/// last published (Common::g_memory_counters).
json CacheStats(ToolState&, const json&) {
    const VideoCommon::Nx2CacheStats::Counters& counters = VideoCommon::Nx2CacheStats::g_counters;
    const Common::MemoryCounters& memory = Common::g_memory_counters;
    const auto get = [](const std::atomic<u64>& value) {
        return value.load(std::memory_order_relaxed);
    };
    const u64 texture_hits = get(counters.texture_hits);
    const u64 texture_misses = get(counters.texture_misses);
    const u64 buffer_hits = get(counters.buffer_hits);
    const u64 buffer_misses = get(counters.buffer_misses);
    const auto hit_rate = [](u64 hits, u64 misses) {
        const u64 total = hits + misses;
        return total != 0 ? static_cast<double>(hits) / static_cast<double>(total) : 0.0;
    };
    return {
        {"texture",
         {{"hits", texture_hits},
          {"misses", texture_misses},
          {"hit_rate", hit_rate(texture_hits, texture_misses)},
          {"created", get(counters.texture_created)},
          {"deleted", get(counters.texture_deleted)},
          {"evicted", get(counters.texture_evicted)},
          {"evicted_downloaded", get(counters.texture_evicted_downloaded)},
          {"device_usage", get(memory.device_usage)},
          {"expected_threshold", get(memory.texture_expected)},
          {"critical_threshold", get(memory.texture_critical)}}},
        {"buffer",
         {{"hits", buffer_hits},
          {"misses", buffer_misses},
          {"hit_rate", hit_rate(buffer_hits, buffer_misses)},
          {"created", get(counters.buffer_created)},
          {"deleted", get(counters.buffer_deleted)},
          {"evicted", get(counters.buffer_evicted)},
          {"bytes", get(memory.buffer_cache_bytes)}}},
        {"staging_bytes", get(memory.staging_bytes)},
        {"graphics_pipelines", get(memory.graphics_pipelines)},
        {"built_pipelines", get(memory.built_pipelines)},
    };
}

/// Guest CPU readback / GPU-flush counters since boot (VideoCore::AsyncReadback::g_counters),
/// the readback-sync bottleneck telemetry: reads, flush time, reader/GPU-side waits, DMA reads,
/// early downloads. Cumulative; EDEN_NX2_GPU_PROFILE also logs a per-second line.
json Readbacks(ToolState&, const json&) {
    const VideoCore::AsyncReadback::Counters& c = VideoCore::AsyncReadback::g_counters;
    const auto get = [](const std::atomic<u64>& value) {
        return value.load(std::memory_order_relaxed);
    };
    const auto us = [&get](const std::atomic<u64>& ns) { return get(ns) / 1000.0; };
    using Read = VideoCore::AsyncReadback::Read;
    return {
        {"reads_async", get(c.reads[static_cast<size_t>(Read::Async)])},
        {"reads_inflight", get(c.reads[static_cast<size_t>(Read::InFlight)])},
        {"read_us", us(c.read_ns[static_cast<size_t>(Read::Async)])},
        {"flush_us", us(c.flush_ns)},
        {"reader_waits", get(c.reader_waits)},
        {"reader_wait_us", us(c.reader_wait_ns)},
        {"reader_max_wait_us", us(c.reader_max_wait_ns)},
        {"gpu_side_waits", get(c.gpu_side_waits)},
        {"gpu_side_wait_us", us(c.gpu_side_wait_ns)},
        {"dma_reads", get(c.dma_reads)},
        {"dma_waited", get(c.dma_waited)},
        {"dma_drained", get(c.dma_drained)},
        {"dma_wait_us", us(c.dma_wait_ns)},
        {"clear_landings", get(c.clear_landings)},
        {"clear_waits", get(c.clear_waits)},
        {"clear_wait_us", us(c.clear_wait_ns)},
        {"sync_ops", get(c.sync_ops)},
        {"sync_us", us(c.sync_ns)},
        {"sync_max_us", us(c.sync_max_ns)},
        {"early_recorded", get(c.early_recorded)},
        {"early_bytes", get(c.early_bytes)},
        {"early_flushes", get(c.early_flushes)},
        {"early_refused", get(c.early_refused)},
        {"note", "cumulative since boot; EDEN_NX2_GPU_PROFILE logs a per-second line"},
    };
}

/// The boot phases Common::BootTiming recorded (Mark and Span), oldest first: when each
/// happened since the process started and how long it took.
json BootTimingTool(ToolState&, const json&) {
    json phases = json::array();
    for (const Common::BootTiming::Phase& phase : Common::BootTiming::RecordedPhases()) {
        phases.push_back(
            {{"phase", phase.name}, {"at_ms", phase.at_ms}, {"took_ms", phase.took_ms}});
    }
    return {{"now_ms", Common::BootTiming::Now()}, {"phases", std::move(phases)}};
}

Handlers MakeHandlers(const std::shared_ptr<ToolState>& state) {
    const auto handler = [&state](json (*tool)(ToolState&, const json&)) -> Handler {
        return [state, tool](const json& params, const ClientGone&) { return tool(*state, params); };
    };
    const auto waiting = [&state](json (*tool)(ToolState&, const json&, const ClientGone&)) -> Handler {
        return [state, tool](const json& params, const ClientGone& client_gone) {
            return tool(*state, params, client_gone);
        };
    };
    return {
        {"status", handler(Status)},
        {"frame_times", handler(FrameTimes)},
        {"screenshot", handler(Screenshot)},
        {"shown_frames", handler(ShownFrames)},
        {"log_tail", handler(LogTail)},
        {"log_filter", handler(LogFilter)},
        {"capture_frame", handler(CaptureFrame)},
        {"capture_bundle", handler(CaptureBundle)},
        {"reject_shaders", handler(RejectShaders)},
        {"set_shading_rate", handler(SetShadingRate)},
        {"set_prepass", handler(SetPrepass)},
        {"sample", handler(SampleThreads)},
        {"trace_record", handler(TraceRecord)},
        {"trace_stop", handler(TraceStop)},
        {"trace_status", handler(TraceStatus)},
        {"speed_limit", handler(SpeedLimit)},
        {"host_timing", handler(HostTiming)},
        {"power", handler(Power)},
        {"power_set", handler(PowerSet)},
        {"gpu_costs", handler(GpuCosts)},
        {"gpu_perf", handler(GpuPerf)},
        {"frame_budget", handler(FrameBudgetTool)},
        {"autopilot", handler(AutopilotTool)},
        {"memory", handler(MemoryReport)},
        {"read_memory", handler(ReadMemory)},
        {"search_memory", handler(SearchMemory)},
        {"read_gpu_memory", handler(ReadGpuMemory)},
        {"input", waiting(Input)},
        {"input_sequence", waiting(InputSequence)},
        {"thread_stats", handler(ThreadStats)},
        {"present_stats", handler(PresentStatsTool)},
        {"cache_stats", handler(CacheStats)},
        {"readbacks", handler(Readbacks)},
        {"boot_timing", handler(BootTimingTool)},
    };
}

} // Anonymous namespace

u64 ParseShaderHash(std::string_view text) {
    const std::string_view digits =
        text.starts_with("0x") || text.starts_with("0X") ? text.substr(2) : text;
    u64 value = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
    if (digits.size() != 16 || error != std::errc{} || end != digits.data() + digits.size()) {
        throw std::runtime_error{fmt::format(
            "bad hash \"{}\": want all 16 hex digits of a native program key (shader dump file "
            "name) or shader hash (frame dump \"shaders\"); a shorter one would match nothing",
            text)};
    }
    return value;
}

std::vector<u8> ParseHexBytes(std::string_view text) {
    std::string digits;
    const std::string_view body =
        text.starts_with("0x") || text.starts_with("0X") ? text.substr(2) : text;
    for (const char c : body) {
        if (c != ' ') {
            digits.push_back(c);
        }
    }
    std::vector<u8> bytes(digits.size() / 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        const auto [end, error] =
            std::from_chars(digits.data() + i * 2, digits.data() + i * 2 + 2, bytes[i], 16);
        if (error != std::errc{} || end != digits.data() + i * 2 + 2) {
            bytes.clear();
            break;
        }
    }
    if (bytes.empty() || digits.size() % 2 != 0) {
        throw std::runtime_error{fmt::format(
            "bad pattern \"{}\": want hex byte pairs in memory order (little endian: 1.0f is "
            "0000803f)",
            text)};
    }
    return bytes;
}

json FormatMemory(std::span<const u8> bytes, std::string_view as) {
    if (as == "hex") {
        std::string hex;
        hex.reserve(bytes.size() * 2);
        for (const u8 byte : bytes) {
            fmt::format_to(std::back_inserter(hex), "{:02x}", byte);
        }
        return {{"hex", std::move(hex)}};
    }
    const size_t element = as == "u32" || as == "f32" ? 4 : as == "u64" ? 8 : 0;
    if (element == 0) {
        throw std::runtime_error{fmt::format("as \"{}\": want hex, u32, u64 or f32", as)};
    }
    if (bytes.size() % element != 0) {
        throw std::runtime_error{fmt::format("size must be a multiple of {} for {}", element, as)};
    }
    json values = json::array();
    for (size_t offset = 0; offset < bytes.size(); offset += element) {
        if (element == 8) {
            u64 value;
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            values.push_back(HexAddress(value));
            continue;
        }
        u32 value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        if (as == "u32") {
            values.push_back(value);
            continue;
        }
        const float number = std::bit_cast<float>(value);
        if (std::isnan(number)) {
            values.push_back("nan");
        } else if (std::isinf(number)) {
            values.push_back(number < 0 ? "-inf" : "inf");
        } else {
            values.push_back(number);
        }
    }
    return {{std::string{as}, std::move(values)}};
}

std::optional<u64> ReadGuestMemory(Memory::Memory& memory, u64 address, std::span<u8> out) {
    for (size_t offset = 0; offset < out.size();) {
        const u64 current = address + offset;
        const size_t length = (std::min)(static_cast<size_t>(Memory::LICHII_PAGESIZE -
                                                             (current & Memory::LICHII_PAGEMASK)),
                                         out.size() - offset);
        const u8* const source = GuestByte(memory, current);
        if (source == nullptr) {
            return current;
        }
        std::memcpy(out.data() + offset, source, length);
        offset += length;
    }
    return std::nullopt;
}

GuestSearch SearchGuestMemory(Memory::Memory& memory, std::span<const u8> pattern, u64 start,
                              u64 end, size_t max_hits,
                              std::chrono::steady_clock::time_point deadline) {
    GuestSearch search{.hits = {}, .next = end, .complete = true, .scanned = 0};
    // The bytes of a run of mapped pages that a match starting there could still need.
    std::vector<u8> window;
    window.reserve(Memory::LICHII_PAGESIZE + pattern.size());
    for (u64 address = start; address < end;) {
        if (std::chrono::steady_clock::now() >= deadline) {
            search.next = address - window.size();
            search.complete = false;
            return search;
        }
        const u64 page_end = (std::min)((address | Memory::LICHII_PAGEMASK) + 1, end);
        const u8* const source = GuestByte(memory, address);
        if (source == nullptr) {
            window.clear();
            address = page_end;
            continue;
        }
        const u64 window_start = address - window.size();
        const size_t length = page_end - address;
        window.insert(window.end(), source, source + length);
        search.scanned += length;
        for (size_t offset = 0; offset + pattern.size() <= window.size(); ++offset) {
            const void* const found = std::memchr(window.data() + offset, pattern[0],
                                                  window.size() - pattern.size() + 1 - offset);
            if (found == nullptr) {
                break;
            }
            offset = static_cast<size_t>(static_cast<const u8*>(found) - window.data());
            if (std::memcmp(window.data() + offset, pattern.data(), pattern.size()) != 0) {
                continue;
            }
            search.hits.push_back(window_start + offset);
            if (search.hits.size() >= max_hits) {
                search.next = window_start + offset + 1;
                search.complete = false;
                return search;
            }
        }
        window.erase(window.begin(),
                     window.end() - static_cast<std::ptrdiff_t>(
                                        (std::min)(pattern.size() - 1, window.size())));
        address = page_end;
    }
    return search;
}

InputStep ParseInputStep(const json& step) {
    if (!step.is_object()) {
        throw std::runtime_error{"a step is an object"};
    }
    for (const auto& [key, value] : step.items()) {
        if (key != "buttons" && key != "left_stick" && key != "right_stick" && key != "frames") {
            throw std::runtime_error{fmt::format(
                "unknown key \"{}\": want buttons, left_stick, right_stick, frames", key)};
        }
    }
    InputStep result{.input = {}, .frames = 0};
    const json& frames = step.at("frames");
    if (!frames.is_number_integer() || frames.get<s64>() < 1 ||
        frames.get<u64>() > MAX_INPUT_FRAMES) {
        throw std::runtime_error{fmt::format("frames must be 1 to {}", MAX_INPUT_FRAMES)};
    }
    result.frames = frames.get<u64>();
    for (const json& button : step.value("buttons", json::array())) {
        const std::string text = button.is_string() ? button.get<std::string>() : button.dump();
        const auto it = std::ranges::find(INPUT_BUTTONS, std::string_view{text},
                                          &std::pair<std::string_view, HID::NpadButton>::first);
        if (it == INPUT_BUTTONS.end()) {
            std::string names;
            for (const auto& [known, _] : INPUT_BUTTONS) {
                names += names.empty() ? "" : ", ";
                names += known;
            }
            throw std::runtime_error{fmt::format("unknown button {}: want one of {}", text, names)};
        }
        result.input.buttons |= it->second;
    }
    const auto stick = [&step](const char* key) -> std::optional<HID::AnalogStickState> {
        if (!step.contains(key)) {
            return std::nullopt;
        }
        const json& value = step.at(key);
        const auto axis = [&](size_t index) {
            if (!value.is_array() || value.size() != 2 || !value[index].is_number() ||
                std::abs(value[index].get<double>()) > 1.0) {
                throw std::runtime_error{fmt::format("{} must be [x, y] with both in -1..1", key)};
            }
            return static_cast<s32>(std::lround(value[index].get<double>() * HID::HID_JOYSTICK_MAX));
        };
        return HID::AnalogStickState{.x = axis(0), .y = axis(1)};
    };
    result.input.left = stick("left_stick");
    result.input.right = stick("right_stick");
    return result;
}

std::optional<HID::InjectedInput> InputAt(std::span<const InputStep> steps, u64 start, u64 now) {
    if (now < start) {
        return std::nullopt;
    }
    u64 end = start;
    for (const InputStep& step : steps) {
        end += step.frames;
        if (now < end) {
            return step.input;
        }
    }
    return std::nullopt;
}

Service::Service(System& system, std::string title, u64 program_id, const Endpoint& endpoint,
                 std::string dump_directory)
    : state{std::make_shared<ToolState>(system, std::move(title), program_id,
                                        std::move(dump_directory))} {
    Common::Log::SetRingCapacity(RING_LINES);
    server = std::make_unique<Server>(endpoint, MakeHandlers(state));
    LOG_INFO(Debug, "Debug server listening on {}:{}, dump dir {}", endpoint.host, server->Port(),
             state->dump_directory.generic_string());
}

Service::~Service() {
    state->stopping.store(true, std::memory_order_relaxed);
    server.reset();
}

std::unique_ptr<Service> MaybeStartService(System& system, std::string title, u64 program_id) {
    const char* const text = std::getenv("EDEN_NX2_DEBUG_SERVER");
    if (text == nullptr || *text == '\0') {
        return nullptr;
    }
    std::string error;
    const auto endpoint = ParseEndpoint(text, error);
    if (!endpoint) {
        LOG_ERROR(Debug, "EDEN_NX2_DEBUG_SERVER={}: {}; the debug server is off", text, error);
        return nullptr;
    }
    const char* const directory = std::getenv("EDEN_NX2_DEBUG_DIR");
    std::string dump_directory = directory != nullptr && *directory != '\0'
        ? std::string{directory}
        : (Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "debug-server").generic_string();
    try {
        return std::make_unique<Service>(system, std::move(title), program_id, *endpoint,
                                         std::move(dump_directory));
    } catch (const std::exception& failure) {
        LOG_ERROR(Debug, "Debug server cannot listen on {}:{}: {}", endpoint->host, endpoint->port,
                  failure.what());
        return nullptr;
    }
}

} // namespace Core::Nx2Debug
