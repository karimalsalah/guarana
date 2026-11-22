// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>

#include <fmt/format.h>

#ifdef _WIN32
#include <windows.h>
#include <powersetting.h>
#include <powrprof.h>
#endif

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/power_policy.h"

namespace Common::PowerPolicy {

bool Requested() {
    static const bool requested = [] {
        const char* const value = std::getenv("EDEN_NX2_POWER_POLICY");
        if (!value || *value == '\0' || std::strcmp(value, "0") == 0) {
            return false;
        }
        if (std::strcmp(value, "igpu") != 0) {
            LOG_WARNING(Common, "EDEN_NX2_POWER_POLICY: unknown policy \"{}\" (want igpu)", value);
            return false;
        }
        return true;
    }();
    return requested;
}

#ifdef _WIN32

namespace {

// winnt.h: GUID_PROCESSOR_SETTINGS_SUBGROUP, GUID_PROCESSOR_PERF_BOOST_MODE,
// GUID_PROCESSOR_THROTTLE_MINIMUM and GUID_PROCESSOR_THROTTLE_MINIMUM_1. Defined here so nothing
// depends on which library carries them.
constexpr GUID ProcessorSubgroup{0x54533251, 0x82be, 0x4824,
                                 {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
constexpr GUID BoostMode{0xbe337238, 0x0d82, 0x4146,
                         {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};
constexpr GUID ThrottleMinimum{0x893dee8e, 0x2bef, 0x41e0,
                               {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c}};
constexpr GUID ThrottleMinimum1{0x893dee8e, 0x2bef, 0x41e0,
                                {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4d}};

constexpr DWORD BoostDisabled = 0;
constexpr DWORD MinimumPercent = 5;
constexpr char FileTag[] = "lichii-power-restore";

struct Entry {
    std::string name;
    GUID guid{};
    DWORD saved_ac{};
    DWORD saved_dc{};
    DWORD applied_ac{};
    DWORD applied_dc{};
};

struct State {
    std::mutex mutex;
    bool active{};
    bool exit_hook{};
    GUID scheme{};
    std::vector<Entry> entries;
    std::string message;
};

State state;

std::string GuidText(const GUID& guid) {
    return fmt::format("{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                       static_cast<u32>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0],
                       guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
                       guid.Data4[6], guid.Data4[7]);
}

std::optional<GUID> ParseGuid(const std::string& text) {
    unsigned int data1{};
    unsigned int data2{};
    unsigned int data3{};
    unsigned int data4[8]{};
    if (text.size() != 36 ||
        std::sscanf(text.c_str(), "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &data1, &data2, &data3,
                    &data4[0], &data4[1], &data4[2], &data4[3], &data4[4], &data4[5], &data4[6],
                    &data4[7]) != 11) {
        return std::nullopt;
    }
    GUID guid{};
    guid.Data1 = data1;
    guid.Data2 = static_cast<unsigned short>(data2);
    guid.Data3 = static_cast<unsigned short>(data3);
    for (size_t i = 0; i < 8; ++i) {
        guid.Data4[i] = static_cast<unsigned char>(data4[i]);
    }
    return guid;
}

std::filesystem::path RestorePath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "nx2_power_restore.txt";
}

/// Creation time of a process, which with its id names it for good; 0 when it cannot be read.
u64 ProcessStart(HANDLE process) {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return 0;
    }
    return (u64{creation.dwHighDateTime} << 32) | creation.dwLowDateTime;
}

bool OwnerAlive(DWORD pid, u64 start) {
    const HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) {
        return false;
    }
    const bool alive =
        ProcessStart(process) == start && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

/// Writes the saved or the applied values of every entry; the first error, or ERROR_SUCCESS.
DWORD WriteValues(const GUID& scheme, const std::vector<Entry>& entries, bool saved) {
    DWORD first = ERROR_SUCCESS;
    for (const Entry& entry : entries) {
        const DWORD ac = PowerWriteACValueIndex(nullptr, &scheme, &ProcessorSubgroup, &entry.guid,
                                                saved ? entry.saved_ac : entry.applied_ac);
        const DWORD dc = PowerWriteDCValueIndex(nullptr, &scheme, &ProcessorSubgroup, &entry.guid,
                                                saved ? entry.saved_dc : entry.applied_dc);
        if (first == ERROR_SUCCESS) {
            first = ac != ERROR_SUCCESS ? ac : dc;
        }
    }
    return first;
}

/// The values of a scheme take effect when it is made active again. A scheme the user has left
/// since is not forced back.
DWORD Reactivate(const GUID& scheme) {
    GUID* active{};
    const DWORD result = PowerGetActiveScheme(nullptr, &active);
    if (result != ERROR_SUCCESS) {
        return result;
    }
    const bool same = IsEqualGUID(*active, scheme) != 0;
    LocalFree(active);
    return same ? PowerSetActiveScheme(nullptr, &scheme) : DWORD{ERROR_SUCCESS};
}

bool WriteRestoreFile(const GUID& scheme, const std::vector<Entry>& entries) {
    std::ofstream file{RestorePath(), std::ios::trunc};
    file << FileTag << " 1\n";
    file << "owner " << GetCurrentProcessId() << ' ' << ProcessStart(GetCurrentProcess()) << '\n';
    file << "scheme " << GuidText(scheme) << '\n';
    for (const Entry& entry : entries) {
        file << "setting " << GuidText(entry.guid) << ' ' << entry.name << ' ' << entry.saved_ac
             << ' ' << entry.saved_dc << '\n';
    }
    file.flush();
    return file.good();
}

struct RestoreFile {
    DWORD pid{};
    u64 start{};
    GUID scheme{};
    std::vector<Entry> entries;
};

std::optional<RestoreFile> ReadRestoreFile() {
    std::ifstream file{RestorePath()};
    std::string tag;
    int version{};
    if (!(file >> tag >> version) || tag != FileTag || version != 1) {
        return std::nullopt;
    }
    RestoreFile restore;
    bool has_scheme = false;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream words{line};
        std::string key;
        if (!(words >> key)) {
            continue;
        }
        if (key == "owner") {
            words >> restore.pid >> restore.start;
        } else if (key == "scheme") {
            std::string text;
            words >> text;
            const auto guid = ParseGuid(text);
            if (!guid) {
                return std::nullopt;
            }
            restore.scheme = *guid;
            has_scheme = true;
        } else if (key == "setting") {
            std::string text;
            Entry entry;
            if (!(words >> text >> entry.name >> entry.saved_ac >> entry.saved_dc)) {
                return std::nullopt;
            }
            const auto guid = ParseGuid(text);
            // Only the settings this code writes are ever written back.
            if (!guid || (!IsEqualGUID(*guid, BoostMode) && !IsEqualGUID(*guid, ThrottleMinimum) &&
                          !IsEqualGUID(*guid, ThrottleMinimum1))) {
                return std::nullopt;
            }
            entry.guid = *guid;
            restore.entries.push_back(std::move(entry));
        }
    }
    return has_scheme ? std::optional<RestoreFile>{std::move(restore)} : std::nullopt;
}

void RemoveRestoreFile() {
    std::error_code error;
    std::filesystem::remove(RestorePath(), error);
}

/// Caller holds state.mutex. True when no restore file is left.
bool RecoverStaleLocked() {
    std::error_code error;
    if (!std::filesystem::exists(RestorePath(), error)) {
        return true;
    }
    const auto restore = ReadRestoreFile();
    if (!restore) {
        LOG_WARNING(Common, "Power policy: unreadable restore file {} removed",
                    RestorePath().string());
        RemoveRestoreFile();
        return true;
    }
    if (OwnerAlive(restore->pid, restore->start)) {
        // Another instance, or this one's running session.
        return false;
    }
    const DWORD written = WriteValues(restore->scheme, restore->entries, true);
    const DWORD activated = written == ERROR_SUCCESS ? Reactivate(restore->scheme) : written;
    if (written != ERROR_SUCCESS) {
        LOG_ERROR(Common, "Power policy: restoring scheme {} after an unclean exit failed with "
                          "error {}; the restore file is kept",
                  GuidText(restore->scheme), written);
        if (written == ERROR_FILE_NOT_FOUND) {
            // The scheme is gone: nothing can be written back.
            RemoveRestoreFile();
            return true;
        }
        return false;
    }
    std::string values;
    for (const Entry& entry : restore->entries) {
        values += fmt::format(" {}={}/{}", entry.name, entry.saved_ac, entry.saved_dc);
    }
    LOG_INFO(Common, "Power policy: restored scheme {} after an unclean exit (AC/DC:{}), "
                     "reactivation result {}",
             GuidText(restore->scheme), values, activated);
    RemoveRestoreFile();
    return true;
}

/// Caller holds state.mutex. The exit hook runs it without logging: the log may be gone.
void RestoreLocked(bool log) {
    if (!state.active) {
        return;
    }
    const DWORD written = WriteValues(state.scheme, state.entries, true);
    const DWORD activated = written == ERROR_SUCCESS ? Reactivate(state.scheme) : written;
    state.active = false;
    if (written != ERROR_SUCCESS) {
        // The file stays: the next start tries again.
        state.message = fmt::format("restore failed with error {}", written);
        if (log) {
            LOG_ERROR(Common, "Power policy: restoring scheme {} failed with error {}",
                      GuidText(state.scheme), written);
        }
        return;
    }
    RemoveRestoreFile();
    state.message = "restored";
    if (log) {
        LOG_INFO(Common, "Power policy: restored scheme {}, reactivation result {}",
                 GuidText(state.scheme), activated);
    }
}

} // Anonymous namespace

void RecoverStale() {
    std::scoped_lock lock{state.mutex};
    RecoverStaleLocked();
}

bool Apply() {
    std::scoped_lock lock{state.mutex};
    if (state.active) {
        return true;
    }
    const auto fail = [](std::string message) {
        LOG_WARNING(Common, "Power policy: {}; the power scheme is left alone", message);
        state.message = std::move(message);
        return false;
    };
    if (!RecoverStaleLocked()) {
        return fail("a restore file is pending (another running session, or its restore failed)");
    }
    GUID* active{};
    if (const DWORD result = PowerGetActiveScheme(nullptr, &active); result != ERROR_SUCCESS) {
        return fail(fmt::format("PowerGetActiveScheme failed with error {}", result));
    }
    state.scheme = *active;
    LocalFree(active);

    state.entries.clear();
    const auto read = [](const char* name, const GUID& guid, DWORD wanted, bool at_most) {
        Entry entry{.name = name, .guid = guid};
        const DWORD ac = PowerReadACValueIndex(nullptr, &state.scheme, &ProcessorSubgroup, &guid,
                                               &entry.saved_ac);
        const DWORD dc = PowerReadDCValueIndex(nullptr, &state.scheme, &ProcessorSubgroup, &guid,
                                               &entry.saved_dc);
        if (ac != ERROR_SUCCESS || dc != ERROR_SUCCESS) {
            // The second core class does not exist on every processor.
            LOG_INFO(Common, "Power policy: {} is not readable (AC {}, DC {}), skipped", name, ac,
                     dc);
            return;
        }
        entry.applied_ac = at_most ? (std::min)(entry.saved_ac, wanted) : wanted;
        entry.applied_dc = at_most ? (std::min)(entry.saved_dc, wanted) : wanted;
        state.entries.push_back(std::move(entry));
    };
    read("boost_mode", BoostMode, BoostDisabled, false);
    read("min_state", ThrottleMinimum, MinimumPercent, true);
    read("min_state_1", ThrottleMinimum1, MinimumPercent, true);
    if (state.entries.empty()) {
        return fail("no processor setting of the active scheme is readable");
    }
    if (std::ranges::all_of(state.entries, [](const Entry& entry) {
            return entry.saved_ac == entry.applied_ac && entry.saved_dc == entry.applied_dc;
        })) {
        state.message = "the active scheme already has these values";
        LOG_INFO(Common, "Power policy: {}", state.message);
        return true;
    }
    // The file comes first: a crash between the writes must stay recoverable.
    if (!WriteRestoreFile(state.scheme, state.entries)) {
        RemoveRestoreFile();
        return fail(fmt::format("cannot write {}", RestorePath().string()));
    }
    DWORD result = WriteValues(state.scheme, state.entries, false);
    const char* call = "PowerWriteACValueIndex/PowerWriteDCValueIndex";
    if (result == ERROR_SUCCESS) {
        result = PowerSetActiveScheme(nullptr, &state.scheme);
        call = "PowerSetActiveScheme";
    }
    if (result != ERROR_SUCCESS) {
        if (WriteValues(state.scheme, state.entries, true) == ERROR_SUCCESS) {
            RemoveRestoreFile();
        }
        return fail(fmt::format("{} failed with error {}{}", call, result,
                                result == ERROR_ACCESS_DENIED
                                    ? " (access denied: this host wants an elevated process or "
                                      "a policy forbids it)"
                                    : ""));
    }
    state.active = true;
    state.message = "applied";
    if (!state.exit_hook) {
        state.exit_hook = true;
        std::atexit([] {
            // Never waits at exit: the restore file covers a restore that could not run.
            const std::unique_lock exit_lock{state.mutex, std::try_to_lock};
            if (exit_lock) {
                RestoreLocked(false);
            }
        });
    }
    std::string values;
    for (const Entry& entry : state.entries) {
        values += fmt::format(" {} {}/{} -> {}/{}", entry.name, entry.saved_ac, entry.saved_dc,
                              entry.applied_ac, entry.applied_dc);
    }
    LOG_INFO(Common, "Power policy igpu: scheme {} changed (AC/DC):{}", GuidText(state.scheme),
             values);
    return true;
}

void Restore() {
    std::scoped_lock lock{state.mutex};
    RestoreLocked(true);
}

Status GetStatus() {
    std::scoped_lock lock{state.mutex};
    Status status{
        .requested = Requested(),
        .active = state.active,
        .scheme = state.entries.empty() ? std::string{} : GuidText(state.scheme),
        .settings = {},
        .message = state.message,
    };
    for (const Entry& entry : state.entries) {
        status.settings.push_back({
            .name = entry.name,
            .saved_ac = entry.saved_ac,
            .saved_dc = entry.saved_dc,
            .applied_ac = entry.applied_ac,
            .applied_dc = entry.applied_dc,
        });
    }
    return status;
}

#else

void RecoverStale() {}

bool Apply() {
    LOG_WARNING(Common, "Power policy: EDEN_NX2_POWER_POLICY works on Windows only");
    return false;
}

void Restore() {}

Status GetStatus() {
    return {.requested = Requested(),
            .active = false,
            .scheme = {},
            .settings = {},
            .message = "Windows only"};
}

#endif

} // namespace Common::PowerPolicy
