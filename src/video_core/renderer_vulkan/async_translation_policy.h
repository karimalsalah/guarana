// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/async_draw_policy.h"

namespace Vulkan {

/// What the pipeline cache knows about a new graphics pipeline when it chooses where to translate
/// it (EDEN_NX2_ASYNC_TRANSLATE).
struct AsyncTranslationInputs {
    bool switch_on{};          ///< EDEN_NX2_ASYNC_TRANSLATE is on
    bool async_shaders{};      ///< The use_asynchronous_shaders setting
    bool native_draw{};        ///< The draw comes from a Switch 2 (native Turing macro) program
    NativePendingMode mode{NativePendingMode::Wait};
    bool can_discard{};        ///< CanDiscardPendingDraw allows skipping this draw
    bool all_programs_native{}; ///< Every program of the pipeline is lowered native code
    bool marked_synchronous{}; ///< An earlier translation of this key needed the GPU thread
    bool has_kept_pointers{};  ///< An evicted pipeline of this key left storage pointer slots
};

/// Whether the pipeline may be translated on a worker, from environments detached from guest
/// memory. A draw that cannot be skipped waits for its pipeline, so it gains nothing: it stays on
/// the GPU thread, as does everything the detached environments cannot answer.
[[nodiscard]] constexpr bool CanTranslateOnWorker(const AsyncTranslationInputs& in) {
    return in.switch_on && in.async_shaders && in.native_draw &&
           in.mode != NativePendingMode::Wait && in.can_discard && in.all_programs_native &&
           !in.marked_synchronous && !in.has_kept_pointers;
}

enum class TranslationThread { GpuThread, Worker };

/// Where a pipeline is translated once the cache tried to detach its environments and to reserve
/// their snapshot bytes in the queued IR budget. The GPU thread when either failed.
[[nodiscard]] constexpr TranslationThread ChooseTranslationThread(bool detached,
                                                                  bool budget_reserved) {
    return detached && budget_reserved ? TranslationThread::Worker : TranslationThread::GpuThread;
}

/// What a lookup of a key whose translation runs as a shader job does.
enum class TranslationWait {
    Adopt, ///< The job ended: take its result
    Skip,  ///< Skip the draw and look again later
    Join,  ///< Run or wait for the job until it is done
    Timed, ///< Promote the job and wait for it within the frame's wait budget
};

/// @param action    DecideNativePending's answer for the current draw.
/// @param finished  The job reached a final state.
/// @param in_flight Translations queued and not adopted yet.
/// @param workers   CPU workers of the shader jobs: a timed wait is not worth it while more
///                  translations are queued than workers could run at once.
[[nodiscard]] constexpr TranslationWait DecideTranslationWait(NativeDrawAction action,
                                                              bool finished, size_t in_flight,
                                                              size_t workers) {
    if (finished) {
        return TranslationWait::Adopt;
    }
    switch (action) {
    case NativeDrawAction::Skip:
        return TranslationWait::Skip;
    case NativeDrawAction::Wait:
        return TranslationWait::Join;
    case NativeDrawAction::WaitTimed:
        return in_flight > workers ? TranslationWait::Skip : TranslationWait::Timed;
    }
    return TranslationWait::Skip;
}

/// What the GPU thread does with a finished translation: adopt its pipeline, or translate the key
/// itself when the detached environments could not answer it (DetachedEnvironmentMiss) or the job
/// ended without a result.
enum class FinishedTranslation { Adopt, TranslateOnGpuThread };

[[nodiscard]] constexpr bool TranslationGenerationMatches(u64 captured, u64 current) {
    return captured == current;
}

[[nodiscard]] constexpr FinishedTranslation ClassifyFinishedTranslation(bool environment_miss,
                                                                        bool has_result) {
    return environment_miss || !has_result ? FinishedTranslation::TranslateOnGpuThread
                                           : FinishedTranslation::Adopt;
}

} // namespace Vulkan
