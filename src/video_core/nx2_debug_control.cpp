// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <unordered_map>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/nx2_debug_control.h"

namespace VideoCore::Nx2Debug {
namespace {

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<CaptureRequest> captures;
    std::unordered_map<u64, CaptureResult> finished;
    std::deque<GpuReadRequest> gpu_reads;
    std::unordered_map<u64, GpuReadResult> gpu_read_results;
    std::vector<u64> rejected;
    u64 reject_generation{};
    u64 applied_generation{};
    bool reject_pending{};
    bool unrejected{};
    bool open{};
};

State state;
std::atomic<bool> pending{};
std::atomic<bool> any_rejected{};

struct ShadingRateState {
    std::mutex mutex;
    ShadingRateTable table;
    bool env_read{};
};

ShadingRateState shading;
/// Starts above the zero a thread's empty copy has, so the first lookup reads the table.
std::atomic<u64> shading_generation{1};

/// Caller holds shading.mutex.
void SetShadingRateLocked(u64 hash, ShadingRate rate) {
    auto& rates = shading.table.rates;
    const auto it = std::ranges::lower_bound(rates, hash, {}, &std::pair<u64, ShadingRate>::first);
    const bool found = it != rates.end() && it->first == hash;
    if (rate == ShadingRate{}) {
        if (found) {
            rates.erase(it);
        }
    } else if (found) {
        it->second = rate;
    } else {
        rates.emplace(it, hash, rate);
    }
}

/// Caller holds shading.mutex.
void LogShadingRatesLocked(std::string_view source) {
    std::string list;
    for (const auto& [hash, rate] : shading.table.rates) {
        list += fmt::format(" {:016x}:{}x{}", hash, rate.width, rate.height);
    }
    LOG_INFO(Render_Vulkan, "Shading rates ({}): default {}x{}, {} shaders:{}", source,
             shading.table.fallback.width, shading.table.fallback.height,
             shading.table.rates.size(), list);
}

/// Caller holds shading.mutex.
void ReadShadingRateEnvLocked() {
    if (shading.env_read) {
        return;
    }
    shading.env_read = true;
    bool any = false;
    if (const char* value = std::getenv("EDEN_NX2_VRS_DEFAULT")) {
        if (const auto rate = ParseShadingRate(value)) {
            shading.table.fallback = *rate;
            any = true;
        } else {
            LOG_WARNING(Render_Vulkan, "EDEN_NX2_VRS_DEFAULT: bad rate \"{}\"", value);
        }
    }
    if (const char* value = std::getenv("EDEN_NX2_VRS_HASHES")) {
        std::string_view rest{value};
        while (!rest.empty()) {
            const size_t comma = rest.find(',');
            const std::string_view item = rest.substr(0, comma);
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            if (item.empty()) {
                continue;
            }
            const size_t colon = item.find(':');
            const std::string_view digits = item.substr(0, colon);
            u64 hash = 0;
            const auto [end, error] =
                std::from_chars(digits.data(), digits.data() + digits.size(), hash, 16);
            const auto rate = colon == std::string_view::npos
                                  ? std::optional<ShadingRate>{}
                                  : ParseShadingRate(item.substr(colon + 1));
            if (digits.size() != 16 || error != std::errc{} ||
                end != digits.data() + digits.size() || !rate) {
                LOG_WARNING(Render_Vulkan,
                            "EDEN_NX2_VRS_HASHES: bad entry \"{}\" (want <16 hex>:<rate>)", item);
                continue;
            }
            SetShadingRateLocked(hash, *rate);
            any = true;
        }
    }
    if (any) {
        LogShadingRatesLocked("environment");
    }
}

struct PrepassState {
    std::mutex mutex;
    PrepassTable table;
    bool env_read{};
};

PrepassState prepass;
/// Starts above the zero a thread's empty copy has, so the first lookup reads the table.
std::atomic<u64> prepass_generation{1};
std::atomic<u64> prepass_pipelines{};
std::atomic<u64> prepass_split_draws{};
std::atomic<u64> prepass_plain_draws{};

/// Caller holds prepass.mutex.
void LogPrepassHashesLocked(std::string_view source) {
    std::string list;
    for (const u64 hash : prepass.table.hashes) {
        list += fmt::format(" {:016x}", hash);
    }
    LOG_INFO(Render_Vulkan, "Prepass split ({}): {} fragment shaders:{}", source,
             prepass.table.hashes.size(), list);
}

/// Caller holds prepass.mutex.
void ReadPrepassEnvLocked() {
    if (prepass.env_read) {
        return;
    }
    prepass.env_read = true;
    const char* const value = std::getenv("EDEN_NX2_PREPASS_HASHES");
    if (value == nullptr) {
        return;
    }
    auto& hashes = prepass.table.hashes;
    std::string_view rest{value};
    while (!rest.empty()) {
        const size_t comma = rest.find(',');
        const std::string_view item = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        if (item.empty()) {
            continue;
        }
        u64 hash = 0;
        const auto [end, error] =
            std::from_chars(item.data(), item.data() + item.size(), hash, 16);
        if (item.size() != 16 || error != std::errc{} || end != item.data() + item.size()) {
            LOG_WARNING(Render_Vulkan, "EDEN_NX2_PREPASS_HASHES: bad entry \"{}\" (want 16 hex)",
                        item);
            continue;
        }
        const auto it = std::ranges::lower_bound(hashes, hash);
        if (it == hashes.end() || *it != hash) {
            hashes.insert(it, hash);
        }
    }
    if (!hashes.empty()) {
        LogPrepassHashesLocked("environment");
    }
}

void UpdatePending() {
    pending.store(!state.captures.empty() || !state.gpu_reads.empty() || state.reject_pending, std::memory_order_relaxed);
}

} // Anonymous namespace

void BeginSession() {
    std::scoped_lock lock{state.mutex};
    state.captures.clear();
    state.finished.clear();
    state.gpu_reads.clear();
    state.gpu_read_results.clear();
    state.rejected.clear();
    state.reject_pending = false;
    state.unrejected = false;
    state.applied_generation = state.reject_generation;
    state.open = true;
    any_rejected.store(false, std::memory_order_relaxed);
    UpdatePending();
}

void EndSession() {
    {
        std::scoped_lock lock{state.mutex};
        state.open = false;
        state.captures.clear();
        state.gpu_reads.clear();
        UpdatePending();
    }
    state.changed.notify_all();
}

bool HasRequests() noexcept {
    return pending.load(std::memory_order_relaxed);
}

void RequestCapture(CaptureRequest request) {
    std::scoped_lock lock{state.mutex};
    state.captures.push_back(std::move(request));
    UpdatePending();
}

void CancelCapture(u64 id) {
    std::scoped_lock lock{state.mutex};
    std::erase_if(state.captures, [id](const CaptureRequest& request) { return request.id == id; });
    state.finished.erase(id);
    UpdatePending();
}

std::optional<CaptureResult> WaitCapture(u64 id, std::chrono::milliseconds timeout) {
    std::unique_lock lock{state.mutex};
    state.changed.wait_for(lock, timeout, [id] { return !state.open || state.finished.contains(id); });
    const auto it = state.finished.find(id);
    if (it == state.finished.end()) {
        return std::nullopt;
    }
    CaptureResult result = std::move(it->second);
    state.finished.erase(it);
    return result;
}

std::optional<CaptureRequest> TakeCapture() {
    std::scoped_lock lock{state.mutex};
    if (state.captures.empty()) {
        return std::nullopt;
    }
    CaptureRequest request = std::move(state.captures.front());
    state.captures.pop_front();
    UpdatePending();
    return request;
}

void FinishCapture(u64 id, CaptureResult result) {
    {
        std::scoped_lock lock{state.mutex};
        if (!state.open) {
            return;
        }
        state.finished.insert_or_assign(id, std::move(result));
    }
    state.changed.notify_all();
}

void RequestGpuRead(GpuReadRequest request) {
    std::scoped_lock lock{state.mutex};
    state.gpu_reads.push_back(request);
    UpdatePending();
}

void CancelGpuRead(u64 id) {
    std::scoped_lock lock{state.mutex};
    std::erase_if(state.gpu_reads, [id](const GpuReadRequest& request) { return request.id == id; });
    state.gpu_read_results.erase(id);
    UpdatePending();
}

std::optional<GpuReadResult> WaitGpuRead(u64 id, std::chrono::milliseconds timeout) {
    std::unique_lock lock{state.mutex};
    state.changed.wait_for(lock, timeout,
                           [id] { return !state.open || state.gpu_read_results.contains(id); });
    const auto it = state.gpu_read_results.find(id);
    if (it == state.gpu_read_results.end()) {
        return std::nullopt;
    }
    GpuReadResult result = std::move(it->second);
    state.gpu_read_results.erase(it);
    return result;
}

std::optional<GpuReadRequest> TakeGpuRead() {
    std::scoped_lock lock{state.mutex};
    if (state.gpu_reads.empty()) {
        return std::nullopt;
    }
    const GpuReadRequest request = state.gpu_reads.front();
    state.gpu_reads.pop_front();
    UpdatePending();
    return request;
}

void FinishGpuRead(u64 id, GpuReadResult result) {
    {
        std::scoped_lock lock{state.mutex};
        if (!state.open) {
            return;
        }
        state.gpu_read_results.insert_or_assign(id, std::move(result));
    }
    state.changed.notify_all();
}

u64 SetRejected(std::span<const u64> hashes, bool rejected) {
    std::scoped_lock lock{state.mutex};
    bool changed = false;
    for (const u64 hash : hashes) {
        const auto it = std::ranges::find(state.rejected, hash);
        if (rejected && it == state.rejected.end()) {
            state.rejected.push_back(hash);
            changed = true;
        } else if (!rejected && it != state.rejected.end()) {
            state.rejected.erase(it);
            state.unrejected = true;
            changed = true;
        }
    }
    any_rejected.store(!state.rejected.empty(), std::memory_order_relaxed);
    if (changed) {
        ++state.reject_generation;
        state.reject_pending = true;
        UpdatePending();
    }
    return state.reject_generation;
}

std::vector<u64> RejectedHashes() {
    std::scoped_lock lock{state.mutex};
    return state.rejected;
}

bool IsRejected(u64 hash) {
    if (!any_rejected.load(std::memory_order_relaxed)) {
        return false;
    }
    std::scoped_lock lock{state.mutex};
    return std::ranges::find(state.rejected, hash) != state.rejected.end();
}

bool AnyRejected() noexcept {
    return any_rejected.load(std::memory_order_relaxed);
}

std::optional<RejectChange> TakeRejectChange() {
    std::scoped_lock lock{state.mutex};
    if (!state.reject_pending) {
        return std::nullopt;
    }
    const RejectChange change{.generation = state.reject_generation,
                              .unrejected = state.unrejected};
    state.reject_pending = false;
    state.unrejected = false;
    UpdatePending();
    return change;
}

void NoteRejectApplied(u64 generation) {
    {
        std::scoped_lock lock{state.mutex};
        state.applied_generation = (std::max)(state.applied_generation, generation);
    }
    state.changed.notify_all();
}

bool WaitRejectApplied(u64 generation, std::chrono::milliseconds timeout) {
    std::unique_lock lock{state.mutex};
    return state.changed.wait_for(lock, timeout, [generation] {
        return !state.open || state.applied_generation >= generation;
    }) && state.applied_generation >= generation;
}

std::optional<ShadingRate> ParseShadingRate(std::string_view text) {
    if (text.size() != 3 || (text[1] != 'x' && text[1] != 'X')) {
        return std::nullopt;
    }
    const auto size = [](char c) -> u8 {
        return c == '1' || c == '2' || c == '4' ? static_cast<u8>(c - '0') : u8{0};
    };
    const ShadingRate rate{.width = size(text[0]), .height = size(text[2])};
    // No 4x1 or 1x4: VK_KHR_fragment_shading_rate has no such rate.
    const bool valid = rate.width != 0 && rate.height != 0 &&
                       (std::max)(rate.width, rate.height) <= 2 * (std::min)(rate.width, rate.height);
    return valid ? std::optional<ShadingRate>{rate} : std::nullopt;
}

void SetShadingRates(std::span<const u64> hashes, ShadingRate rate, bool all) {
    std::scoped_lock lock{shading.mutex};
    ReadShadingRateEnvLocked();
    const ShadingRateTable before = shading.table;
    if (all) {
        shading.table.fallback = rate;
    }
    for (const u64 hash : hashes) {
        SetShadingRateLocked(hash, rate);
    }
    if (before.fallback == shading.table.fallback && before.rates == shading.table.rates) {
        return;
    }
    LogShadingRatesLocked("debug server");
    shading_generation.fetch_add(1, std::memory_order_release);
}

void SetAutoShadingRates(std::vector<std::pair<u64, ShadingRate>> rates) {
    std::erase_if(rates, [](const auto& entry) { return entry.second == ShadingRate{}; });
    std::ranges::sort(rates, {}, &std::pair<u64, ShadingRate>::first);
    std::scoped_lock lock{shading.mutex};
    ReadShadingRateEnvLocked();
    if (shading.table.auto_rates == rates) {
        return;
    }
    shading.table.auto_rates = std::move(rates);
    shading_generation.fetch_add(1, std::memory_order_release);
}

ShadingRateTable ShadingRates() {
    std::scoped_lock lock{shading.mutex};
    ReadShadingRateEnvLocked();
    ShadingRateTable table = shading.table;
    // Read under the lock, which every change holds: the copy is the table of this generation.
    table.generation = shading_generation.load(std::memory_order_acquire);
    return table;
}

ShadingRate ShadingRateFor(u64 fragment_hash) {
    thread_local ShadingRateTable table;
    if (table.generation != shading_generation.load(std::memory_order_acquire)) {
        table = ShadingRates();
    }
    const auto it = std::ranges::lower_bound(table.rates, fragment_hash, {},
                                             &std::pair<u64, ShadingRate>::first);
    if (it != table.rates.end() && it->first == fragment_hash) {
        return it->second;
    }
    if (table.auto_rates.empty() || table.fallback != ShadingRate{}) {
        return table.fallback;
    }
    const auto automatic = std::ranges::lower_bound(table.auto_rates, fragment_hash, {},
                                                    &std::pair<u64, ShadingRate>::first);
    return automatic != table.auto_rates.end() && automatic->first == fragment_hash
               ? automatic->second
               : table.fallback;
}

void SetPrepassHashes(std::span<const u64> hashes, bool on) {
    std::scoped_lock lock{prepass.mutex};
    ReadPrepassEnvLocked();
    auto& listed = prepass.table.hashes;
    bool changed = false;
    for (const u64 hash : hashes) {
        const auto it = std::ranges::lower_bound(listed, hash);
        const bool found = it != listed.end() && *it == hash;
        if (on && !found) {
            listed.insert(it, hash);
            changed = true;
        } else if (!on && found) {
            listed.erase(it);
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    LogPrepassHashesLocked("debug server");
    prepass_generation.fetch_add(1, std::memory_order_release);
}

PrepassTable PrepassHashes() {
    std::scoped_lock lock{prepass.mutex};
    ReadPrepassEnvLocked();
    PrepassTable table = prepass.table;
    // Read under the lock, which every change holds: the copy is the table of this generation.
    table.generation = prepass_generation.load(std::memory_order_acquire);
    return table;
}

u64 PrepassGeneration() noexcept {
    return prepass_generation.load(std::memory_order_acquire);
}

bool IsPrepassHash(u64 fragment_hash) {
    thread_local PrepassTable table;
    if (table.generation != prepass_generation.load(std::memory_order_acquire)) {
        table = PrepassHashes();
    }
    return std::ranges::binary_search(table.hashes, fragment_hash);
}

bool PrepassForced() {
    static const bool forced = [] {
        const char* const value = std::getenv("EDEN_NX2_PREPASS_FORCE");
        return value != nullptr && value[0] == '1';
    }();
    return forced;
}

void CountPrepassPipeline() noexcept {
    prepass_pipelines.fetch_add(1, std::memory_order_relaxed);
}

void CountPrepassDraw(bool split) noexcept {
    (split ? prepass_split_draws : prepass_plain_draws).fetch_add(1, std::memory_order_relaxed);
}

PrepassCounts PrepassCountsSoFar() noexcept {
    return {
        .pipelines = prepass_pipelines.load(std::memory_order_relaxed),
        .split_draws = prepass_split_draws.load(std::memory_order_relaxed),
        .plain_draws = prepass_plain_draws.load(std::memory_order_relaxed),
    };
}

} // namespace VideoCore::Nx2Debug
