// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

// Don't move this! put it below and MSVC will get angry!
#include "video_core/vulkan_common/vulkan_wrapper.h"

#ifdef _WIN32
#include <cstring>
#include <processthreadsapi.h>
#include <windows.h>
#else
#include <cstring>
#include <errno.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <fmt/core.h>
#include "common/boot_timing.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "lichii/startup_checks.h"

#ifdef _WIN32
#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "common/string_util.h"
#endif

namespace {
VulkanCheckOutcome vulkan_check_outcome;
} // Anonymous namespace

#ifdef _WIN32
namespace {

namespace fs = std::filesystem;

// The check passed in a child process with these drivers, layers and loader. A start that finds
// the same ones skips the child, which takes as long as the driver needs to create an instance,
// unless a session since then did not end cleanly.
constexpr std::string_view VulkanCheckTag = "lichii-vulkan-check 1\n";
constexpr std::wstring_view DisplayClass =
    L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
constexpr std::wstring_view SoftwareComponentClass =
    L"SYSTEM\\CurrentControlSet\\Control\\Class\\{5c4c3332-344d-483c-8739-259e934c9cc8}";
constexpr std::wstring_view SessionPrefix = L"vulkan_check_session_";

fs::path vulkan_check_session;

fs::path VulkanCheckDir() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
}

fs::path VulkanCheckMarker() {
    return VulkanCheckDir() / "vulkan_check.txt";
}

struct RegistryValue {
    std::wstring name;
    std::string text;
    DWORD number{}; ///< The data of a REG_DWORD
};

std::string RegistryText(DWORD type, const std::vector<BYTE>& data) {
    switch (type) {
    case REG_SZ:
    case REG_EXPAND_SZ:
    case REG_MULTI_SZ: {
        std::wstring text(data.size() / sizeof(wchar_t), L'\0');
        if (!text.empty()) {
            std::memcpy(text.data(), data.data(), text.size() * sizeof(wchar_t));
        }
        while (!text.empty() && text.back() == L'\0') {
            text.pop_back();
        }
        std::replace(text.begin(), text.end(), L'\0', L'|');
        return Common::UTF16ToUTF8(text);
    }
    case REG_DWORD:
        if (data.size() >= sizeof(DWORD)) {
            DWORD number{};
            std::memcpy(&number, data.data(), sizeof(number));
            return std::to_string(number);
        }
        break;
    case REG_QWORD:
        if (data.size() >= sizeof(ULONGLONG)) {
            ULONGLONG number{};
            std::memcpy(&number, data.data(), sizeof(number));
            return std::to_string(number);
        }
        break;
    default:
        break;
    }
    return fmt::format("type {}, {} bytes", type, data.size());
}

std::vector<RegistryValue> ReadRegistryValues(HKEY root, const std::wstring& path) {
    std::vector<RegistryValue> values;
    HKEY key{};
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS) {
        return values;
    }
    SCOPE_EXIT {
        RegCloseKey(key);
    };
    for (DWORD index = 0; index < 4096; ++index) {
        // 16383 characters is the longest value name.
        std::wstring name(16384, L'\0');
        DWORD name_length = static_cast<DWORD>(name.size());
        DWORD type{};
        DWORD size{};
        LSTATUS result = RegEnumValueW(key, index, name.data(), &name_length, nullptr, &type,
                                       nullptr, &size);
        if (result == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (result != ERROR_SUCCESS) {
            continue;
        }
        std::vector<BYTE> data(size);
        name_length = static_cast<DWORD>(name.size());
        result = RegEnumValueW(key, index, name.data(), &name_length, nullptr, &type,
                               data.empty() ? nullptr : data.data(), &size);
        if (result != ERROR_SUCCESS) {
            continue;
        }
        data.resize(size);
        name.resize(name_length);
        RegistryValue value{.name = std::move(name), .text = RegistryText(type, data)};
        if (type == REG_DWORD && data.size() >= sizeof(DWORD)) {
            std::memcpy(&value.number, data.data(), sizeof(DWORD));
        }
        values.push_back(std::move(value));
    }
    return values;
}

std::vector<std::wstring> RegistrySubkeys(HKEY root, const std::wstring& path) {
    std::vector<std::wstring> names;
    HKEY key{};
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS) {
        return names;
    }
    SCOPE_EXIT {
        RegCloseKey(key);
    };
    for (DWORD index = 0; index < 4096; ++index) {
        wchar_t name[256]{};
        DWORD length = static_cast<DWORD>(std::size(name));
        const LSTATUS result =
            RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (result == ERROR_SUCCESS) {
            names.emplace_back(name, length);
        }
    }
    return names;
}

/// Size and write time, so that a file replaced in place changes the key.
std::string FileStamp(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error) {
        return "missing";
    }
    const auto written = fs::last_write_time(path, error);
    return fmt::format("{} bytes, written {}", size,
                       error ? 0 : written.time_since_epoch().count());
}

std::string Upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    });
    return text;
}

/// The variables a layer manifest names to turn its layers on or off.
void AddLayerEnvironmentNames(const fs::path& manifest, std::set<std::string>& names) {
    std::error_code error;
    const auto bytes = fs::file_size(manifest, error);
    if (error || bytes > 1024 * 1024) {
        return;
    }
    std::ifstream file{manifest};
    if (!file) {
        return;
    }
    const auto json = nlohmann::json::parse(file, nullptr, false);
    if (!json.is_object()) {
        return;
    }
    const auto add = [&names](const nlohmann::json& layer) {
        if (!layer.is_object()) {
            return;
        }
        for (const char* control : {"enable_environment", "disable_environment"}) {
            const auto it = layer.find(control);
            if (it == layer.end() || !it->is_object()) {
                continue;
            }
            for (const auto& item : it->items()) {
                names.insert(Upper(item.key()));
            }
        }
    };
    if (const auto layer = json.find("layer"); layer != json.end()) {
        add(*layer);
    }
    if (const auto layers = json.find("layers"); layers != json.end() && layers->is_array()) {
        for (const auto& layer : *layers) {
            add(layer);
        }
    }
}

/// "NAME=value" of the variables that steer the loader or turn layers on or off.
std::vector<std::string> LoaderEnvironment(const std::set<std::string>& layer_names) {
    std::vector<std::string> entries;
    wchar_t* const block = GetEnvironmentStringsW();
    if (block == nullptr) {
        return entries;
    }
    SCOPE_EXIT {
        FreeEnvironmentStringsW(block);
    };
    for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
        const std::string text = Common::UTF16ToUTF8(std::wstring_view{entry});
        // The hidden per-drive entries start with '='.
        const auto equals = text.find('=', 1);
        if (text.empty() || text[0] == '=' || equals == std::string::npos) {
            continue;
        }
        const std::string name = Upper(text.substr(0, equals));
        if (name.starts_with("VK_") || name.starts_with("ENABLE_VULKAN_") ||
            name.starts_with("DISABLE_VULKAN_") || layer_names.contains(name)) {
            entries.push_back(text);
        }
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

struct DriverKey {
    std::string text;    ///< What the marker holds after a pass
    std::string drivers; ///< The display drivers, for the log
};

/// What the Vulkan loader would load, read without loading it: each display driver's version and
/// Vulkan entries, the drivers and implicit layers registered with the loader (a newly installed
/// overlay layer is what crashes most), the loader itself, the Windows build, and the
/// environment that steers the loader.
DriverKey VulkanDriverKey() {
    DriverKey key;
    key.text = VulkanCheckTag;
    const auto add_class = [&key](std::wstring_view class_path, std::string_view label,
                                  bool display) {
        const std::wstring base{class_path};
        for (const std::wstring& device : RegistrySubkeys(HKEY_LOCAL_MACHINE, base)) {
            std::string line;
            std::string description;
            std::string version;
            bool vulkan = false;
            for (const RegistryValue& value :
                 ReadRegistryValues(HKEY_LOCAL_MACHINE, base + L"\\" + device)) {
                const bool is_vulkan = _wcsnicmp(value.name.c_str(), L"Vulkan", 6) == 0;
                const bool is_description = _wcsicmp(value.name.c_str(), L"DriverDesc") == 0;
                const bool is_version = _wcsicmp(value.name.c_str(), L"DriverVersion") == 0;
                if (!is_vulkan && !is_description && !is_version) {
                    continue;
                }
                vulkan = vulkan || is_vulkan;
                if (is_description) {
                    description = value.text;
                }
                if (is_version) {
                    version = value.text;
                }
                line += fmt::format(" {}={};", Common::UTF16ToUTF8(value.name), value.text);
            }
            if (!vulkan) {
                continue;
            }
            key.text += fmt::format("{} {}:{}\n", label, Common::UTF16ToUTF8(device), line);
            if (display) {
                if (!key.drivers.empty()) {
                    key.drivers += ", ";
                }
                key.drivers += fmt::format("{} {}", description, version);
            }
        }
    };
    add_class(DisplayClass, "display", true);
    add_class(SoftwareComponentClass, "component", false);

    std::set<std::string> layer_environment;
    const std::pair<HKEY, const char*> roots[] = {{HKEY_LOCAL_MACHINE, "HKLM"},
                                                  {HKEY_CURRENT_USER, "HKCU"}};
    for (const auto& [root, root_name] : roots) {
        for (const wchar_t* list :
             {L"Drivers", L"ImplicitLayers", L"LoaderSettings", L"Settings"}) {
            for (const RegistryValue& value :
                 ReadRegistryValues(root, std::wstring{L"SOFTWARE\\Khronos\\Vulkan\\"} + list)) {
                const fs::path file{value.name};
                key.text += fmt::format("{} {}: {}={}, {}\n", root_name,
                                        Common::UTF16ToUTF8(list),
                                        Common::UTF16ToUTF8(value.name), value.text,
                                        FileStamp(file));
                if (std::wstring_view{list} == L"ImplicitLayers" && value.number == 0) {
                    AddLayerEnvironmentNames(file, layer_environment);
                }
            }
        }
    }

    std::error_code error;
    fs::path loader = Common::FS::GetExeDirectory() / "vulkan-1.dll";
    if (!fs::exists(loader, error)) {
        wchar_t system[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(system, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            loader = fs::path{std::wstring_view{system, length}} / "vulkan-1.dll";
        }
    }
    key.text += fmt::format("loader {}: {}\n", Common::FS::PathToUTF8String(loader),
                            FileStamp(loader));

    for (const RegistryValue& value : ReadRegistryValues(
             HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion")) {
        if (_wcsicmp(value.name.c_str(), L"CurrentBuild") == 0 ||
            _wcsicmp(value.name.c_str(), L"UBR") == 0) {
            key.text +=
                fmt::format("windows {}={}\n", Common::UTF16ToUTF8(value.name), value.text);
        }
    }
    for (const std::string& entry : LoaderEnvironment(layer_environment)) {
        key.text += fmt::format("env {}\n", entry);
    }
    if (key.drivers.empty()) {
        key.drivers = "no display driver lists a Vulkan driver";
    }
    return key;
}

std::string ReadWholeFile(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return {};
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

/// Creation time of a process, which with its id names it for good; 0 when it cannot be read.
ULONGLONG ProcessStart(HANDLE process) {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return 0;
    }
    return (ULONGLONG{creation.dwHighDateTime} << 32) | creation.dwLowDateTime;
}

bool OwnerAlive(DWORD pid, ULONGLONG start) {
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

/// Removes the session files of sessions that are gone. Each was left by a session that did not
/// end cleanly (a crash, a kill or a power loss), since a clean end removes its own.
bool CollectUncleanSessions() {
    std::vector<fs::path> ended;
    std::error_code error;
    for (fs::directory_iterator it{VulkanCheckDir(), error}, end; !error && it != end;
         it.increment(error)) {
        const std::wstring name = it->path().filename().wstring();
        if (!name.starts_with(SessionPrefix) || !name.ends_with(L".txt")) {
            continue;
        }
        std::ifstream file{it->path()};
        DWORD pid{};
        ULONGLONG start{};
        if (file >> pid >> start && OwnerAlive(pid, start)) {
            // Another instance that is still running.
            continue;
        }
        ended.push_back(it->path());
    }
    for (const fs::path& path : ended) {
        std::error_code remove_error;
        fs::remove(path, remove_error);
    }
    return !ended.empty();
}

void BeginVulkanCheckSession() {
    std::error_code error;
    if (!fs::is_directory(VulkanCheckDir(), error)) {
        return;
    }
    const DWORD pid = GetCurrentProcessId();
    const fs::path path = VulkanCheckDir() / fmt::format("vulkan_check_session_{}.txt", pid);
    std::ofstream file{path, std::ios::trunc};
    file << pid << ' ' << ProcessStart(GetCurrentProcess()) << '\n';
    file.flush();
    if (file.good()) {
        vulkan_check_session = path;
    }
}

/// Written only into a configuration folder that exists: on the first start the folder is made
/// later, and the profile import that may run first must find it empty.
void WriteVulkanCheckMarker(const std::string& text) {
    std::error_code error;
    if (!fs::is_directory(VulkanCheckDir(), error)) {
        return;
    }
    std::ofstream file{VulkanCheckMarker(), std::ios::binary | std::ios::trunc};
    file << text;
}

} // Anonymous namespace
#endif

void CheckVulkan() {
    // Just start the Vulkan loader, this will crash if something is wrong
    try {
        Vulkan::vk::InstanceDispatch dld;
        const auto library = Vulkan::OpenLibrary();
        const Vulkan::vk::Instance instance =
            Vulkan::CreateInstance(*library, dld, VK_API_VERSION_1_1);

    } catch (const Vulkan::vk::Exception& exception) {
        fmt::print(stderr, "Failed to initialize Vulkan: {}\n", exception.what());
    }
}

bool CheckEnvVars(bool* is_child) {
#ifdef _WIN32
    // Check environment variable to see if we are the child
    char variable_contents[8];
    const DWORD startup_check_var =
        GetEnvironmentVariableA(STARTUP_CHECK_ENV_VAR, variable_contents, 8);
    if (startup_check_var > 0 && std::strncmp(variable_contents, ENV_VAR_ENABLED_TEXT, 8) == 0) {
        CheckVulkan();
        return true;
    }

    // Don't perform startup checks if we are a child process
    char is_child_s[8];
    const DWORD is_child_len = GetEnvironmentVariableA(IS_CHILD_ENV_VAR, is_child_s, 8);
    if (is_child_len > 0 && std::strncmp(is_child_s, ENV_VAR_ENABLED_TEXT, 8) == 0) {
        *is_child = true;
        return false;
    } else if (!SetEnvironmentVariableA(IS_CHILD_ENV_VAR, ENV_VAR_ENABLED_TEXT)) {
        fmt::print(stderr, "SetEnvironmentVariableA failed to set {} with error {}\n",
                   IS_CHILD_ENV_VAR, GetLastError());
        return true;
    }
#else
    const char* startup_check_var = getenv(STARTUP_CHECK_ENV_VAR);
    if (startup_check_var != nullptr &&
        std::strncmp(startup_check_var, ENV_VAR_ENABLED_TEXT, 8) == 0) {
        CheckVulkan();
        return true;
    }
#endif
    return false;
}

bool StartupChecks(const char* arg0, bool* has_broken_vulkan, bool perform_vulkan_check) {
#ifdef _WIN32
    // Set the startup variable for child processes
    const bool env_var_set = SetEnvironmentVariableA(STARTUP_CHECK_ENV_VAR, ENV_VAR_ENABLED_TEXT);
    if (!env_var_set) {
        fmt::print(stderr, "SetEnvironmentVariableA failed to set {} with error {}\n",
                   STARTUP_CHECK_ENV_VAR, GetLastError());
        return false;
    }

    if (perform_vulkan_check) {
        vulkan_check_outcome.start = Common::BootTiming::Now();
        const DriverKey key = VulkanDriverKey();
        const bool unclean = CollectUncleanSessions();
        const std::string marker = ReadWholeFile(VulkanCheckMarker());
        if (!unclean && marker == key.text) {
            vulkan_check_outcome.reason =
                fmt::format("skipped, it passed before with these drivers ({})", key.drivers);
        } else {
            vulkan_check_outcome.ran = true;
            vulkan_check_outcome.reason = fmt::format(
                "ran in a child process: {} ({})",
                unclean          ? "a session since the last pass did not end cleanly"
                : marker.empty() ? "no pass on record"
                                 : "the drivers, layers or loader changed since the last pass",
                key.drivers);

            // No marker until this check passes: a start killed while it runs checks again.
            std::error_code remove_error;
            std::filesystem::remove(VulkanCheckMarker(), remove_error);

            // Spawn child process that performs Vulkan check
            PROCESS_INFORMATION process_info;
            std::memset(&process_info, '\0', sizeof(process_info));

            if (!SpawnChild(arg0, &process_info, 0)) {
                vulkan_check_outcome.end = Common::BootTiming::Now();
                return false;
            }

            // Wait until the process exits and get exit code from it
            WaitForSingleObject(process_info.hProcess, INFINITE);
            DWORD exit_code = STILL_ACTIVE;
            const int err = GetExitCodeProcess(process_info.hProcess, &exit_code);
            if (err == 0) {
                fmt::print(stderr, "GetExitCodeProcess failed with error {}\n", GetLastError());
            }

            // Vulkan is broken if the child crashed (return value is not zero)
            *has_broken_vulkan = (exit_code != 0);

            if (CloseHandle(process_info.hProcess) == 0) {
                fmt::print(stderr, "CloseHandle failed with error {}\n", GetLastError());
            }
            if (CloseHandle(process_info.hThread) == 0) {
                fmt::print(stderr, "CloseHandle failed with error {}\n", GetLastError());
            }

            if (*has_broken_vulkan) {
                vulkan_check_outcome.reason += fmt::format(", and it failed (exit code {})",
                                                           exit_code);
            } else {
                WriteVulkanCheckMarker(key.text);
            }
        }
        vulkan_check_outcome.end = Common::BootTiming::Now();
        BeginVulkanCheckSession();
    }

    if (!SetEnvironmentVariableA(STARTUP_CHECK_ENV_VAR, nullptr)) {
        fmt::print(stderr, "SetEnvironmentVariableA failed to clear {} with error {}\n",
                   STARTUP_CHECK_ENV_VAR, GetLastError());
    }

#else
    const int env_var_set = setenv(STARTUP_CHECK_ENV_VAR, ENV_VAR_ENABLED_TEXT, 1);
    if (env_var_set == -1) {
        const int err = errno;
        fmt::print(stderr, "setenv failed to set {} with error {}\n", STARTUP_CHECK_ENV_VAR, err);
        return false;
    }

    if (perform_vulkan_check) {
        vulkan_check_outcome.ran = true;
        vulkan_check_outcome.start = Common::BootTiming::Now();
        vulkan_check_outcome.reason = "ran in a child process, as on every start on this system";
        const pid_t pid = SpawnChild(arg0);
        if (pid == -1) {
            vulkan_check_outcome.end = Common::BootTiming::Now();
            return false;
        }

        // Get exit code from child process
        int status;
        const int r_val = waitpid(pid, &status, 0);
        vulkan_check_outcome.end = Common::BootTiming::Now();
        if (r_val == -1) {
            const int err = errno;
            fmt::print(stderr, "wait failed with error {}\n", err);
            return false;
        }
        // Vulkan is broken if the child crashed (return value is not zero)
        *has_broken_vulkan = (status != 0);
    }

    const int env_var_cleared = unsetenv(STARTUP_CHECK_ENV_VAR);
    if (env_var_cleared == -1) {
        const int err = errno;
        fmt::print(stderr, "unsetenv failed to clear {} with error {}\n", STARTUP_CHECK_ENV_VAR,
                   err);
    }
#endif
    return false;
}

const VulkanCheckOutcome& GetVulkanCheckOutcome() {
    return vulkan_check_outcome;
}

void EndVulkanCheckSession() {
#ifdef _WIN32
    if (!vulkan_check_session.empty()) {
        std::error_code error;
        std::filesystem::remove(vulkan_check_session, error);
        vulkan_check_session.clear();
    }
#endif
}

#ifdef _WIN32
bool SpawnChild(const char* arg0, PROCESS_INFORMATION* pi, int flags) {
    STARTUPINFOA startup_info;

    std::memset(&startup_info, '\0', sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);

    char p_name[255];
    std::strncpy(p_name, arg0, 254);
    p_name[254] = '\0';

    const bool process_created = CreateProcessA(nullptr,       // lpApplicationName
                                                p_name,        // lpCommandLine
                                                nullptr,       // lpProcessAttributes
                                                nullptr,       // lpThreadAttributes
                                                false,         // bInheritHandles
                                                flags,         // dwCreationFlags
                                                nullptr,       // lpEnvironment
                                                nullptr,       // lpCurrentDirectory
                                                &startup_info, // lpStartupInfo
                                                pi             // lpProcessInformation
    );
    if (!process_created) {
        fmt::print(stderr, "CreateProcessA failed with error {}\n", GetLastError());
        return false;
    }

    return true;
}
#else
pid_t SpawnChild(const char* arg0) {
    const pid_t pid = fork();

    if (pid == -1) {
        // error
        const int err = errno;
        fmt::print(stderr, "fork failed with error {}\n", err);
        return pid;
    } else if (pid == 0) {
        // child
        execlp(arg0, arg0, nullptr);
        const int err = errno;
        fmt::print(stderr, "execl failed with error {}\n", err);
        _exit(0);
    }

    return pid;
}
#endif
