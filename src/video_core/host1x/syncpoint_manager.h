// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon::GpuTrace {
struct StateAccess;
}

namespace Tegra {

namespace Host1x {

class SyncpointManager {
    friend struct ::VideoCommon::GpuTrace::StateAccess;

public:
    static constexpr size_t NUM_MAX_SYNCPOINTS = 192;

    u32 GetGuestSyncpointValue(u32 id) const {
        return syncpoints_guest[id].load(std::memory_order_acquire);
    }

    u32 GetHostSyncpointValue(u32 id) const {
        return syncpoints_host[id].load(std::memory_order_acquire);
    }

    struct RegisteredAction {
        explicit RegisteredAction(u32 expected_value_, u64 id_, std::function<void()>&& action_)
            : expected_value{expected_value_}, id{id_}, action{std::move(action_)} {}
        u32 expected_value;
        u64 id;
        std::function<void()> action;
    };
    using ActionHandle = u64;

    template <typename Func>
    ActionHandle RegisterGuestAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        return RegisterAction(syncpoints_guest[syncpoint_id], guest_action_storage[syncpoint_id],
                              expected_value, std::move(action), guard);
    }

    template <typename Func>
    ActionHandle RegisterHostAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        return RegisterAction(syncpoints_host[syncpoint_id], host_action_storage[syncpoint_id],
                              expected_value, std::move(action), HostMutex());
    }

    void DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle);

    void DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle);

    void IncrementGuest(u32 syncpoint_id);

    /// Called with a syncpoint's new guest value on every guest increment, before the value is
    /// published and the actions it meets run (empty to stop). Switch 2 clients read syncpoint
    /// values from a page the gsv service fills, and an action may wake a client that reads it
    /// right away.
    using GuestMirror = std::function<void(u32 syncpoint_id, u32 value)>;
    void SetGuestMirror(GuestMirror mirror);

    /// Runs func with guest increments held off, so a copy of the guest values it makes can't
    /// overwrite a newer value the mirror wrote.
    template <typename Func>
    void WithGuestValuesStable(Func&& func) {
        std::scoped_lock lk(guard);
        func();
    }

    void IncrementHost(u32 syncpoint_id);

    void WaitGuest(u32 syncpoint_id, u32 expected_value);

    void WaitHost(u32 syncpoint_id, u32 expected_value);

    bool IsReadyGuest(u32 syncpoint_id, u32 expected_value) const {
        return syncpoints_guest[syncpoint_id].load(std::memory_order_acquire) >= expected_value;
    }

    bool IsReadyHost(u32 syncpoint_id, u32 expected_value) const {
        return syncpoints_host[syncpoint_id].load(std::memory_order_acquire) >= expected_value;
    }

private:
    void Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                   std::list<RegisteredAction>& action_storage, std::mutex& bank_guard,
                   std::optional<u32> mirrored_id = std::nullopt);

    ActionHandle RegisterAction(std::atomic<u32>& syncpoint,
                                std::list<RegisteredAction>& action_storage, u32 expected_value,
                                std::function<void()>&& action, std::mutex& bank_guard);

    void DeregisterAction(std::list<RegisteredAction>& action_storage, const ActionHandle& handle,
                          std::mutex& bank_guard);

    void Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv, u32 expected_value,
              std::mutex& bank_guard);

    /// EDEN_NX2_SYNCPOINT_SPLIT=1: give the host bank its own lock. The guest bank (incremented on
    /// the GPU thread) and the host bank (incremented on the fencing thread) share no state but the
    /// action-id counter, yet one mutex serialized the two threads — each running callbacks that
    /// take the global scheduler lock — thousands of times a second. Default off keeps the single
    /// lock, byte for byte the original behaviour.
    static bool SplitBankLocks() {
        static const bool split = [] {
            const char* value = std::getenv("EDEN_NX2_SYNCPOINT_SPLIT");
            return value != nullptr && value[0] == '1';
        }();
        return split;
    }
    std::mutex& HostMutex() {
        return SplitBankLocks() ? host_guard : guard;
    }

    /// Due actions an Increment took out of storage and runs after releasing its lock.
    struct DueActions {
        std::list<RegisteredAction> pending; ///< Not started yet; a cancel removes them.
        ActionHandle running{};              ///< The one being called now, 0 between calls.
        std::thread::id thread{std::this_thread::get_id()};
    };

    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_guest{};
    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_host{};

    std::array<std::list<RegisteredAction>, NUM_MAX_SYNCPOINTS> guest_action_storage;
    std::array<std::list<RegisteredAction>, NUM_MAX_SYNCPOINTS> host_action_storage;

    std::mutex guard;      ///< Guest bank (and, unless split, the host bank too).
    std::mutex host_guard; ///< Host bank only, when EDEN_NX2_SYNCPOINT_SPLIT=1.
    /// Atomic so the two bank locks can mint ids concurrently when split; under the single lock
    /// the sequence is unchanged.
    std::atomic<u64> last_action_id{};
    GuestMirror guest_mirror; ///< Guarded by `guard`.
    /// Each bank tracks the batches its own Increment runs after releasing its lock. Under the
    /// single lock both banks share one vector, guarded by `guard`; when split, each vector is
    /// guarded by its bank's lock.
    std::vector<DueActions*> due_actions_guest;
    std::vector<DueActions*> due_actions_host;
    std::vector<DueActions*>& DueActionsFor(std::mutex& bank_guard) {
        return &bank_guard == &host_guard ? due_actions_host : due_actions_guest;
    }
    std::condition_variable wait_guest_cv;
    std::condition_variable wait_host_cv;
    std::condition_variable action_done_cv;
};

} // namespace Host1x

} // namespace Tegra
