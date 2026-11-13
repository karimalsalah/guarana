// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <exception>
#include <mutex>
#include <random>
#include <system_error>

#include <fmt/format.h>

#include "common/fs/data_dir_migration.h"
#include "common/fs/fs_util.h"
#include "common/logging.h"

namespace Common::FS::DataDirMigration {

namespace fs = std::filesystem;

namespace {

std::mutex report_mutex;
Result pending_report;
bool has_pending_report = false;

std::string Describe(const std::error_code& ec) {
    return fmt::format("{} ({})", ec.message(), ec.value());
}

fs::path StagingPath(const fs::path& current, std::uint64_t token) {
    auto staging = current;
    staging += fmt::format(".migrating-{:x}", token);
    return staging;
}

void RemoveQuietly(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
}

bool IsDir(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

bool Exists(const fs::path& path) {
    std::error_code ec;
    // symlink_status: a dangling link at the new location still counts as "exists", so the copy
    // never tries to rename over it.
    return fs::exists(fs::symlink_status(path, ec));
}

} // namespace

bool CopyTree(const fs::path& from, const fs::path& to, CopyStats& stats, std::string& error) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        error = fmt::format("cannot create {}: {}", PathToUTF8String(to), Describe(ec));
        return false;
    }

    fs::recursive_directory_iterator it(from, fs::directory_options::none, ec);
    if (ec) {
        error = fmt::format("cannot read {}: {}", PathToUTF8String(from), Describe(ec));
        return false;
    }
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const auto& source = it->path();
        const auto relative = source.lexically_relative(from);
        const auto target = to / relative;

        const auto status = it->symlink_status(ec);
        if (ec) {
            error = fmt::format("cannot read {}: {}", PathToUTF8String(source), Describe(ec));
            return false;
        }
        if (fs::is_symlink(status)) {
            // Keep links as links; following them would copy the Ryujinx save folder in.
            fs::copy_symlink(source, target, ec);
        } else if (fs::is_directory(status)) {
            fs::create_directories(target, ec);
        } else if (fs::is_regular_file(status)) {
            fs::copy_file(source, target, fs::copy_options::none, ec);
            if (!ec) {
                ++stats.files;
                const auto size = fs::file_size(source, ec);
                if (!ec) {
                    stats.bytes += size;
                }
            }
        }
        if (ec) {
            error = fmt::format("cannot copy {}: {}", PathToUTF8String(source), Describe(ec));
            return false;
        }
    }
    if (ec) {
        error = fmt::format("cannot read {}: {}", PathToUTF8String(from), Describe(ec));
        return false;
    }
    return true;
}

Result Resolve(const std::vector<Root>& roots, const CopyFunction& copy) {
    Result result;
    if (roots.empty()) {
        return result;
    }

    result.decision = Decide(Exists(roots[0].current), IsDir(roots[0].legacy));
    if (result.decision != Decision::CopyFromLegacy) {
        return result;
    }

    // Roots that take part: the primary, plus every other root that has an old folder to copy and
    // no new one yet.
    std::vector<const Root*> todo;
    todo.push_back(&roots[0]);
    for (std::size_t i = 1; i < roots.size(); ++i) {
        if (IsDir(roots[i].legacy) && !Exists(roots[i].current)) {
            todo.push_back(&roots[i]);
        }
    }

    const auto started = std::chrono::steady_clock::now();
    const auto fail = [&](const std::string& why) {
        result.outcome = OutcomeAfterCopy(false);
        result.warning = true;
        result.message = fmt::format(
            "Could not copy the user data from {} to {}: {}. Using the old folder for this "
            "session; the copy is tried again on the next start.",
            PathToUTF8String(roots[0].legacy), PathToUTF8String(roots[0].current), why);
        return result;
    };

    std::vector<fs::path> stagings;
    const auto drop_stagings = [&] {
        for (const auto& staging : stagings) {
            RemoveQuietly(staging);
        }
    };
    std::vector<fs::path> committed; // only names that did not exist before this call

    try {
        std::random_device rd;
        const std::uint64_t token = (std::uint64_t{rd()} << 32) | rd();

        CopyStats stats;
        for (const Root* root : todo) {
            std::error_code ec;
            fs::create_directories(root->current.parent_path(), ec);
            const auto staging = StagingPath(root->current, token);
            stagings.push_back(staging);
            std::string error;
            bool ok = false;
            try {
                ok = copy(root->legacy, staging, stats, error);
            } catch (const std::exception& e) {
                error = e.what();
            }
            if (!ok) {
                drop_stagings();
                return fail(error.empty() ? "copy failed" : error);
            }
        }

        // Commit: every staging folder becomes its final name, or none of them does.
        for (std::size_t i = 0; i < todo.size(); ++i) {
            const auto& current = todo[i]->current;
            if (Exists(current)) {
                // Another instance finished the same copy first; use its result.
                if (i == 0) {
                    drop_stagings();
                    return result;
                }
                RemoveQuietly(stagings[i]);
                continue;
            }
            std::error_code ec;
            fs::rename(stagings[i], current, ec);
            if (ec) {
                const auto why = fmt::format("cannot move {} into place: {}",
                                             PathToUTF8String(stagings[i]), Describe(ec));
                for (const auto& done : committed) {
                    RemoveQuietly(done);
                }
                drop_stagings();
                return fail(why);
            }
            committed.push_back(current);
        }

        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
        result.outcome = OutcomeAfterCopy(true);
        result.message = fmt::format(
            "Copied the user data from {} to {} ({} files, {:.1f} MiB{}, {} ms). The old folder was "
            "left as it was.",
            PathToUTF8String(roots[0].legacy), PathToUTF8String(roots[0].current), stats.files,
            static_cast<double>(stats.bytes) / (1024.0 * 1024.0),
            todo.size() > 1 ? fmt::format(", plus {} more folders", todo.size() - 1) : std::string{},
            ms);
        return result;
    } catch (const std::exception& e) {
        for (const auto& done : committed) {
            RemoveQuietly(done);
        }
        drop_stagings();
        return fail(e.what());
    }
}

void SetPendingReport(const Result& result) {
    std::scoped_lock lock{report_mutex};
    if (result.message.empty()) {
        return;
    }
    pending_report = result;
    has_pending_report = true;
}

void LogPendingReport() {
    Result result;
    {
        std::scoped_lock lock{report_mutex};
        if (!has_pending_report) {
            return;
        }
        result = pending_report;
        has_pending_report = false;
    }
    if (result.warning) {
        LOG_WARNING(Common_Filesystem, "{}", result.message);
    } else {
        LOG_INFO(Common_Filesystem, "{}", result.message);
    }
}

} // namespace Common::FS::DataDirMigration
