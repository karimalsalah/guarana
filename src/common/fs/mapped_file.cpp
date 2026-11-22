// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/fs/mapped_file.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Common::FS {

#ifdef _WIN32

MappedFile::MappedFile(const std::filesystem::path& path) {
    // Others may append to the file, and rename another file over it, while it is mapped.
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER length{};
    void* pointer = nullptr;
    u64 size = 0;
    if (GetFileSizeEx(file, &length) != FALSE && length.QuadPart > 0) {
        size = static_cast<u64>(length.QuadPart);
        // Sized to the length seen: fails when the file was cut shorter since
        const HANDLE section =
            CreateFileMappingW(file, nullptr, PAGE_READONLY, static_cast<DWORD>(size >> 32),
                               static_cast<DWORD>(size & 0xffffffffu), nullptr);
        if (section != nullptr) {
            pointer = MapViewOfFile(section, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(size));
            // The view keeps the section and the file open
            CloseHandle(section);
        }
    }
    CloseHandle(file);
    if (pointer == nullptr) {
        return;
    }
    view = std::shared_ptr<const void>(
        pointer, [](const void* mapped) { static_cast<void>(UnmapViewOfFile(mapped)); });
    bytes = std::span<const u8>{static_cast<const u8*>(pointer), static_cast<size_t>(size)};
}

#else

MappedFile::MappedFile(const std::filesystem::path& path) {
    const int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (file == -1) {
        return;
    }
    struct stat status {};
    void* pointer = MAP_FAILED;
    size_t size = 0;
    if (::fstat(file, &status) == 0 && status.st_size > 0) {
        size = static_cast<size_t>(status.st_size);
        pointer = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file, 0);
    }
    // The mapping keeps the file
    ::close(file);
    if (pointer == MAP_FAILED) {
        return;
    }
    view = std::shared_ptr<const void>(pointer, [size](const void* mapped) {
        static_cast<void>(::munmap(const_cast<void*>(mapped), size));
    });
    bytes = std::span<const u8>{static_cast<const u8*>(pointer), size};
}

#endif

} // namespace Common::FS
