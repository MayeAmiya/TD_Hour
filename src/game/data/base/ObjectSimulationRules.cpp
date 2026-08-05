#include "ObjectSimulationRules.h"

#include "core/container/string_utils.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "game/data/base/ContentFloatParsing.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

using container::asciiEqualIgnoreCase;

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

[[nodiscard]] std::optional<uint32_t> parseUint32(
    container::StringView text) noexcept {
    text = container::trimAsciiView(text);
    if (text.empty() || text.front() == '-') return std::nullopt;
    const container::String owned{text};
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(owned.c_str(), &end, 10);
    if (end == owned.c_str() || *end != '\0' || errno == ERANGE ||
        value > std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(value);
}

[[nodiscard]] std::optional<math::q32_32> parseFixedReal(
    container::StringView text) noexcept {
    const std::optional<float> parsed = game::parseContentFloat(text, {
        .source = __FILE__, .block = "GameData", .field = "FixedReal"});
    if (!parsed) return std::nullopt;
    constexpr float minimum =
        static_cast<float>(std::numeric_limits<int32_t>::min());
    constexpr float maximum =
        static_cast<float>(std::numeric_limits<int32_t>::max());
    if (*parsed <= minimum || *parsed >= maximum) return std::nullopt;
    return math::q32_32{*parsed};
}

[[nodiscard]] std::optional<ObjectBodyDamageState> parseBodyDamageState(
    container::StringView text) noexcept {
    text = container::trimAsciiView(text);
    if (asciiEqualIgnoreCase(text, "PRISTINE"))
        return ObjectBodyDamageState::Pristine;
    if (asciiEqualIgnoreCase(text, "DAMAGED"))
        return ObjectBodyDamageState::Damaged;
    if (asciiEqualIgnoreCase(text, "REALLYDAMAGED"))
        return ObjectBodyDamageState::ReallyDamaged;
    if (asciiEqualIgnoreCase(text, "RUBBLE"))
        return ObjectBodyDamageState::Rubble;
    return std::nullopt;
}

[[nodiscard]] bool applyAggregateGameDataBlocks(
    const container::Vector<game::IniBlock>& blocks,
    ObjectSimulationRules& candidate, bool& modified,
    container::String* error) {
    for (const game::IniBlock& block : blocks) {
        if (!asciiEqualIgnoreCase(block.type, "GameData")) continue;
        for (const auto& [key, text] : block.values) {
            if (asciiEqualIgnoreCase(key, "UnitDamagedThreshold")) {
                const std::optional<math::q32_32> parsed =
                    parseFixedReal(text);
                if (!parsed) {
                    setError(error, "invalid GameData.UnitDamagedThreshold value '" +
                                        text + "'");
                    return false;
                }
                candidate.unitDamagedThresholdFixed = *parsed;
            } else if (asciiEqualIgnoreCase(
                           key, "UnitReallyDamagedThreshold")) {
                const std::optional<math::q32_32> parsed =
                    parseFixedReal(text);
                if (!parsed) {
                    setError(error,
                             "invalid GameData.UnitReallyDamagedThreshold value '" +
                                 text + "'");
                    return false;
                }
                candidate.unitReallyDamagedThresholdFixed = *parsed;
            } else if (asciiEqualIgnoreCase(
                           key, "MovementPenaltyDamageState")) {
                const std::optional<ObjectBodyDamageState> parsed =
                    parseBodyDamageState(text);
                if (!parsed) {
                    setError(error, "invalid GameData.MovementPenaltyDamageState value '" +
                                        text + "'");
                    return false;
                }
                candidate.movementPenaltyDamageState = *parsed;
            } else if (asciiEqualIgnoreCase(key, "MaxTunnelCapacity")) {
                const std::optional<uint32_t> parsed = parseUint32(text);
                if (!parsed) {
                    setError(error, "invalid GameData.MaxTunnelCapacity value '" +
                                        text + "'");
                    return false;
                }
                candidate.maxTunnelCapacity = *parsed;
            } else if (asciiEqualIgnoreCase(
                           key, "StandardMinefieldDistance")) {
                const std::optional<math::q32_32> parsed = parseFixedReal(text);
                if (!parsed) {
                    continue;
                }
                candidate.standardMinefieldDistance = *parsed;
            } else if (asciiEqualIgnoreCase(
                           key, "StandardMinefieldDensity")) {
                const std::optional<math::q32_32> parsed = parseFixedReal(text);
                if (!parsed) {
                    continue;
                }
                candidate.standardMinefieldDensity = *parsed;
            } else if (asciiEqualIgnoreCase(
                           key, "GroupMoveClickToGatherAreaFactor")) {
                const std::optional<math::q32_32> parsed =
                    parseFixedReal(text);
                if (!parsed) continue;
                candidate.groupMoveClickToGatherFactor = *parsed;
            } else if (asciiEqualIgnoreCase(
                           key, "SpecialPowerViewObject")) {
                container::String name{container::trimAsciiCopy(
                    container::StringView{text})};
                if (asciiEqualIgnoreCase(name, "None")) name.clear();
                candidate.specialPowerViewObject = std::move(name);
            } else {
                continue;
            }
            modified = true;
        }
    }
    return true;
}

[[nodiscard]] bool validateAggregate(
    const ObjectSimulationRules& rules, container::String* error) {
    const math::q32_32 zero{};
    const math::q32_32 one{int32_t{1}};
    if (rules.unitDamagedThresholdFixed < zero ||
        rules.unitDamagedThresholdFixed > one) {
        setError(error, "GameData.UnitDamagedThreshold must be within [0, 1]");
        return false;
    }
    if (rules.unitReallyDamagedThresholdFixed < zero ||
        rules.unitReallyDamagedThresholdFixed >
            rules.unitDamagedThresholdFixed) {
        setError(error,
                 "GameData.UnitReallyDamagedThreshold must be within [0, UnitDamagedThreshold]");
        return false;
    }
    if (rules.groupMoveClickToGatherFactor < math::q32_32{}) {
        setError(error,
                 "GameData.GroupMoveClickToGatherAreaFactor must not be negative");
        return false;
    }
    return true;
}

} // namespace

bool ObjectSimulationRules::applyLegacyGameDataOverrides(
    container::StringView content, container::StringView sourceName,
    container::String* error) {
    if (error) error->clear();

    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        setError(error, "could not parse GameData modifier '" +
                            container::String{sourceName} + "'");
        return false;
    }

    const bool hasGameDataBlock = std::any_of(
        parser.blocks().begin(), parser.blocks().end(),
        [](const game::IniBlock& block) {
            return asciiEqualIgnoreCase(block.type, "GameData");
        });
    if (!hasGameDataBlock) return true;

    ObjectSimulationRules candidate = *this;
    bool aggregateModified = false;
    if (!applyAggregateGameDataBlocks(
            parser.blocks(), candidate, aggregateModified, error) ||
        !candidate.baseRegeneration.applyLegacyGameDataOverrides(
            content, sourceName, error) ||
        !candidate.buildPlacement.applyLegacyGameDataOverrides(
            content, sourceName, error) ||
        !candidate.energy.applyLegacyGameDataOverrides(
            content, sourceName, error) ||
        !candidate.economy.applyLegacyGameDataOverrides(
            content, sourceName, error) ||
        !candidate.difficulty.applyLegacyGameDataOverrides(
            content, sourceName, error) ||
        !candidate.veterancy.applyLegacyGameDataOverrides(
            content, sourceName, error) ||
        !candidate.physics.applyLegacyGameDataOverrides(
            content, sourceName, error)) {
        return false;
    }

    if (aggregateModified && !validateAggregate(candidate, error)) return false;
    *this = std::move(candidate);
    return true;
}

bool ObjectSimulationRules::applyLegacyAIDataOverrides(
    container::StringView content, container::StringView sourceName,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content)) {
        setError(error, "could not parse AIData modifier '" +
                            container::String{sourceName} + "'");
        return false;
    }
    const bool hasAIDataBlock = std::any_of(
        parser.blocks().begin(), parser.blocks().end(),
        [](const game::IniBlock& block) {
            return asciiEqualIgnoreCase(block.type, "AIData");
        });
    if (!hasAIDataBlock) return true;

    ObjectSimulationRules candidate = *this;
    if (!candidate.ai.applyLegacyAIDataOverrides(content, sourceName, error))
        return false;
    *this = std::move(candidate);
    return true;
}

} // namespace engine
