// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace Common {

/// Bounded history. Tags must have static storage duration. No allocations after construction.
template <std::size_t Capacity>
class EventRing {
    static_assert(Capacity > 0);

public:
    struct Event {
        std::chrono::steady_clock::time_point timestamp{};
        std::uint16_t kind{};
        std::array<std::uint64_t, 4> fields{};
        const char* tag{};
    };

    struct History {
        std::array<Event, Capacity> events{};
        std::size_t size{};
        std::uint64_t overwritten{};
    };

    void Record(std::uint16_t kind, std::array<std::uint64_t, 4> fields, const char* tag) {
        std::scoped_lock lock{mutex};
        events[next] = {std::chrono::steady_clock::now(), kind, fields, tag};
        next = (next + 1) % Capacity;
        if (size < Capacity) {
            ++size;
        } else {
            ++overwritten;
        }
    }

    /// Consistent copy, oldest first; consumption does not hold the writer lock.
    [[nodiscard]] History Snapshot() const {
        std::scoped_lock lock{mutex};
        History result;
        result.size = size;
        result.overwritten = overwritten;
        const auto first = (next + Capacity - size) % Capacity;
        for (std::size_t index = 0; index < size; ++index) {
            result.events[index] = events[(first + index) % Capacity];
        }
        return result;
    }

private:
    mutable std::mutex mutex;
    std::array<Event, Capacity> events{};
    std::size_t next{};
    std::size_t size{};
    std::uint64_t overwritten{};
};

} // namespace Common
