// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/shutdown_progress.h"

#include <algorithm>
#include <chrono>
#include <mutex>

#include "common/logging.h"

namespace Common {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t MaxRecordedSteps = 256;

std::atomic<const char*> last_step{nullptr};

struct Timeline {
    std::mutex mutex;
    bool active{};
    Clock::time_point begun{};
    Clock::time_point last_done{};
    size_t steps{};
    std::vector<ShutdownStepRecord> records;
};

/// Never destroyed: a renderer torn down at exit still ends its steps here.
Timeline& GetTimeline() {
    static Timeline* const timeline = new Timeline;
    return *timeline;
}

double Ms(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

void StepDone(const char* step, std::optional<size_t> count) {
    NoteShutdownProgress();
    Timeline& timeline = GetTimeline();
    std::scoped_lock lock{timeline.mutex};
    if (!timeline.active) {
        return;
    }
    const Clock::time_point now = Clock::now();
    const double took = Ms(now - timeline.last_done);
    timeline.last_done = now;
    ++timeline.steps;
    const u64 progress = ShutdownProgress();
    if (timeline.records.size() < MaxRecordedSteps) {
        timeline.records.push_back({step, took, count, progress});
    }
    last_step.store(step, std::memory_order_relaxed);
    if (count) {
        LOG_INFO(Core, "Shutdown step {} took {:.1f} ms: {}, {} (progress {})", timeline.steps,
                 took, step, *count, progress);
    } else {
        LOG_INFO(Core, "Shutdown step {} took {:.1f} ms: {} (progress {})", timeline.steps, took,
                 step, progress);
    }
}

} // Anonymous namespace

void BeginShutdownSteps() {
    Timeline& timeline = GetTimeline();
    std::scoped_lock lock{timeline.mutex};
    timeline.active = true;
    timeline.begun = Clock::now();
    timeline.last_done = timeline.begun;
    timeline.steps = 0;
    timeline.records.clear();
    timeline.records.reserve(MaxRecordedSteps);
    last_step.store("begun", std::memory_order_relaxed);
    LOG_INFO(Core, "Shutdown begins (progress {})", ShutdownProgress());
}

void ShutdownStepDone(const char* step) {
    StepDone(step, std::nullopt);
}

void ShutdownStepDone(const char* step, size_t count) {
    StepDone(step, count);
}

void EndShutdownSteps() {
    Timeline& timeline = GetTimeline();
    std::scoped_lock lock{timeline.mutex};
    if (!timeline.active) {
        return;
    }
    timeline.active = false;
    last_step.store("done", std::memory_order_relaxed);
    const double total = Ms(Clock::now() - timeline.begun);
    const auto slowest = std::max_element(
        timeline.records.begin(), timeline.records.end(),
        [](const ShutdownStepRecord& a, const ShutdownStepRecord& b) { return a.ms < b.ms; });
    if (slowest == timeline.records.end()) {
        LOG_INFO(Core, "Shutdown took {:.1f} ms", total);
        return;
    }
    LOG_INFO(Core, "Shutdown took {:.1f} ms in {} steps; the slowest took {:.1f} ms: {}", total,
             timeline.steps, slowest->ms, slowest->step);
}

void ClearLastShutdownStep() noexcept {
    last_step.store(nullptr, std::memory_order_relaxed);
}

const char* LastShutdownStep() noexcept {
    return last_step.load(std::memory_order_relaxed);
}

std::vector<ShutdownStepRecord> RecordedShutdownSteps() {
    Timeline& timeline = GetTimeline();
    std::scoped_lock lock{timeline.mutex};
    return timeline.records;
}

void ShutdownWatch::Reset(u64 progress, std::optional<u64> cpu_ns) {
    last_progress = progress;
    last_cpu_ns = cpu_ns;
    busy_checks = 0;
}

bool ShutdownWatch::Moved(u64 progress, std::optional<u64> cpu_ns) {
    const bool progressed = progress != last_progress;
    const bool busy = cpu_ns && last_cpu_ns && *cpu_ns >= *last_cpu_ns + busy_cpu_ns;
    last_progress = progress;
    last_cpu_ns = cpu_ns;
    if (progressed) {
        busy_checks = 0;
        return true;
    }
    if (busy && busy_checks < max_busy_checks) {
        ++busy_checks;
        return true;
    }
    return false;
}

} // namespace Common
