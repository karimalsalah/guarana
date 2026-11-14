// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/switch_origin.h"

#include <mutex>
#include <utility>

namespace Common::SwitchOrigin {
namespace {

struct Record {
    std::mutex mutex;
    SwitchMap switches;
    std::set<std::string> set_by_process;
};

Record& TheRecord() {
    static Record record;
    return record;
}

} // Anonymous namespace

SwitchMap ShippedOnly(const SwitchMap& shipped, const SwitchMap& merged,
                      const std::set<std::string>& named_by_player,
                      const std::function<bool(const std::string&)>& in_environment) {
    SwitchMap result;
    for (const auto& entry : shipped) {
        const std::string& name = entry.first;
        const auto it = merged.find(name);
        // Not in `merged`: dropped by the loader's name filter, so never put in the environment.
        if (it == merged.end() || named_by_player.contains(name) || in_environment(name)) {
            continue;
        }
        result.emplace(name, it->second);
    }
    return result;
}

void RecordShippedOnly(SwitchMap switches) {
    Record& record = TheRecord();
    const std::scoped_lock lock{record.mutex};
    record.switches = std::move(switches);
}

std::optional<std::string> ShippedOnlyValue(std::string_view name) {
    Record& record = TheRecord();
    const std::scoped_lock lock{record.mutex};
    const auto it = record.switches.find(std::string{name});
    if (it == record.switches.end()) {
        return std::nullopt;
    }
    return it->second;
}

void RecordSetByProcess(const std::string& name) {
    Record& record = TheRecord();
    const std::scoped_lock lock{record.mutex};
    record.set_by_process.insert(name);
}

std::set<std::string> SetByProcess() {
    Record& record = TheRecord();
    const std::scoped_lock lock{record.mutex};
    return record.set_by_process;
}

} // namespace Common::SwitchOrigin
