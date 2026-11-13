// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <system_error>
#include <utility>

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

// A path as configs spell it, for comparing: forward slashes, no doubled separators (Qt escapes a
// backslash as two) except a leading pair (a network path), no trailing separator.
std::string ForwardSlashed(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const char ch = c == '\\' ? '/' : c;
        if (ch == '/' && out.size() > 1 && out.back() == '/') {
            continue;
        }
        out.push_back(ch);
    }
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

// Whether `value` is `folder` itself or a path inside it.
bool IsInside(std::string_view value, std::string_view folder) {
    if (folder.empty() || value.size() < folder.size()) {
        return false;
    }
    for (std::size_t i = 0; i < folder.size(); ++i) {
#ifdef _WIN32
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(folder[i]))) {
            return false;
        }
#else
        if (value[i] != folder[i]) {
            return false;
        }
#endif
    }
    return value.size() == folder.size() || value[folder.size()] == '/';
}

bool IsIniFile(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && path.extension() == ".ini";
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

std::string RewriteIniPaths(std::string_view ini_text, const std::vector<Root>& roots) {
    // The longest old folder first, so a folder inside another one is matched by its own root.
    std::vector<std::pair<std::string, std::string>> moves;
    for (const auto& root : roots) {
        auto from = ForwardSlashed(PathToUTF8String(root.legacy));
        auto to = ForwardSlashed(PathToUTF8String(root.current));
        if (!from.empty() && !to.empty() && from != to) {
            moves.emplace_back(std::move(from), std::move(to));
        }
    }
    std::ranges::sort(moves,
                      [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });

    std::string out;
    out.reserve(ini_text.size());
    bool changed = false;
    std::size_t pos = 0;
    while (pos < ini_text.size()) {
        std::size_t end = ini_text.find('\n', pos);
        const bool has_newline = end != std::string_view::npos;
        if (!has_newline) {
            end = ini_text.size();
        }
        std::string_view line = ini_text.substr(pos, end - pos);
        pos = has_newline ? end + 1 : end;
        const bool has_cr = !line.empty() && line.back() == '\r';
        if (has_cr) {
            line.remove_suffix(1);
        }

        std::string rewritten{line};
        const std::size_t eq = line.find('=');
        if (eq != std::string_view::npos && !line.starts_with('[') && !line.starts_with(';') &&
            !line.starts_with('#')) {
            std::string_view value = line.substr(eq + 1);
            const bool quoted = value.size() >= 2 && value.front() == '"' && value.back() == '"';
            if (quoted) {
                value = value.substr(1, value.size() - 2);
            }
            const std::string slashed = ForwardSlashed(value);
            for (const auto& [from, to] : moves) {
                if (!IsInside(slashed, from)) {
                    continue;
                }
                std::string moved = to + slashed.substr(from.size());
                // Keep a trailing separator the value had.
                if (!value.empty() && (value.back() == '/' || value.back() == '\\') &&
                    moved.back() != '/') {
                    moved.push_back('/');
                }
                rewritten = fmt::format("{}{}{}{}", line.substr(0, eq + 1), quoted ? "\"" : "",
                                        moved, quoted ? "\"" : "");
                changed = true;
                break;
            }
        }
        out += rewritten;
        if (has_cr) {
            out.push_back('\r');
        }
        if (has_newline) {
            out.push_back('\n');
        }
    }
    return changed ? out : std::string{ini_text};
}

std::size_t RewriteConfigFiles(const fs::path& copied_root, const std::vector<Root>& roots) {
    // Best effort: a config that cannot be rewritten keeps the old paths, the copy itself stands.
    std::size_t changed = 0;
    try {
        std::vector<fs::path> files;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(copied_root, ec)) {
            if (IsIniFile(entry.path())) {
                files.push_back(entry.path());
            }
        }
        for (const char* folder : {"config", "custom"}) {
            ec.clear();
            if (!IsDir(copied_root / folder)) {
                continue;
            }
            for (const auto& entry : fs::recursive_directory_iterator(copied_root / folder, ec)) {
                if (IsIniFile(entry.path())) {
                    files.push_back(entry.path());
                }
            }
        }

        for (const auto& file : files) {
            std::string text;
            {
                std::ifstream in(file, std::ios::binary);
                if (!in) {
                    continue;
                }
                text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            const std::string updated = RewriteIniPaths(text, roots);
            if (updated == text) {
                continue;
            }
            // Written beside the file and renamed over it, so a failure leaves the copied file as
            // it was.
            auto temporary = file;
            temporary += ".rewrite";
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out.write(updated.data(), static_cast<std::streamsize>(updated.size()));
                if (!out) {
                    RemoveQuietly(temporary);
                    continue;
                }
            }
            ec.clear();
            fs::rename(temporary, file, ec);
            if (ec) {
                RemoveQuietly(temporary);
                continue;
            }
            ++changed;
        }
    } catch (const std::exception&) {
    }
    return changed;
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
        std::size_t config_files = 0;
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
            // The copied configs name the old folders in full: point them at the new ones.
            config_files += RewriteConfigFiles(staging, roots);
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
            "Copied the user data from {} to {} ({} files, {:.1f} MiB{}, {} ms){}. The old folder "
            "was left as it was.",
            PathToUTF8String(roots[0].legacy), PathToUTF8String(roots[0].current), stats.files,
            static_cast<double>(stats.bytes) / (1024.0 * 1024.0),
            todo.size() > 1 ? fmt::format(", plus {} more folders", todo.size() - 1)
                            : std::string{},
            ms,
            config_files > 0
                ? fmt::format("; folder paths in {} config files now point at the new folder",
                              config_files)
                : std::string{});
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
