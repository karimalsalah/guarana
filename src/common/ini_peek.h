// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace Common {

/// The value of `key` in `[section]` of an INI text laid out as the configuration writes it (one
/// `key=value` per line, `\n` or `\r\n` line ends), or nothing when the section or the key is
/// missing. For reading a setting before the configuration is loaded.
inline std::optional<std::string> IniValue(std::string_view text, std::string_view section,
                                           std::string_view key) {
    bool in_section = false;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.starts_with('[')) {
            in_section = line.size() == section.size() + 2 && line.back() == ']' &&
                         line.substr(1, section.size()) == section;
            continue;
        }
        if (in_section && line.size() > key.size() && line.starts_with(key) &&
            line[key.size()] == '=') {
            return std::string{line.substr(key.size() + 1)};
        }
    }
    return std::nullopt;
}

/// A boolean setting as the configuration loads it (Config::ReadSettingGeneric): the stored
/// value counts only where `key\default` is `false`; otherwise, or when either is missing, the
/// setting keeps `fallback`, its default. A stored value is true only when it is `true`.
inline bool IniBoolSetting(std::string_view text, std::string_view section, std::string_view key,
                           bool fallback) {
    const std::optional<std::string> is_default =
        IniValue(text, section, std::string{key} + "\\default");
    if (!is_default || *is_default != "false") {
        return fallback;
    }
    const std::optional<std::string> value = IniValue(text, section, key);
    if (!value) {
        return fallback;
    }
    return *value == "true";
}

} // namespace Common
