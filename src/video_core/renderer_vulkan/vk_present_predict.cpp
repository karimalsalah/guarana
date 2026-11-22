// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <thread>

#include "video_core/nx2_frame_predictor.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

// EDEN_NX2_PREDICT on the FLUID present thread (nx2_frame_predictor.h): the two hooks
// FluidPresentSlot calls. They are here, not in vk_present_manager.cpp, to keep that file's
// change to two lines.
namespace Vulkan {

namespace {

[[nodiscard]] s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // Anonymous namespace

void PresentManager::FluidPredictHold(u64 slot, u64 after) {
    using VideoCore::Nx2Predict::FramePredictor;
    FramePredictor& predictor = FramePredictor::Instance();
    if (!predictor.Holds() || fluid.showing != nullptr) {
        return;
    }
    // The real frame (or its predictions) for this slot whose GPU work is not done, when no
    // frame of a batch as new is: showing the older batch's prediction instead would put the
    // motion ahead of the real frame that follows it.
    const auto landing = [this, slot, after]() -> const Frame* {
        scheduler.GetMasterSemaphore().Refresh();
        u64 done_batch = 0;
        const Frame* waiting = nullptr;
        for (const Frame* const frame : fluid.pending) {
            if (frame->fluid_slot == 0 || frame->fluid_slot == FLUID_NOT_SHOWN ||
                frame->fluid_slot <= after || frame->fluid_slot > slot) {
                continue;
            }
            if (scheduler.IsFree(frame->gpu_tick)) {
                done_batch = (std::max)(done_batch, frame->fluid_batch);
            } else if (waiting == nullptr || frame->fluid_batch > waiting->fluid_batch ||
                       (frame->fluid_batch == waiting->fluid_batch &&
                        frame->gpu_tick < waiting->gpu_tick)) {
                waiting = frame;
            }
        }
        return waiting != nullptr && waiting->fluid_batch > done_batch ? waiting : nullptr;
    };
    const Frame* const waiting = landing();
    if (waiting == nullptr) {
        return;
    }
    const s64 start = NowNs();
    const s64 budget = predictor.HoldBudget(
        start,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            waiting->fluid_recorded.time_since_epoch())
            .count(),
        static_cast<s64>(fluid.latency_ns));
    if (budget <= 0) {
        return;
    }
    bool landed = false;
    s64 now = start;
    while (now - start < budget) {
        // A look at the GPU's progress is a system call: not more often than every 50 us.
        const s64 next = now + 50'000;
        while (NowNs() < next) {
            std::this_thread::yield();
        }
        landed = landing() == nullptr;
        now = NowNs();
        if (landed) {
            break;
        }
    }
    predictor.HoldResult(landed, now - start);
}

void PresentManager::FluidPredictNote(u64 slot, const Frame* best) {
    VideoCore::Nx2Predict::FramePredictor& predictor = VideoCore::Nx2Predict::FramePredictor::Instance();
    if (predictor.Enabled()) {
        predictor.NoteSlot(slot, best != nullptr ? best->fluid_batch : 0, NowNs());
    }
}

} // namespace Vulkan
