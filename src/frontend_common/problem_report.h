// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// The text of a problem report (Help > Report a problem): what goes into the zip and the issue
// link, without the user's name, home folder or computer name.
namespace FrontendCommon::ProblemReport {

struct Redaction {
    std::string text; ///< Matched without regard to ASCII case
    std::string replacement;
};

/// The home folder (with either slash, and with escaped backslashes), then the user and computer
/// names. Each is replaced only where no letter, digit or '_' adjoins it. Names shorter than three
/// characters, and generic ones such as "user", are left alone: they would match all over the text.
[[nodiscard]] std::vector<Redaction> MakeRedactions(std::string_view home,
                                                    std::span<const std::string> users,
                                                    std::span<const std::string> hosts);

[[nodiscard]] std::string Scrub(std::string_view text, std::span<const Redaction> redactions);

/// Every byte but A-Z a-z 0-9 - . _ ~ as %XX.
[[nodiscard]] std::string PercentEncode(std::string_view text);

struct IssueField {
    std::string name; ///< "title" or a form field's id
    std::string value;
    /// When not empty, the value may be cut to fit the link; it then ends with this line.
    std::string shortened_note;
};

/// `base` (the form's link up to its template) and "&name=value" per field, percent-encoded. A
/// link longer than `max_length` cuts the values that may be cut, the last field first, at a line
/// end where it can.
[[nodiscard]] std::string IssueUrl(std::string_view base, std::vector<IssueField> fields,
                                   std::size_t max_length);

/// `head` and `tail` (the start and end of a longer text, `left_out` bytes apart) joined at line
/// ends by a line saying how much was left out.
[[nodiscard]] std::string JoinEnds(std::string_view head, std::string_view tail,
                                   std::size_t left_out);

/// A text longer than head + tail bytes as JoinEnds of its first `head` and last `tail` bytes.
[[nodiscard]] std::string KeepEnds(std::string_view text, std::size_t head, std::size_t tail);

/// The text after `marker` on the last line that has it, to the end of that line.
[[nodiscard]] std::optional<std::string> LastValue(std::string_view log, std::string_view marker);

struct BootedGame {
    u64 program_id{};
    std::string title;
    std::string version;
};

/// The last "Booting game: <program id> | <title> | <version>" line of a log.
[[nodiscard]] std::optional<BootedGame> LastBootedGame(std::string_view log);

} // namespace FrontendCommon::ProblemReport
