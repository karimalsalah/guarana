// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>

namespace Vulkan::PresentSync {

class FrameSubmission {
public:
    void Submitted() {
        submitted = true;
    }

    [[nodiscard]] bool CanRetry() const {
        return !submitted;
    }

private:
    bool submitted{};
};

// Match recreation: exclude the presenter (including its dedicated queue) before submissions.
template <typename Mutex>
class IdleLocks {
public:
    IdleLocks(Mutex& swapchain_mutex, Mutex& submit_mutex)
        : swapchain_lock{swapchain_mutex}, submit_lock{submit_mutex} {}

private:
    std::unique_lock<Mutex> swapchain_lock;
    std::unique_lock<Mutex> submit_lock;
};

} // namespace Vulkan::PresentSync
