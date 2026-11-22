// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <vector>

#include "common/common_types.h"

namespace Common {

/// Counts the steps of an emulation teardown that can take long (pipeline caches written,
/// thousands of pipelines destroyed), so a frontend waiting for its emulation thread can tell a
/// slow teardown that still moves from one that hangs. Terminating the thread mid-teardown left a
/// lock it held (the heap's, the driver's) taken for good, and the whole app froze at 0% CPU.
inline std::atomic<u64> shutdown_progress{0};

inline void NoteShutdownProgress() noexcept {
    shutdown_progress.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline u64 ShutdownProgress() noexcept {
    return shutdown_progress.load(std::memory_order_relaxed);
}

/// The steps of a teardown, timed: BeginShutdownSteps starts the clock, and each ShutdownStepDone
/// ends the step that ran since the previous one ended: it notes progress and logs at Info
/// "Shutdown step N took X ms: <step> (progress P)". EndShutdownSteps logs the total and the
/// slowest step. Outside a teardown a step only notes progress. Steps run on the tearing-down
/// thread and are named by string literals: the frontend reads the last one from its own thread.
void BeginShutdownSteps();
void ShutdownStepDone(const char* step);
/// As ShutdownStepDone, with how many objects the step went through.
void ShutdownStepDone(const char* step, size_t count);
void EndShutdownSteps();

/// Forgets the last step done: a stop is requested, and a teardown that has not begun yet names
/// no step.
void ClearLastShutdownStep() noexcept;

/// The step the current or last teardown ended last ("begun" before its first step, "done" after
/// EndShutdownSteps), or null when none began since ClearLastShutdownStep.
[[nodiscard]] const char* LastShutdownStep() noexcept;

struct ShutdownStepRecord {
    const char* step;
    double ms;
    std::optional<size_t> count;
    u64 progress; ///< ShutdownProgress() once the step was done
};

/// The steps of the current or last teardown, in order (at most the first 256).
[[nodiscard]] std::vector<ShutdownStepRecord> RecordedShutdownSteps();

/// Ends a teardown step when it is destroyed. As a member, declared just before the members whose
/// destruction it times (members are destroyed in reverse order), it ends the step of theirs.
class ShutdownStepMarker {
public:
    explicit constexpr ShutdownStepMarker(const char* step_) noexcept : step{step_} {}
    ~ShutdownStepMarker() {
        ShutdownStepDone(step);
    }

    ShutdownStepMarker(const ShutdownStepMarker&) = delete;
    ShutdownStepMarker& operator=(const ShutdownStepMarker&) = delete;

private:
    const char* step;
};

/// Tells whether a teardown that a frontend waits for still moves: its progress count changed,
/// or the tearing-down thread used at least `busy_cpu_ns` of CPU time since the last check. A step
/// that notes no progress while it works (guest memory released, thousands of objects freed by a
/// driver) is busy on the CPU; a hung one sleeps on a lock or a wait. One that spins is busy too,
/// so being busy alone passes at most `max_busy_checks` checks in a row.
class ShutdownWatch {
public:
    static constexpr u64 DefaultBusyCpuNs = 1'000'000'000;
    static constexpr u32 DefaultMaxBusyChecks = 6;

    explicit ShutdownWatch(u64 busy_cpu_ns_ = DefaultBusyCpuNs,
                           u32 max_busy_checks_ = DefaultMaxBusyChecks)
        : busy_cpu_ns{busy_cpu_ns_}, max_busy_checks{max_busy_checks_} {}

    /// Starts watching from the current progress count and the thread's CPU time (nullopt where
    /// the host cannot read it).
    void Reset(u64 progress, std::optional<u64> cpu_ns);

    /// True when the teardown moved since Reset or the previous call, which starts the next check.
    [[nodiscard]] bool Moved(u64 progress, std::optional<u64> cpu_ns);

private:
    u64 busy_cpu_ns;
    u32 max_busy_checks;
    u32 busy_checks{}; ///< Checks in a row passed by a busy thread alone
    u64 last_progress{};
    std::optional<u64> last_cpu_ns;
};

} // namespace Common
