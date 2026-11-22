// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

constexpr char IS_CHILD_ENV_VAR[] = "LICHII_IS_CHILD";
constexpr char STARTUP_CHECK_ENV_VAR[] = "LICHII_DO_STARTUP_CHECKS";
constexpr char ENV_VAR_ENABLED_TEXT[] = "ON";

void CheckVulkan();
bool CheckEnvVars(bool* is_child);
bool StartupChecks(const char* arg0, bool* has_broken_vulkan, bool perform_vulkan_check);

/// What StartupChecks did about Vulkan, for the log once it is open. Times are
/// Common::BootTiming::Now() values; both are 0 when no check was asked for.
struct VulkanCheckOutcome {
    bool ran{};
    /// EDEN_NX2_STARTUP_CHECK_ASYNC: the child still runs; `end` is 0 until FinishVulkanCheck
    bool beside_start{};
    double start{};
    double end{};
    std::string reason;
};
[[nodiscard]] const VulkanCheckOutcome& GetVulkanCheckOutcome();

/// The Vulkan check's verdict, true when Vulkan is broken. With EDEN_NX2_STARTUP_CHECK_ASYNC=1
/// StartupChecks leaves the check's child running while the frontend starts; this waits for it,
/// and must run before the first use of Vulkan. Without a running child it returns false.
bool FinishVulkanCheck();

/// The saved "Perform Vulkan check" setting (Debug), read from the configuration file: the
/// check runs before the configuration is loaded. True when it is not saved.
[[nodiscard]] bool PerformVulkanCheckSetting();

/// The session ended without a crash. On Windows a start finds the session file of one that did
/// not end this way and checks Vulkan again.
void EndVulkanCheckSession();

#ifdef _WIN32
bool SpawnChild(const char* arg0, PROCESS_INFORMATION* pi, int flags);
#else
pid_t SpawnChild(const char* arg0);
#endif
