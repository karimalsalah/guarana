// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>

#include "common/thread.h"

namespace VideoCommon {

/// A thread whose priority another thread may change: a shader job worker, as seen by the draw
/// that waits for the job it runs.
class PriorityTarget {
public:
    virtual ~PriorityTarget() = default;

    /// @return True when the thread now has `priority`.
    virtual bool Set(Common::ThreadPriority priority) = 0;
};

/// EDEN_NX2_WAIT_BOOST: the priority of the thread that runs one stage of a shader job. The worker
/// brackets the stage with Attach and Detach; a draw that waits for the job calls Raise.
/// Whatever Raise changed, Detach changes back before it returns, and once it has returned Raise
/// does nothing, so the target may go away.
class StageBoost {
public:
    /// The worker, before it runs the stage. `target` must outlive the stage.
    void Attach(PriorityTarget& target, Common::ThreadPriority base_priority) {
        std::scoped_lock lock{mutex};
        attached = &target;
        base = base_priority;
        raised = false;
        refused = false;
    }

    /// Raises the thread to normal priority for what is left of the stage. Safe from any thread,
    /// at any time, and at most one attempt succeeds per stage.
    /// @return True when this call raised it.
    bool Raise() {
        std::scoped_lock lock{mutex};
        if (attached == nullptr || raised || refused) {
            return false;
        }
        raised = attached->Set(Common::ThreadPriority::Normal);
        refused = !raised;
        return raised;
    }

    /// The worker, once the stage ended: back to the priority it had.
    void Detach() {
        std::scoped_lock lock{mutex};
        if (attached != nullptr && raised) {
            attached->Set(base);
        }
        attached = nullptr;
        raised = false;
    }

private:
    std::mutex mutex;
    PriorityTarget* attached{};
    Common::ThreadPriority base{Common::ThreadPriority::Low};
    bool raised{};
    bool refused{};
};

} // namespace VideoCommon
