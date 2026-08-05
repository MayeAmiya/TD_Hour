#include "DamageFxCatalog.h"

#include "LegacyIniDirectory.h"
#include "VFS.h"
#include "core/container/string_utils.h"
#include "ContentFloatParsing.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "debug/debug.h"
#include "presentation/fx/content/FxListCatalog.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <variant>

namespace game {
namespace {

[[nodiscard]] container::String canonicalName(container::StringView value) {
    value = container::trimAsciiView(value);
    container::String result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(character >= 'A' && character <= 'Z'
            ? static_cast<char>(character + ('a' - 'A'))
            : static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] bool equalInsensitive(
    container::StringView left, container::StringView right) noexcept {
    return container::asciiEqualIgnoreCase(
        container::trimAsciiView(left), container::trimAsciiView(right));
}

[[nodiscard]] container::Vector<container::StringView> words(
    container::StringView value) {
    container::Vector<container::StringView> output;
    size_t cursor = 0;
    while (cursor < value.size()) {
        cursor = value.find_first_not_of(" \t", cursor);
        if (cursor == container::StringView::npos) break;
        const size_t end = value.find_first_of(" \t", cursor);
        output.push_back(value.substr(cursor, end - cursor));
        cursor = end;
    }
    return output;
}

[[nodiscard]] std::optional<ObjectVeterancyLevel> parseVeterancy(
    container::StringView value) noexcept {
    if (equalInsensitive(value, "REGULAR")) return ObjectVeterancyLevel::Regular;
    if (equalInsensitive(value, "VETERAN")) return ObjectVeterancyLevel::Veteran;
    if (equalInsensitive(value, "ELITE")) return ObjectVeterancyLevel::Elite;
    if (equalInsensitive(value, "HEROIC")) return ObjectVeterancyLevel::Heroic;
    return std::nullopt;
}

[[nodiscard]] std::optional<uint32_t> parseDurationMilliseconds(
    container::StringView value) noexcept {
    value = container::trimAsciiView(value);
    if (value.empty()) return std::nullopt;

    uint32_t output = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto [cursor, error] = std::from_chars(begin, end, output);
    if (error != std::errc{} || cursor != end) return std::nullopt;
    return output;
}

[[nodiscard]] container::String parseFxListName(
    container::StringView value) {
    value = container::trimAsciiView(value);
    return equalInsensitive(value, "NONE") ? container::String{}
                                            : container::String{value};
}

template <typename Callback>
void forEachTargetRule(
    DamageFxDefinition& definition,
    const std::optional<ObjectVeterancyLevel>& veterancy,
    const std::optional<DamageType>& damageType, Callback&& callback) {
    const size_t firstVeterancy = veterancy
        ? static_cast<size_t>(*veterancy) : 0u;
    const size_t lastVeterancy = veterancy
        ? firstVeterancy + 1u : DamageFxDefinition::kVeterancyLevelCount;
    const size_t firstDamage = damageType
        ? static_cast<size_t>(*damageType) : 0u;
    const size_t lastDamage = damageType
        ? firstDamage + 1u : DamageFxDefinition::kDamageTypeCount;
    for (size_t damage = firstDamage; damage < lastDamage; ++damage) {
        for (size_t level = firstVeterancy; level < lastVeterancy; ++level) {
            callback(definition.rules[damage][level]);
        }
    }
}

[[nodiscard]] bool applyField(
    container::StringView key, container::StringView rawValue,
    DamageFxDefinition& output, container::StringView sourceName,
    container::Vector<container::String>& diagnostics,
    container::String* error) {
    constexpr container::StringView kVeterancyPrefix = "Veterancy";
    const bool veterancyField =
        key.size() > kVeterancyPrefix.size() &&
        container::asciiEqualIgnoreCase(
            key.substr(0, kVeterancyPrefix.size()), kVeterancyPrefix);
    const container::StringView baseKey = veterancyField
        ? key.substr(kVeterancyPrefix.size()) : key;
    const bool amountField = equalInsensitive(baseKey, "AmountForMajorFX");
    const bool majorField = equalInsensitive(baseKey, "MajorFX");
    const bool minorField = equalInsensitive(baseKey, "MinorFX");
    const bool throttleField = equalInsensitive(baseKey, "ThrottleTime");
    if (!amountField && !majorField && !minorField && !throttleField) {
        diagnostics.push_back(
            "DamageFX '" + output.name + "' has unknown field '" +
            container::String{key} + "' in " + container::String{sourceName});
        return true;
    }

    const container::Vector<container::StringView> tokens = words(rawValue);
    const size_t expected = veterancyField ? 3u : 2u;
    const auto fail = [&](container::StringView reason) {
        if (error) {
            *error = "DamageFX '" + output.name + "' " +
                container::String{reason} + " for field '" +
                container::String{key} + " = " + container::String{rawValue} +
                "' in " + container::String{sourceName};
        }
        return false;
    };
    if (tokens.size() != expected) return fail("has invalid token count");

    size_t cursor = 0;
    std::optional<ObjectVeterancyLevel> veterancy;
    if (veterancyField) {
        veterancy = parseVeterancy(tokens[cursor++]);
        if (!veterancy) return fail("has unknown veterancy");
    }

    std::optional<DamageType> damageType;
    const container::StringView damageToken = tokens[cursor++];
    if (!equalInsensitive(damageToken, "DEFAULT")) {
        damageType = tryParseDamageType(damageToken);
        if (!damageType) return fail("has unknown damage type");
    }
    const container::StringView authoredValue = tokens[cursor];

    if (amountField) {
        const std::optional<float> amount =
            game::parseContentFloat(authoredValue, {
                .source = __FILE__, .block = "DamageFX",
                .field = "MajorThreshold"});
        if (!amount) return fail("has invalid major threshold");
        forEachTargetRule(output, veterancy, damageType,
            [value = math::q32_32{*amount}](DamageFxRule& rule) {
                rule.amountForMajorFx = value;
            });
        return true;
    }
    if (throttleField) {
        const std::optional<uint32_t> duration =
            parseDurationMilliseconds(authoredValue);
        if (!duration) return fail("has invalid throttle duration");
        forEachTargetRule(output, veterancy, damageType,
            [value = *duration](DamageFxRule& rule) {
                rule.throttleTimeMilliseconds = value;
            });
        return true;
    }

    container::String fxListName = parseFxListName(authoredValue);
    forEachTargetRule(output, veterancy, damageType,
        [&](DamageFxRule& rule) {
            DamageFxEffectReference& effect = majorField
                ? rule.major : rule.minor;
            effect.fxListName = fxListName;
        });
    return true;
}

void collectSoundEvents(
    const engine::fx::FxListCatalog& catalog,
    const engine::fx::FxListDefinition& definition,
    container::HashSet<uint32_t>& activeChain,
    container::Vector<container::String>& output) {
    if (!definition.id || !activeChain.insert(definition.id.value).second) return;
    for (const engine::fx::FxNugget& nugget : definition.nuggets) {
        if (const auto* sound =
                std::get_if<engine::fx::FxSoundNugget>(&nugget)) {
            if (!sound->name.empty() &&
                !equalInsensitive(sound->name, "NONE")) {
                output.push_back(sound->name);
            }
        } else if (const auto* nested =
                       std::get_if<engine::fx::FxListAtBoneNugget>(&nugget)) {
            const engine::fx::FxListDefinition* nestedDefinition = nested->fx
                ? catalog.find(nested->fx) : catalog.find(nested->fxName);
            if (nestedDefinition) {
                collectSoundEvents(
                    catalog, *nestedDefinition, activeChain, output);
            }
        }
    }
    activeChain.erase(definition.id.value);
}

} // namespace

const DamageFxRule* DamageFxDefinition::findRule(
    DamageType damageType, ObjectVeterancyLevel veterancy) const noexcept {
    const size_t damageIndex = static_cast<size_t>(damageType);
    const size_t veterancyIndex = static_cast<size_t>(veterancy);
    if (damageIndex >= rules.size() ||
        veterancyIndex >= rules[damageIndex].size()) {
        return nullptr;
    }
    return &rules[damageIndex][veterancyIndex];
}

container::Vector<container::String> DamageFxCatalog::enumerateVfsLoadFiles(
    container::Span<const container::StringView> loadRoots) {
    return ini::enumerateLegacyIniDirectories(loadRoots);
}

bool DamageFxCatalog::loadFromVfsFiles(
    const container::Vector<container::String>& logicalFiles,
    container::String* error) {
    if (error) error->clear();
    clear();
    auto& vfs = io::VFS::instance();
    container::HashSet<container::String> parsedFiles;
    for (const container::String& rawPath : logicalFiles) {
        const container::String path = ini::canonicalLegacyIniPath(rawPath);
        if (path.empty() || !parsedFiles.insert(path).second) continue;
        // Check reachability explicitly.  `readAll` returns a single String, so
        // the vector below always holds exactly one element and the old
        // `layers.empty()` test could never fire — an INI that became unreadable
        // mid-load was silently parsed as empty content and the catalog sealed
        // successfully with definitions missing.
        if (!vfs.exists(path)) {
            if (error) {
                *error = "DamageFX source disappeared from VFS during load: " +
                    path;
            }
            clear();
            return false;
        }
        const container::Vector<container::String> layers{
            vfs.readAll(path)};
        for (size_t layer = 0; layer < layers.size(); ++layer) {
            const container::String source =
                path + "#layer" + std::to_string(layer);
            if (!appendParsedLayer(layers[layer], source, error)) {
                clear();
                return false;
            }
        }
    }
    sealDefinitions();
    TD_LOG_INFO(
        "[DamageFxCatalog] Loaded {} DamageFX definitions from {} source files",
        m_definitions.size(), parsedFiles.size());
    return true;
}

bool DamageFxCatalog::applyOverridesFromVfs(
    container::StringView rawPath, container::String* error) {
    if (error) error->clear();
    if (!m_loaded) {
        if (error) *error = "DamageFX override requires a loaded base catalog";
        return false;
    }

    const container::String path = ini::canonicalLegacyIniPath(rawPath);
    if (path.empty()) {
        if (error) *error = "DamageFX override path is empty";
        return false;
    }
    auto& vfs = io::VFS::instance();
    if (!vfs.exists(path)) {
        if (error) *error = "DamageFX override source is absent from VFS: " + path;
        return false;
    }

    const container::String content = vfs.readAll(path);

    // appendParsedLayer already implements RefCode's exact duplicate rule:
    // m_dfxmap[key] is cleared before the successor block is parsed. Stage
    // the operation so malformed Map.ini data cannot partially mutate the
    // receiver (which is expected to be the session-private catalog copy).
    DamageFxCatalog staged = *this;
    if (!staged.appendParsedLayer(content, path, error)) return false;
    staged.sealDefinitions();

    // Sound projections are keyed by the retained symbolic FXList names and
    // remain valid for the same immutable FXList catalog. Newly introduced
    // names stay symbolic in DamageFxEffectReference and can be projected by
    // the normal resolveFxReferences() pass without renderer access here.
    *this = std::move(staged);
    return true;
}

bool DamageFxCatalog::loadFromVfsLoadDirectories(
    container::Span<const container::StringView> loadRoots,
    container::String* error) {
    return loadFromVfsFiles(enumerateVfsLoadFiles(loadRoots), error);
}

bool DamageFxCatalog::loadFromText(
    container::StringView content, container::StringView sourceName,
    container::String* error) {
    if (error) error->clear();
    clear();
    if (!appendParsedLayer(content, sourceName, error)) {
        clear();
        return false;
    }
    sealDefinitions();
    return true;
}

bool DamageFxCatalog::appendParsedLayer(
    container::StringView content, container::StringView sourceName,
    container::String* error) {
    GeneralsIniParser parser;
    if (!parser.parse(content)) {
        if (error) {
            *error = "could not parse DamageFX source '" +
                container::String{sourceName} + "'";
        }
        return false;
    }
    for (const IniBlock& block : parser.blocks()) {
        if (!equalInsensitive(block.type, "DamageFX")) continue;
        const container::String key = canonicalName(block.name);
        if (key.empty()) {
            if (error) {
                *error = "DamageFX with empty name in " +
                    container::String{sourceName};
            }
            return false;
        }

        // RefCode's parseDamageFXDefinition obtains m_dfxmap[key], clears the
        // complete table, then applies the new block. A later VFS layer or
        // fragment therefore replaces, rather than inherits, the definition.
        DamageFxDefinition definition;
        definition.name = container::String{container::trimAsciiView(block.name)};
        for (const auto& [field, value] : block.values) {
            if (!applyField(field, value, definition, sourceName,
                            m_diagnostics, error)) {
                return false;
            }
        }

        const auto existing = m_indices.find(key);
        if (existing == m_indices.end()) {
            m_indices.emplace(key, m_definitions.size());
            m_definitions.push_back(std::move(definition));
        } else {
            m_definitions[existing->second] = std::move(definition);
        }
    }
    return true;
}

void DamageFxCatalog::sealDefinitions() {
    std::sort(m_definitions.begin(), m_definitions.end(),
        [](const DamageFxDefinition& left,
           const DamageFxDefinition& right) {
            return canonicalName(left.name) < canonicalName(right.name);
        });
    m_indices.clear();
    m_indices.reserve(m_definitions.size());
    for (size_t index = 0; index < m_definitions.size(); ++index) {
        DamageFxDefinition& definition = m_definitions[index];
        definition.id = DamageFxContentId{
            static_cast<uint32_t>(index + 1u)};
        m_indices.emplace(canonicalName(definition.name), index);
    }
    m_loaded = true;
}

void DamageFxCatalog::resolveFxReferences(
    const engine::fx::FxListCatalog& fxLists) {
    m_soundEventsByFxList.clear();
    container::HashSet<container::String> referenced;
    for (const DamageFxDefinition& definition : m_definitions) {
        for (const auto& byVeterancy : definition.rules) {
            for (const DamageFxRule& rule : byVeterancy) {
                if (!rule.minor.fxListName.empty()) {
                    referenced.insert(canonicalName(rule.minor.fxListName));
                }
                if (!rule.major.fxListName.empty()) {
                    referenced.insert(canonicalName(rule.major.fxListName));
                }
            }
        }
    }

    container::Vector<container::String> ordered{
        referenced.begin(), referenced.end()};
    std::sort(ordered.begin(), ordered.end());
    for (const container::String& key : ordered) {
        const engine::fx::FxListDefinition* definition = fxLists.find(key);
        if (!definition) {
            m_diagnostics.push_back(
                "DamageFX references missing FXList '" + key + "'");
            m_soundEventsByFxList.emplace(key,
                container::Vector<container::String>{});
            continue;
        }
        container::Vector<container::String> sounds;
        container::HashSet<uint32_t> activeChain;
        collectSoundEvents(fxLists, *definition, activeChain, sounds);
        m_soundEventsByFxList.emplace(key, std::move(sounds));
    }
}

void DamageFxCatalog::clear() noexcept {
    m_definitions.clear();
    m_indices.clear();
    m_soundEventsByFxList.clear();
    m_diagnostics.clear();
    m_loaded = false;
}

const DamageFxDefinition* DamageFxCatalog::find(
    container::StringView name) const noexcept {
    const auto found = m_indices.find(canonicalName(name));
    return found != m_indices.end() && found->second < m_definitions.size()
        ? &m_definitions[found->second] : nullptr;
}

const DamageFxDefinition* DamageFxCatalog::find(
    DamageFxContentId id) const noexcept {
    return id.value > 0 && id.value <= m_definitions.size()
        ? &m_definitions[id.value - 1u] : nullptr;
}

DamageFxContentId DamageFxCatalog::findId(
    container::StringView name) const noexcept {
    if (name.empty() || equalInsensitive(name, "NONE")) return {};
    const DamageFxDefinition* definition = find(name);
    return definition ? definition->id : DamageFxContentId{};
}

const DamageFxRule* DamageFxCatalog::findRule(
    container::StringView name, DamageType damageType,
    ObjectVeterancyLevel veterancy) const noexcept {
    const DamageFxDefinition* definition = find(name);
    return definition ? definition->findRule(damageType, veterancy) : nullptr;
}

const DamageFxRule* DamageFxCatalog::findRule(
    DamageFxContentId id, DamageType damageType,
    ObjectVeterancyLevel veterancy) const noexcept {
    const DamageFxDefinition* definition = find(id);
    return definition ? definition->findRule(damageType, veterancy) : nullptr;
}

const DamageFxEffectReference* DamageFxCatalog::selectEffect(
    container::StringView name, DamageType damageType,
    ObjectVeterancyLevel veterancy,
    math::q32_32 damageAmount) const noexcept {
    // RefCode intentionally suppresses DamageFX for zero-damage faux weapons.
    if (damageAmount == math::q32_32{}) return nullptr;
    const DamageFxRule* rule = findRule(name, damageType, veterancy);
    if (!rule) return nullptr;
    return damageAmount >= rule->amountForMajorFx
        ? &rule->major : &rule->minor;
}

const DamageFxEffectReference* DamageFxCatalog::selectEffect(
    DamageFxContentId id, DamageType damageType,
    ObjectVeterancyLevel veterancy,
    math::q32_32 damageAmount) const noexcept {
    if (damageAmount == math::q32_32{}) return nullptr;
    const DamageFxRule* rule = findRule(id, damageType, veterancy);
    if (!rule) return nullptr;
    return damageAmount >= rule->amountForMajorFx
        ? &rule->major : &rule->minor;
}

container::Span<const container::String> DamageFxCatalog::soundEvents(
    const DamageFxEffectReference& effect) const noexcept {
    if (effect.fxListName.empty()) return {};
    const auto found =
        m_soundEventsByFxList.find(canonicalName(effect.fxListName));
    return found == m_soundEventsByFxList.end()
        ? container::Span<const container::String>{}
        : container::Span<const container::String>{found->second};
}

} // namespace game
