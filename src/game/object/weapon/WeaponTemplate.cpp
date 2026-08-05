#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/data/base/PhysicsSimulationRules.h"
#include "WeaponTemplate.h"

#include "VFS.h"
#include "debug/debug.h"
#include "core/data/ini/GeneralsIniParser.h"
#include "game/object/contracts/ObjectDeathReaction.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace game {
namespace {

constexpr auto trim = container::trimAsciiView;

using container::asciiEqualIgnoreCase;

[[nodiscard]] container::String optionalContentReference(
    container::StringView value) {
    value = trim(value);
    return value.empty() || asciiEqualIgnoreCase(value, "None")
        ? container::String{}
        : container::String{value};
}

[[nodiscard]] container::Vector<container::StringView> splitTokens(container::StringView value) {
    container::Vector<container::StringView> result;
    while (!value.empty()) {
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                                  value.front() == ',' || value.front() == ':')) {
            value.remove_prefix(1);
        }
        if (value.empty()) break;
        size_t length = 0;
        while (length < value.size() && value[length] != ' ' && value[length] != '\t' &&
               value[length] != ',' && value[length] != ':') {
            ++length;
        }
        result.push_back(value.substr(0, length));
        value.remove_prefix(length);
    }
    return result;
}

[[nodiscard]] std::optional<size_t> parseVeterancyIndex(
    container::StringView value) noexcept {
    if (asciiEqualIgnoreCase(value, "REGULAR")) return 0;
    if (asciiEqualIgnoreCase(value, "VETERAN")) return 1;
    if (asciiEqualIgnoreCase(value, "ELITE")) return 2;
    if (asciiEqualIgnoreCase(value, "HEROIC")) return 3;
    return std::nullopt;
}

[[nodiscard]] float parseFloat(container::StringView value, float fallback = 0.0f) noexcept {
    return game::parseContentFloatOr(value, {
        .source = __FILE__, .block = "Weapon", .field = "Real",
        .fallback = fallback});
}

[[nodiscard]] std::optional<WeaponScatterTarget>
parseScatterTarget(container::StringView value) noexcept {
    const auto parseAxis = [&](char axis) -> std::optional<math::q32_32> {
        for (size_t cursor = 0; cursor < value.size(); ++cursor) {
            if ((value[cursor] != axis && value[cursor] !=
                    static_cast<char>(axis + ('a' - 'A'))) ||
                cursor + 1 >= value.size()) {
                continue;
            }
            size_t colon = cursor + 1;
            while (colon < value.size() &&
                   (value[colon] == ' ' || value[colon] == '\t')) ++colon;
            if (colon >= value.size() || value[colon] != ':') continue;
            size_t begin = colon + 1;
            while (begin < value.size() &&
                   (value[begin] == ' ' || value[begin] == '\t')) ++begin;
            size_t end = begin;
            while (end < value.size() && value[end] != ' ' &&
                   value[end] != '\t' && value[end] != ',') ++end;
            if (begin == end) return std::nullopt;
            const std::optional<float> parsed = parseContentFloat(
                value.substr(begin, end - begin),
                {.source = __FILE__, .block = "Weapon",
                 .field = "ScatterTarget", .fallback = 0.0f});
            return parsed ? std::optional<math::q32_32>{math::q32_32{*parsed}}
                          : std::nullopt;
        }
        return std::nullopt;
    };
    const std::optional<math::q32_32> x = parseAxis('X');
    const std::optional<math::q32_32> y = parseAxis('Y');
    if (!x || !y) return std::nullopt;
    return WeaponScatterTarget{.x = *x, .y = *y};
}

[[nodiscard]] std::optional<WeaponBonus::Scalar>
parsePercentMultiplier(container::StringView value,
                       WeaponBonus::Scalar fallback) noexcept {
    value = trim(value);
    if (!value.empty() && value.back() == '%') {
        value.remove_suffix(1);
        value = trim(value);
    }
    const game::ContentFloatContext context{
        .source = __FILE__, .block = "GameData",
        .module = "WeaponBonus", .field = "PercentMultiplier",
        .fallback = fallback.to_float() * 100.0f};
    const std::optional<float> parsed =
        game::parseContentFloat(value, context);
    if (!parsed) return fallback;

    constexpr long double kFixedScale = 4294967296.0L;
    const long double multiplier = static_cast<long double>(*parsed) / 100.0L;
    const long double raw = multiplier * kFixedScale;
    if (raw < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
        raw > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
        game::warnContentFloatFallback(
            value, context,
            "finite percent prefix is outside the Q32.32 field range; retained the prior/default value");
        return fallback;
    }
    return WeaponBonus::Scalar::from_raw(static_cast<int64_t>(raw));
}

[[nodiscard]] bool applyWeaponBonusAssignment(container::StringView value,
                                               WeaponBonusSet& target) noexcept {
    const container::Vector<container::StringView> tokens = splitTokens(value);
    if (tokens.size() != 3) return false;
    const std::optional<WeaponBonusCondition> condition =
        parseWeaponBonusCondition(tokens[0]);
    const std::optional<WeaponBonusField> field = parseWeaponBonusField(tokens[1]);
    if (!condition || !field) return false;
    const WeaponBonus::Scalar fallback =
        target.conditions[static_cast<size_t>(*condition)].multiplier(*field);
    const std::optional<WeaponBonus::Scalar> multiplier =
        parsePercentMultiplier(tokens[2], fallback);
    if (!multiplier) return false;
    target.set(*condition, *field, *multiplier);
    return true;
}

[[nodiscard]] bool applyWeaponBonusBlocks(
    const container::Vector<IniBlock>& blocks, WeaponBonusSet& target,
    bool& modified, container::String* error) {
    for (const IniBlock& block : blocks) {
        if (!asciiEqualIgnoreCase(block.type, "GameData")) continue;
        for (size_t valueIndex = 0;
             valueIndex < block.values.size(); ++valueIndex) {
            const auto& [key, value] = block.values[valueIndex];
            const auto fieldDiagnosticScope =
                contentDiagnosticProvenanceScope(
                    block.valueSource(valueIndex));
            if (!asciiEqualIgnoreCase(key, "WeaponBonus")) continue;
            if (!applyWeaponBonusAssignment(value, target)) {
                if (error) {
                    *error = "invalid GameData.WeaponBonus value '" + value + "'";
                }
                return false;
            }
            modified = true;
        }
    }
    return true;
}

[[nodiscard]] WeaponBonus::Scalar saturatingMultiplyFixed(
    WeaponBonus::Scalar left, WeaponBonus::Scalar right) noexcept {
    const int64_t leftRaw = left.raw();
    const int64_t rightRaw = right.raw();
    if (leftRaw == 0 || rightRaw == 0) return {};

#if defined(_MSC_VER) && defined(_M_X64)
    int64_t high = 0;
    const uint64_t low = static_cast<uint64_t>(_mul128(leftRaw, rightRaw, &high));
    const uint64_t shiftedBits =
        (static_cast<uint64_t>(high) << 32u) | (low >> 32u);
    const int64_t shifted = static_cast<int64_t>(shiftedBits);
    const int64_t upper = high >> 32u;
    if ((upper == 0 && shifted >= 0) || (upper == -1 && shifted < 0)) {
        return WeaponBonus::Scalar::from_raw(shifted);
    }
#else
    const __int128 product =
        static_cast<__int128>(leftRaw) * static_cast<__int128>(rightRaw);
    const __int128 shifted = product >> 32u;
    if (shifted >= static_cast<__int128>(std::numeric_limits<int64_t>::min()) &&
        shifted <= static_cast<__int128>(std::numeric_limits<int64_t>::max())) {
        return WeaponBonus::Scalar::from_raw(static_cast<int64_t>(shifted));
    }
#endif

    const bool negative = (leftRaw < 0) != (rightRaw < 0);
    return WeaponBonus::Scalar::from_raw(
        negative ? std::numeric_limits<int64_t>::min()
                 : std::numeric_limits<int64_t>::max());
}

[[nodiscard]] int64_t saturatingAddRaw(int64_t left, int64_t right) noexcept {
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right) {
        return std::numeric_limits<int64_t>::max();
    }
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right) {
        return std::numeric_limits<int64_t>::min();
    }
    return left + right;
}

[[nodiscard]] int32_t parseInt(container::StringView value, int32_t fallback = 0) noexcept {
    value = trim(value);
    int32_t parsed = fallback;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size() ? parsed : fallback;
}

[[nodiscard]] uint32_t parseMilliseconds(container::StringView value) noexcept {
    const float parsed = parseFloat(value);
    if (!std::isfinite(parsed) || parsed <= 0.0f) return 0;
    constexpr float maximum = static_cast<float>(std::numeric_limits<uint32_t>::max());
    return static_cast<uint32_t>(std::min(std::ceil(parsed), maximum));
}

[[nodiscard]] float parseAngleRadians(container::StringView value) noexcept {
    value = trim(value);
    bool radians = false;
    if (value.size() >= 3 && asciiEqualIgnoreCase(value.substr(value.size() - 3), "rad")) {
        radians = true;
        value.remove_suffix(3);
    } else if (value.size() >= 3 && asciiEqualIgnoreCase(value.substr(value.size() - 3), "deg")) {
        value.remove_suffix(3);
    }
    const float parsed = parseFloat(value);
    return radians ? parsed : parsed * (std::numbers::pi_v<float> / 180.0f);
}

[[nodiscard]] bool parseBoolean(container::StringView value, bool fallback = false) noexcept {
    value = trim(value);
    return asciiEqualIgnoreCase(value, "YES") || asciiEqualIgnoreCase(value, "TRUE") || value == "1"
        ? true
        : asciiEqualIgnoreCase(value, "NO") || asciiEqualIgnoreCase(value, "FALSE") || value == "0"
            ? false
            : fallback;
}

template <typename Bit>
[[nodiscard]] bool applyMask(container::StringView value, uint32_t& target,
                             const container::Vector<std::pair<container::StringView, Bit>>& names) {
    const container::Vector<container::StringView> tokens = splitTokens(value);
    bool sawEdit = false;
    bool sawPlain = false;
    uint32_t result = target;
    for (container::StringView token : tokens) {
        bool add = true;
        bool edit = false;
        if (!token.empty() && (token.front() == '+' || token.front() == '-')) {
            add = token.front() == '+';
            edit = true;
            token.remove_prefix(1);
        }
        if (token.empty()) return false;
        if (asciiEqualIgnoreCase(token, "NONE")) {
            if (edit && add) return false;
            if (!sawPlain && !sawEdit) result = 0;
            else if (!add) result = 0;
            sawPlain = true;
            continue;
        }
        const auto found = std::find_if(names.begin(), names.end(), [token](const auto& candidate) {
            return asciiEqualIgnoreCase(token, candidate.first);
        });
        if (found == names.end()) return false;
        if (edit) {
            sawEdit = true;
        } else if (!sawPlain && !sawEdit) {
            result = 0;
            sawPlain = true;
        }
        const uint32_t bit = uint32_t{1} << static_cast<uint8_t>(found->second);
        if (add) result |= bit;
        else result &= ~bit;
    }
    target = result;
    return true;
}

void parseDelayRange(container::StringView value, WeaponAuthoringTemplate& target) noexcept {
    const container::Vector<container::StringView> tokens = splitTokens(value);
    if (tokens.empty()) {
        target.minimumDelayBetweenShotsMilliseconds = 0;
        target.maximumDelayBetweenShotsMilliseconds = 0;
        return;
    }
    uint32_t minimum = 0;
    uint32_t maximum = 0;
    if (tokens.size() >= 4 && asciiEqualIgnoreCase(tokens[0], "MIN") &&
        asciiEqualIgnoreCase(tokens[2], "MAX")) {
        minimum = parseMilliseconds(tokens[1]);
        maximum = parseMilliseconds(tokens[3]);
    } else {
        minimum = parseMilliseconds(tokens.front());
        maximum = minimum;
    }
    if (maximum < minimum) maximum = minimum;
    target.minimumDelayBetweenShotsMilliseconds = minimum;
    target.maximumDelayBetweenShotsMilliseconds = maximum;
}

[[nodiscard]] WeaponReloadType parseReloadType(container::StringView value) noexcept {
    value = trim(value);
    if (asciiEqualIgnoreCase(value, "NO")) return WeaponReloadType::None;
    if (asciiEqualIgnoreCase(value, "RETURN_TO_BASE")) return WeaponReloadType::ReturnToBase;
    return WeaponReloadType::Auto;
}

[[nodiscard]] WeaponPreAttackType parsePreAttackType(container::StringView value) noexcept {
    value = trim(value);
    if (asciiEqualIgnoreCase(value, "PER_ATTACK")) return WeaponPreAttackType::PerAttack;
    if (asciiEqualIgnoreCase(value, "PER_CLIP")) return WeaponPreAttackType::PerClip;
    return WeaponPreAttackType::PerShot;
}

void warnUnknownMask(container::StringView field, container::StringView weapon, container::StringView value) {
    TD_LOG_WARN("[WeaponStore] Weapon '{}' has invalid {} mask '{}'", weapon, field, value);
}

} // namespace

std::optional<WeaponBonusCondition>
parseWeaponBonusCondition(container::StringView value) noexcept {
    value = trim(value);
    static constexpr std::pair<container::StringView, WeaponBonusCondition> names[] = {
        {"GARRISONED", WeaponBonusCondition::Garrisoned},
        {"HORDE", WeaponBonusCondition::Horde},
        {"CONTINUOUS_FIRE_MEAN", WeaponBonusCondition::ContinuousFireMean},
        {"CONTINUOUS_FIRE_FAST", WeaponBonusCondition::ContinuousFireFast},
        {"NATIONALISM", WeaponBonusCondition::Nationalism},
        {"PLAYER_UPGRADE", WeaponBonusCondition::PlayerUpgrade},
        {"DRONE_SPOTTING", WeaponBonusCondition::DroneSpotting},
        {"DEMORALIZED", WeaponBonusCondition::Demoralized},
        {"DEMORALIZED_OBSOLETE", WeaponBonusCondition::Demoralized},
        {"ENTHUSIASTIC", WeaponBonusCondition::Enthusiastic},
        {"VETERAN", WeaponBonusCondition::Veteran},
        {"ELITE", WeaponBonusCondition::Elite},
        {"HERO", WeaponBonusCondition::Hero},
        {"BATTLEPLAN_BOMBARDMENT", WeaponBonusCondition::BattleplanBombardment},
        {"BATTLEPLAN_HOLDTHELINE", WeaponBonusCondition::BattleplanHoldTheLine},
        {"BATTLEPLAN_HOLD_THE_LINE", WeaponBonusCondition::BattleplanHoldTheLine},
        {"BATTLEPLAN_SEARCHANDDESTROY", WeaponBonusCondition::BattleplanSearchAndDestroy},
        {"BATTLEPLAN_SEARCH_AND_DESTROY", WeaponBonusCondition::BattleplanSearchAndDestroy},
        {"SUBLIMINAL", WeaponBonusCondition::Subliminal},
        {"SOLO_HUMAN_EASY", WeaponBonusCondition::SoloHumanEasy},
        {"SOLO_HUMAN_NORMAL", WeaponBonusCondition::SoloHumanNormal},
        {"SOLO_HUMAN_HARD", WeaponBonusCondition::SoloHumanHard},
        {"SOLO_AI_EASY", WeaponBonusCondition::SoloAiEasy},
        {"SOLO_AI_NORMAL", WeaponBonusCondition::SoloAiNormal},
        {"SOLO_AI_HARD", WeaponBonusCondition::SoloAiHard},
        {"TARGET_FAERIE_FIRE", WeaponBonusCondition::TargetFaerieFire},
        {"FANATICISM", WeaponBonusCondition::Fanaticism},
        {"FRENZY_ONE", WeaponBonusCondition::FrenzyOne},
        {"FRENZY_TWO", WeaponBonusCondition::FrenzyTwo},
        {"FRENZY_THREE", WeaponBonusCondition::FrenzyThree},
    };
    const auto found = std::find_if(std::begin(names), std::end(names),
        [value](const auto& entry) { return asciiEqualIgnoreCase(value, entry.first); });
    return found == std::end(names)
        ? std::optional<WeaponBonusCondition>{}
        : std::optional<WeaponBonusCondition>{found->second};
}

std::optional<WeaponBonusField>
parseWeaponBonusField(container::StringView value) noexcept {
    value = trim(value);
    static constexpr std::pair<container::StringView, WeaponBonusField> names[] = {
        {"DAMAGE", WeaponBonusField::Damage},
        {"RADIUS", WeaponBonusField::Radius},
        {"RANGE", WeaponBonusField::Range},
        {"RATE_OF_FIRE", WeaponBonusField::RateOfFire},
        {"PRE_ATTACK", WeaponBonusField::PreAttack},
    };
    const auto found = std::find_if(std::begin(names), std::end(names),
        [value](const auto& entry) { return asciiEqualIgnoreCase(value, entry.first); });
    return found == std::end(names)
        ? std::optional<WeaponBonusField>{}
        : std::optional<WeaponBonusField>{found->second};
}

WeaponBonus::Scalar WeaponBonus::multiplier(WeaponBonusField field) const noexcept {
    const size_t index = static_cast<size_t>(field);
    return index < multipliers.size() ? multipliers[index] : Scalar{int32_t{1}};
}

WeaponBonus::Scalar WeaponBonus::scale(Scalar value, WeaponBonusField field) const noexcept {
    return saturatingMultiplyFixed(value, multiplier(field));
}

void WeaponBonus::set(WeaponBonusField field, Scalar value) noexcept {
    const size_t index = static_cast<size_t>(field);
    if (index < multipliers.size()) multipliers[index] = value;
}

void WeaponBonus::append(const WeaponBonus& other) noexcept {
    const int64_t identity = Scalar{int32_t{1}}.raw();
    for (size_t index = 0; index < multipliers.size(); ++index) {
        const int64_t otherRaw = other.multipliers[index].raw();
        const int64_t current = multipliers[index].raw();
        int64_t combined = 0;
        if (otherRaw >= 0) {
            // `otherRaw - identity` is always representable in this branch.
            combined = saturatingAddRaw(current, otherRaw - identity);
        } else if (current < std::numeric_limits<int64_t>::min() + identity) {
            // Both remaining terms are negative, so the exact affine sum is
            // already below the representable Q32.32 range.
            combined = std::numeric_limits<int64_t>::min();
        } else {
            // Reorder the affine sum so neither intermediate underflows:
            // current + other - identity == (current - identity) + other.
            combined = saturatingAddRaw(current - identity, otherRaw);
        }
        multipliers[index] = Scalar::from_raw(combined);
    }
}

void WeaponBonusSet::set(WeaponBonusCondition condition, WeaponBonusField field,
                         WeaponBonus::Scalar value) noexcept {
    const size_t index = static_cast<size_t>(condition);
    if (index < conditions.size()) conditions[index].set(field, value);
}

void WeaponBonusSet::append(WeaponBonusConditionMask activeConditions,
                            WeaponBonus& destination) const noexcept {
    if (activeConditions == 0) return;
    for (size_t index = 0; index < conditions.size(); ++index) {
        if ((activeConditions & (WeaponBonusConditionMask{1} << index)) != 0) {
            destination.append(conditions[index]);
        }
    }
}

bool WeaponBonusSet::applyLegacyGameDataOverrides(
    container::StringView content, container::StringView sourceName,
    container::String* error) {
    if (error) error->clear();
    GeneralsIniParser parser;
    if (!parser.parse(content, sourceName)) {
        if (error) {
            *error = "could not parse GameData modifier '" +
                container::String{sourceName} + "'";
        }
        return false;
    }
    WeaponBonusSet candidate = *this;
    bool modified = false;
    if (!applyWeaponBonusBlocks(parser.blocks(), candidate, modified, error))
        return false;
    if (!modified) return true;
    *this = std::move(candidate);
    return true;
}

bool WeaponBonusSet::loadFromLegacyGameData(container::StringView path,
                                             WeaponBonusSet& output,
                                             container::String* error) {
    if (error) error->clear();
    WeaponBonusSet compiled;
    bool modified = false;

    auto& vfs = io::VFS::instance();
    if (vfs.exists(path)) {
        const container::String winner = vfs.readAll(path);
        GeneralsIniParser parser;
        if (!parser.parse(winner, path) ||
            !applyWeaponBonusBlocks(parser.blocks(), compiled, modified, error)) {
            if (error && error->empty()) {
                *error = "could not parse GameData source '" + container::String{path} + "'";
            }
            return false;
        }
    } else {
        GeneralsIniParser parser;
        if (!parser.parseFile(container::String{path}) ||
            !applyWeaponBonusBlocks(parser.blocks(), compiled, modified, error)) {
            if (error && error->empty()) {
                *error = "could not parse GameData source '" + container::String{path} + "'";
            }
            return false;
        }
    }

    output = std::move(compiled);
    return true;
}

WeaponStore& WeaponStore::instance() {
    static WeaponStore instance;
    return instance;
}

void WeaponStore::clear() {
    m_weapons.clear();
}

void WeaponAuthoringTemplate::synchronizeAuthoritativeScalars() {
    const auto quantize = [this](float value,
                                 container::StringView field,
                                 math::q32_32 previous) {
        const math::q32_32 result{value};
        constexpr float kMinimum = -2147483648.0f;
        constexpr float kMaximumExclusive = 2147483648.0f;
        const bool finite = std::isfinite(value);
        if ((!finite || value < kMinimum || value >= kMaximumExclusive) &&
            (!finite || previous != result)) {
            warnContentFloatFallback(
                std::to_string(value),
                {.source = __FILE__, .block = "Weapon",
                 .definition = name, .field = field,
                 .fallback = result.to_float()},
                finite
                    ? "finite value is outside Q32.32; accepted with saturation at the authoritative ingress"
                    : "non-finite compatibility value was replaced at the authoritative ingress");
        }
        return result;
    };

    fixed.primaryDamage = quantize(
        primaryDamage, "PrimaryDamage", fixed.primaryDamage);
    fixed.primaryDamageRadius = quantize(
        primaryDamageRadius, "PrimaryDamageRadius",
        fixed.primaryDamageRadius);
    fixed.secondaryDamage = quantize(
        secondaryDamage, "SecondaryDamage", fixed.secondaryDamage);
    fixed.secondaryDamageRadius = quantize(
        secondaryDamageRadius, "SecondaryDamageRadius",
        fixed.secondaryDamageRadius);
    fixed.attackRange = quantize(
        attackRange, "AttackRange", fixed.attackRange);
    fixed.minimumAttackRange = quantize(
        minimumAttackRange, "MinimumAttackRange", fixed.minimumAttackRange);
    fixed.requestAssistRange = quantize(
        requestAssistRange, "RequestAssistRange", fixed.requestAssistRange);
    fixed.acceptableAimDeltaRadians = quantize(
        acceptableAimDeltaRadians, "AcceptableAimDelta",
        fixed.acceptableAimDeltaRadians);
    fixed.minTargetPitchRadians = quantize(
        minTargetPitchRadians, "MinTargetPitch",
        fixed.minTargetPitchRadians);
    fixed.maxTargetPitchRadians = quantize(
        maxTargetPitchRadians, "MaxTargetPitch",
        fixed.maxTargetPitchRadians);
    fixed.radiusDamageAngleRadians = quantize(
        radiusDamageAngleRadians, "RadiusDamageAngle",
        fixed.radiusDamageAngleRadians);
    fixed.scatterRadius = quantize(
        scatterRadius, "ScatterRadius", fixed.scatterRadius);
    fixed.scatterTargetScalar = quantize(
        scatterTargetScalar, "ScatterTargetScalar",
        fixed.scatterTargetScalar);
    fixed.scatterRadiusVsInfantry = quantize(
        scatterRadiusVsInfantry, "ScatterRadiusVsInfantry",
        fixed.scatterRadiusVsInfantry);
    fixed.continueAttackRange = quantize(
        continueAttackRange, "ContinueAttackRange",
        fixed.continueAttackRange);
    fixed.weaponSpeed = quantize(
        weaponSpeed, "WeaponSpeed", fixed.weaponSpeed);
    fixed.minimumWeaponSpeed = quantize(
        minimumWeaponSpeed, "MinWeaponSpeed", fixed.minimumWeaponSpeed);
    fixed.weaponRecoilRadians = quantize(
        weaponRecoilRadians, "WeaponRecoil", fixed.weaponRecoilRadians);
    fixed.stunDuration = quantize(
        stunDuration, "StunDuration", fixed.stunDuration);
    // WeaponTemplate::ShockWaveAmount is another direct producer for legacy
    // PhysicsBehavior::applyForce(). Preserve its authored per-frame force
    // semantics while storing the authoritative value in the modern
    // mass*world-units/second^2 domain used by ObjectPhysicsComponent.
    constexpr float legacyForceScale =
        engine::PhysicsSimulationRules::kLegacyPerFrameSquaredToPerSecondSquared;
    fixed.shockWaveAmount = quantize(
        shockWaveAmount * legacyForceScale,
        "ShockWaveAmount", fixed.shockWaveAmount);
    fixed.shockWaveRadius = quantize(
        shockWaveRadius, "ShockWaveRadius", fixed.shockWaveRadius);
    fixed.shockWaveTaperOff = quantize(
        shockWaveTaperOff, "ShockWaveTaperOff", fixed.shockWaveTaperOff);
    fixed.historicBonusRadius = quantize(
        historicBonusRadius, "HistoricBonusRadius",
        fixed.historicBonusRadius);
}

bool WeaponStore::loadFromIni(
    const container::String& filePath, ini::LegacyIniLoadType loadType) {
    const container::Vector<std::pair<container::StringView, WeaponAntiTarget>> antiNames = {
        {"AIRBORNE_VEHICLE", WeaponAntiTarget::AirborneVehicle},
        {"GROUND", WeaponAntiTarget::Ground},
        {"PROJECTILE", WeaponAntiTarget::Projectile},
        {"SMALL_MISSILE", WeaponAntiTarget::SmallMissile},
        {"MINE", WeaponAntiTarget::Mine},
        {"AIRBORNE_INFANTRY", WeaponAntiTarget::AirborneInfantry},
        {"BALLISTIC_MISSILE", WeaponAntiTarget::BallisticMissile},
        {"PARACHUTE", WeaponAntiTarget::Parachute},
    };
    const container::Vector<std::pair<container::StringView, WeaponAffectsTarget>> affectsNames = {
        {"SELF", WeaponAffectsTarget::Self},
        {"ALLIES", WeaponAffectsTarget::Allies},
        {"ENEMIES", WeaponAffectsTarget::Enemies},
        {"NEUTRALS", WeaponAffectsTarget::Neutrals},
        {"SUICIDE", WeaponAffectsTarget::KillsSelf},
        {"NOT_SIMILAR", WeaponAffectsTarget::NotSimilar},
        {"NOT_AIRBORNE", WeaponAffectsTarget::NotAirborne},
    };
    const container::Vector<std::pair<container::StringView, WeaponCollideTarget>> collideNames = {
        {"ALLIES", WeaponCollideTarget::Allies},
        {"ENEMIES", WeaponCollideTarget::Enemies},
        {"STRUCTURES", WeaponCollideTarget::Structures},
        {"SHRUBBERY", WeaponCollideTarget::Shrubbery},
        {"PROJECTILES", WeaponCollideTarget::Projectiles},
        {"WALLS", WeaponCollideTarget::Walls},
        {"SMALL_MISSILES", WeaponCollideTarget::SmallMissiles},
        {"BALLISTIC_MISSILES", WeaponCollideTarget::BallisticMissiles},
        {"CONTROLLED_STRUCTURES", WeaponCollideTarget::ControlledStructures},
    };

    const auto applyBlocks = [&](const container::Vector<IniBlock>& blocks) {
    for (const IniBlock& block : blocks) {
        if (!asciiEqualIgnoreCase(block.type, "Weapon")) continue;
        const auto diagnosticScope =
            contentDiagnosticProvenanceScope(block.source);

        if (block.name.empty()) continue;

        const auto prior = m_weapons.find(block.name);
        if (prior != m_weapons.end()) {
            if (!ini::createsOverrides(loadType)) {
                // WeaponStore rejects an ordinary duplicate and leaves the
                // first effective definition intact. It only copies and
                // patches an existing weapon for CreateOverrides sources.
                TD_LOG_WARN(
                    "[WeaponStore] Ignored duplicate Weapon '{}' without CreateOverrides",
                    block.name);
                continue;
            }
        }

        WeaponAuthoringTemplate target = prior != m_weapons.end()
            ? prior->second : WeaponAuthoringTemplate{};
        target.name = block.name;
        for (size_t valueIndex = 0;
             valueIndex < block.values.size(); ++valueIndex) {
            const auto& [key, value] = block.values[valueIndex];
            const auto fieldDiagnosticScope =
                contentDiagnosticProvenanceScope(
                    block.valueSource(valueIndex));
            if (asciiEqualIgnoreCase(key, "PrimaryDamage")) target.primaryDamage = parseFloat(value);
            else if (asciiEqualIgnoreCase(key, "PrimaryDamageRadius")) target.primaryDamageRadius = parseFloat(value);
            else if (asciiEqualIgnoreCase(key, "SecondaryDamage")) target.secondaryDamage = parseFloat(value);
            else if (asciiEqualIgnoreCase(key, "SecondaryDamageRadius")) target.secondaryDamageRadius = parseFloat(value);
            else if (asciiEqualIgnoreCase(key, "AttackRange")) target.attackRange = std::max(0.0f, parseFloat(value));
            else if (asciiEqualIgnoreCase(key, "MinimumAttackRange")) target.minimumAttackRange = std::max(0.0f, parseFloat(value));
            else if (asciiEqualIgnoreCase(key, "RequestAssistRange")) target.requestAssistRange = std::max(0.0f, parseFloat(value));
            else if (asciiEqualIgnoreCase(key, "AcceptableAimDelta")) target.acceptableAimDeltaRadians = parseAngleRadians(value);
            else if (asciiEqualIgnoreCase(key, "MinTargetPitch")) target.minTargetPitchRadians = parseAngleRadians(value);
            else if (asciiEqualIgnoreCase(key, "MaxTargetPitch")) target.maxTargetPitchRadians = parseAngleRadians(value);
            else if (asciiEqualIgnoreCase(key, "RadiusDamageAngle")) target.radiusDamageAngleRadians = parseAngleRadians(value);
            else if (asciiEqualIgnoreCase(key, "ScatterRadius")) target.scatterRadius = std::max(0.0f, parseFloat(value));
            else if (asciiEqualIgnoreCase(key, "ScatterTargetScalar")) target.scatterTargetScalar = std::max(0.0f, parseFloat(value));
            else if (asciiEqualIgnoreCase(key, "ScatterRadiusVsInfantry")) target.scatterRadiusVsInfantry = std::max(0.0f, parseFloat(value));
            else if (asciiEqualIgnoreCase(key, "ContinueAttackRange")) target.continueAttackRange = std::max(0.0f, parseFloat(value));
            else if (asciiEqualIgnoreCase(key, "DamageType")) target.damageType = parseDamageType(value);
            else if (asciiEqualIgnoreCase(key, "DamageStatusType")) {
                const ObjectStatusMaskParseResult parsed =
                    parseObjectStatusMask(value);
                target.damageStatusMask = parsed.resolved &&
                        std::popcount(parsed.mask) == 1
                    ? parsed.mask : 0;
            }
            else if (asciiEqualIgnoreCase(key, "DeathType")) target.deathType = parseDeathType(value);
            else if (asciiEqualIgnoreCase(key, "WeaponSpeed")) target.weaponSpeed = std::max(0.0f, parseFloat(value, target.weaponSpeed));
            else if (asciiEqualIgnoreCase(key, "MinWeaponSpeed")) target.minimumWeaponSpeed = std::max(0.0f, parseFloat(value, target.minimumWeaponSpeed));
            else if (asciiEqualIgnoreCase(key, "ScaleWeaponSpeed")) target.scaleWeaponSpeed = parseBoolean(value);
            else if (asciiEqualIgnoreCase(key, "WeaponRecoil")) target.weaponRecoilRadians = parseAngleRadians(value);
            else if (asciiEqualIgnoreCase(key, "ShotsPerBarrel")) target.shotsPerBarrel = static_cast<uint32_t>(std::max(1, parseInt(value)));
            else if (asciiEqualIgnoreCase(key, "ClipSize")) target.clipSize = std::max(0, parseInt(value));
            else if (asciiEqualIgnoreCase(key, "ShowsAmmoPips")) target.showsAmmoPips = parseBoolean(value);
            else if (asciiEqualIgnoreCase(key, "ClipReloadTime")) target.clipReloadTimeMilliseconds = parseMilliseconds(value);
            else if (asciiEqualIgnoreCase(key, "DelayBetweenShots")) parseDelayRange(value, target);
            else if (asciiEqualIgnoreCase(key, "ContinuousFireOne")) target.continuousFireOneShotsNeeded = static_cast<uint32_t>(std::max(0, parseInt(value)));
            else if (asciiEqualIgnoreCase(key, "ContinuousFireTwo")) target.continuousFireTwoShotsNeeded = static_cast<uint32_t>(std::max(0, parseInt(value)));
            else if (asciiEqualIgnoreCase(key, "ContinuousFireCoast")) target.continuousFireCoastMilliseconds = parseMilliseconds(value);
            else if (asciiEqualIgnoreCase(key, "AutoReloadWhenIdle")) target.autoReloadWhenIdleMilliseconds = parseMilliseconds(value);
            else if (asciiEqualIgnoreCase(key, "AutoReloadsClip")) target.reloadType = parseReloadType(value);
            else if (asciiEqualIgnoreCase(key, "PreAttackDelay")) target.preAttackDelayMilliseconds = parseMilliseconds(value);
            else if (asciiEqualIgnoreCase(key, "PreAttackType")) target.preAttackType = parsePreAttackType(value);
            else if (asciiEqualIgnoreCase(key, "HistoricBonusTime")) target.historicBonusTimeMilliseconds = parseMilliseconds(value);
            else if (asciiEqualIgnoreCase(key, "HistoricBonusRadius")) target.historicBonusRadius = std::max(0.0f, parseFloat(value));
            else if (asciiEqualIgnoreCase(key, "HistoricBonusCount")) target.historicBonusCount = static_cast<uint32_t>(std::max(0, parseInt(value)));
            else if (asciiEqualIgnoreCase(key, "HistoricBonusWeapon")) target.historicBonusWeaponName = value;
            else if (asciiEqualIgnoreCase(key, "LeechRangeWeapon")) target.leechRangeWeapon = parseBoolean(value);
            else if (asciiEqualIgnoreCase(key, "ScatterTarget")) {
                if (const std::optional<WeaponScatterTarget> parsed =
                        parseScatterTarget(value)) {
                    target.scatterTargets.push_back(*parsed);
                } else {
                    processContentDiagnostics().warn({
                        .block = "Weapon", .definition = target.name,
                        .field = key, .rawValue = value,
                        .adoptedValue = "entry ignored",
                        .reason = "ScatterTarget requires X:<finite> Y:<finite>",
                    });
                }
            }
            else if (asciiEqualIgnoreCase(key, "ProjectileObject")) {
                // In stock INI, NONE is the explicit sentinel for an
                // instantaneous/non-projectile weapon. Keep that distinction
                // at the authored-data boundary so combat never submits an
                // object spawn transaction for a template literally named
                // "NONE".
                target.projectileObject = asciiEqualIgnoreCase(value, "NONE")
                    ? container::String{}
                    : value;
            }
            else if (asciiEqualIgnoreCase(key, "LaserName")) target.laserName = value;
            else if (asciiEqualIgnoreCase(key, "LaserBoneName")) target.laserBoneName = value;
            else if (asciiEqualIgnoreCase(key, "MissileCallsOnDie")) target.missileCallsOnDie = parseBoolean(value);
            else if (asciiEqualIgnoreCase(key, "CapableOfFollowingWaypoints")) target.capableOfFollowingWaypoints = parseBoolean(value);
            else if (asciiEqualIgnoreCase(key, "ProjectileExhaust")) target.projectileExhausts.fill(value);
            else if (asciiEqualIgnoreCase(key, "VeterancyProjectileExhaust")) {
                const container::Vector<container::StringView> tokens = splitTokens(value);
                if (tokens.size() >= 2) {
                    if (const std::optional<size_t> level = parseVeterancyIndex(tokens[0])) {
                        target.projectileExhausts[*level] = container::String{tokens[1]};
                    }
                }
            }
            else if (asciiEqualIgnoreCase(key, "ProjectileStreamName")) target.projectileStreamName = value;
            else if (asciiEqualIgnoreCase(key, "FireSound")) target.fireSound = value;
            else if (asciiEqualIgnoreCase(key, "FireSoundLoopTime")) target.fireSoundLoopTimeMilliseconds = parseMilliseconds(value);
            else if (asciiEqualIgnoreCase(key, "PlayFXWhenStealthed")) target.playFxWhenStealthed = parseBoolean(value);
            else if (asciiEqualIgnoreCase(key, "SuspendFXDelay")) target.suspendFxDelayMilliseconds = parseMilliseconds(value);
            else if (asciiEqualIgnoreCase(key, "FireFX")) {
                target.fireFXs.fill(optionalContentReference(value));
            } else if (asciiEqualIgnoreCase(key, "VeterancyFireFX")) {
                const container::Vector<container::StringView> tokens =
                    splitTokens(value);
                if (tokens.size() >= 2) {
                    if (const std::optional<size_t> level =
                            parseVeterancyIndex(tokens[0])) {
                        target.fireFXs[*level] =
                            optionalContentReference(tokens[1]);
                    }
                }
            }
            else if (asciiEqualIgnoreCase(key, "FireOCL")) {
                target.fireOcl = optionalContentReference(value);
                target.fireOcls.fill(target.fireOcl);
            } else if (asciiEqualIgnoreCase(key, "VeterancyFireOCL")) {
                const container::Vector<container::StringView> tokens = splitTokens(value);
                if (tokens.size() >= 2) {
                    if (const std::optional<size_t> level =
                            parseVeterancyIndex(tokens[0])) {
                        target.fireOcls[*level] =
                            optionalContentReference(tokens[1]);
                    }
                }
            } else if (asciiEqualIgnoreCase(key, "ProjectileDetonationOCL")) {
                target.projectileDetonationOcls.fill(
                    optionalContentReference(value));
            } else if (asciiEqualIgnoreCase(
                           key, "VeterancyProjectileDetonationOCL")) {
                const container::Vector<container::StringView> tokens = splitTokens(value);
                if (tokens.size() >= 2) {
                    if (const std::optional<size_t> level =
                            parseVeterancyIndex(tokens[0])) {
                        target.projectileDetonationOcls[*level] =
                            optionalContentReference(tokens[1]);
                    }
                }
            }
            else if (asciiEqualIgnoreCase(key, "ProjectileDetonationFX")) {
                target.projectileDetonationFXs.fill(
                    optionalContentReference(value));
            } else if (asciiEqualIgnoreCase(
                           key, "VeterancyProjectileDetonationFX")) {
                const container::Vector<container::StringView> tokens =
                    splitTokens(value);
                if (tokens.size() >= 2) {
                    if (const std::optional<size_t> level =
                            parseVeterancyIndex(tokens[0])) {
                        target.projectileDetonationFXs[*level] =
                            optionalContentReference(tokens[1]);
                    }
                }
            }
            else if (asciiEqualIgnoreCase(key, "RadiusDamageAffects")) {
                if (!applyMask(value, target.radiusDamageAffects, affectsNames)) {
                    warnUnknownMask("RadiusDamageAffects", target.name, value);
                }
            } else if (asciiEqualIgnoreCase(key, "ProjectileCollidesWith")) {
                if (!applyMask(value, target.projectileCollidesWith, collideNames)) {
                    warnUnknownMask("ProjectileCollidesWith", target.name, value);
                }
            } else if (asciiEqualIgnoreCase(key, "AntiMask")) {
                if (!applyMask(value, target.antiMask, antiNames)) {
                    warnUnknownMask("AntiMask", target.name, value);
                }
            } else if (asciiEqualIgnoreCase(key, "AntiAirborneVehicle")) {
                const WeaponAntiMask bit = weaponAntiBit(WeaponAntiTarget::AirborneVehicle);
                target.antiMask = parseBoolean(value) ? target.antiMask | bit : target.antiMask & ~bit;
            } else if (asciiEqualIgnoreCase(key, "AntiGround")) {
                const WeaponAntiMask bit = weaponAntiBit(WeaponAntiTarget::Ground);
                target.antiMask = parseBoolean(value) ? target.antiMask | bit : target.antiMask & ~bit;
            } else if (asciiEqualIgnoreCase(key, "AntiProjectile")) {
                const WeaponAntiMask bit = weaponAntiBit(WeaponAntiTarget::Projectile);
                target.antiMask = parseBoolean(value) ? target.antiMask | bit : target.antiMask & ~bit;
            } else if (asciiEqualIgnoreCase(key, "AntiSmallMissile")) {
                const WeaponAntiMask bit = weaponAntiBit(WeaponAntiTarget::SmallMissile);
                target.antiMask = parseBoolean(value) ? target.antiMask | bit : target.antiMask & ~bit;
            } else if (asciiEqualIgnoreCase(key, "AntiMine")) {
                const WeaponAntiMask bit = weaponAntiBit(WeaponAntiTarget::Mine);
                target.antiMask = parseBoolean(value) ? target.antiMask | bit : target.antiMask & ~bit;
            } else if (asciiEqualIgnoreCase(key, "AntiAirborneInfantry")) {
                const WeaponAntiMask bit = weaponAntiBit(WeaponAntiTarget::AirborneInfantry);
                target.antiMask = parseBoolean(value) ? target.antiMask | bit : target.antiMask & ~bit;
            } else if (asciiEqualIgnoreCase(key, "AntiBallisticMissile")) {
                const WeaponAntiMask bit = weaponAntiBit(WeaponAntiTarget::BallisticMissile);
                target.antiMask = parseBoolean(value) ? target.antiMask | bit : target.antiMask & ~bit;
            } else if (asciiEqualIgnoreCase(key, "AntiParachute")) {
                const WeaponAntiMask bit = weaponAntiBit(WeaponAntiTarget::Parachute);
                target.antiMask = parseBoolean(value) ? target.antiMask | bit : target.antiMask & ~bit;
            } else if (asciiEqualIgnoreCase(key, "DamageDealtAtSelfPosition")) {
                target.damageDealtAtSelfPosition = parseBoolean(value);
            } else if (asciiEqualIgnoreCase(key, "AllowAttackGarrisonedBldgs")) {
                target.allowAttackGarrisonedBldgs = parseBoolean(value);
            } else if (asciiEqualIgnoreCase(key, "StunDuration")) {
                target.stunDuration = std::max(0.0f, parseFloat(value));
            } else if (asciiEqualIgnoreCase(key, "ShockWaveAmount")) {
                target.shockWaveAmount = parseFloat(value);
            } else if (asciiEqualIgnoreCase(key, "ShockWaveRadius")) {
                target.shockWaveRadius = std::max(0.0f, parseFloat(value));
            } else if (asciiEqualIgnoreCase(key, "ShockWaveTaperOff")) {
                target.shockWaveTaperOff = std::max(0.0f, parseFloat(value));
            } else if (asciiEqualIgnoreCase(key, "WeaponBonus")) {
                if (!applyWeaponBonusAssignment(value, target.weaponBonuses)) {
                    TD_LOG_WARN("[WeaponStore] Weapon '{}' has invalid WeaponBonus '{}'",
                                target.name, value);
                }
            } else {
                processContentDiagnostics().warn({
                    .block = "Weapon",
                    .definition = target.name,
                    .field = key,
                    .rawValue = value,
                    .adoptedValue = "field ignored",
                    .reason = "Weapon field is not recognized by the typed compiler; the authored value has no effect",
                });
            }
        }

        if (target.maximumDelayBetweenShotsMilliseconds < target.minimumDelayBetweenShotsMilliseconds) {
            target.maximumDelayBetweenShotsMilliseconds = target.minimumDelayBetweenShotsMilliseconds;
        }
        if (!std::isfinite(target.minTargetPitchRadians)) {
            target.minTargetPitchRadians = -std::numbers::pi_v<float>;
        }
        if (!std::isfinite(target.maxTargetPitchRadians)) {
            target.maxTargetPitchRadians = std::numbers::pi_v<float>;
        }
        if (target.maxTargetPitchRadians < target.minTargetPitchRadians) {
            std::swap(target.minTargetPitchRadians, target.maxTargetPitchRadians);
        }
        target.synchronizeAuthoritativeScalars();
        target.loaded = true;
        m_weapons[target.name] = std::move(target);
    }
    };

    auto& vfs = io::VFS::instance();
    bool valid = true;
    if (vfs.exists(filePath)) {
        const container::String winner = vfs.readAll(filePath);
        GeneralsIniParser parser;
        if (!parser.parse(winner, filePath)) return false;
        applyBlocks(parser.blocks());
    } else {
        GeneralsIniParser parser;
        if (!parser.parseFile(filePath)) return false;
        applyBlocks(parser.blocks());
    }

    TD_LOG_INFO("[WeaponStore] Loaded {} weapon templates from {}", m_weapons.size(), filePath);
    return valid;
}

const WeaponAuthoringTemplate* WeaponStore::find(const container::String& name) const {
    const auto found = m_weapons.find(name);
    return found == m_weapons.end() ? nullptr : &found->second;
}

} // namespace game
