// SPDX-FileCopyrightText: Copyright 2026 Guarana contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/community_evidence.h"

#include <charconv>
#include <system_error>
#include <tuple>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "core/community_evidence_json.h"

namespace Core::CommunityEvidence {
namespace {

using nlohmann::json;
using Values = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] std::optional<Common::GpuFamily> FamilyNamed(std::string_view name) {
    for (u32 index = 0; index < static_cast<u32>(Common::GpuFamily::Count); ++index) {
        const auto family = static_cast<Common::GpuFamily>(index);
        if (family != Common::GpuFamily::None && Common::GpuFamilyName(family) == name) {
            return family;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string TextOf(const json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        return {};
    }
    return found->get<std::string>();
}

[[nodiscard]] u32 CountOf(const json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_unsigned()) {
        return 0;
    }
    const u64 count = found->get<u64>();
    return count > 0xFFFF'FFFFULL ? 0xFFFF'FFFFU : static_cast<u32>(count);
}

/// The median, lowest or highest of an {"min", "median", "max"} object.
[[nodiscard]] std::optional<double> SpreadOf(const json& object, const char* key, const char* part) {
    const auto spread = object.find(key);
    if (spread == object.end() || !spread->is_object()) {
        return std::nullopt;
    }
    const auto found = spread->find(part);
    if (found == spread->end() || !found->is_number()) {
        return std::nullopt;
    }
    return found->get<double>();
}

[[nodiscard]] Values ValuesOf(const json& object, const char* key) {
    Values values;
    const auto found = object.find(key);
    if (found == object.end() || !found->is_object()) {
        return values;
    }
    for (auto item = found->begin(); item != found->end(); ++item) {
        if (item.value().is_string()) {
            values.emplace_back(item.key(), item.value().get<std::string>());
        }
    }
    return values;
}

[[nodiscard]] u64 TitleIdOf(const json& object) {
    const std::string text = TextOf(object, "title_id");
    if (text.size() != 16) {
        return 0;
    }
    u64 title_id = 0;
    const std::from_chars_result read =
        std::from_chars(text.data(), text.data() + text.size(), title_id, 16);
    if (read.ec != std::errc{} || read.ptr != text.data() + text.size()) {
        return 0;
    }
    return title_id;
}

/// A switch is named as it is; a setting by its label, the key after its section.
[[nodiscard]] bool SameName(std::string_view key, std::string_view name) {
    if (key == name) {
        return true;
    }
    constexpr std::string_view Setting = "setting:";
    if (!key.starts_with(Setting)) {
        return false;
    }
    key.remove_prefix(Setting.size());
    const std::size_t slash = key.rfind('/');
    return (slash == std::string_view::npos ? key : key.substr(slash + 1)) == name;
}

[[nodiscard]] std::string ValuesText(const Values& values) {
    std::string text;
    for (const auto& [key, setting] : values) {
        text += fmt::format("{}{}={}", text.empty() ? "" : ", ", key, setting);
    }
    return text;
}

} // Anonymous namespace

std::vector<Effect> Parse(std::string_view text) {
    std::vector<Effect> effects;
    if (text.empty()) {
        return effects;
    }
    const json document = json::parse(text.data(), text.data() + text.size(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return effects;
    }
    const auto list = document.find("effects");
    if (list == document.end() || !list->is_array()) {
        return effects;
    }
    for (const json& item : *list) {
        if (!item.is_object()) {
            continue;
        }
        const std::optional<Common::GpuFamily> family = FamilyNamed(TextOf(item, "family"));
        if (!family) {
            continue;
        }
        Effect effect;
        effect.family = *family;
        effect.title_id = TitleIdOf(item);
        effect.scene = TextOf(item, "scene");
        effect.set = ValuesOf(item, "set");
        effect.against = ValuesOf(item, "against");
        if (const auto context = item.find("context"); context != item.end() && context->is_array()) {
            for (const json& entry : *context) {
                if (entry.is_string()) {
                    effect.context.push_back(entry.get<std::string>());
                }
            }
        }
        effect.build = TextOf(item, "build");
        effect.power = TextOf(item, "power");
        effect.devices = CountOf(item, "devices");
        effect.pairs = CountOf(item, "pairs");
        effect.fps_change_median = SpreadOf(item, "fps_change_pct", "median");
        effect.fps_change_min = SpreadOf(item, "fps_change_pct", "min");
        effect.fps_change_max = SpreadOf(item, "fps_change_pct", "max");
        effect.p99_change_median = SpreadOf(item, "p99_change_pct", "median");
        if (const auto issues = item.find("issues"); issues != item.end() && issues->is_array()) {
            for (const json& issue : *issues) {
                if (issue.is_number_unsigned() && issue.get<u64>() <= 0xFFFF'FFFFULL) {
                    effect.issues.push_back(static_cast<u32>(issue.get<u64>()));
                }
            }
        }
        effects.push_back(std::move(effect));
    }
    return effects;
}

std::span<const Effect> Embedded() {
    static const std::vector<Effect> effects = Parse(std::string_view{
        reinterpret_cast<const char*>(Generated::CommunityEvidenceJson),
        static_cast<std::size_t>(Generated::CommunityEvidenceJsonSize)});
    return effects;
}

const Effect* Find(std::span<const Effect> effects, Common::GpuFamily family, u64 title_id,
                   std::string_view name, std::string_view value) {
    const auto rank = [](const Effect& effect) {
        return std::tuple{effect.devices, effect.pairs, effect.power == "ac"};
    };
    // evidence.py puts the off or lower value against the other, so the value may be on either side.
    const auto measures = [name, value](const Values& side) {
        return side.size() == 1 && side.front().second == value && SameName(side.front().first, name);
    };
    const Effect* best = nullptr;
    for (const Effect& effect : effects) {
        if (effect.family != family || effect.title_id != title_id ||
            effect.set.size() != 1 || effect.against.size() != 1 ||
            !(measures(effect.set) || measures(effect.against))) {
            continue;
        }
        if (best == nullptr || rank(effect) > rank(*best)) {
            best = &effect;
        }
    }
    return best;
}

std::string Describe(const Effect& effect) {
    std::string text = fmt::format("{} against {}", ValuesText(effect.set), ValuesText(effect.against));
    if (!effect.scene.empty()) {
        text += fmt::format(" in {}", effect.scene);
    }
    text += ':';
    if (effect.fps_change_median) {
        text += fmt::format(" game fps {:+.1f}% median", *effect.fps_change_median);
        if (effect.devices > 1 && effect.fps_change_min && effect.fps_change_max) {
            text += fmt::format(" ({:+.1f}% to {:+.1f}%)", *effect.fps_change_min,
                                *effect.fps_change_max);
        }
        text += ',';
    }
    if (effect.p99_change_median) {
        text += fmt::format(" p99 {:+.1f}%,", *effect.p99_change_median);
    }
    text += fmt::format(" {} device{}, {} pair{}", effect.devices, effect.devices == 1 ? "" : "s",
                        effect.pairs, effect.pairs == 1 ? "" : "s");
    if (!effect.context.empty()) {
        text += ", with";
        for (const std::string& item : effect.context) {
            text += ' ';
            text += item;
        }
    }
    if (!effect.build.empty()) {
        text += fmt::format(", {}", effect.build);
    }
    if (effect.power == "ac") {
        text += ", mains";
    } else if (effect.power == "battery") {
        text += ", battery";
    }
    if (!effect.issues.empty()) {
        text += ", community reports";
        for (const u32 issue : effect.issues) {
            text += fmt::format(" #{}", issue);
        }
    }
    return text;
}

} // namespace Core::CommunityEvidence
