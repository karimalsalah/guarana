// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <cstdint>

#include <mach-o/dyld.h>
#endif

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "core/file_sys/patch_pack.h"

namespace FileSys {
namespace {

using Json = nlohmann::json;

// The limits docs/patch-packs.md gives.
constexpr size_t MaxPackBytes = 1024 * 1024;
constexpr size_t MaxPatches = 64;
constexpr size_t MaxRecords = 256;
constexpr size_t MaxRecordBytes = 0x1000;
constexpr size_t MaxStateBytes = 0x10000;
// Every byte a pack touches lies in the first 512 MiB of its module, the bound
// zortos293/Lichii#1358's patch catalogue puts on its writes.
constexpr size_t ModuleLimit = 0x20000000;
constexpr size_t MaxNameBytes = 128;
constexpr size_t MinDescriptionBytes = 40;
constexpr size_t MaxDescriptionBytes = 1000;
constexpr size_t MaxGroupBytes = 64;
constexpr size_t MaxGameBytes = 128;
constexpr size_t MaxCommentBytes = 4000;
constexpr std::string_view BuiltinSuffix = " (built-in)";
constexpr std::string_view PackDocumentType = "guarana_patch_pack";
constexpr std::string_view PackFolderName = "patch-packs";

constexpr std::array<std::string_view, 13> ModuleNames{
    "main",    "rtld",    "sdk",     "subsdk0", "subsdk1", "subsdk2", "subsdk3",
    "subsdk4", "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9",
};

constexpr std::array<std::string_view, 9> PackFields{
    "document_type", "schema_version", "game",    "title_id", "module",
    "build_id",      "offset_basis",   "comment", "patches",
};
constexpr std::array<std::string_view, 9> PatchFields{
    "name",           "description", "level",   "group",   "replaces",
    "off_by_default", "state",       "records", "comment",
};
constexpr std::array<std::string_view, 3> StateFields{"offset", "size", "comment"};
constexpr std::array<std::string_view, 6> RecordFields{
    "offset", "bytes", "original_sha256", "original_size", "cave", "comment",
};

// Collects one line per problem of one pack.
class Problems {
public:
    Problems(std::string_view source_, std::vector<std::string>& lines_)
        : source{source_}, lines{lines_}, first{lines_.size()} {}

    void Add(std::string_view field, std::string_view problem) const {
        if (field.empty()) {
            lines.push_back(fmt::format("{}: {}", source, problem));
        } else {
            lines.push_back(fmt::format("{}: {}: {}", source, field, problem));
        }
    }

    [[nodiscard]] bool Any() const {
        return lines.size() != first;
    }

private:
    std::string_view source;
    std::vector<std::string>& lines;
    size_t first;
};

std::string FieldPath(std::string_view parent, std::string_view key) {
    return parent.empty() ? std::string{key} : fmt::format("{}.{}", parent, key);
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool IsHexDigits(std::string_view text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](char c) { return HexValue(c) >= 0; });
}

// Hex digits, two a byte, with spaces allowed between bytes.
std::optional<std::vector<u8>> ParseHexBytes(std::string_view text) {
    std::vector<u8> out;
    int high = -1;
    for (const char c : text) {
        if (c == ' ' && high < 0) {
            continue;
        }
        const int digit = HexValue(c);
        if (digit < 0) {
            return std::nullopt;
        }
        if (high < 0) {
            high = digit;
        } else {
            out.push_back(static_cast<u8>(high * 16 + digit));
            high = -1;
        }
    }
    if (high >= 0) {
        return std::nullopt;
    }
    return out;
}

// An offset or a size: 0x and 1 to 8 hex digits.
std::optional<size_t> ParseHexNumber(std::string_view text) {
    if (text.size() < 3 || text.size() > 10 || !text.starts_with("0x")) {
        return std::nullopt;
    }
    size_t value = 0;
    for (const char c : text.substr(2)) {
        const int digit = HexValue(c);
        if (digit < 0) {
            return std::nullopt;
        }
        value = value * 16 + static_cast<size_t>(digit);
    }
    return value;
}

bool IsPrintableAscii(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return c >= ' ' && c <= '~'; });
}

// Text shown to players as it is: any UTF-8, but no control characters.
bool HasNoControlCharacters(std::string_view text) {
    return std::none_of(text.begin(), text.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < 0x20 || byte == 0x7F;
    });
}

template <size_t N>
void CheckFields(const Problems& problems, const Json& object, std::string_view path,
                 const std::array<std::string_view, N>& known) {
    for (const auto& item : object.items()) {
        if (std::find(known.begin(), known.end(), item.key()) == known.end()) {
            problems.Add(FieldPath(path, item.key()), "is not a field of this pack format");
        }
    }
}

// A comment for reviewers, which the program does not read: a string or a list of strings.
void CheckComment(const Problems& problems, const Json& object, std::string_view path) {
    const auto comment = object.find("comment");
    if (comment == object.end()) {
        return;
    }
    const auto is_line = [](const Json& line) {
        return line.is_string() && line.get_ref<const std::string&>().size() <= MaxCommentBytes;
    };
    const bool valid = comment->is_string()
                           ? is_line(*comment)
                           : comment->is_array() &&
                                 std::all_of(comment->begin(), comment->end(), is_line);
    if (!valid) {
        problems.Add(FieldPath(path, "comment"),
                     "must be a string or a list of strings of at most 4000 bytes each");
    }
}

std::optional<std::string> TextField(const Problems& problems, const Json& object, const char* key,
                                     std::string_view path, bool required) {
    const auto field = object.find(key);
    if (field == object.end()) {
        if (required) {
            problems.Add(FieldPath(path, key), "is missing");
        }
        return std::nullopt;
    }
    if (!field->is_string()) {
        problems.Add(FieldPath(path, key), "must be a string");
        return std::nullopt;
    }
    return field->get<std::string>();
}

std::optional<size_t> HexField(const Problems& problems, const Json& object, const char* key,
                               std::string_view path) {
    const auto text = TextField(problems, object, key, path, true);
    if (!text) {
        return std::nullopt;
    }
    const auto value = ParseHexNumber(*text);
    if (!value) {
        problems.Add(FieldPath(path, key), "must be 0x and 1 to 8 hex digits");
    }
    return value;
}

std::optional<bool> FlagField(const Problems& problems, const Json& object, const char* key,
                              std::string_view path) {
    const auto field = object.find(key);
    if (field == object.end()) {
        return std::nullopt;
    }
    if (!field->is_boolean()) {
        problems.Add(FieldPath(path, key), "must be true or false");
        return std::nullopt;
    }
    return field->get<bool>();
}

PatchPackRecord ParseRecord(const Problems& problems, const Json& json, const std::string& path) {
    PatchPackRecord record;
    if (!json.is_object()) {
        problems.Add(path, "must be an object");
        return record;
    }
    CheckFields(problems, json, path, RecordFields);
    CheckComment(problems, json, path);
    const auto offset = HexField(problems, json, "offset", path);
    if (offset) {
        record.offset = *offset;
    }
    if (const auto text = TextField(problems, json, "bytes", path, true)) {
        auto bytes = ParseHexBytes(*text);
        if (!bytes || bytes->empty() || bytes->size() > MaxRecordBytes) {
            problems.Add(FieldPath(path, "bytes"),
                         "must be 1 to 4096 bytes as hex digits, two a byte, with spaces only "
                         "between bytes");
        } else {
            record.bytes = std::move(*bytes);
        }
    }
    record.cave = FlagField(problems, json, "cave", path).value_or(false);
    const bool has_hash = json.contains("original_sha256");
    const bool has_size = json.contains("original_size");
    if (record.cave) {
        if (has_hash || has_size) {
            problems.Add(path, "is a cave, whose bytes must be zero before it applies: it has no "
                               "original_sha256 or original_size");
        }
    } else {
        if (const auto digest = TextField(problems, json, "original_sha256", path, true)) {
            if (digest->size() != 2 * record.original_sha256.size() || !IsHexDigits(*digest)) {
                problems.Add(FieldPath(path, "original_sha256"),
                             "must be the SHA-256 of the game's bytes the record replaces: 64 hex "
                             "digits");
            } else {
                for (size_t i = 0; i < record.original_sha256.size(); ++i) {
                    record.original_sha256[i] = static_cast<u8>(HexValue((*digest)[2 * i]) * 16 +
                                                                HexValue((*digest)[2 * i + 1]));
                }
            }
        }
        record.original_size = record.bytes.size();
        if (has_size) {
            if (const auto size = HexField(problems, json, "original_size", path)) {
                if (*size < record.bytes.size() || *size > MaxRecordBytes) {
                    problems.Add(FieldPath(path, "original_size"),
                                 "must cover every byte the record writes, and at most 0x1000");
                } else {
                    record.original_size = *size;
                }
            }
        }
    }
    if (offset && *offset > ModuleLimit - (std::max)(record.bytes.size(), record.original_size)) {
        problems.Add(FieldPath(path, "offset"), "puts the record past 512 MiB into the module");
    }
    return record;
}

PatchPackPatch ParsePatch(const Problems& problems, const Json& json, const std::string& path) {
    PatchPackPatch patch;
    if (!json.is_object()) {
        problems.Add(path, "must be an object");
        return patch;
    }
    CheckFields(problems, json, path, PatchFields);
    CheckComment(problems, json, path);

    if (const auto name = TextField(problems, json, "name", path, true)) {
        if (name->size() > MaxNameBytes || name->size() <= BuiltinSuffix.size() ||
            !IsPrintableAscii(*name) || !name->ends_with(BuiltinSuffix) || name->front() == ' ' ||
            name->front() == '+') {
            problems.Add(FieldPath(path, "name"),
                         "must be printable ASCII of at most 128 bytes ending with \" (built-in)\", "
                         "and must not start with a space or a +");
        }
        patch.name = *name;
    }
    if (const auto description = TextField(problems, json, "description", path, true)) {
        if (description->size() < MinDescriptionBytes ||
            description->size() > MaxDescriptionBytes || !description->ends_with('.') ||
            !HasNoControlCharacters(*description)) {
            problems.Add(FieldPath(path, "description"),
                         "must tell a player what the patch does in 40 to 1000 bytes, ending with "
                         "a full stop");
        }
        patch.description = *description;
    }
    if (const auto level = TextField(problems, json, "level", path, false)) {
        if (*level == "standard") {
            patch.level = BuiltinPatchLevel::Standard;
        } else if (*level == "recommended") {
            patch.level = BuiltinPatchLevel::Recommended;
        } else if (*level == "experimental") {
            patch.level = BuiltinPatchLevel::Experimental;
        } else {
            problems.Add(FieldPath(path, "level"), "must be standard, recommended or experimental");
        }
    }
    if (const auto group = TextField(problems, json, "group", path, false)) {
        if (group->empty() || group->size() > MaxGroupBytes || !IsPrintableAscii(*group) ||
            group->front() == ' ' || group->back() == ' ') {
            problems.Add(FieldPath(path, "group"), "must be printable ASCII of 1 to 64 bytes");
        }
        patch.group = *group;
    }
    if (const auto replaces = TextField(problems, json, "replaces", path, false)) {
        patch.replaces = *replaces;
    }
    patch.off_by_default = FlagField(problems, json, "off_by_default", path).value_or(false);
    if (patch.off_by_default && patch.level == BuiltinPatchLevel::Recommended) {
        problems.Add(FieldPath(path, "level"),
                     "cannot be recommended for a patch that is off until checked");
    }

    if (const auto state = json.find("state"); state != json.end()) {
        const auto state_path = FieldPath(path, "state");
        if (!state->is_object()) {
            problems.Add(state_path, "must be an object with an offset and a size");
        } else {
            CheckFields(problems, *state, state_path, StateFields);
            CheckComment(problems, *state, state_path);
            const auto offset = HexField(problems, *state, "offset", state_path);
            const auto size = HexField(problems, *state, "size", state_path);
            if (offset && size) {
                if (*size == 0 || *size > MaxStateBytes || *offset > ModuleLimit - *size) {
                    problems.Add(state_path, "must be 1 to 0x10000 bytes in the first 512 MiB of "
                                             "the module");
                } else {
                    patch.state_offset = *offset;
                    patch.state_size = *size;
                }
            }
        }
    }

    const auto records_path = FieldPath(path, "records");
    const auto records = json.find("records");
    if (records == json.end()) {
        problems.Add(records_path, "is missing");
    } else if (!records->is_array() || records->empty() || records->size() > MaxRecords) {
        problems.Add(records_path, "must be a list of 1 to 256 records");
    } else {
        for (size_t i = 0; i < records->size(); ++i) {
            patch.records.push_back(
                ParseRecord(problems, (*records)[i], fmt::format("{}[{}]", records_path, i)));
        }
    }
    return patch;
}

// Names are [DisabledAddOns] keys, and `replaces` follows the order PatchNSO applies patches in.
void CheckPatchRelations(const Problems& problems, const std::vector<PatchPackPatch>& patches) {
    for (size_t i = 0; i < patches.size(); ++i) {
        const auto& patch = patches[i];
        for (size_t j = 0; j < i; ++j) {
            if (patches[j].name == patch.name) {
                problems.Add(fmt::format("patches[{}].name", i),
                             fmt::format("is also the name of patches[{}]", j));
                break;
            }
        }
        if (patch.replaces.empty()) {
            continue;
        }
        const auto later = std::find_if(
            patches.begin() + static_cast<std::ptrdiff_t>(i + 1), patches.end(),
            [&patch](const PatchPackPatch& other) { return other.name == patch.replaces; });
        if (later == patches.end()) {
            problems.Add(fmt::format("patches[{}].replaces", i),
                         "must name a patch later in this pack: the one this patch contains, "
                         "which is skipped once this one applies");
        }
    }
}

// The text of a pack file in the folder, or nothing, with the reason in `errors`.
std::optional<std::string> ReadPackFile(const std::filesystem::path& path,
                                        std::string_view file_name,
                                        std::vector<std::string>& errors) {
    std::error_code size_error;
    const auto file_bytes = std::filesystem::file_size(path, size_error);
    if (size_error || file_bytes > MaxPackBytes) {
        errors.push_back(
            fmt::format("{}: cannot be read, or is over {} bytes", file_name, MaxPackBytes));
        return std::nullopt;
    }
    auto text = Common::FS::ReadStringFromFile(path, Common::FS::FileType::BinaryFile);
    if (text.size() != file_bytes) {
        errors.push_back(fmt::format("{}: could not be read", file_name));
        return std::nullopt;
    }
    return text;
}

#ifdef __APPLE__
// The running program's file, links resolved. Empty when it cannot be found.
std::filesystem::path ExecutablePath() {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    std::error_code error;
    auto resolved = std::filesystem::canonical(buffer, error);
    return error ? std::filesystem::path{buffer} : resolved;
}
#endif

} // Anonymous namespace

std::optional<PatchPack> ParsePatchPack(std::string_view text, std::string_view source,
                                        std::vector<std::string>& errors) {
    const Problems problems{source, errors};
    if (text.size() > MaxPackBytes) {
        problems.Add({}, fmt::format("is {} bytes, more than the {} a pack may have", text.size(),
                                     MaxPackBytes));
        return std::nullopt;
    }

    // nlohmann keeps the last of two equal keys of an object without a word, while a reader of
    // the file sees the first: collect each object's keys as it is parsed.
    std::vector<std::vector<std::string>> open_objects;
    std::string duplicate_key;
    const Json::parser_callback_t on_event = [&open_objects, &duplicate_key](
                                                 int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) {
            open_objects.emplace_back();
        } else if (event == Json::parse_event_t::object_end) {
            if (!open_objects.empty()) {
                open_objects.pop_back();
            }
        } else if (event == Json::parse_event_t::key && !open_objects.empty() &&
                   parsed.is_string()) {
            auto& keys = open_objects.back();
            const auto& key = parsed.get_ref<const std::string&>();
            if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
                keys.push_back(key);
            } else if (duplicate_key.empty()) {
                duplicate_key = key;
            }
        }
        return true;
    };
    const Json document =
        Json::parse(text.data(), text.data() + text.size(), on_event, false);
    if (document.is_discarded()) {
        problems.Add({}, "is not valid JSON");
        return std::nullopt;
    }
    if (!duplicate_key.empty()) {
        problems.Add({}, fmt::format("has the key \"{}\" twice in one object", duplicate_key));
    }
    if (!document.is_object()) {
        problems.Add({}, "must be a JSON object");
        return std::nullopt;
    }
    CheckFields(problems, document, {}, PackFields);
    CheckComment(problems, document, {});

    PatchPack pack;
    pack.source = std::string{source};
    if (const auto type = TextField(problems, document, "document_type", {}, true);
        type && *type != PackDocumentType) {
        problems.Add("document_type", "must be \"guarana_patch_pack\"");
    }
    if (const auto version = document.find("schema_version"); version == document.end()) {
        problems.Add("schema_version", "is missing");
    } else if (!version->is_number_unsigned() || version->get<u64>() != 1) {
        problems.Add("schema_version", "must be 1, the version this program reads");
    }
    if (const auto game = TextField(problems, document, "game", {}, true);
        game && (game->empty() || game->size() > MaxGameBytes || !HasNoControlCharacters(*game))) {
        problems.Add("game", "must name the game in 1 to 128 bytes");
    }
    if (const auto title = TextField(problems, document, "title_id", {}, true)) {
        u64 title_id = 0;
        if (title->size() == 16 && IsHexDigits(*title)) {
            for (const char c : *title) {
                title_id = title_id * 16 + static_cast<u64>(HexValue(c));
            }
        }
        if (title_id == 0) {
            problems.Add("title_id", "must be the program id: 16 hex digits, not all zero");
        }
        pack.title_id = title_id;
    }
    if (const auto module_name = TextField(problems, document, "module", {}, true)) {
        if (std::find(ModuleNames.begin(), ModuleNames.end(), *module_name) == ModuleNames.end()) {
            problems.Add("module", "must be main, rtld, sdk or subsdk0 to subsdk9");
        }
        pack.module = *module_name;
    }
    if (const auto build = TextField(problems, document, "build_id", {}, true)) {
        // PatchNSO compares the build id in upper case without its trailing zeros.
        std::string upper{*build};
        for (char& c : upper) {
            if (c >= 'a' && c <= 'f') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        upper.resize(upper.find_last_not_of('0') + 1);
        if (build->size() < 2 || build->size() > 64 || build->size() % 2 != 0 ||
            !IsHexDigits(*build) || upper.empty()) {
            problems.Add("build_id",
                         "must be the module's build id: 2 to 64 hex digits, not all zero");
        }
        pack.build_id = std::move(upper);
    }
    if (const auto basis = TextField(problems, document, "offset_basis", {}, true);
        basis && *basis != "module") {
        problems.Add("offset_basis",
                     "must be \"module\": offsets count from the module's start, as main+0x... in "
                     "the log, without the 0x100-byte NSO header an IPS offset includes");
    }

    if (const auto patches = document.find("patches"); patches == document.end()) {
        problems.Add("patches", "is missing");
    } else if (!patches->is_array() || patches->empty() || patches->size() > MaxPatches) {
        problems.Add("patches", "must be a list of 1 to 64 patches");
    } else {
        for (size_t i = 0; i < patches->size(); ++i) {
            pack.patches.push_back(
                ParsePatch(problems, (*patches)[i], fmt::format("patches[{}]", i)));
        }
        CheckPatchRelations(problems, pack.patches);
    }
    if (problems.Any()) {
        return std::nullopt;
    }
    return pack;
}

PatchPackLoad LoadPatchPacks(const std::filesystem::path& directory,
                             std::span<const EmbeddedPatchPack> compiled_in) {
    PatchPackLoad load;
    // Every pack by file name, in the order they are used: its file in the folder and its
    // compiled-in copy, either of which may be missing.
    struct Copies {
        std::filesystem::path file;
        const EmbeddedPatchPack* compiled{};
    };
    std::map<std::string, Copies> copies_by_name;
    for (const auto& embedded : compiled_in) {
        copies_by_name[std::string{embedded.file_name}].compiled = &embedded;
    }
    std::error_code error;
    if (!directory.empty() && std::filesystem::is_directory(directory, error)) {
        for (std::filesystem::directory_iterator entry{directory, error}, last;
             !error && entry != last; entry.increment(error)) {
            std::error_code type_error;
            if (entry->path().extension() == ".json" && entry->is_regular_file(type_error)) {
                copies_by_name[Common::FS::PathToUTF8String(entry->path().filename())].file =
                    entry->path();
            }
        }
        if (error) {
            load.errors.push_back(fmt::format("{}: the folder could not be read: {}",
                                              Common::FS::PathToUTF8String(directory),
                                              error.message()));
        }
    }
    for (const auto& [file_name, copies] : copies_by_name) {
        if (!copies.file.empty()) {
            if (const auto file_text = ReadPackFile(copies.file, file_name, load.errors)) {
                if (auto pack = ParsePatchPack(*file_text, file_name, load.errors)) {
                    load.packs.push_back(std::move(*pack));
                    continue;
                }
            }
            if (copies.compiled == nullptr) {
                continue;
            }
            load.errors.push_back(
                fmt::format("{}: the copy compiled into the program is used instead", file_name));
        }
        const std::string_view compiled_text{
            reinterpret_cast<const char*>(copies.compiled->text.data()),
            copies.compiled->text.size()};
        if (auto pack = ParsePatchPack(compiled_text, fmt::format("{} (compiled in)", file_name),
                                       load.errors)) {
            pack->source = file_name;
            pack->compiled_in = true;
            load.packs.push_back(std::move(*pack));
        }
    }
    return load;
}

std::filesystem::path ShippedPatchPackDirectory() {
#if defined(_WIN32)
    const auto exe_directory = Common::FS::GetExeDirectory();
    return exe_directory.empty() ? std::filesystem::path{} : exe_directory / PackFolderName;
#elif defined(__APPLE__)
    // A bundle keeps them in Contents/Resources. guarana-cli, in the bundle's Contents/MacOS, also
    // looks from its own path, in case the main bundle is not the .app around it. A program outside
    // a bundle (tests) has them next to it.
    const auto bundle = Common::FS::GetBundleDirectory();
    std::vector<std::filesystem::path> candidates{bundle / "Contents" / "Resources" /
                                                  PackFolderName};
    if (const auto exe = ExecutablePath(); !exe.empty()) {
        candidates.push_back(exe.parent_path().parent_path() / "Resources" / PackFolderName);
        candidates.push_back(exe.parent_path() / PackFolderName);
    }
    candidates.push_back(bundle / PackFolderName);
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_directory(candidate, error)) {
            return candidate;
        }
    }
    return candidates.front();
#elif defined(__linux__) && !defined(__ANDROID__)
    std::error_code error;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path{} : exe.parent_path() / PackFolderName;
#else
    return {};
#endif
}

bool PatchPackSha256(std::span<const u8> data, std::array<u8, 32>& digest) {
    unsigned int length = 0;
    return EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_sha256(), nullptr) ==
               1 &&
           length == digest.size();
}

} // namespace FileSys
