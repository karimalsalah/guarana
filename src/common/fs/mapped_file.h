// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <span>

#include "common/common_types.h"

namespace Common::FS {

/// A file mapped read-only (MapViewOfFile on Windows, mmap on POSIX), as many bytes as it held
/// when it was mapped: bytes appended later are not part of it. Others may read and append to the
/// file while it is mapped, and must not cut it shorter (on POSIX a read past its new end faults).
/// On POSIX a file renamed over it leaves the mapped bytes as they were; on Windows, replacing or
/// deleting the file can fail while a view of it is mapped. Copies share the mapping, which ends
/// with the last of them.
class MappedFile {
public:
    MappedFile() = default;

    /// Maps the whole file. Unmapped (IsMapped is false) when the file is missing, empty or
    /// can't be mapped.
    explicit MappedFile(const std::filesystem::path& path);

    [[nodiscard]] bool IsMapped() const noexcept {
        return view != nullptr;
    }

    [[nodiscard]] std::span<const u8> Bytes() const noexcept {
        return bytes;
    }

private:
    std::shared_ptr<const void> view;
    std::span<const u8> bytes;
};

} // namespace Common::FS
