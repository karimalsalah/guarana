// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/present_sync.h"

namespace {

struct TracedMutex {
    std::vector<int>& events;
    int id;

    void lock() {
        events.push_back(id);
    }

    void unlock() {
        events.push_back(-id);
    }
};

} // namespace

TEST_CASE("A failed present cannot retry or discard submitted work", "[present_sync]") {
    Vulkan::PresentSync::FrameSubmission submission;
    // An acquire/recreation failure leaves the frame available for retry or discard.
    REQUIRE(submission.CanRetry());
    submission.Submitted();
    // Every later attempt, including an exhausted retry loop, must leave the fence alone.
    for (int attempt = 0; attempt < 3; ++attempt) {
        REQUIRE_FALSE(submission.CanRetry());
    }
}

TEST_CASE("Device idle excludes presentation before queue submissions", "[present_sync]") {
    std::vector<int> events;
    TracedMutex swapchain{events, 1};
    TracedMutex submit{events, 2};
    SECTION("successful wait") {
        {
            const Vulkan::PresentSync::IdleLocks locks{swapchain, submit};
            REQUIRE(events == std::vector<int>{1, 2});
            events.push_back(3);
        }
        REQUIRE(events == std::vector<int>{1, 2, 3, -2, -1});
    }
    SECTION("failed wait releases both locks") {
        const auto lock_and_throw = [&] {
            const Vulkan::PresentSync::IdleLocks locks{swapchain, submit};
            throw std::runtime_error{"device lost"};
        };
        REQUIRE_THROWS_AS(lock_and_throw(), std::runtime_error);
        REQUIRE(events == std::vector<int>{1, 2, -2, -1});
    }
}
