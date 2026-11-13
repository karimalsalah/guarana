// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include <QStringList>

#include "lichii/compatibility_report.h"

class QWidget;

namespace ProblemReport {

struct Context {
    /// The game booted in this session, with the GPU and driver it ran on.
    std::optional<CompatibilityReporting::Session> session;
    /// The settings' device list, when it was listed.
    QStringList vulkan_devices;
};

/// Help > Report a problem: asks what went wrong, writes a zip with this session's and the last
/// session's logs and the PC's and game's details, without the user name, home folder and
/// computer name, next to the log folder, opens that folder and opens the GitHub issue form filled
/// in with a summary. It sends nothing itself.
void Run(QWidget* parent, const Context& context);

} // namespace ProblemReport
