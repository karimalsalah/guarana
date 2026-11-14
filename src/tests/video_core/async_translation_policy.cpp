// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/async_translation_policy.h"
#include "video_core/detached_snapshot_policy.h"
#include "video_core/renderer_vulkan/pipeline_build.h"

using Vulkan::AsyncTranslationInputs;
using Vulkan::CanTranslateOnWorker;
using Vulkan::ChooseTranslationThread;
using Vulkan::ClassifyFinishedTranslation;
using Vulkan::DecideTranslationWait;
using Vulkan::FinishedTranslation;
using Vulkan::NativeDrawAction;
using Vulkan::NativePendingMode;
using Vulkan::TranslationThread;
using Vulkan::TranslationWait;

TEST_CASE("Detached buffers budget pointer flags and tail reads", "[video_core][async_translation]") {
    using VideoCommon::DetachedConstantBufferBytes;
    REQUIRE(DetachedConstantBufferBytes(0, 65536) == 0);
    REQUIRE(DetachedConstantBufferBytes(1, 65536) == 9);
    REQUIRE(DetachedConstantBufferBytes(4, 65536) == 9);
    REQUIRE(DetachedConstantBufferBytes(5, 65536) == 14);
    REQUIRE(DetachedConstantBufferBytes(65536, 65536) == 81924);
    REQUIRE_FALSE(DetachedConstantBufferBytes(65537, 65536));
}

TEST_CASE("Snapshot charges leave headroom without wrapping", "[video_core][async_translation]") {
    using VideoCommon::ConservativeSnapshotCharge;
    REQUIRE(ConservativeSnapshotCharge(100) == 4296);
    const u64 largest = (std::numeric_limits<u64>::max() - 4096) / 2;
    REQUIRE(ConservativeSnapshotCharge(largest).has_value());
    REQUIRE_FALSE(ConservativeSnapshotCharge(largest + 1));
}

TEST_CASE("Translations from an old prepass generation must be retried",
          "[video_core][async_translation]") {
    REQUIRE(Vulkan::TranslationGenerationMatches(0, 0));
    REQUIRE(Vulkan::TranslationGenerationMatches(42, 42));
    REQUIRE_FALSE(Vulkan::TranslationGenerationMatches(0, 1));
    REQUIRE_FALSE(Vulkan::TranslationGenerationMatches(41, 42));
}

TEST_CASE("Queued snapshots compete with queued IR before copying", "[video_core][async_translation]") {
    const auto charge = VideoCommon::ConservativeSnapshotCharge(
        *VideoCommon::DetachedConstantBufferBytes(65536, 65536));
    REQUIRE(charge.has_value());
    Vulkan::QueuedIrBudget budget{*charge + 1024};
    auto ir = budget.TryReserve(1025);
    REQUIRE(ir);
    REQUIRE_FALSE(budget.TryReserve(*charge));
    ir.reset();
    auto snapshot = budget.TryReserve(*charge);
    REQUIRE(snapshot);
    REQUIRE_FALSE(budget.TryReserve(1025));
    snapshot.reset();
    REQUIRE(budget.TryReserve(*charge));
}

namespace {
AsyncTranslationInputs Eligible() {
    AsyncTranslationInputs in;
    in.switch_on = true;
    in.async_shaders = true;
    in.native_draw = true;
    in.mode = NativePendingMode::Bounded;
    in.can_discard = true;
    in.all_programs_native = true;
    return in;
}
} // namespace

TEST_CASE("A skippable native pipeline translates on a worker", "[video_core][async_translation]") {
    REQUIRE(CanTranslateOnWorker(Eligible()));
    AsyncTranslationInputs skip = Eligible();
    skip.mode = NativePendingMode::Skip;
    REQUIRE(CanTranslateOnWorker(skip));
}

TEST_CASE("Everything but the eligible case stays on the GPU thread",
          "[video_core][async_translation]") {
    REQUIRE_FALSE(CanTranslateOnWorker(AsyncTranslationInputs{}));

    AsyncTranslationInputs in = Eligible();
    in.switch_on = false;
    REQUIRE_FALSE(CanTranslateOnWorker(in));

    in = Eligible();
    in.async_shaders = false;
    REQUIRE_FALSE(CanTranslateOnWorker(in));

    in = Eligible();
    in.native_draw = false;
    REQUIRE_FALSE(CanTranslateOnWorker(in));

    in = Eligible();
    in.mode = NativePendingMode::Wait;
    REQUIRE_FALSE(CanTranslateOnWorker(in));

    // A draw that cannot be skipped waits for its pipeline whatever the mode is.
    in = Eligible();
    in.can_discard = false;
    REQUIRE_FALSE(CanTranslateOnWorker(in));

    // Only lowered native programs read no guest memory for their instructions.
    in = Eligible();
    in.all_programs_native = false;
    REQUIRE_FALSE(CanTranslateOnWorker(in));

    // A key whose detached translation missed an answer is translated on the GPU thread for good.
    in = Eligible();
    in.marked_synchronous = true;
    REQUIRE_FALSE(CanTranslateOnWorker(in));

    // The slots an evicted pipeline of this key bound are kept on the GPU thread.
    in = Eligible();
    in.has_kept_pointers = true;
    REQUIRE_FALSE(CanTranslateOnWorker(in));
}

TEST_CASE("A pipeline goes to a worker only when detached and reserved",
          "[video_core][async_translation]") {
    REQUIRE(ChooseTranslationThread(true, true) == TranslationThread::Worker);
    REQUIRE(ChooseTranslationThread(true, false) == TranslationThread::GpuThread);
    REQUIRE(ChooseTranslationThread(false, true) == TranslationThread::GpuThread);
    REQUIRE(ChooseTranslationThread(false, false) == TranslationThread::GpuThread);
}

TEST_CASE("A lookup of a translating key waits as its draw allows",
          "[video_core][async_translation]") {
    // A finished job is adopted whatever the draw would do.
    REQUIRE(DecideTranslationWait(NativeDrawAction::Skip, true, 100, 1) == TranslationWait::Adopt);
    REQUIRE(DecideTranslationWait(NativeDrawAction::Wait, true, 100, 1) == TranslationWait::Adopt);

    REQUIRE(DecideTranslationWait(NativeDrawAction::Skip, false, 0, 4) == TranslationWait::Skip);
    REQUIRE(DecideTranslationWait(NativeDrawAction::Wait, false, 100, 4) == TranslationWait::Join);

    REQUIRE(DecideTranslationWait(NativeDrawAction::WaitTimed, false, 1, 4) ==
            TranslationWait::Timed);
    // As many translations queued as workers still waits; one more does not.
    REQUIRE(DecideTranslationWait(NativeDrawAction::WaitTimed, false, 4, 4) ==
            TranslationWait::Timed);
    REQUIRE(DecideTranslationWait(NativeDrawAction::WaitTimed, false, 5, 4) ==
            TranslationWait::Skip);
}

TEST_CASE("A finished translation is adopted unless the GPU thread must redo it",
          "[video_core][async_translation]") {
    REQUIRE(ClassifyFinishedTranslation(false, true) == FinishedTranslation::Adopt);
    // The detached environments could not answer a read.
    REQUIRE(ClassifyFinishedTranslation(true, false) == FinishedTranslation::TranslateOnGpuThread);
    REQUIRE(ClassifyFinishedTranslation(true, true) == FinishedTranslation::TranslateOnGpuThread);
    // The job was cancelled or failed without handing in a result.
    REQUIRE(ClassifyFinishedTranslation(false, false) ==
            FinishedTranslation::TranslateOnGpuThread);
}
