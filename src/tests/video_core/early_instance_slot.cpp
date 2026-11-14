// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/early_slot.h"

namespace {

/// Stands in for the Vulkan instance and what it points at.
struct Made {
    std::mutex use;
    int resource{7};

    explicit operator bool() const noexcept {
        return resource != 0;
    }
};

struct Key {
    int window;
    bool validation;

    bool operator==(const Key&) const = default;
};

using Slot = Vulkan::EarlySlot<Key, Made>;
using Outcome = Slot::Outcome;

constexpr Key Windows{1, false};
constexpr Key WindowsValidated{1, true};

/// A future that is ready already, as when the creation finished while the boot did other work.
Slot::Future Ready() {
    std::promise<std::shared_ptr<Made>> promise;
    promise.set_value(std::make_shared<Made>());
    return promise.get_future().share();
}

Slot::Future Throwing() {
    std::promise<std::shared_ptr<Made>> promise;
    promise.set_exception(std::make_exception_ptr(std::runtime_error{"driver init failed"}));
    return promise.get_future().share();
}

int TakeResource(Made& made) {
    return std::exchange(made.resource, 0);
}

} // Anonymous namespace

TEST_CASE("early_instance: nothing made ahead leaves everything to the caller", "[early_instance]") {
    Slot slot;
    bool called = false;
    CHECK_FALSE(slot.Borrow(Windows, [&](Made&) { called = true; }));
    const auto taken = slot.Take(Windows, [&](Made&) { called = true; });
    CHECK(taken.outcome == Outcome::Empty);
    CHECK(taken.made == nullptr);
    CHECK_FALSE(called);
    // A take that found nothing is not a take.
    CHECK_FALSE(slot.WasTaken());
}

TEST_CASE("early_instance: the frontend borrows and the renderer still takes it",
          "[early_instance]") {
    Slot slot;
    CHECK(slot.Reserve(Windows, [] { return Ready(); }));

    int seen = 0;
    CHECK(slot.Borrow(Windows, [&](Made& made) { seen = made.resource; }));
    CHECK(seen == 7);
    // A second look is as good as the first: the listing may run again, from the settings.
    seen = 0;
    CHECK(slot.Borrow(Windows, [&](Made& made) { seen = made.resource; }));
    CHECK(seen == 7);

    int taken_resource = 0;
    const auto taken = slot.Take(Windows, [&](Made& made) { taken_resource = TakeResource(made); });
    CHECK(taken.outcome == Outcome::Taken);
    CHECK(taken_resource == 7);
    REQUIRE(taken.made != nullptr);
    CHECK_FALSE(static_cast<bool>(*taken.made));
}

TEST_CASE("early_instance: one request at a time, and the work starts once", "[early_instance]") {
    Slot slot;
    int started = 0;
    const auto make = [&started] {
        ++started;
        return Ready();
    };
    CHECK(slot.Reserve(Windows, make));
    CHECK_FALSE(slot.Reserve(Windows, make));
    CHECK_FALSE(slot.Reserve(WindowsValidated, make));
    CHECK(started == 1);
}

TEST_CASE("early_instance: nothing is lent or taken after the take", "[early_instance]") {
    Slot slot;
    REQUIRE(slot.Reserve(Windows, [] { return Ready(); }));
    CHECK_FALSE(slot.WasTaken());
    CHECK(slot.Take(Windows, [](Made& made) { TakeResource(made); }).outcome == Outcome::Taken);
    CHECK(slot.WasTaken());

    bool called = false;
    CHECK_FALSE(slot.Borrow(Windows, [&](Made&) { called = true; }));
    CHECK(slot.Take(Windows, [&](Made&) { called = true; }).outcome == Outcome::Empty);
    CHECK_FALSE(called);
    // The slot is free again for a later request.
    CHECK(slot.Reserve(Windows, [] { return Ready(); }));
}

TEST_CASE("early_instance: another window type or validation setting is not lent",
          "[early_instance]") {
    Slot slot;
    REQUIRE(slot.Reserve(Windows, [] { return Ready(); }));

    bool called = false;
    CHECK_FALSE(slot.Borrow(WindowsValidated, [&](Made&) { called = true; }));
    CHECK_FALSE(slot.Borrow(Key{2, false}, [&](Made&) { called = true; }));
    CHECK_FALSE(called);

    // The renderer asks for validation: what was made ahead does not fit, it is dropped, and the
    // renderer creates its own.
    const auto taken = slot.Take(WindowsValidated, [&](Made&) { called = true; });
    CHECK(taken.outcome == Outcome::Mismatch);
    CHECK_FALSE(called);
    CHECK(slot.WasTaken());
    CHECK(slot.Take(Windows, [&](Made&) { called = true; }).outcome == Outcome::Empty);
}

TEST_CASE("early_instance: a creation that failed is reported once and the caller creates its own",
          "[early_instance]") {
    Slot slot;
    REQUIRE(slot.Reserve(Windows, [] { return Throwing(); }));

    bool called = false;
    CHECK_FALSE(slot.Borrow(Windows, [&](Made&) { called = true; }));
    const auto taken = slot.Take(Windows, [&](Made&) { called = true; });
    CHECK(taken.outcome == Outcome::Failed);
    CHECK(taken.error == "driver init failed");
    CHECK_FALSE(called);
    CHECK(slot.Take(Windows, [&](Made&) { called = true; }).outcome == Outcome::Empty);
}

TEST_CASE("early_instance: the borrower waits for a creation under way", "[early_instance]") {
    Slot slot;
    std::promise<std::shared_ptr<Made>> promise;
    REQUIRE(slot.Reserve(Windows, [&] { return promise.get_future().share(); }));

    auto borrowed = std::async(std::launch::async, [&] {
        int seen = 0;
        const bool used = slot.Borrow(Windows, [&](Made& made) { seen = made.resource; });
        return used ? seen : -1;
    });
    // Not done while the instance is still being made.
    CHECK(borrowed.wait_for(std::chrono::milliseconds{30}) == std::future_status::timeout);
    promise.set_value(std::make_shared<Made>());
    CHECK(borrowed.get() == 7);
}

TEST_CASE("early_instance: a take waits for the borrower that is using the instance",
          "[early_instance]") {
    Slot slot;
    REQUIRE(slot.Reserve(Windows, [] { return Ready(); }));

    std::future<int> taker;
    int resource_after_wait = -1;
    CHECK(slot.Borrow(Windows, [&](Made& made) {
        taker = std::async(std::launch::async, [&slot] {
            int moved = 0;
            const auto taken = slot.Take(Windows, [&](Made& m) { moved = TakeResource(m); });
            return taken.outcome == Outcome::Taken ? moved : -1;
        });
        // The take must not move the resource out while this callback uses it.
        std::this_thread::sleep_for(std::chrono::milliseconds{30});
        resource_after_wait = made.resource;
    }));
    CHECK(resource_after_wait == 7);
    CHECK(taker.get() == 7);
}
