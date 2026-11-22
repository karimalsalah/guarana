// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace Vulkan {

/**
 * One resource made ahead on another thread (the Vulkan instance, EDEN_NX2_VULKAN_INSTANCE_ONCE)
 * that several users may look at and one finally takes. It has no Vulkan in it, so it is tested
 * without a driver.
 *
 * `Made` holds the resource and the things it points at. It has a `std::mutex use`, and an
 * explicit operator bool that says whether it still holds the resource. Borrow and Take run their
 * callback with `use` locked, so a borrower never sees the resource while Take moves it out, and
 * Take waits for a borrower that is working.
 *
 * `Key` says what the resource was made for; it needs operator==. A user with another key gets
 * nothing, and a Take with another key drops the resource made ahead.
 */
template <typename Key, typename Made>
class EarlySlot {
public:
    using Future = std::shared_future<std::shared_ptr<Made>>;

    enum class Outcome {
        Empty,    ///< Nothing was made ahead
        Mismatch, ///< It was made for another key; it is dropped
        Failed,   ///< Making it threw, or it was gone
        Taken,
    };

    struct TakeResult {
        Outcome outcome{Outcome::Empty};
        std::string error;             ///< Why it failed
        std::shared_ptr<Made> made{};  ///< Set when Taken: keep it as long as the resource is used
    };

    /// Calls `make()` for the future, and keeps it for `key`, unless one is kept already. `make`
    /// runs under the slot's lock, so two callers never both start the work. True if it ran.
    template <typename Make>
    bool Reserve(const Key& key, Make&& make) {
        std::scoped_lock lock{slot_mutex};
        if (request) {
            return false;
        }
        request.emplace(Request{key, make()});
        return true;
    }

    /// Waits for the resource made ahead for `key` and calls `use(Made&)` with it, leaving it in
    /// the slot for Take. False when nothing is kept for `key`, it failed or it was taken.
    template <typename Use>
    bool Borrow(const Key& key, Use&& use) {
        std::optional<Future> future;
        {
            std::scoped_lock lock{slot_mutex};
            if (!request || !(request->key == key)) {
                return false;
            }
            future = request->future;
        }
        std::shared_ptr<Made> made;
        try {
            made = future->get();
        } catch (const std::exception&) {
            return false;
        }
        if (!made) {
            return false;
        }
        std::scoped_lock use_lock{made->use};
        if (!*made) {
            return false;
        }
        use(*made);
        return true;
    }

    /// Removes what is kept and, when it was made for `key`, waits for it and calls `take(Made&)`
    /// to move the resource out. Whatever the outcome, the slot is empty afterwards.
    template <typename Taker>
    TakeResult Take(const Key& key, Taker&& take) {
        std::optional<Request> mine;
        {
            std::scoped_lock lock{slot_mutex};
            mine = std::move(request);
            request.reset();
            was_taken = was_taken || mine.has_value();
        }
        if (!mine) {
            return TakeResult{Outcome::Empty, std::string{}, nullptr};
        }
        std::shared_ptr<Made> made;
        try {
            made = mine->future.get();
        } catch (const std::exception& exception) {
            return TakeResult{Outcome::Failed, exception.what(), nullptr};
        }
        if (!(mine->key == key)) {
            return TakeResult{Outcome::Mismatch, std::string{}, nullptr};
        }
        if (!made) {
            return TakeResult{Outcome::Failed, "nothing was made", nullptr};
        }
        std::scoped_lock use_lock{made->use};
        if (!*made) {
            return TakeResult{Outcome::Failed, "it was taken already", nullptr};
        }
        take(*made);
        return TakeResult{Outcome::Taken, std::string{}, std::move(made)};
    }

    /// Whether a Take ever found something kept, whatever its outcome: the last user has come, and
    /// nothing new should be made ahead for a borrower.
    [[nodiscard]] bool WasTaken() {
        std::scoped_lock lock{slot_mutex};
        return was_taken;
    }

private:
    struct Request {
        Key key;
        Future future;
    };

    std::mutex slot_mutex;
    std::optional<Request> request;
    bool was_taken{false};
};

} // namespace Vulkan
