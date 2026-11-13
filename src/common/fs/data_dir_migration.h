// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// One-time copy of the pre-rename "lichii" user data folder to the "guarana" one.
//
// The old folder is never moved, modified or deleted. The copy goes into a staging folder next to
// the new one and is renamed into place only when it is complete, so a half-copied "guarana"
// folder can never be mistaken for the real one on the next start.
namespace Common::FS::DataDirMigration {

/// What to do at start-up, from the two folder checks alone.
enum class Decision {
    UseCurrent,     ///< The new folder exists (even if the old one does too): use it as it is.
    CopyFromLegacy, ///< Only the old folder exists: copy it, then use the new one.
    StartFresh,     ///< Neither exists: use the new one, it is created later on demand.
};

/// Which folder this session uses once the decision has been carried out.
enum class Outcome {
    UseCurrent, ///< The new folder.
    UseLegacy,  ///< The old folder, because copying it failed. Tried again on the next start.
};

[[nodiscard]] constexpr Decision Decide(bool current_exists, bool legacy_exists) {
    if (current_exists) {
        return Decision::UseCurrent;
    }
    return legacy_exists ? Decision::CopyFromLegacy : Decision::StartFresh;
}

[[nodiscard]] constexpr Outcome OutcomeAfterCopy(bool copy_succeeded) {
    return copy_succeeded ? Outcome::UseCurrent : Outcome::UseLegacy;
}

struct Root {
    std::filesystem::path legacy;  ///< e.g. %APPDATA%\lichii
    std::filesystem::path current; ///< e.g. %APPDATA%\guarana
};

struct CopyStats {
    std::uint64_t files{};
    std::uint64_t bytes{};
};

/// Copies the directory tree `from` into the not yet existing `to`. Returns false and sets `error`
/// on failure. Replaceable so tests can inject a failure half way through.
using CopyFunction = std::function<bool(const std::filesystem::path& from,
                                        const std::filesystem::path& to, CopyStats& stats,
                                        std::string& error)>;

/// Copies a directory tree, keeping symbolic links as links (the Ryujinx save link relies on it).
bool CopyTree(const std::filesystem::path& from, const std::filesystem::path& to, CopyStats& stats,
              std::string& error);

struct Result {
    Outcome outcome{Outcome::UseCurrent};
    Decision decision{Decision::StartFresh};
    bool warning{}; ///< `message` describes a failure.
    std::string message; ///< One line for the log, empty when nothing happened.
};

/// Runs the migration. The first root is the primary one and alone drives the decision; any further
/// roots (the config and cache folders on Linux and macOS) are copied in the same step when their
/// old folder exists and their new one does not. All roots succeed or none stays behind.
[[nodiscard]] Result Resolve(const std::vector<Root>& roots, const CopyFunction& copy = CopyTree);

/// The path manager runs before logging exists, so it parks the outcome here and the frontends
/// write it to the log once logging is up. Does nothing when there is nothing to report.
void SetPendingReport(const Result& result);
void LogPendingReport();

} // namespace Common::FS::DataDirMigration
