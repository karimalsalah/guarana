// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend_common/problem_report.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <system_error>

#include <fmt/format.h>

namespace FrontendCommon::ProblemReport {
namespace {

constexpr char LowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Bytes of UTF-8 letters count, so that a name is not cut out of a longer word.
constexpr bool IsWordByte(char c) {
    const auto byte = static_cast<unsigned char>(c);
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') || byte == '_' || byte >= 0x80;
}

constexpr bool IsUnreserved(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

/// Bytes of the UTF-8 sequence `lead` starts (1 for a byte that starts none).
constexpr std::size_t SequenceLength(unsigned char lead) {
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

bool SameText(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return LowerAscii(x) == LowerAscii(y);
           });
}

bool MatchesAt(std::string_view text, std::size_t at, std::string_view needle) {
    return at + needle.size() <= text.size() && SameText(text.substr(at, needle.size()), needle);
}

std::string Replace(std::string_view text, const Redaction& redaction) {
    const std::string_view needle = redaction.text;
    std::string result;
    result.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        if (!needle.empty() && LowerAscii(text[at]) == LowerAscii(needle[0]) &&
            MatchesAt(text, at, needle) && (at == 0 || !IsWordByte(text[at - 1])) &&
            (at + needle.size() == text.size() || !IsWordByte(text[at + needle.size()]))) {
            result += redaction.replacement;
            at += needle.size();
        } else {
            result += text[at];
            ++at;
        }
    }
    return result;
}

} // Anonymous namespace

std::vector<Redaction> MakeRedactions(std::string_view home, std::span<const std::string> users,
                                      std::span<const std::string> hosts) {
    std::vector<Redaction> redactions;
    const auto add = [&redactions](const std::string& text, std::string_view replacement) {
        static constexpr std::array<std::string_view, 7> generic{
            "user", "users", "admin", "administrator", "root", "guest", "localhost"};
        if (text.size() < 3 ||
            std::ranges::any_of(generic, [&text](std::string_view name) {
                return SameText(text, name);
            }) ||
            std::ranges::any_of(redactions, [&text](const Redaction& redaction) {
                return SameText(text, redaction.text);
            })) {
            return;
        }
        redactions.push_back({.text = text, .replacement = std::string{replacement}});
    };

    std::string path{home};
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) {
        path.pop_back();
    }
    std::string slashes = path;
    std::replace(slashes.begin(), slashes.end(), '\\', '/');
    std::string backslashes = path;
    std::replace(backslashes.begin(), backslashes.end(), '/', '\\');
    std::string escaped;
    for (const char c : backslashes) {
        escaped += c;
        if (c == '\\') {
            escaped += '\\';
        }
    }
    // A path with no folder in it would match too much.
    if (slashes.find('/', 1) != std::string::npos) {
        add(escaped, "<home>");
        add(backslashes, "<home>");
        add(slashes, "<home>");
    }
    for (const std::string& user : users) {
        add(user, "<user>");
    }
    for (const std::string& host : hosts) {
        add(host, "<host>");
    }
    return redactions;
}

std::string Scrub(std::string_view text, std::span<const Redaction> redactions) {
    std::string result{text};
    for (const Redaction& redaction : redactions) {
        result = Replace(result, redaction);
    }
    return result;
}

std::string PercentEncode(std::string_view text) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(text.size() * 3);
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (IsUnreserved(byte)) {
            encoded += c;
        } else {
            encoded += '%';
            encoded += hex[byte >> 4];
            encoded += hex[byte & 0xF];
        }
    }
    return encoded;
}

std::string IssueUrl(std::string_view base, std::vector<IssueField> fields,
                     std::size_t max_length) {
    const auto build = [base, &fields] {
        std::string url{base};
        for (const IssueField& field : fields) {
            url += '&';
            url += PercentEncode(field.name);
            url += '=';
            url += PercentEncode(field.value);
        }
        return url;
    };
    std::string url = build();
    for (auto field = fields.rbegin(); field != fields.rend() && url.size() > max_length;
         ++field) {
        if (field->shortened_note.empty()) {
            continue;
        }
        std::string& value = field->value;
        const std::size_t others = url.size() - PercentEncode(value).size();
        const std::size_t note_cost = PercentEncode("\n" + field->shortened_note).size();
        // The longest start of the value whose encoding fits, in whole UTF-8 sequences.
        std::size_t keep = 0;
        if (others + note_cost < max_length) {
            const std::size_t budget = max_length - others - note_cost;
            std::size_t cost = 0;
            while (keep < value.size()) {
                const std::size_t length =
                    std::min(SequenceLength(static_cast<unsigned char>(value[keep])),
                             value.size() - keep);
                std::size_t sequence_cost = 0;
                for (std::size_t i = 0; i < length; ++i) {
                    sequence_cost +=
                        IsUnreserved(static_cast<unsigned char>(value[keep + i])) ? 1 : 3;
                }
                if (cost + sequence_cost > budget) {
                    break;
                }
                cost += sequence_cost;
                keep += length;
            }
        }
        if (keep >= value.size()) {
            continue;
        }
        // End on a whole line where one fits.
        if (const auto newline = value.rfind('\n', keep);
            newline != std::string::npos && newline > 0) {
            keep = newline;
        }
        value.resize(keep);
        value += value.empty() ? field->shortened_note : "\n" + field->shortened_note;
        url = build();
    }
    return url;
}

std::string JoinEnds(std::string_view head, std::string_view tail, std::size_t left_out) {
    std::size_t head_end = head.size();
    if (const auto newline = head.rfind('\n'); newline != std::string_view::npos) {
        head_end = newline + 1;
    }
    std::size_t tail_start = 0;
    if (const auto newline = tail.find('\n'); newline != std::string_view::npos) {
        tail_start = newline + 1;
    }
    left_out += (head.size() - head_end) + tail_start;
    std::string text{head.substr(0, head_end)};
    if (!text.empty() && text.back() != '\n') {
        text += '\n';
    }
    text += fmt::format("[... {} bytes of this log are left out of the report ...]\n", left_out);
    text += tail.substr(tail_start);
    return text;
}

std::string KeepEnds(std::string_view text, std::size_t head, std::size_t tail) {
    if (text.size() <= head + tail) {
        return std::string{text};
    }
    return JoinEnds(text.substr(0, head), text.substr(text.size() - tail),
                    text.size() - head - tail);
}

std::optional<std::string> LastValue(std::string_view log, std::string_view marker) {
    const auto at = log.rfind(marker);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const auto start = at + marker.size();
    auto end = log.find_first_of("\r\n", start);
    if (end == std::string_view::npos) {
        end = log.size();
    }
    return std::string{log.substr(start, end - start)};
}

std::optional<BootedGame> LastBootedGame(std::string_view log) {
    const auto line = LastValue(log, "Booting game: ");
    if (!line) {
        return std::nullopt;
    }
    const std::string_view text{*line};
    BootedGame game;
    const char* const digits_end = text.data() + std::min<std::size_t>(16, text.size());
    const auto [end, error] = std::from_chars(text.data(), digits_end, game.program_id, 16);
    if (error != std::errc{} || end == text.data()) {
        return std::nullopt;
    }
    constexpr std::string_view separator = " | ";
    const auto title = text.find(separator);
    if (title == std::string_view::npos) {
        return game;
    }
    const auto version = text.find(separator, title + separator.size());
    game.title = std::string{text.substr(title + separator.size(),
                                         version == std::string_view::npos
                                             ? std::string_view::npos
                                             : version - title - separator.size())};
    if (version != std::string_view::npos) {
        game.version = std::string{text.substr(version + separator.size())};
        while (!game.version.empty() && game.version.back() == ' ') {
            game.version.pop_back();
        }
    }
    return game;
}

} // namespace FrontendCommon::ProblemReport
