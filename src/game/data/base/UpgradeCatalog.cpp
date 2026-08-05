#include "core/container/hash_containers.h"
#include "UpgradeCatalog.h"

#include "LegacyIniDirectory.h"
#include "VFS.h"
#include "core/container/string_utils.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "ContentFloatParsing.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
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

[[nodiscard]] bool parseNonNegativeInteger(container::StringView value, int64_t& output) {
    const container::String cleaned = trim(value);
    if (cleaned.empty()) return false;
    const char* first = cleaned.data();
    const char* last = first + cleaned.size();
    int64_t parsed = 0;
    const auto [end, error] = std::from_chars(first, last, parsed);
    if (error != std::errc{} || end != last || parsed < 0) return false;
    output = parsed;
    return true;
}

[[nodiscard]] bool parseNonNegativeTime(container::StringView value, math::q32_32& output) {
    const game::ContentFloatContext context{
        .source = __FILE__, .block = "Upgrade", .field = "BuildTime",
        .fallback = output.to_float()};
    const std::optional<float> parsed =
        game::parseContentFloat(value, context);
    if (!parsed) return true;
    if (*parsed < 0.0f ||
        *parsed >= static_cast<float>(std::numeric_limits<int32_t>::max())) {
        game::warnContentFloatFallback(
            value, context,
            "finite numeric prefix is outside the non-negative BuildTime range; retained the prior/default value");
        return true;
    }
    // RefCode parses Upgrade.BuildTime as `Real`, which is a 32-bit float,
    // before ProductionUpdate multiplies it by the logic frame rate.  Preserve
    // that authoring ingress exactly, then freeze the resulting value as Q32
    // for all modern confirmed-frame math (notably 0.1f * 30 == 3 frames).
    output = math::q32_32{*parsed};
    return true;
}

[[nodiscard]] bool parseType(container::StringView value, UpgradeDefinitionType& output) {
    const container::String normalized = canonical(value);
    if (normalized == "player") {
        output = UpgradeDefinitionType::Player;
        return true;
    }
    if (normalized == "object") {
        output = UpgradeDefinitionType::Object;
        return true;
    }
    return false;
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

uint64_t UpgradeCatalog::calculateFingerprint(const container::Vector<Entry>& entries) {
    CanonicalHasher hash;
    hash.string("UpgradeCatalog.simulation");
    hash.u32(static_cast<uint32_t>(entries.size()));
    for (const Entry& entry : entries) {
        hash.string(entry.key);
        hash.u32(static_cast<uint32_t>(entry.definition.type));
        hash.i64(entry.definition.buildTimeSeconds.raw());
        hash.i64(entry.definition.buildCost);
    }
    return hash.finish();
}

container::Vector<container::String>
UpgradeCatalog::enumerateVfsLoadFiles(container::Span<const container::StringView> loadRoots) {
    return game::ini::enumerateLegacyIniDirectories(loadRoots);
}

bool UpgradeCatalog::loadFromVfs(container::StringView path, container::String* error) {
    return loadFromVfsFiles(container::Vector<container::String>{container::String{path}}, error);
}

bool UpgradeCatalog::applyOverridesFromVfs(
    container::StringView path, container::String* error) {
    return loadFromVfsFilesImpl(
        container::Vector<container::String>{container::String{path}},
        false, error);
}

bool UpgradeCatalog::loadFromVfsLoadDirectories(container::Span<const container::StringView> loadRoots,
                                                container::String* error) {
    return loadFromVfsFiles(enumerateVfsLoadFiles(loadRoots), error);
}

bool UpgradeCatalog::loadFromVfsFiles(const container::Vector<container::String>& logicalFiles,
                                      container::String* error) {
    return loadFromVfsFilesImpl(logicalFiles, true, error);
}

bool UpgradeCatalog::loadFromVfsFilesImpl(
    const container::Vector<container::String>& logicalFiles,
    bool resetCatalog, container::String* error) {
    if (error) error->clear();
    if (resetCatalog) clear();
    else if (!m_loaded) {
        if (error) *error = "Upgrade override requires a loaded base catalog";
        return false;
    }
    const auto fail = [this, error](container::String message) {
        if (error) *error = std::move(message);
        clear();
        return false;
    };

    // Minimal probes and early content may deliberately omit Upgrade.ini.
    // Publish a sealed empty catalog so active sessions still never query a
    // mutable global store.
    if (logicalFiles.empty()) {
        // UpgradeCenter::init creates these three Object-scoped templates
        // before it parses any Upgrade.ini.  Retain them even for minimal
        // fixtures so future Object/Veterancy systems never need a hidden
        // special-case global lookup.  An authored block of the same name is
        // still allowed to override it below, matching the legacy sequence.
        for (const container::StringView name :
             {well_known_upgrade::VeterancyVeteran,
              well_known_upgrade::VeterancyElite,
              well_known_upgrade::VeterancyHeroic}) {
            m_entries.push_back({
                .key = container::String{name},
                .definition = {
                    .name = container::String{name},
                    .type = UpgradeDefinitionType::Object,
                },
            });
        }
        std::sort(m_entries.begin(), m_entries.end(),
                  [](const Entry& left, const Entry& right) { return left.key < right.key; });
        for (size_t index = 0; index < m_entries.size(); ++index) {
            m_entries[index].definition.id = UpgradeContentId{static_cast<uint32_t>(index + 1u)};
        }
        m_simulationFingerprint = calculateFingerprint(m_entries);
        m_loaded = true;
        return true;
    }

    // RefCode UpgradeCenter::init creates these before INI parsing; preserve
    // their Object/zero-time/zero-cost identity and let later authored blocks
    // partially override them using the normal existing-entry path.
    if (resetCatalog) for (const container::StringView name :
                           {well_known_upgrade::VeterancyVeteran,
                            well_known_upgrade::VeterancyElite,
                            well_known_upgrade::VeterancyHeroic}) {
        m_entries.push_back({
            .key = container::String{name},
            .definition = {
                .name = container::String{name},
                .type = UpgradeDefinitionType::Object,
            },
        });
    }
    std::sort(m_entries.begin(), m_entries.end(),
              [](const Entry& left, const Entry& right) { return left.key < right.key; });

    UpgradeDefinition currentDefault;
    currentDefault.name = "DefaultUpgrade";
    if (!resetCatalog) {
        if (const UpgradeDefinition* existing = find("DefaultUpgrade")) {
            currentDefault = *existing;
        }
    }
    container::HashSet<container::String> parsedFiles;
    for (const container::String& rawPath : logicalFiles) {
        const container::String path = canonicalPath(rawPath);
        if (path.empty()) return fail("Upgrade INI input set contains an empty logical path");
        if (!parsedFiles.insert(path).second) continue;

        // `readAll` returns a single String, so the vector below always holds
        // exactly one element: the old `layers.empty()` guard was unreachable and
        // an unreadable Upgrade.ini silently yielded a catalog containing only
        // the synthetic veterancy upgrades.  Test reachability directly.
        if (!io::VFS::instance().exists(path)) {
            return fail("Upgrade INI disappeared from VFS during load: " + path);
        }
        const container::Vector<container::String> layers{
            io::VFS::instance().readAll(path)};
        for (size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
            const container::String source = path + " [layer " +
                std::to_string(layerIndex) + "]";
            game::GeneralsIniParser parser;
            if (!parser.parse(layers[layerIndex], source)) {
                return fail("could not parse Upgrade INI layer " + std::to_string(layerIndex) +
                            ": " + path);
            }

            for (const game::IniBlock& block : parser.blocks()) {
                if (!equalInsensitive(block.type, "Upgrade")) continue;
                const auto diagnosticScope =
                    game::contentDiagnosticProvenanceScope(block.source);

                const container::String name = trim(block.name);
                if (name.empty()) {
                    return fail("Upgrade block with an empty name in " + path + " layer " +
                                std::to_string(layerIndex));
                }
                // Unlike Science, RefCode applies same-layer Upgrade blocks
                // in parser order. A second block is a defined partial
                // override, and the time at which DefaultUpgrade changes is
                // observable by later first-time names in this very layer.
                const container::String& key = name;
                const bool isDefault = name == "DefaultUpgrade";
                const auto found = std::lower_bound(m_entries.begin(), m_entries.end(), key,
                    [](const Entry& entry, container::StringView wanted) { return entry.key < wanted; });
                Entry* entry = nullptr;
                if (found == m_entries.end() || found->key != key) {
                    UpgradeDefinition definition = currentDefault;
                    definition.name = name;
                    definition.id = INVALID_UPGRADE_CONTENT_ID;
                    auto inserted = m_entries.insert(found, {
                        .key = key,
                        .definition = std::move(definition),
                    });
                    entry = &*inserted;
                } else {
                    entry = &*found;
                }

                for (size_t valueIndex = 0;
                     valueIndex < block.values.size(); ++valueIndex) {
                    const auto& [fieldName, rawValue] =
                        block.values[valueIndex];
                    const auto fieldDiagnosticScope =
                        game::contentDiagnosticProvenanceScope(
                            block.valueSource(valueIndex));
                    const container::String field = canonical(fieldName);
                    if (field == "type") {
                        if (!parseType(rawValue, entry->definition.type)) {
                            return fail("invalid Upgrade.Type for '" + name + "' in " + path +
                                        " layer " + std::to_string(layerIndex));
                        }
                    } else if (field == "buildtime") {
                        if (!parseNonNegativeTime(rawValue, entry->definition.buildTimeSeconds)) {
                            return fail("invalid Upgrade.BuildTime for '" + name + "' in " + path +
                                        " layer " + std::to_string(layerIndex));
                        }
                    } else if (field == "buildcost") {
                        if (!parseNonNegativeInteger(rawValue, entry->definition.buildCost)) {
                            return fail("invalid Upgrade.BuildCost for '" + name + "' in " + path +
                                        " layer " + std::to_string(layerIndex));
                        }
                    } else if (field == "displayname") {
                        entry->definition.displayNameLabel = rawValue;
                    } else if (field == "buttonimage") {
                        entry->definition.buttonImage = rawValue;
                    } else if (field == "researchsound") {
                        entry->definition.researchCompleteSound = rawValue;
                    } else if (field == "unitspecificsound") {
                        entry->definition.unitSpecificSound = rawValue;
                    } else if (field == "academyclassify") {
                        entry->definition.academyClassification = rawValue;
                    }
                }

                if (isDefault) {
                    currentDefault = entry->definition;
                    currentDefault.name = "DefaultUpgrade";
                    currentDefault.id = INVALID_UPGRADE_CONTENT_ID;
                }
            }
        }
    }

    if (m_entries.size() > kUpgradeMaskBits) {
        return fail("Upgrade catalog exceeds UpgradeMask bit capacity");
    }
    for (size_t index = 0; index < m_entries.size(); ++index) {
        const uint64_t next = static_cast<uint64_t>(index) + 1u;
        if (next > std::numeric_limits<uint32_t>::max()) {
            return fail("Upgrade catalog exceeds its 32-bit content ID space");
        }
        m_entries[index].definition.id = UpgradeContentId{static_cast<uint32_t>(next)};
    }
    m_simulationFingerprint = calculateFingerprint(m_entries);
    m_loaded = true;
    return true;
}

void UpgradeCatalog::clear() noexcept {
    m_entries.clear();
    m_simulationFingerprint = 0;
    m_loaded = false;
}

const UpgradeDefinition* UpgradeCatalog::find(container::StringView name) const {
    if (!m_loaded || name.empty()) return nullptr;
    const container::String key = trim(name);
    const auto found = std::lower_bound(m_entries.begin(), m_entries.end(), key,
        [](const Entry& entry, container::StringView wanted) { return entry.key < wanted; });
    return found == m_entries.end() || found->key != key ? nullptr : &found->definition;
}

const UpgradeDefinition* UpgradeCatalog::find(UpgradeContentId id) const noexcept {
    if (!m_loaded || !id || id.value > m_entries.size()) return nullptr;
    return &m_entries[id.value - 1].definition;
}

} // namespace engine
