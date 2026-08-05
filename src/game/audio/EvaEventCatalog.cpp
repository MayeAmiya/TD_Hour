#include "EvaEventCatalog.h"

#include "core/container/string_utils.h"
#include "core/data/ini/GeneralsIniParser.h"

#include <charconv>
#include <limits>

namespace game {
namespace {

constexpr container::Array<container::StringView, kEvaEventTypeCount>
    kEvaEventNames{
        "LOWPOWER",
        "INSUFFICIENTFUNDS",
        "SUPERWEAPONDETECTED_OWN_PARTICLECANNON",
        "SUPERWEAPONDETECTED_OWN_NUKE",
        "SUPERWEAPONDETECTED_OWN_SCUDSTORM",
        "SUPERWEAPONDETECTED_ALLY_PARTICLECANNON",
        "SUPERWEAPONDETECTED_ALLY_NUKE",
        "SUPERWEAPONDETECTED_ALLY_SCUDSTORM",
        "SUPERWEAPONDETECTED_ENEMY_PARTICLECANNON",
        "SUPERWEAPONDETECTED_ENEMY_NUKE",
        "SUPERWEAPONDETECTED_ENEMY_SCUDSTORM",
        "SUPERWEAPONLAUNCHED_OWN_PARTICLECANNON",
        "SUPERWEAPONLAUNCHED_OWN_NUKE",
        "SUPERWEAPONLAUNCHED_OWN_SCUDSTORM",
        "SUPERWEAPONLAUNCHED_ALLY_PARTICLECANNON",
        "SUPERWEAPONLAUNCHED_ALLY_NUKE",
        "SUPERWEAPONLAUNCHED_ALLY_SCUDSTORM",
        "SUPERWEAPONLAUNCHED_ENEMY_PARTICLECANNON",
        "SUPERWEAPONLAUNCHED_ENEMY_NUKE",
        "SUPERWEAPONLAUNCHED_ENEMY_SCUDSTORM",
        "SUPERWEAPONREADY_OWN_PARTICLECANNON",
        "SUPERWEAPONREADY_OWN_NUKE",
        "SUPERWEAPONREADY_OWN_SCUDSTORM",
        "SUPERWEAPONREADY_ALLY_PARTICLECANNON",
        "SUPERWEAPONREADY_ALLY_NUKE",
        "SUPERWEAPONREADY_ALLY_SCUDSTORM",
        "SUPERWEAPONREADY_ENEMY_PARTICLECANNON",
        "SUPERWEAPONREADY_ENEMY_NUKE",
        "SUPERWEAPONREADY_ENEMY_SCUDSTORM",
        "BUILDINGLOST",
        "BASEUNDERATTACK",
        "ALLYUNDERATTACK",
        "BEACONDETECTED",
        "ENEMYBLACKLOTUSDETECTED",
        "ENEMYJARMENKELLDETECTED",
        "ENEMYCOLONELBURTONDETECTED",
        "OWNBLACKLOTUSDETECTED",
        "OWNJARMENKELLDETECTED",
        "OWNCOLONELBURTONDETECTED",
        "UNITLOST",
        "GENERALLEVELUP",
        "VEHICLESTOLEN",
        "BUILDINGSTOLEN",
        "CASHSTOLEN",
        "UPGRADECOMPLETE",
        "BUILDINGBEINGSTOLEN",
        "BUILDINGSABOTAGED",
        "SUPERWEAPONLAUNCHED_OWN_GPS_SCRAMBLER",
        "SUPERWEAPONLAUNCHED_ALLY_GPS_SCRAMBLER",
        "SUPERWEAPONLAUNCHED_ENEMY_GPS_SCRAMBLER",
        "SUPERWEAPONLAUNCHED_OWN_SNEAK_ATTACK",
        "SUPERWEAPONLAUNCHED_ALLY_SNEAK_ATTACK",
        "SUPERWEAPONLAUNCHED_ENEMY_SNEAK_ATTACK",
    };

[[nodiscard]] uint32_t parseUnsignedOr(container::StringView source,
                                       uint32_t fallback) noexcept {
    source = container::trimAsciiView(source);
    if (source.empty()) return fallback;
    uint32_t value = 0;
    const char* const begin = source.data();
    const char* const end = begin + source.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end ? value : fallback;
}

[[nodiscard]] container::Vector<container::String>
splitSounds(container::StringView source) {
    container::Vector<container::String> output;
    while (true) {
        source = container::trimAsciiView(source);
        if (source.empty()) break;
        const size_t split = source.find_first_of(" \t\r\n");
        const container::StringView token = split == container::StringView::npos
            ? source : source.substr(0, split);
        if (!token.empty()) output.emplace_back(token);
        if (split == container::StringView::npos) break;
        source.remove_prefix(split + 1u);
    }
    return output;
}

} // namespace

std::optional<EvaEventType>
parseEvaEventType(container::StringView name) noexcept {
    for (size_t index = 0; index < kEvaEventNames.size(); ++index) {
        if (container::asciiEqualIgnoreCase(name, kEvaEventNames[index])) {
            return static_cast<EvaEventType>(index);
        }
    }
    return std::nullopt;
}

container::StringView evaEventTypeName(EvaEventType type) noexcept {
    const size_t index = static_cast<size_t>(type);
    return index < kEvaEventNames.size() ? kEvaEventNames[index]
                                        : container::StringView{};
}

bool EvaEventCatalog::compile(container::StringView content,
                              container::StringView sourcePath,
                              container::String* error) {
    if (error) error->clear();
    m_definitions = {};
    m_size = 0;
    if (content.empty()) return true;

    GeneralsIniParser parser;
    if (!parser.parse(content, sourcePath)) {
        if (error) *error = "Eva.ini parse failed";
        return false;
    }
    for (const IniBlock& block : parser.blocks()) {
        if (!container::asciiEqualIgnoreCase(block.type, "EvaEvent"))
            continue;
        const std::optional<EvaEventType> type = parseEvaEventType(block.name);
        if (!type) continue;
        const size_t index = static_cast<size_t>(*type);
        // Eva::newEvaCheckInfo rejects a second definition: first wins.
        if (m_definitions[index]) continue;

        EvaEventDefinition definition;
        definition.type = *type;
        for (const auto& [key, value] : block.values) {
            if (container::asciiEqualIgnoreCase(key, "Priority")) {
                definition.priority = parseUnsignedOr(value, 1u);
            } else if (container::asciiEqualIgnoreCase(
                           key, "TimeBetweenChecksMS")) {
                definition.cooldownMilliseconds =
                    parseUnsignedOr(value, 30'000u);
            } else if (container::asciiEqualIgnoreCase(
                           key, "ExpirationTimeMS")) {
                definition.expirationMilliseconds =
                    parseUnsignedOr(value, 5'000u);
            }
        }
        for (const IniBlock& child : block.children) {
            if (!container::asciiEqualIgnoreCase(child.type, "SideSounds"))
                continue;
            EvaSideSounds side;
            for (const auto& [key, value] : child.values) {
                if (container::asciiEqualIgnoreCase(key, "Side")) {
                    side.side = container::trimAsciiCopy(value);
                } else if (container::asciiEqualIgnoreCase(key, "Sounds")) {
                    side.sounds = splitSounds(value);
                }
            }
            if (!side.side.empty())
                definition.sideSounds.push_back(std::move(side));
        }
        m_definitions[index] = std::move(definition);
        ++m_size;
    }
    return true;
}

const EvaEventDefinition*
EvaEventCatalog::find(EvaEventType type) const noexcept {
    const size_t index = static_cast<size_t>(type);
    return index < m_definitions.size() && m_definitions[index]
        ? &*m_definitions[index] : nullptr;
}

container::String EvaEventCatalog::resolveSound(
    EvaEventType type, container::StringView side,
    uint64_t variationSeed) const {
    const EvaEventDefinition* definition = find(type);
    if (!definition) return {};
    for (const EvaSideSounds& candidate : definition->sideSounds) {
        if (!container::asciiEqualIgnoreCase(candidate.side, side)) continue;
        if (candidate.sounds.empty()) return {};
        const container::String& selected = candidate.sounds[
            static_cast<size_t>(variationSeed % candidate.sounds.size())];
        return container::asciiEqualIgnoreCase(selected, "NoSound")
            ? container::String{} : selected;
    }
    return {};
}

} // namespace game
