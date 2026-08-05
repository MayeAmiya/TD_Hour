#include "SpecialPowerCatalog.h"

#include "LegacyIniDirectory.h"
#include "VFS.h"
#include "core/container/hash_containers.h"
#include "core/container/string_utils.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "ContentFloatParsing.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace engine {
namespace {

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

constexpr auto trim = container::trimAsciiCopy;

[[nodiscard]] container::String canonical(container::StringView value) {
    container::String result = trim(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
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

[[nodiscard]] bool equalInsensitive(container::StringView left, container::StringView right) {
    return canonical(left) == canonical(right);
}

[[nodiscard]] bool parseMilliseconds(container::StringView value, uint32_t& output) {
    const container::String cleaned = trim(value);
    if (cleaned.empty()) return false;
    const char* first = cleaned.data();
    const char* last = first + cleaned.size();
    uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(first, last, parsed);
    if (error != std::errc{} || end != last) return false;
    output = parsed;
    return true;
}

[[nodiscard]] bool parseBool(container::StringView value, bool& output) {
    const container::String normalized = canonical(value);
    if (normalized == "yes" || normalized == "true" || normalized == "on" || normalized == "1") {
        output = true;
        return true;
    }
    if (normalized == "no" || normalized == "false" || normalized == "off" || normalized == "0") {
        output = false;
        return true;
    }
    return false;
}

[[nodiscard]] bool parseReal(container::StringView value, math::q32_32& output) {
    const game::ContentFloatContext context{
        .source = __FILE__, .block = "SpecialPower", .field = "Real",
        .fallback = output.to_float()};
    const std::optional<float> parsed =
        game::parseContentFloat(value, context);
    if (!parsed) return true;
    // RefCode's parseReal first narrows authored text to a 32-bit Real. Keep
    // that ingress behavior, then freeze it in deterministic fixed point.
    const float narrowed = *parsed;
    const double narrowedValue = static_cast<double>(narrowed);
    constexpr double kQ32Minimum = -2'147'483'648.0;
    constexpr double kQ32MaximumExclusive = 2'147'483'648.0;
    if (!std::isfinite(narrowed) || narrowedValue < kQ32Minimum ||
        narrowedValue >= kQ32MaximumExclusive) {
        game::warnContentFloatFallback(
            value, context,
            "finite numeric prefix is outside the Q32.32 field range; retained the prior/default value");
        return true;
    }
    output = math::q32_32{narrowed};
    return true;
}

[[nodiscard]] bool parseToken(container::StringView value, container::String& output) {
    container::String parsed = trim(value);
    if (parsed.empty() || std::any_of(parsed.begin(), parsed.end(), [](unsigned char character) {
            return std::isspace(character) != 0;
        })) {
        return false;
    }
    output = std::move(parsed);
    return true;
}

void warnSpecialPowerContent(
    container::StringView source, container::StringView definition,
    container::StringView field, container::StringView rawValue,
    container::StringView adoptedValue, container::String reason) {
    game::processContentDiagnostics().warn({
        .source = container::String{source},
        .block = "SpecialPower",
        .definition = container::String{definition},
        .module = "SpecialPowerCatalog",
        .field = container::String{field},
        .rawValue = container::String{rawValue},
        .adoptedValue = container::String{adoptedValue},
        .reason = std::move(reason),
    });
}

[[nodiscard]] container::String layerSource(
    container::StringView path, size_t layerIndex) {
    return container::String{path} + " [layer " +
        std::to_string(layerIndex) + "]";
}

class CanonicalHasher final {
public:
    void byte(uint8_t value) noexcept {
        m_value ^= value;
        m_value *= kFnvPrime;
    }

    void boolean(bool value) noexcept { byte(value ? 1u : 0u); }

    void u32(uint32_t value) noexcept {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
        }
    }

    void u64(uint64_t value) noexcept {
        for (uint32_t shift = 0; shift < 64; shift += 8) {
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
        }
    }

    void i64(int64_t value) noexcept { u64(static_cast<uint64_t>(value)); }

    void string(container::StringView value) noexcept {
        u32(static_cast<uint32_t>(value.size()));
        for (const unsigned char character : value) byte(character);
    }

    [[nodiscard]] uint64_t finish() const noexcept { return m_value; }

private:
    uint64_t m_value = kFnvOffsetBasis;
};

} // namespace

uint64_t SpecialPowerCatalog::calculateFingerprint(const container::Vector<Entry>& entries) {
    CanonicalHasher hash;
    hash.string("SpecialPowerCatalog.simulation");
    hash.u32(static_cast<uint32_t>(entries.size()));
    for (const Entry& entry : entries) {
        const SpecialPowerDefinition& definition = entry.definition;
        hash.string(entry.key);
        hash.u32(definition.reloadTimeMilliseconds);
        hash.string(definition.requiredScience);
        hash.boolean(definition.publicTimer);
        hash.u32(static_cast<uint32_t>(definition.specialPowerType));
        hash.u32(definition.detectionTimeMilliseconds);
        hash.boolean(definition.sharedSyncedTimer);
        hash.u32(definition.viewObjectDurationMilliseconds);
        hash.i64(definition.viewObjectRange.raw());
        hash.i64(definition.radiusCursorRadius.raw());
        hash.boolean(definition.shortcutPower);
        hash.string(definition.academyClassification);
    }
    return hash.finish();
}

container::Vector<container::String>
SpecialPowerCatalog::enumerateVfsLoadFiles(container::Span<const container::StringView> loadRoots) {
    return game::ini::enumerateLegacyIniDirectories(loadRoots);
}

bool SpecialPowerCatalog::loadFromVfs(container::StringView path, container::String* error) {
    return loadFromVfsFiles(container::Vector<container::String>{container::String{path}}, error);
}

bool SpecialPowerCatalog::applyOverridesFromVfs(
    container::StringView path, container::String* error) {
    return loadFromVfsFilesImpl(
        container::Vector<container::String>{container::String{path}},
        false, error);
}

bool SpecialPowerCatalog::loadFromVfsLoadDirectories(
    container::Span<const container::StringView> loadRoots, container::String* error) {
    return loadFromVfsFiles(enumerateVfsLoadFiles(loadRoots), error);
}

bool SpecialPowerCatalog::loadFromVfsFiles(
    const container::Vector<container::String>& logicalFiles, container::String* error) {
    return loadFromVfsFilesImpl(logicalFiles, true, error);
}

bool SpecialPowerCatalog::loadFromVfsFilesImpl(
    const container::Vector<container::String>& logicalFiles,
    bool resetCatalog, container::String* error) {
    if (error) error->clear();
    if (resetCatalog) clear();
    else if (!m_loaded) {
        if (error) *error = "SpecialPower override requires a loaded base catalog";
        return false;
    }
    const auto hardFail = [this, error](container::String message) {
        if (error) *error = std::move(message);
        clear();
        return false;
    };

    // Minimal content fixtures may omit SpecialPower.ini. Publish a sealed
    // empty value table so sessions still never fall back to a mutable global.
    if (logicalFiles.empty()) {
        m_simulationFingerprint = calculateFingerprint(m_entries);
        m_loaded = true;
        return true;
    }

    SpecialPowerDefinition currentDefault;
    currentDefault.name = "DefaultSpecialPower";
    if (!resetCatalog) {
        if (const SpecialPowerDefinition* existing =
                find("DefaultSpecialPower")) {
            currentDefault = *existing;
        }
    }

    container::HashSet<container::String> parsedFiles;
    for (const container::String& rawPath : logicalFiles) {
        const container::String path = canonicalPath(rawPath);
        if (path.empty()) {
            warnSpecialPowerContent(
                __FILE__, {}, "LogicalPath", rawPath, "input skipped",
                "SpecialPower INI input set contains an empty logical path");
            continue;
        }
        if (!parsedFiles.insert(path).second) continue;

        if (!io::VFS::instance().exists(path)) {
            warnSpecialPowerContent(
                path, {}, "File", path, "input skipped",
                "SpecialPower INI disappeared from VFS during load");
            continue;
        }
        const container::Vector<container::String> layers{
            io::VFS::instance().readAll(path)};
        for (size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
            const container::String source = layerSource(path, layerIndex);
            game::GeneralsIniParser parser;
            if (!parser.parse(layers[layerIndex], source)) {
                warnSpecialPowerContent(
                    source, {}, "IniLayer", {}, "layer skipped",
                    "could not parse SpecialPower INI layer");
                continue;
            }

            // RefCode applies declarations in parser order. Existing names
            // receive partial overrides; a first-time name copies the default
            // as it exists at that precise point in the layered stream.
            container::HashSet<container::String> definitionsInLayer;
            for (const game::IniBlock& block : parser.blocks()) {
                if (!equalInsensitive(block.type, "SpecialPower")) continue;
                const auto diagnosticScope =
                    game::contentDiagnosticProvenanceScope(block.source);

                const container::String name = trim(block.name);
                if (name.empty()) {
                    warnSpecialPowerContent(
                        source, {}, "Name", block.name, "block skipped",
                        "SpecialPower block has an empty definition name");
                    continue;
                }
                // RefCode accepts an existing definition only while loading a
                // later override source. A duplicate declaration inside one
                // physical source is invalid rather than a partial override.
                if (!definitionsInLayer.insert(name).second) {
                    warnSpecialPowerContent(
                        source, name, "Name", block.name,
                        "duplicate block skipped",
                        "duplicate SpecialPower declaration in one physical INI layer");
                    continue;
                }
                const bool isDefault = name == "DefaultSpecialPower";
                const auto found = std::lower_bound(
                    m_entries.begin(), m_entries.end(), name,
                    [](const Entry& entry, container::StringView wanted) { return entry.key < wanted; });
                const bool createsDefinition =
                    found == m_entries.end() || found->key != name;
                SpecialPowerDefinition candidate = createsDefinition
                    ? currentDefault
                    : found->definition;
                candidate.name = name;
                if (createsDefinition) {
                    candidate.id = INVALID_SPECIAL_POWER_CONTENT_ID;
                }
                bool validDefinition = true;
                const container::StringView rejectedValue = createsDefinition
                    ? container::StringView{"block skipped; no definition created"}
                    : container::StringView{"block skipped; retained prior definition"};

                const auto rejectField = [&](container::StringView field,
                                             container::StringView raw,
                                             container::String reason) {
                    warnSpecialPowerContent(
                        source, name, field, raw, rejectedValue,
                        std::move(reason));
                    validDefinition = false;
                };

                for (size_t valueIndex = 0;
                     valueIndex < block.values.size(); ++valueIndex) {
                    const auto& [fieldName, rawValue] =
                        block.values[valueIndex];
                    const auto fieldDiagnosticScope =
                        game::contentDiagnosticProvenanceScope(
                            block.valueSource(valueIndex));
                    const container::String field = canonical(fieldName);
                    if (field == "reloadtime") {
                        if (!parseMilliseconds(rawValue, candidate.reloadTimeMilliseconds)) {
                            rejectField(fieldName, rawValue,
                                        "invalid unsigned 32-bit ReloadTime");
                            break;
                        }
                    } else if (field == "requiredscience") {
                        if (!parseToken(rawValue, candidate.requiredScience)) {
                            rejectField(fieldName, rawValue,
                                        "RequiredScience must be one non-empty token");
                            break;
                        }
                    } else if (field == "initiatesound") {
                        if (!parseToken(rawValue, candidate.initiateSound)) {
                            rejectField(fieldName, rawValue,
                                        "InitiateSound must be one non-empty token");
                            break;
                        }
                    } else if (field == "initiateatlocationsound") {
                        if (!parseToken(rawValue, candidate.initiateAtLocationSound)) {
                            rejectField(fieldName, rawValue,
                                        "InitiateAtLocationSound must be one non-empty token");
                            break;
                        }
                    } else if (field == "publictimer") {
                        if (!parseBool(rawValue, candidate.publicTimer)) {
                            rejectField(fieldName, rawValue,
                                        "invalid boolean PublicTimer");
                            break;
                        }
                    } else if (field == "enum") {
                        container::String token;
                        if (!parseToken(rawValue, token)) {
                            rejectField(fieldName, rawValue,
                                        "Enum must be one non-empty token");
                            break;
                        }
                        const std::optional<game::SpecialPowerType> parsed =
                            game::tryParseSpecialPowerType(token);
                        if (!parsed) {
                            rejectField(fieldName, rawValue,
                                        "Enum is not a known SpecialPowerType");
                            break;
                        }
                        candidate.specialPowerType = *parsed;
                    } else if (field == "detectiontime") {
                        if (!parseMilliseconds(rawValue, candidate.detectionTimeMilliseconds)) {
                            rejectField(fieldName, rawValue,
                                        "invalid unsigned 32-bit DetectionTime");
                            break;
                        }
                    } else if (field == "sharedsyncedtimer") {
                        if (!parseBool(rawValue, candidate.sharedSyncedTimer)) {
                            rejectField(fieldName, rawValue,
                                        "invalid boolean SharedSyncedTimer");
                            break;
                        }
                    } else if (field == "viewobjectduration") {
                        if (!parseMilliseconds(rawValue,
                                               candidate.viewObjectDurationMilliseconds)) {
                            rejectField(fieldName, rawValue,
                                        "invalid unsigned 32-bit ViewObjectDuration");
                            break;
                        }
                    } else if (field == "viewobjectrange") {
                        if (!parseReal(rawValue, candidate.viewObjectRange)) {
                            rejectField(fieldName, rawValue,
                                        "invalid ViewObjectRange");
                            break;
                        }
                    } else if (field == "radiuscursorradius") {
                        if (!parseReal(rawValue, candidate.radiusCursorRadius)) {
                            rejectField(fieldName, rawValue,
                                        "invalid RadiusCursorRadius");
                            break;
                        }
                    } else if (field == "shortcutpower") {
                        if (!parseBool(rawValue, candidate.shortcutPower)) {
                            rejectField(fieldName, rawValue,
                                        "invalid boolean ShortcutPower");
                            break;
                        }
                    } else if (field == "academyclassify") {
                        if (!parseToken(rawValue, candidate.academyClassification)) {
                            rejectField(fieldName, rawValue,
                                        "AcademyClassify must be one non-empty token");
                            break;
                        }
                    }
                }

                if (!validDefinition) continue;
                Entry* entry = nullptr;
                if (createsDefinition) {
                    entry = &*m_entries.insert(found, {
                        .key = name,
                        .definition = std::move(candidate),
                    });
                } else {
                    found->definition = std::move(candidate);
                    entry = &*found;
                }
                if (isDefault) {
                    currentDefault = entry->definition;
                    currentDefault.name = "DefaultSpecialPower";
                    currentDefault.id = INVALID_SPECIAL_POWER_CONTENT_ID;
                }
            }
        }
    }

    for (size_t index = 0; index < m_entries.size(); ++index) {
        const uint64_t next = static_cast<uint64_t>(index) + 1u;
        if (next > std::numeric_limits<uint32_t>::max()) {
            return hardFail("SpecialPower catalog exceeds its 32-bit content ID space");
        }
        m_entries[index].definition.id = SpecialPowerContentId{static_cast<uint32_t>(next)};
    }
    m_simulationFingerprint = calculateFingerprint(m_entries);
    m_loaded = true;
    return true;
}

void SpecialPowerCatalog::clear() noexcept {
    m_entries.clear();
    m_simulationFingerprint = 0;
    m_loaded = false;
}

const SpecialPowerDefinition* SpecialPowerCatalog::find(container::StringView name) const {
    if (!m_loaded || name.empty()) return nullptr;
    const auto found = std::lower_bound(
        m_entries.begin(), m_entries.end(), name,
        [](const Entry& entry, container::StringView wanted) { return entry.key < wanted; });
    return found == m_entries.end() || container::StringView{found->key} != name
        ? nullptr
        : &found->definition;
}

const SpecialPowerDefinition* SpecialPowerCatalog::find(SpecialPowerContentId id) const noexcept {
    if (!m_loaded || !id || id.value > m_entries.size()) return nullptr;
    return &m_entries[id.value - 1u].definition;
}

} // namespace engine
