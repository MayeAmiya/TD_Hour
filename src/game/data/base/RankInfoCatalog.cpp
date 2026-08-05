#include "RankInfoCatalog.h"

#include "ContentDiagnostics.h"
#include "core/container/string_utils.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "core/data/ini/LegacyIniDirectory.h"
#include "VFS.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <system_error>
#include <utility>

namespace engine {
namespace {

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

constexpr auto trim = container::trimAsciiCopy;

[[nodiscard]] container::String canonical(container::StringView value) {
    container::String result = trim(value);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return result;
}

[[nodiscard]] container::String canonicalPath(container::StringView value) {
    container::String result = canonical(value);
    std::replace(result.begin(), result.end(), '\\', '/');
    while (!result.empty() && result.back() == '/') result.pop_back();
    return result;
}

[[nodiscard]] bool equalInsensitive(
    container::StringView left, container::StringView right) {
    return canonical(left) == canonical(right);
}

template <typename Integer>
[[nodiscard]] bool parseInteger(container::StringView value, Integer& output) {
    const container::String cleaned = trim(value);
    if (cleaned.empty()) return false;
    Integer parsed{};
    const char* first = cleaned.data();
    const char* last = first + cleaned.size();
    const auto [end, error] = std::from_chars(first, last, parsed);
    if (error != std::errc{} || end != last) return false;
    output = parsed;
    return true;
}

[[nodiscard]] container::Vector<container::String> parseScienceNames(
    container::StringView value) {
    container::Vector<container::String> result;
    size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() &&
               (std::isspace(static_cast<unsigned char>(value[cursor])) ||
                value[cursor] == ',')) {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[cursor])) &&
               value[cursor] != ',') {
            ++cursor;
        }
        if (begin != cursor) {
            result.emplace_back(value.substr(begin, cursor - begin));
        }
    }
    if (result.size() == 1 && equalInsensitive(result.front(), "None")) {
        result.clear();
    }
    return result;
}

void warnRankContent(
    container::StringView source, container::StringView definition,
    container::StringView field, container::StringView rawValue,
    container::StringView adoptedValue,
    container::String reason) {
    game::processContentDiagnostics().warn({
        .source = container::String{source},
        .block = "Rank",
        .definition = container::String{definition},
        .module = "RankInfoCatalog",
        .field = container::String{field},
        .rawValue = container::String{rawValue},
        .adoptedValue = container::String{adoptedValue},
        .reason = std::move(reason),
    });
}

class CanonicalHasher final {
public:
    void byte(uint8_t value) noexcept {
        m_value ^= value;
        m_value *= kFnvPrime;
    }

    void u32(uint32_t value) noexcept {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
        }
    }

    void i32(int32_t value) noexcept {
        u32(static_cast<uint32_t>(value));
    }

    void string(container::StringView value) noexcept {
        u32(static_cast<uint32_t>(value.size()));
        for (const unsigned char character : value) byte(character);
    }

    [[nodiscard]] uint64_t finish() const noexcept { return m_value; }

private:
    uint64_t m_value = kFnvOffsetBasis;
};

} // namespace

uint64_t RankInfoCatalog::calculateFingerprint(
    const container::Vector<RankInfoDefinition>& entries) {
    CanonicalHasher hash;
    hash.string("RankInfoCatalog.simulation.v1");
    hash.u32(static_cast<uint32_t>(entries.size()));
    for (const RankInfoDefinition& entry : entries) {
        hash.u32(entry.level);
        hash.string(entry.rankName);
        hash.i32(entry.skillPointsNeeded);
        hash.u32(entry.sciencePurchasePointsGranted);
        hash.u32(static_cast<uint32_t>(entry.sciencesGranted.size()));
        for (const container::String& science : entry.sciencesGranted) {
            hash.string(science);
        }
    }
    return hash.finish();
}

container::Vector<container::String>
RankInfoCatalog::enumerateVfsLoadFiles(
    container::Span<const container::StringView> loadRoots) {
    return game::ini::enumerateLegacyIniDirectories(loadRoots);
}

bool RankInfoCatalog::loadFromVfs(
    container::StringView path, container::String* error) {
    return loadFromVfsFiles(
        container::Vector<container::String>{container::String{path}}, error);
}

bool RankInfoCatalog::applyOverridesFromVfs(
    container::StringView path, container::String* error) {
    return loadFromVfsFilesImpl(
        container::Vector<container::String>{container::String{path}},
        true, error);
}

bool RankInfoCatalog::loadFromVfsLoadDirectories(
    container::Span<const container::StringView> loadRoots,
    container::String* error) {
    return loadFromVfsFiles(enumerateVfsLoadFiles(loadRoots), error);
}

bool RankInfoCatalog::loadFromVfsFiles(
    const container::Vector<container::String>& logicalFiles,
    container::String* error) {
    return loadFromVfsFilesImpl(logicalFiles, false, error);
}

bool RankInfoCatalog::loadFromVfsFilesImpl(
    const container::Vector<container::String>& logicalFiles,
    bool overrideExisting, container::String* error) {
    if (error) error->clear();
    if (overrideExisting && !m_loaded) {
        if (error) *error = "Rank override requires a loaded base catalog";
        return false;
    }

    container::Vector<RankInfoDefinition> candidate =
        overrideExisting ? m_entries
                         : container::Vector<RankInfoDefinition>{};
    const auto fail = [error, overrideExisting](
        container::StringView source, container::StringView definition,
        container::StringView field, container::StringView rawValue,
        container::String reason) {
        warnRankContent(
            source, definition, field, rawValue,
            overrideExisting
                ? "override directory ignored; sealed base catalog retained"
                : "Rank directory disabled",
            reason);
        if (error) *error = std::move(reason);
        return false;
    };

    // Missing Rank.ini is represented by a sealed empty catalog. Consumers
    // then disable rank progression instead of consulting mutable defaults.
    if (logicalFiles.empty()) {
        candidate.clear();
        m_entries = std::move(candidate);
        m_simulationFingerprint = calculateFingerprint(m_entries);
        m_loaded = true;
        return true;
    }

    for (const container::String& rawPath : logicalFiles) {
        const container::String path = canonicalPath(rawPath);
        if (path.empty()) {
            return fail(__FILE__, {}, "LogicalPath", rawPath,
                        "Rank INI input set contains an empty logical path");
        }
        if (!io::VFS::instance().exists(path)) {
            return fail(path, {}, "File", path,
                        "Rank INI disappeared from VFS during load");
        }

        game::GeneralsIniParser parser;
        if (!parser.parse(io::VFS::instance().readAll(path), path)) {
            return fail(path, {}, "Ini", {},
                        "could not parse Rank INI source");
        }

        for (const game::IniBlock& block : parser.blocks()) {
            if (!equalInsensitive(block.type, "Rank")) continue;

            uint32_t level = 0;
            if (!parseInteger(block.name, level) || level == 0) {
                return fail(path, block.name, "Level", block.name,
                            "Rank level must be a positive 32-bit integer");
            }
            if (overrideExisting) {
                if (level > candidate.size()) {
                    return fail(path, block.name, "Level", block.name,
                                "Rank override may only modify an existing level");
                }
            } else {
                const uint64_t expected =
                    static_cast<uint64_t>(candidate.size()) + 1u;
                if (expected > std::numeric_limits<uint32_t>::max() ||
                    level != expected) {
                    return fail(path, block.name, "Level", block.name,
                                "base Rank levels must be one-based and contiguous");
                }
                candidate.push_back({.level = level});
            }

            RankInfoDefinition& definition = candidate[level - 1u];
            for (const auto& [fieldName, rawValue] : block.values) {
                const container::String field = canonical(fieldName);
                if (field == "rankname") {
                    definition.rankName = trim(rawValue);
                } else if (field == "skillpointsneeded") {
                    int32_t value = 0;
                    if (!parseInteger(rawValue, value)) {
                        return fail(path, block.name, fieldName, rawValue,
                                    "invalid signed 32-bit SkillPointsNeeded");
                    }
                    definition.skillPointsNeeded = value;
                } else if (field == "sciencesgranted") {
                    definition.sciencesGranted = parseScienceNames(rawValue);
                } else if (field == "sciencepurchasepointsgranted") {
                    uint32_t value = 0;
                    if (!parseInteger(rawValue, value)) {
                        return fail(
                            path, block.name, fieldName, rawValue,
                            "invalid unsigned 32-bit SciencePurchasePointsGranted");
                    }
                    definition.sciencePurchasePointsGranted = value;
                }
            }
        }
    }

    if (candidate.empty()) {
        return fail("data/ini/Rank", {}, "Rank", {},
                    "Rank directory contains no Rank definitions");
    }
    int32_t previousThreshold = 0;
    for (const RankInfoDefinition& definition : candidate) {
        if (definition.skillPointsNeeded < 0 ||
            definition.skillPointsNeeded < previousThreshold) {
            return fail(
                "data/ini/Rank", std::to_string(definition.level),
                "SkillPointsNeeded",
                std::to_string(definition.skillPointsNeeded),
                "Rank skill thresholds must be non-negative and monotonically non-decreasing");
        }
        previousThreshold = definition.skillPointsNeeded;
    }

    m_entries = std::move(candidate);
    m_simulationFingerprint = calculateFingerprint(m_entries);
    m_loaded = true;
    return true;
}

void RankInfoCatalog::clear() noexcept {
    m_entries.clear();
    m_simulationFingerprint = 0;
    m_loaded = false;
}

const RankInfoDefinition* RankInfoCatalog::find(uint32_t level) const noexcept {
    if (!m_loaded || level == 0 || level > m_entries.size()) return nullptr;
    return &m_entries[level - 1u];
}

} // namespace engine
