// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <optional>
#include "common/common_types.h"
#include "network/room.h"

namespace Core {

class System;
struct LaunchParams {
    std::string argv0{};
    std::string filepath{};
    std::string nickname{};
    std::string password{};
    std::string address{};
    std::string input_profile{};
    std::string program_args{};
    std::optional<std::string> config_path{};
    std::optional<int> selected_user{};
    std::optional<u16> override_gdb_port{};
    std::optional<std::string> log_filter{};
    // --pack-nup <dump> [<out.nup>]: packs a loose dump instead of booting a game (eden-cli).
    std::optional<std::string> pack_nup_source{};
    // --unpack-nup <file.nup> [<out_dir>]: extracts a NUP back into a loose dump.
    std::optional<std::string> unpack_nup_source{};
    // --edit-nup <file.nup>: rewrites default-language metadata in place.
    std::optional<std::string> edit_nup_source{};
    std::optional<std::string> edit_nup_name{};
    std::optional<std::string> edit_nup_icon{};
    std::optional<std::string> edit_nup_version{};
    std::optional<std::string> edit_nup_publisher{};
    // --warm-pipelines <title id, hex>: builds a title's cached pipelines into the driver's
    // caches without booting it, then exits (eden-cli, docs/warm-up.md).
    std::optional<std::string> warm_pipelines_title{};
    u16 port = Network::DefaultRoomPort;
    bool use_multiplayer = false;
    bool fullscreen = false;
    bool force_null_render = false;
    bool force_single_core = false;
    // --launcher-session: the Guarana launcher started the game. The Qt frontend shows only the
    // fullscreen game, never its desktop window, and exits when the emulation stops.
    bool launcher_session = false;
    // print
    bool print_version = false;
    bool print_help = false;
    // qt
    bool launch_hlaunch = false;
    bool launch_qlaunch = false;
    bool launch_setup = false;
};

LaunchParams ParseLaunchParams(Core::System& system, int argc, char *argv[], wchar_t *argv_w[]) noexcept;
void ApplyLaunchParams(LaunchParams const& lp) noexcept;

}
