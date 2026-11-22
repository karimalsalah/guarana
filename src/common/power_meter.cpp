// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <cfgmgr32.h>
#include <winioctl.h>
#endif

#include "common/common_types.h"
#include "common/power_meter.h"

namespace Common {

#ifdef _WIN32

namespace {

// The interface of <emi.h>, which older SDKs and MinGW lack.
constexpr DWORD EmiGetVersion = CTL_CODE(FILE_DEVICE_UNKNOWN, 0, METHOD_BUFFERED, FILE_READ_ACCESS);
constexpr DWORD EmiGetMetadataSize =
    CTL_CODE(FILE_DEVICE_UNKNOWN, 1, METHOD_BUFFERED, FILE_READ_ACCESS);
constexpr DWORD EmiGetMetadata = CTL_CODE(FILE_DEVICE_UNKNOWN, 2, METHOD_BUFFERED, FILE_READ_ACCESS);
constexpr DWORD EmiGetMeasurement =
    CTL_CODE(FILE_DEVICE_UNKNOWN, 3, METHOD_BUFFERED, FILE_READ_ACCESS);

// EMI_METADATA_V1: unit (4), OEM (32), model (32), revision (2), name size (2), name.
constexpr size_t V1NameSize = 70;
constexpr size_t V1Name = 72;
// EMI_METADATA_V2: OEM (32), model (32), revision (2), channel count (2), channels.
constexpr size_t V2ChannelCount = 66;
constexpr size_t V2Channels = 68;
// EMI_CHANNEL_V2: unit (4), name size in bytes (2), name; the next one follows the name.
constexpr size_t V2ChannelNameSize = 4;
constexpr size_t V2ChannelName = 6;

/// EMI_CHANNEL_MEASUREMENT_DATA
struct Measurement {
    u64 energy; ///< Picowatt-hours
    u64 time;   ///< 100 ns units
};
static_assert(sizeof(Measurement) == 16);

enum class Kind { Other, Package, Cpu, Gpu };

struct Channel {
    std::string name;
    Kind kind{};
    Measurement last{};
};

struct Device {
    HANDLE handle{};
    std::vector<Channel> channels;
};

u16 ReadU16(const std::vector<u8>& bytes, size_t offset) {
    u16 value = 0;
    if (offset + sizeof(value) <= bytes.size()) {
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
    }
    return value;
}

/// A UTF-16 name of `size` bytes at `offset`; channel names are ASCII.
std::string ReadName(const std::vector<u8>& bytes, size_t offset, size_t size) {
    std::string name;
    for (size_t i = 0; i + 1 < size && offset + i + 1 < bytes.size(); i += 2) {
        const u16 unit = ReadU16(bytes, offset + i);
        if (unit == 0) {
            break;
        }
        name.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
    }
    return name;
}

Kind KindOf(std::string_view name) {
    std::string upper{name};
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (upper.find("PKG") != std::string::npos) {
        return Kind::Package;
    }
    if (upper.find("PP0") != std::string::npos) {
        return Kind::Cpu;
    }
    if (upper.find("PP1") != std::string::npos) {
        return Kind::Gpu;
    }
    return Kind::Other;
}

bool Control(HANDLE handle, DWORD code, void* out, size_t size, DWORD* returned) {
    return DeviceIoControl(handle, code, nullptr, 0, out, static_cast<DWORD>(size), returned,
                           nullptr) != FALSE;
}

std::vector<Channel> ReadChannels(HANDLE handle) {
    DWORD returned = 0;
    u16 version = 0;
    if (!Control(handle, EmiGetVersion, &version, sizeof(version), &returned) ||
        returned < sizeof(version) || (version != 1 && version != 2)) {
        return {};
    }
    u32 size = 0;
    if (!Control(handle, EmiGetMetadataSize, &size, sizeof(size), &returned) ||
        returned < sizeof(size) || size < V1Name || size > (1U << 20)) {
        return {};
    }
    std::vector<u8> metadata(size);
    if (!Control(handle, EmiGetMetadata, metadata.data(), metadata.size(), &returned)) {
        return {};
    }
    metadata.resize((std::min)(metadata.size(), static_cast<size_t>(returned)));
    std::vector<Channel> channels;
    if (version == 1) {
        Channel& channel = channels.emplace_back();
        channel.name = ReadName(metadata, V1Name, ReadU16(metadata, V1NameSize));
    } else {
        const size_t count = ReadU16(metadata, V2ChannelCount);
        size_t offset = V2Channels;
        for (size_t i = 0; i < count && offset + V2ChannelName <= metadata.size(); ++i) {
            const size_t name_size = ReadU16(metadata, offset + V2ChannelNameSize);
            Channel& channel = channels.emplace_back();
            channel.name = ReadName(metadata, offset + V2ChannelName, name_size);
            offset += V2ChannelName + name_size;
        }
        if (channels.size() != count) {
            return {};
        }
    }
    for (Channel& channel : channels) {
        channel.kind = KindOf(channel.name);
    }
    return channels;
}

std::vector<Device> OpenDevices() {
    // GUID_DEVICE_ENERGY_METER
    GUID guid{0x45bd8344, 0x7ed6, 0x49cf, {0xa4, 0x40, 0xc2, 0x76, 0xc9, 0x33, 0xb0, 0x53}};
    ULONG length = 0;
    if (CM_Get_Device_Interface_List_SizeW(&length, &guid, nullptr,
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        length <= 1) {
        return {};
    }
    std::vector<wchar_t> paths(length);
    if (CM_Get_Device_Interface_ListW(&guid, nullptr, paths.data(), length,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        return {};
    }
    paths.back() = L'\0';
    std::vector<Device> devices;
    for (const wchar_t* path = paths.data(); *path != L'\0'; path += std::wcslen(path) + 1) {
        const HANDLE handle =
            CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            continue;
        }
        std::vector<Channel> channels = ReadChannels(handle);
        if (channels.empty()) {
            CloseHandle(handle);
            continue;
        }
        Device& device = devices.emplace_back();
        device.handle = handle;
        device.channels = std::move(channels);
    }
    return devices;
}

} // Anonymous namespace

struct PowerMeter::Impl {
    std::mutex mutex;
    std::vector<Device> devices;
    std::vector<Measurement> buffer;
    bool available{};
    bool primed{};
};

PowerMeter::PowerMeter() : impl{std::make_unique<Impl>()} {
    impl->devices = OpenDevices();
    for (const Device& device : impl->devices) {
        for (const Channel& channel : device.channels) {
            impl->available |= channel.kind != Kind::Other;
        }
    }
}

PowerMeter::~PowerMeter() {
    for (const Device& device : impl->devices) {
        CloseHandle(device.handle);
    }
}

bool PowerMeter::Available() const {
    return impl->available;
}

PowerMeter::Sample PowerMeter::Read() {
    std::scoped_lock lock{impl->mutex};
    Sample sample{};
    bool measured = false;
    for (Device& device : impl->devices) {
        // Both versions answer with one EMI_CHANNEL_MEASUREMENT_DATA per channel.
        impl->buffer.assign(device.channels.size(), Measurement{});
        const size_t size = impl->buffer.size() * sizeof(Measurement);
        DWORD returned = 0;
        if (!Control(device.handle, EmiGetMeasurement, impl->buffer.data(), size, &returned) ||
            returned < size) {
            continue;
        }
        for (size_t i = 0; i < device.channels.size(); ++i) {
            Channel& channel = device.channels[i];
            const Measurement now = impl->buffer[i];
            const Measurement last = channel.last;
            channel.last = now;
            if (!impl->primed || now.time <= last.time || now.energy < last.energy) {
                continue;
            }
            // pWh per 100 ns to watts: 1e-12 * 3600 J over 1e-7 s.
            const double watts = static_cast<double>(now.energy - last.energy) * 0.036 /
                                 static_cast<double>(now.time - last.time);
            switch (channel.kind) {
            case Kind::Package:
                sample.package_w += watts;
                break;
            case Kind::Cpu:
                sample.cpu_w += watts;
                break;
            case Kind::Gpu:
                sample.gpu_w += watts;
                break;
            default:
                continue;
            }
            measured = true;
        }
    }
    sample.valid = measured;
    impl->primed = true;
    return sample;
}

std::string PowerMeter::ChannelNames() const {
    std::string names;
    for (const Device& device : impl->devices) {
        for (const Channel& channel : device.channels) {
            names += names.empty() ? "" : ", ";
            names += channel.name;
        }
    }
    return names.empty() ? "none" : names;
}

double ProcessCpuSeconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        return 0.0;
    }
    const auto ticks = [](const FILETIME& time) {
        return (u64{time.dwHighDateTime} << 32) | time.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) * 1e-7;
}

#else

struct PowerMeter::Impl {};

PowerMeter::PowerMeter() = default;

PowerMeter::~PowerMeter() = default;

bool PowerMeter::Available() const {
    return false;
}

PowerMeter::Sample PowerMeter::Read() {
    return {};
}

std::string PowerMeter::ChannelNames() const {
    return "none";
}

double ProcessCpuSeconds() {
    return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
}

#endif

PowerMeter& PowerMeter::Instance() {
    static PowerMeter meter;
    return meter;
}

} // namespace Common
