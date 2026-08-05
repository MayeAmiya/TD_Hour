#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "EnergySimulationRules.h"

#include "ContentFloatParsing.h"
#include "core/data/ini/GeneralsIniParser.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

using container::asciiEqualIgnoreCase;

[[nodiscard]] std::optional<EnergySimulationRules::Scalar>
parseFiniteScalar(container::StringView text, container::StringView source,
                  container::StringView field,
                  EnergySimulationRules::Scalar fallback) {
    const auto parsed = game::parseContentFloat(text, {
        .source = source,
        .block = "GameData",
        .module = "EnergySimulationRules",
        .field = field,
        .fallback = fallback.to_float(),
    });
    if (!parsed) return std::nullopt;
    constexpr float maximum =
        static_cast<float>(std::numeric_limits<int32_t>::max());
    constexpr float minimum =
        static_cast<float>(std::numeric_limits<int32_t>::min());
    if (*parsed >= maximum || *parsed <= minimum) {
        game::processContentDiagnostics().warn({
            .source = container::String{source},
            .block = "GameData",
            .module = "EnergySimulationRules",
            .field = container::String{field},
            .rawValue = container::String{text},
            .adoptedValue = std::to_string(fallback.to_float()),
            .reason = "real is not representable in Q32.32; retained the prior/default value",
        });
        return std::nullopt;
    }
    if (*parsed < 0.0f) {
        game::processContentDiagnostics().warn({
            .source = container::String{source},
            .block = "GameData",
            .module = "EnergySimulationRules",
            .field = container::String{field},
            .rawValue = container::String{text},
            .adoptedValue = std::to_string(*parsed),
            .reason = "accepted original negative domain; production consumers derive a safe disabled/fallback rate where required",
        });
    }
    return EnergySimulationRules::Scalar{*parsed};
}

void setError(container::String* error, container::String message) {
    if (error) *error = std::move(message);
}

[[nodiscard]] constexpr EnergySimulationRules::Scalar scalarZero() noexcept {
    return EnergySimulationRules::Scalar{};
}

[[nodiscard]] constexpr EnergySimulationRules::Scalar scalarOne() noexcept {
    return EnergySimulationRules::Scalar{int32_t{1}};
}

[[nodiscard]] bool applyGameDataBlocks(
    const container::Vector<game::IniBlock>& blocks,
    EnergySimulationRules& compiled, bool& modified,
    container::StringView source, container::String* /*error*/) {
    for (const game::IniBlock& block : blocks) {
        if (!asciiEqualIgnoreCase(block.type, "GameData")) continue;
        const auto blockDiagnosticScope =
            game::contentDiagnosticProvenanceScope(block.source);
        for (size_t valueIndex = 0;
             valueIndex < block.values.size(); ++valueIndex) {
            const auto& [key, text] = block.values[valueIndex];
            const auto fieldDiagnosticScope =
                game::contentDiagnosticProvenanceScope(
                    block.valueSource(valueIndex));
            EnergySimulationRules::Scalar* destination = nullptr;
            if (asciiEqualIgnoreCase(key, "MinLowEnergyProductionSpeed")) {
                destination = &compiled.minimumLowEnergyProductionSpeed;
            } else if (asciiEqualIgnoreCase(key, "MaxLowEnergyProductionSpeed")) {
                destination = &compiled.maximumLowEnergyProductionSpeed;
            } else if (asciiEqualIgnoreCase(key, "LowEnergyPenaltyModifier")) {
                destination = &compiled.lowEnergyPenaltyModifier;
            } else if (asciiEqualIgnoreCase(key, "MultipleFactory")) {
                destination = &compiled.multipleFactoryMultiplier;
            }
            if (!destination) continue;

            const std::optional<EnergySimulationRules::Scalar> parsed =
                parseFiniteScalar(text, source, key, *destination);
            if (!parsed) continue;
            *destination = *parsed;
            modified = true;
        }
    }
    return true;
}

} // namespace

void EnergySimulationRules::canonicalize() noexcept {
    // Preserve finite authored values. productionSpeed() and
    // adjustForMultipleFactories() own their explicit safe derived behavior.
}

bool EnergySimulationRules::applyLegacyGameDataOverrides(
    container::StringView content, container::StringView sourceName,
    container::String* error) {
    if (error) error->clear();
    game::GeneralsIniParser parser;
    if (!parser.parse(content, sourceName)) {
        setError(error, "could not parse GameData modifier '" +
                            container::String{sourceName} + "'");
        return false;
    }
    EnergySimulationRules candidate = *this;
    bool modified = false;
    if (!applyGameDataBlocks(parser.blocks(), candidate, modified,
                             sourceName, error))
        return false;
    if (!modified) return true;
    candidate.canonicalize();
    *this = candidate;
    return true;
}

EnergySimulationRules::Scalar EnergySimulationRules::productionSpeed(
    int32_t production, int32_t consumption, bool powerSabotaged) const noexcept {
    production = std::max(production, 0);
    consumption = std::max(consumption, 0);

    Scalar energyPercent{};
    if (!powerSabotaged) {
        if (consumption == 0) {
            // RefCode returns the raw production amount when there is no
            // consumption. The next clamp deliberately maps surplus to 1.
            energyPercent = Scalar{production};
        } else {
            energyPercent = Scalar::from_fraction(production, consumption);
        }
    }
    energyPercent = Scalar::min(energyPercent, scalarOne());

    const Scalar energyShort = scalarOne() - energyPercent;
    Scalar penaltyRate = scalarOne() - energyShort * lowEnergyPenaltyModifier;
    penaltyRate = Scalar::max(penaltyRate, minimumLowEnergyProductionSpeed);
    if (energyPercent < scalarOne()) {
        penaltyRate = Scalar::min(penaltyRate, maximumLowEnergyProductionSpeed);
    }
    if (penaltyRate <= scalarZero()) {
        penaltyRate = Scalar{kFallbackPositiveProductionSpeed};
    }
    return penaltyRate;
}

uint32_t EnergySimulationRules::adjustBuildFrames(
    uint32_t baseFrames, int32_t production, int32_t consumption,
    bool powerSabotaged) const noexcept {
    if (baseFrames == 0) return 1;
    const int64_t speedRaw = productionSpeed(production, consumption, powerSabotaged).raw();
    if (speedRaw <= 0) return std::numeric_limits<uint32_t>::max();

    // `baseFrames` is uint32, therefore shifting it by Q32's fractional
    // width remains within uint64 (the maximum numerator is 2^64 - 2^32).
    // This is a fixed-point positive divide with legacy truncation, not a
    // float/ceil conversion.
    const uint64_t numerator = static_cast<uint64_t>(baseFrames) << 32u;
    const uint64_t quotient = numerator / static_cast<uint64_t>(speedRaw);
    return static_cast<uint32_t>(std::clamp<uint64_t>(
        quotient, 1u, std::numeric_limits<uint32_t>::max()));
}

uint32_t EnergySimulationRules::adjustForMultipleFactories(
    uint32_t baseFrames, uint32_t facilityCount) const noexcept {
    if (baseFrames == 0) return 1;
    if (facilityCount <= 1 || multipleFactoryMultiplier <= scalarZero())
        return baseFrames;
    uint64_t frames = baseFrames;
    const uint64_t multiplierRaw =
        static_cast<uint64_t>(multipleFactoryMultiplier.raw());
    const uint64_t integerMultiplier = multiplierRaw >> 32u;
    const uint64_t fractionalMultiplier =
        multiplierRaw & 0xffffffffull;
    for (uint32_t index = 1; index < facilityCount; ++index) {
        // `frames` is clamped to uint32 after every source-equivalent Int
        // assignment. Split Q32.32 into integer/fractional terms so even a
        // hostile but representable GameData scalar cannot overflow uint64.
        frames = frames * integerMultiplier +
            ((frames * fractionalMultiplier) >> 32u);
        frames = std::clamp<uint64_t>(
            frames, 1u, std::numeric_limits<uint32_t>::max());
    }
    return static_cast<uint32_t>(frames);
}

bool EnergySimulationRules::loadFromLegacyGameData(
    container::StringView path, EnergySimulationRules& rules, container::String* error) {
    if (error) error->clear();

    game::GeneralsIniParser parser;
    if (!parser.parseFile(container::String{path})) {
        setError(error, "could not parse GameData source '" + container::String(path) + "'");
        return false;
    }

    EnergySimulationRules compiled;
    bool modified = false;
    if (!applyGameDataBlocks(parser.blocks(), compiled, modified, path, error))
        return false;

    compiled.canonicalize();
    rules = compiled;
    return true;
}

} // namespace engine
