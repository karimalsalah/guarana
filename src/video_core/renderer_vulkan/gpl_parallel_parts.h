// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "video_core/shader_jobs.h"

namespace Vulkan {

// EDEN_NX2_GPL_PARALLEL_PARTS=1 (experiment, issue #136). A pipeline built from graphics pipeline
// libraries (EDEN_NX2_GPL) builds its pre-rasterization and fragment shader libraries one after the
// other, on one shader job driver worker. With the switch the fragment library is its own job of
// the shader job scheduler while the building thread builds pre-rasterization, so a second driver
// worker can take it. The pieces here need no device, for the tests.

/// Parses the switch like its neighbours: unset, empty, 0 and false (strtoul reads none of them as
/// a number above 0) mean off.
inline bool GplParallelPartsRequested(const char* value) {
    return value != nullptr && std::strtoul(value, nullptr, 10) != 0;
}

/// A piece of work that runs once, on whichever thread claims it first, and that the other side
/// can wait for. The work is borrowed: it is only called while the owner of the reference waits
/// (RunPartsInParallel does not return before the work has ended or has been claimed by nobody).
class SplitWork {
public:
    explicit SplitWork(const std::function<void()>& work_) : work{&work_} {}

    /// Runs the work here unless a thread claimed it already (then does nothing). What the work
    /// throws is kept for Exception(). @return True when it ran on this thread.
    bool RunHere() {
        if (claimed.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        runner = std::this_thread::get_id();
        try {
            (*work)();
        } catch (...) {
            exception = std::current_exception();
        }
        {
            std::scoped_lock lock{mutex};
            finished = true;
        }
        condition.notify_all();
        return true;
    }

    /// Waits until a thread that claimed the work has finished it.
    void WaitFinished() {
        std::unique_lock lock{mutex};
        condition.wait(lock, [this] { return finished; });
    }

    /// Valid once the work finished (RunHere returned true, or WaitFinished returned).
    [[nodiscard]] std::exception_ptr Exception() const {
        return exception;
    }
    [[nodiscard]] std::thread::id Runner() const {
        return runner;
    }

private:
    const std::function<void()>* work;
    std::atomic_bool claimed{};
    std::mutex mutex;
    std::condition_variable condition;
    bool finished{};
    std::thread::id runner;
    std::exception_ptr exception;
};

/// Runs `first` on the calling thread and `second` as a job of `jobs` at `priority`, and returns
/// once both have ended. Run-or-wait: when no worker has started `second` by the time `first` is
/// done (every driver worker is busy, or the scheduler was shut down and cancelled the job), the
/// calling thread runs it itself, so a saturated pool never waits on itself. The job carries no
/// memory estimate: the calling thread's own job already holds the pipeline's. An exception of
/// either part is thrown here after both have ended, `first`'s before `second`'s.
/// @return True when `second` ran on the calling thread.
inline bool RunPartsInParallel(VideoCommon::ShaderJobScheduler& jobs,
                               VideoCommon::ShaderJobPriority priority,
                               const std::function<void()>& first,
                               const std::function<void()>& second) {
    const auto split = std::make_shared<SplitWork>(second);
    const VideoCommon::ShaderJobHandle handle = jobs.Submit(VideoCommon::ShaderJob{
        .priority = priority,
        .driver = [split] { split->RunHere(); },
    });
    std::exception_ptr first_exception;
    try {
        first();
    } catch (...) {
        first_exception = std::current_exception();
    }
    // Promotes the job and runs it here when it is still queued, or waits for the worker running it.
    jobs.RunOrWait(handle);
    // A cancelled job never ran; a job a worker claimed has ended or is about to.
    if (!split->RunHere()) {
        split->WaitFinished();
    }
    // The job's stale queue entry, if any, finds the work claimed and does nothing, so the
    // borrowed work is never called after this function returns.
    const bool ran_here = split->Runner() == std::this_thread::get_id();
    if (first_exception) {
        std::rethrow_exception(first_exception);
    }
    if (const std::exception_ptr second_exception = split->Exception()) {
        std::rethrow_exception(second_exception);
    }
    return ran_here;
}

} // namespace Vulkan
