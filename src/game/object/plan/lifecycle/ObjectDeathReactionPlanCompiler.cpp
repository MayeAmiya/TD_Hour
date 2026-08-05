#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "game/data/base/ContentBoolParsing.h"
#include "game/data/base/ContentFloatParsing.h"
#include "game/data/base/PhysicsSimulationRules.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/plan/combat/ObjectFireWeaponBehaviorPlanTypes.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <iterator>
#include <numbers>
#include <optional>
#include <string>
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingModuleRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"

namespace game {
namespace {

constexpr uint32_t kAllDeathTypes =
    (uint32_t{1} << static_cast<uint8_t>(DeathType::COUNT)) - uint32_t{1};
constexpr ObjectVeterancyMask kAllVeterancyLevels =
    static_cast<ObjectVeterancyMask>((ObjectVeterancyMask{1}
        << (static_cast<uint8_t>(ObjectVeterancyLevel::Heroic) + 1u)) - ObjectVeterancyMask{1});

using container::asciiEqualIgnoreCase;

[[nodiscard]] container::Vector<container::StringView> splitTokens(container::StringView value) {
    container::Vector<container::StringView> tokens;
    size_t cursor = 0;
    while (cursor < value.size()) {
        while (cursor < value.size() &&
               (std::isspace(static_cast<unsigned char>(value[cursor])) || value[cursor] == ',')) {
            ++cursor;
        }
        const size_t begin = cursor;
        while (cursor < value.size() &&
               !std::isspace(static_cast<unsigned char>(value[cursor])) && value[cursor] != ',') {
            ++cursor;
        }
        if (begin != cursor) tokens.push_back(value.substr(begin, cursor - begin));
    }
    return tokens;
}

[[nodiscard]] const container::String* moduleValue(const ModuleData& module,
                                               container::StringView key) noexcept {
    // `values` preserves author order and is therefore the authoritative
    // recipe form. RefCode feeds scalar parse callbacks once per authored
    // line, so a repeated scalar is last-write-wins rather than first-write-
    // wins. `properties` covers a manually assembled ModuleData used by
    // tools/tests without relying on unordered-map iteration order.
    const container::String* result = nullptr;
    for (const auto& [entryKey, value] : module.values) {
        if (asciiEqualIgnoreCase(entryKey, key))
            result = &value;
    }
    if (result)
        return result;
    for (const auto& [entryKey, value] : module.properties) {
        if (asciiEqualIgnoreCase(entryKey, key)) return &value;
    }
    return nullptr;
}

using container::trimAsciiView;

[[nodiscard]] float parseFiniteFloat(container::StringView text, float fallback = 0.0f) noexcept {
    return parseContentFloatOr(text, {
        .source = __FILE__, .block = "Object", .module = "DeathReaction",
        .field = "Real", .fallback = fallback});
}

[[nodiscard]] math::q32_32 parseLegacyFrameForce(
    container::StringView text) noexcept {
    // PhysicsBehavior::applyForce() consumes legacy world-units/frame^2 and
    // integrates without a delta. The modern component stores force in
    // mass*world-units/second^2 and integrates with logicDeltaSeconds, so this
    // authored field must cross the unit boundary exactly once here.
    constexpr double legacyForceScale =
        engine::PhysicsSimulationRules::kLegacyPerFrameSquaredToPerSecondSquared;
    const double forcePerSecondSquared =
        static_cast<double>(parseFiniteFloat(text)) * legacyForceScale;
    constexpr double minimum =
        static_cast<double>(std::numeric_limits<int32_t>::min());
    constexpr double maximum =
        static_cast<double>(std::numeric_limits<int32_t>::max());
    return math::q32_32{
        std::clamp(forcePerSecondSquared, minimum, maximum)};
}

[[nodiscard]] bool parseBoolean(container::StringView value, bool fallback) noexcept
{
    return parseContentBool(value, fallback);
}

[[nodiscard]] uint32_t parseMilliseconds(container::StringView text) noexcept {
    const float value = parseFiniteFloat(text);
    if (!std::isfinite(value) || value <= 0.0f) return 0;
    constexpr float maximum = static_cast<float>(std::numeric_limits<uint32_t>::max());
    return static_cast<uint32_t>(std::min(std::ceil(value), maximum));
}

[[nodiscard]] int32_t parsePositiveInt(container::StringView text, int32_t fallback) noexcept {
    const float value = parseFiniteFloat(text, static_cast<float>(fallback));
    if (!std::isfinite(value)) return fallback;
    constexpr float maximum = static_cast<float>(std::numeric_limits<int32_t>::max());
    if (value >= maximum) return std::numeric_limits<int32_t>::max();
    if (value <= 1.0f) return 1;
    return std::max(1, static_cast<int32_t>(std::trunc(value)));
}

[[nodiscard]] int32_t parseSignedInt(container::StringView text, int32_t fallback) noexcept {
    const float value = parseFiniteFloat(text, static_cast<float>(fallback));
    if (!std::isfinite(value)) return fallback;
    constexpr float maximum = static_cast<float>(std::numeric_limits<int32_t>::max());
    constexpr float minimum = static_cast<float>(std::numeric_limits<int32_t>::min());
    if (value >= maximum) return std::numeric_limits<int32_t>::max();
    if (value <= minimum) return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(std::trunc(value));
}

[[nodiscard]] math::q32_32 parsePercentToFixed(container::StringView text) noexcept {
    text = trimAsciiView(text);
    if (!text.empty() && text.back() == '%') text.remove_suffix(1);
    const float authoredPercent = parseFiniteFloat(text);
    return math::q32_32{authoredPercent * 0.01f};
}

[[nodiscard]] float parseAngleRadians(container::StringView text) noexcept {
    text = trimAsciiView(text);
    bool radians = false;
    if (text.size() >= 3 && asciiEqualIgnoreCase(text.substr(text.size() - 3), "rad")) {
        text.remove_suffix(3);
        radians = true;
    } else if (text.size() >= 3 && asciiEqualIgnoreCase(text.substr(text.size() - 3), "deg")) {
        text.remove_suffix(3);
    }
    const float value = parseFiniteFloat(text);
    return radians ? value : value * (std::numbers::pi_v<float> / 180.0f);
}

[[nodiscard]] std::optional<DeathType> parseDeathTypeToken(container::StringView token) noexcept {
    constexpr container::Array<container::StringView, static_cast<size_t>(DeathType::COUNT)> names = {
        "NORMAL", "NONE", "CRUSHED", "BURNED", "EXPLODED", "POISONED", "TOPPLED",
        "FLOODED", "SUICIDED", "LASERED", "DETONATED", "SPLATTED", "POISONED_BETA",
        "EXTRA_2", "EXTRA_3", "EXTRA_4", "EXTRA_5", "EXTRA_6", "EXTRA_7", "EXTRA_8",
        "POISONED_GAMMA",
    };
    for (size_t index = 0; index < names.size(); ++index) {
        if (asciiEqualIgnoreCase(token, names[index])) {
            return static_cast<DeathType>(index);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ObjectVeterancyLevel>
parseVeterancyToken(container::StringView token) noexcept {
    constexpr container::Array<container::StringView, 4> names = {
        "REGULAR", "VETERAN", "ELITE", "HEROIC",
    };
    for (size_t index = 0; index < names.size(); ++index) {
        if (asciiEqualIgnoreCase(token, names[index])) {
            return static_cast<ObjectVeterancyLevel>(index);
        }
    }
    return std::nullopt;
}


[[nodiscard]] uint32_t parseDeathTypeMask(container::StringView value, bool& resolved) {
    uint32_t mask = kAllDeathTypes;
    for (const container::StringView token : splitTokens(value)) {
        if (asciiEqualIgnoreCase(token, "ALL")) {
            mask = kAllDeathTypes;
            continue;
        }
        if (asciiEqualIgnoreCase(token, "NONE")) {
            mask = 0;
            continue;
        }
        if (token.empty() || (token.front() != '+' && token.front() != '-')) {
            resolved = false;
            continue;
        }
        const std::optional<DeathType> type = parseDeathTypeToken(token.substr(1));
        if (!type) {
            resolved = false;
            continue;
        }
        const uint32_t bit = uint32_t{1} << static_cast<uint8_t>(*type);
        if (token.front() == '+') mask |= bit;
        else mask &= ~bit;
    }
    return mask;
}

[[nodiscard]] ObjectVeterancyMask parseVeterancyMask(container::StringView value, bool& resolved) {
    ObjectVeterancyMask mask = kAllVeterancyLevels;
    for (const container::StringView token : splitTokens(value)) {
        if (asciiEqualIgnoreCase(token, "ALL")) {
            mask = kAllVeterancyLevels;
            continue;
        }
        if (asciiEqualIgnoreCase(token, "NONE")) {
            mask = 0;
            continue;
        }
        if (token.empty() || (token.front() != '+' && token.front() != '-')) {
            resolved = false;
            continue;
        }
        const std::optional<ObjectVeterancyLevel> level = parseVeterancyToken(token.substr(1));
        if (!level) {
            resolved = false;
            continue;
        }
        const ObjectVeterancyMask bit = objectVeterancyBit(*level);
        if (token.front() == '+') mask = static_cast<ObjectVeterancyMask>(mask | bit);
        else mask = static_cast<ObjectVeterancyMask>(mask & ~bit);
    }
    return mask;
}


[[nodiscard]] std::optional<ObjectSlowDeathPhase>
parseSlowDeathPhase(container::StringView token) noexcept {
    if (asciiEqualIgnoreCase(token, "INITIAL")) return ObjectSlowDeathPhase::Initial;
    if (asciiEqualIgnoreCase(token, "MIDPOINT")) return ObjectSlowDeathPhase::Midpoint;
    if (asciiEqualIgnoreCase(token, "FINAL")) return ObjectSlowDeathPhase::Final;
    return std::nullopt;
}

void appendSlowDeathPhaseValues(
    const ModuleData& module, container::StringView key,
    container::Array<container::Vector<container::String>, static_cast<size_t>(ObjectSlowDeathPhase::Count)>& output) {
    const auto append = [&output](container::StringView value) {
        const container::Vector<container::StringView> tokens = splitTokens(value);
        if (tokens.size() < 2) return;
        const std::optional<ObjectSlowDeathPhase> phase = parseSlowDeathPhase(tokens.front());
        if (!phase) return;
        container::Vector<container::String>& list = output[static_cast<size_t>(*phase)];
        for (size_t index = 1; index < tokens.size(); ++index) {
            if (!tokens[index].empty()) list.emplace_back(tokens[index]);
        }
    };

    bool foundInOrderedValues = false;
    for (const auto& [entryKey, value] : module.values) {
        if (!asciiEqualIgnoreCase(entryKey, key)) continue;
        foundInOrderedValues = true;
        append(value);
    }
    // Hand-authored probe data sometimes populates only `properties`. Normal
    // INI data always takes the ordered branch above, preserving repeated FX
    // / OCL / Weapon fields and their source order.
    if (!foundInOrderedValues) {
        if (const container::String* value = moduleValue(module, key)) append(*value);
    }
}

// InstantDeathBehavior does not use the INITIAL/MIDPOINT/FINAL grammar of a
// generic SlowDeath.  The legacy parser appends every token from every
// repeated field to one flat candidate list, preserving source order.
void appendInstantDeathValues(const ModuleData& module, container::StringView key,
                              container::Vector<container::String>& output) {
    const auto append = [&output](container::StringView value) {
        for (const container::StringView token : splitTokens(value)) {
            if (!token.empty()) output.emplace_back(token);
        }
    };

    bool foundInOrderedValues = false;
    for (const auto& [entryKey, value] : module.values) {
        if (!asciiEqualIgnoreCase(entryKey, key)) continue;
        foundInOrderedValues = true;
        append(value);
    }
    // Tool-created ModuleData may only contain `properties`; production INI
    // recipes take the ordered branch above, including repeated fields.
    if (!foundInOrderedValues) {
        if (const container::String* value = moduleValue(module, key)) append(*value);
    }
}

template <size_t PhaseCount, typename ParsePhase>
void appendStructurePhaseValues(
    const ModuleData& module, container::StringView key,
    container::Array<container::Vector<container::String>, PhaseCount>& output,
    ParsePhase&& parsePhase) {
    const auto append = [&](container::StringView value) {
        const container::Vector<container::StringView> tokens =
            splitTokens(value);
        if (tokens.size() < 2) return;
        const std::optional<size_t> phase = parsePhase(tokens.front());
        if (!phase || *phase >= output.size()) return;
        for (size_t index = 1; index < tokens.size(); ++index) {
            if (!tokens[index].empty() &&
                !asciiEqualIgnoreCase(tokens[index], "NONE")) {
                output[*phase].emplace_back(tokens[index]);
            }
        }
    };
    bool ordered = false;
    for (const auto& [entryKey, value] : module.values) {
        if (!asciiEqualIgnoreCase(entryKey, key)) continue;
        ordered = true;
        append(value);
    }
    if (!ordered) {
        if (const container::String* value = moduleValue(module, key)) {
            append(*value);
        }
    }
}

[[nodiscard]] std::optional<size_t> parseTopplePhase(
    container::StringView token) noexcept {
    if (asciiEqualIgnoreCase(token, "INITIAL")) {
        return static_cast<size_t>(ObjectStructureTopplePhase::Initial);
    }
    if (asciiEqualIgnoreCase(token, "DELAY")) {
        return static_cast<size_t>(ObjectStructureTopplePhase::Delay);
    }
    if (asciiEqualIgnoreCase(token, "FINAL")) {
        return static_cast<size_t>(ObjectStructureTopplePhase::Final);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<size_t> parseCollapsePhase(
    container::StringView token) noexcept {
    if (asciiEqualIgnoreCase(token, "INITIAL")) {
        return static_cast<size_t>(ObjectStructureCollapsePhase::Initial);
    }
    if (asciiEqualIgnoreCase(token, "DELAY")) {
        return static_cast<size_t>(ObjectStructureCollapsePhase::Delay);
    }
    if (asciiEqualIgnoreCase(token, "BURST")) {
        return static_cast<size_t>(ObjectStructureCollapsePhase::Burst);
    }
    if (asciiEqualIgnoreCase(token, "FINAL")) {
        return static_cast<size_t>(ObjectStructureCollapsePhase::Final);
    }
    return std::nullopt;
}

[[nodiscard]] uint64_t parseDamageTypeFlags(
    container::StringView value, bool& resolved) {
    constexpr uint8_t count = static_cast<uint8_t>(DamageType::COUNT);
    const uint64_t all = count >= 64
        ? std::numeric_limits<uint64_t>::max()
        : (uint64_t{1} << count) - 1u;
    uint64_t mask = all;
    for (container::StringView token : splitTokens(value)) {
        if (asciiEqualIgnoreCase(token, "ALL")) {
            mask = all;
            continue;
        }
        if (asciiEqualIgnoreCase(token, "NONE")) {
            mask = 0;
            continue;
        }
        if (token.empty() ||
            (token.front() != '+' && token.front() != '-')) {
            resolved = false;
            continue;
        }
        const bool remove = token.front() == '-';
        token.remove_prefix(1);
        const std::optional<DamageType> type = tryParseDamageType(token);
        if (!type || static_cast<uint8_t>(*type) >= 64) {
            resolved = false;
            continue;
        }
        const uint64_t bit = uint64_t{1} << static_cast<uint8_t>(*type);
        if (remove) mask &= ~bit;
        else mask |= bit;
    }
    return mask;
}

void appendToppleAngleFx(const ModuleData& module,
                         ObjectStructureToppleParameters& output) {
    const auto append = [&](container::StringView value) {
        const container::Vector<container::StringView> tokens =
            splitTokens(value);
        if (tokens.size() < 2 ||
            asciiEqualIgnoreCase(tokens[1], "NONE")) return;
        output.angleFx.push_back({
            .angleRadians = math::q32_32{
                parseFiniteFloat(tokens[0]) *
                (std::numbers::pi_v<float> / 180.0f)},
            .fx = container::String{tokens[1]},
        });
    };
    bool ordered = false;
    for (const auto& [entryKey, value] : module.values) {
        if (!asciiEqualIgnoreCase(entryKey, "AngleFX")) continue;
        ordered = true;
        append(value);
    }
    if (!ordered) {
        if (const container::String* value = moduleValue(module, "AngleFX")) {
            append(*value);
        }
    }
    std::stable_sort(output.angleFx.begin(), output.angleFx.end(),
        [](const ObjectStructureAngleFx& left,
           const ObjectStructureAngleFx& right) {
            return left.angleRadians < right.angleRadians;
        });
}

// UpgradeMux uses INI::parseAsciiStringVector for these fields.  Preserve
// repeated authored entries and their source order; activation is a pure
// membership test, so there is no reason to introduce a hidden unordered
// container or a legacy UpgradeMask at recipe-compile time.
void appendUpgradeNames(const ModuleData& module, container::StringView key, container::Vector<container::String>& output)
{
    const auto append = [&output](container::StringView value)
    {
        for (const container::StringView token : splitTokens(value))
        {
            if (!token.empty())
                output.emplace_back(token);
        }
    };

    const container::String* finalOrderedValue = nullptr;
    for (const auto& [entryKey, value] : module.values)
    {
        if (!asciiEqualIgnoreCase(entryKey, key))
            continue;
        // INI::parseAsciiStringVector clears the destination before parsing
        // each line.  Repeated TriggeredBy/ConflictsWith/RemovesUpgrades
        // therefore retain only the final authored list, unlike SlowDeath's
        // intentionally append-only FX/OCL/Weapon grammar.
        finalOrderedValue = &value;
    }
    if (finalOrderedValue)
        append(*finalOrderedValue);
    else if (const container::String* value = moduleValue(module, key))
        append(*value);
}

[[nodiscard]] ObjectDeathReactionKind classifyReaction(container::StringView moduleClass) noexcept {
    if (asciiEqualIgnoreCase(moduleClass, "DestroyDie")) {
        return ObjectDeathReactionKind::Destroy;
    }
    if (asciiEqualIgnoreCase(moduleClass, "KeepObjectDie")) {
        return ObjectDeathReactionKind::KeepObject;
    }
    if (asciiEqualIgnoreCase(moduleClass, "FXListDie"))
    {
        return ObjectDeathReactionKind::FxList;
    }
    if (asciiEqualIgnoreCase(moduleClass, "UpgradeDie")) {
        return ObjectDeathReactionKind::Upgrade;
    }
    if (asciiEqualIgnoreCase(moduleClass, "CrushDie")) {
        return ObjectDeathReactionKind::Crush;
    }
    if (asciiEqualIgnoreCase(moduleClass, "FireWeaponWhenDeadBehavior")) {
        return ObjectDeathReactionKind::FireWeaponWhenDead;
    }
    if (asciiEqualIgnoreCase(moduleClass, "LeafletDropBehavior")) {
        return ObjectDeathReactionKind::LeafletDrop;
    }
    if (asciiEqualIgnoreCase(moduleClass, "EjectPilotDie")) {
        return ObjectDeathReactionKind::EjectPilot;
    }
    if (asciiEqualIgnoreCase(moduleClass, "DamDie")) {
        return ObjectDeathReactionKind::Dam;
    }
    if (asciiEqualIgnoreCase(moduleClass, "CreateCrateDie")) {
        return ObjectDeathReactionKind::CreateCrate;
    }
    if (asciiEqualIgnoreCase(moduleClass, "CreateObjectDie")) {
        return ObjectDeathReactionKind::CreateObject;
    }
    if (asciiEqualIgnoreCase(moduleClass, "SpecialPowerCompletionDie")) {
        return ObjectDeathReactionKind::SpecialPowerCompletion;
    }
    if (asciiEqualIgnoreCase(moduleClass, "NeutronBlastBehavior")) {
        return ObjectDeathReactionKind::NeutronBlast;
    }
    if (asciiEqualIgnoreCase(moduleClass, "StructureToppleUpdate")) {
        return ObjectDeathReactionKind::StructureTopple;
    }
    if (asciiEqualIgnoreCase(moduleClass, "StructureCollapseUpdate")) {
        return ObjectDeathReactionKind::StructureCollapse;
    }
    if (asciiEqualIgnoreCase(moduleClass, "JetSlowDeathBehavior") ||
        asciiEqualIgnoreCase(moduleClass,
                             "HelicopterSlowDeathBehavior")) {
        return ObjectDeathReactionKind::AircraftSlowDeath;
    }
    if (asciiEqualIgnoreCase(moduleClass, "RebuildHoleExposeDie")) {
        return ObjectDeathReactionKind::RebuildHoleExpose;
    }
    if (asciiEqualIgnoreCase(moduleClass, "RebuildHoleBehavior")) {
        return ObjectDeathReactionKind::RebuildHoleBehavior;
    }
    if (asciiEqualIgnoreCase(moduleClass, "InstantDeathBehavior")) {
        return ObjectDeathReactionKind::InstantDeath;
    }
    if (asciiEqualIgnoreCase(moduleClass, "SlowDeathBehavior") ||
        asciiEqualIgnoreCase(moduleClass, "BattleBusSlowDeathBehavior") ||
        asciiEqualIgnoreCase(moduleClass,
                             "NeutronMissileSlowDeathBehavior")) {
        return ObjectDeathReactionKind::SlowDeath;
    }
    return ObjectDeathReactionKind::Unsupported;
}

[[nodiscard]] ObjectOnDieHandlerKind classifyOnDieHandler(
    container::StringView moduleClass,
    ObjectDeathReactionKind reaction) noexcept {
    if (const std::optional<ObjectModuleCatalogEntry> catalog =
            findObjectModuleCatalogEntry(moduleClass);
        catalog && catalog->onDieHandler != ObjectOnDieHandlerKind::None) {
        return catalog->onDieHandler;
    }
    if (reaction != ObjectDeathReactionKind::Unsupported)
        return ObjectOnDieHandlerKind::DeathReaction;
    return ObjectOnDieHandlerKind::Unknown;
}

} // namespace


container::SharedPtr<const ObjectDeathReactionPlan>
compileObjectDeathReactionPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog) {
    auto plan = std::make_shared<ObjectDeathReactionPlan>();
    plan->hasAiDeathGate = std::any_of(templateData.modules.begin(), templateData.modules.end(),
        [](const ModuleData& module) { return module.isAiModule; });
    uint32_t finalBehaviorIndex = 0;
    for (const ModuleData& module : templateData.modules) {
        const uint32_t currentBehaviorIndex = finalBehaviorIndex;
        if (module.category == ModuleRecipeCategory::Behavior ||
            module.category == ModuleRecipeCategory::Unknown) {
            ++finalBehaviorIndex;
        }
        const container::StringView moduleClass = !module.moduleClass.empty()
            ? container::StringView{module.moduleClass}
            : container::StringView{module.type};
        const ObjectDeathReactionKind kind = classifyReaction(moduleClass);
        const bool isDieModule = kind != ObjectDeathReactionKind::Unsupported ||
            (module.interfaceMask & ModuleRecipeInterfaceDie) != 0;
        if (!isDieModule) {
            continue;
        }

        const ObjectOnDieHandlerKind onDieHandler =
            classifyOnDieHandler(moduleClass, kind);
        ObjectOnDieBehaviorEntry behavior{
            .handler = onDieHandler,
            .authoredOrder = module.authoredOrder,
            .finalBehaviorIndex = currentBehaviorIndex,
            .deathTypeMask = kAllDeathTypes,
            .veterancyMask = kAllVeterancyLevels,
        };
        if (const container::String* value = moduleValue(module, "DeathTypes")) {
            behavior.deathTypeMask = parseDeathTypeMask(
                *value, behavior.filtersFullyResolved);
        }
        if (const container::String* value =
                moduleValue(module, "VeterancyLevels")) {
            behavior.veterancyMask = parseVeterancyMask(
                *value, behavior.filtersFullyResolved);
        }
        if (const container::String* value = moduleValue(module, "ExemptStatus")) {
            const ObjectStatusMaskParseResult parsed =
                parseObjectStatusMask(*value);
            behavior.exemptStatuses = parsed.mask;
            behavior.filtersFullyResolved =
                behavior.filtersFullyResolved && parsed.resolved;
        }
        if (const container::String* value = moduleValue(module, "RequiredStatus")) {
            const ObjectStatusMaskParseResult parsed =
                parseObjectStatusMask(*value);
            behavior.requiredStatuses = parsed.mask;
            behavior.filtersFullyResolved =
                behavior.filtersFullyResolved && parsed.resolved;
        }
        plan->onDieBehaviors.push_back(behavior);
        if (onDieHandler == ObjectOnDieHandlerKind::Unknown) {
            ++plan->unknownOnDieHandlerCount;
            plan->diagnostics.push_back(
                "unknown Die interface module '" + module.moduleClass +
                "' at authored order " +
                std::to_string(module.authoredOrder) +
                "; preserved as an inert Mod capability");
        }
        // Non-reaction Behavior families already own typed immutable plans in
        // their ECS subsystem. Keep only their authored routing entry here;
        // manufacturing an Unsupported reaction rule duplicates payload
        // ownership and makes stock ZH modules look unimplemented.
        if (onDieHandler != ObjectOnDieHandlerKind::DeathReaction) {
            continue;
        }

        ObjectDeathReactionRule rule{
            .kind = kind,
            .authoredOrder = module.authoredOrder,
            .deathTypeMask = behavior.deathTypeMask,
            .veterancyMask = behavior.veterancyMask,
            .exemptStatuses = behavior.exemptStatuses,
            .requiredStatuses = behavior.requiredStatuses,
            .filtersFullyResolved = behavior.filtersFullyResolved,
        };
        if (kind == ObjectDeathReactionKind::SlowDeath ||
            kind == ObjectDeathReactionKind::AircraftSlowDeath) {
            ObjectSlowDeathParameters slow;
            slow.cancelsBattleBusUndeath = asciiEqualIgnoreCase(
                moduleClass, "BattleBusSlowDeathBehavior");
            if (const container::String* value = moduleValue(module, "ProbabilityModifier")) {
                slow.probabilityModifier = parsePositiveInt(*value, slow.probabilityModifier);
            }
            if (const container::String* value = moduleValue(module, "ModifierBonusPerOverkillPercent")) {
                slow.modifierBonusPerOverkillPercent = parsePercentToFixed(*value);
            }
            if (const container::String* value = moduleValue(module, "SinkDelay")) {
                slow.sinkDelayMilliseconds = parseMilliseconds(*value);
            }
            if (const container::String* value = moduleValue(module, "SinkDelayVariance")) {
                slow.sinkDelayVarianceMilliseconds = parseMilliseconds(*value);
            }
            if (const container::String* value = moduleValue(module, "DestructionDelay")) {
                slow.destructionDelayMilliseconds = parseMilliseconds(*value);
            }
            if (const container::String* value = moduleValue(module, "DestructionDelayVariance")) {
                slow.destructionDelayVarianceMilliseconds = parseMilliseconds(*value);
            }
            if (const container::String* value = moduleValue(module, "SinkRate")) {
                slow.sinkRateUnitsPerSecond =
                    math::q32_32{parseFiniteFloat(*value)};
            }
            if (const container::String* value = moduleValue(module, "DestructionAltitude")) {
                static_cast<void>(parseFiniteFloat(*value, -10.0f));
            }
            if (const container::String* value = moduleValue(module, "FlingForce")) {
                slow.flingForce = parseLegacyFrameForce(*value);
            }
            if (const container::String* value = moduleValue(module, "FlingForceVariance")) {
                slow.flingForceVariance = parseLegacyFrameForce(*value);
            }
            if (const container::String* value = moduleValue(module, "FlingPitch")) {
                slow.flingPitchRadians =
                    math::q32_32{parseAngleRadians(*value)};
            }
            if (const container::String* value = moduleValue(module, "FlingPitchVariance")) {
                slow.flingPitchVarianceRadians =
                    math::q32_32{parseAngleRadians(*value)};
            }
            appendSlowDeathPhaseValues(module, "FX", slow.fx);
            appendSlowDeathPhaseValues(module, "OCL", slow.ocls);
            appendSlowDeathPhaseValues(module, "Weapon", slow.weapons);
            rule.slowDeath = std::move(slow);
        }
        if (kind == ObjectDeathReactionKind::InstantDeath) {
            ObjectInstantDeathParameters instant;
            appendInstantDeathValues(module, "FX", instant.fx);
            appendInstantDeathValues(module, "OCL", instant.ocls);
            appendInstantDeathValues(module, "Weapon", instant.weapons);
            rule.instantDeath = std::move(instant);
        }
        if (kind == ObjectDeathReactionKind::CreateObject) {
            ObjectCreateObjectDieParameters create;
            if (const container::String* value = moduleValue(module, "CreationList")) {
                const container::StringView name = trimAsciiView(*value);
                if (!asciiEqualIgnoreCase(name, "NONE")) {
                    create.creationList = container::String{name};
                }
            }
            if (const container::String* value =
                    moduleValue(module, "TransferPreviousHealth")) {
                create.transferPreviousHealth = parseBoolean(*value, false);
            }
            if (const container::String* value =
                    moduleValue(module, "TransferSelection")) {
                create.transferSelection = parseBoolean(*value, false);
            }
            rule.createObjectDie = std::move(create);
        }
        if (kind == ObjectDeathReactionKind::SpecialPowerCompletion) {
            ObjectSpecialPowerCompletionDieParameters completion;
            if (const container::String* value =
                    moduleValue(module, "SpecialPowerTemplate")) {
                const container::StringView name = trimAsciiView(*value);
                if (!name.empty() && !asciiEqualIgnoreCase(name, "NONE")) {
                    completion.specialPowerTemplate = container::String{name};
                }
            }
            rule.specialPowerCompletionDie = std::move(completion);
        }
        if (kind == ObjectDeathReactionKind::NeutronBlast) {
            ObjectNeutronBlastDieParameters blast;
            if (const container::String* value =
                    moduleValue(module, "BlastRadius")) {
                blast.blastRadius = math::q32_32::max(
                    math::q32_32{},
                    math::q32_32{parseFiniteFloat(*value, 10.0f)});
            }
            if (const container::String* value =
                    moduleValue(module, "AffectAirborne")) {
                blast.affectAirborne = parseBoolean(
                    *value, blast.affectAirborne);
            }
            if (const container::String* value =
                    moduleValue(module, "AffectAllies")) {
                blast.affectAllies = parseBoolean(*value, blast.affectAllies);
            }
            rule.neutronBlastDie = blast;
        }
        if (kind == ObjectDeathReactionKind::EjectPilot) {
            ObjectEjectPilotDieParameters eject;
            const auto readOcl = [&](container::StringView key,
                                     container::String& destination) {
                const container::String* value = moduleValue(module, key);
                if (!value) return;
                const container::StringView name = trimAsciiView(*value);
                if (!name.empty() && !asciiEqualIgnoreCase(name, "NONE")) {
                    destination = container::String{name};
                }
            };
            readOcl("AirCreationList", eject.airCreationList);
            readOcl("GroundCreationList", eject.groundCreationList);
            if (const container::String* value =
                    moduleValue(module, "InvulnerableTime")) {
                eject.invulnerableTimeMilliseconds = parseMilliseconds(*value);
            }
            rule.ejectPilotDie = std::move(eject);
        }
        if (kind == ObjectDeathReactionKind::CreateCrate) {
            ObjectCreateCrateDieParameters crate;
            for (const auto& [key, value] : module.values) {
                if (!asciiEqualIgnoreCase(key, "CrateData")) continue;
                const container::StringView name = trimAsciiView(value);
                if (!name.empty() && !asciiEqualIgnoreCase(name, "NONE")) {
                    crate.crateData.emplace_back(name);
                }
            }
            if (crate.crateData.empty()) {
                if (const container::String* value =
                        moduleValue(module, "CrateData")) {
                    const container::StringView name = trimAsciiView(*value);
                    if (!name.empty() &&
                        !asciiEqualIgnoreCase(name, "NONE")) {
                        crate.crateData.emplace_back(name);
                    }
                }
            }
            rule.createCrateDie = std::move(crate);
        }
        if (kind == ObjectDeathReactionKind::StructureTopple) {
            ObjectStructureToppleParameters topple;
            const auto duration = [&](container::StringView key,
                                      uint32_t& destination) {
                if (const container::String* value = moduleValue(module, key)) {
                    destination = parseMilliseconds(*value);
                }
            };
            duration("MinToppleDelay", topple.minToppleDelayMilliseconds);
            duration("MaxToppleDelay", topple.maxToppleDelayMilliseconds);
            duration("MinToppleBurstDelay", topple.minBurstDelayMilliseconds);
            duration("MaxToppleBurstDelay", topple.maxBurstDelayMilliseconds);
            if (topple.maxToppleDelayMilliseconds <
                topple.minToppleDelayMilliseconds) {
                topple.maxToppleDelayMilliseconds =
                    topple.minToppleDelayMilliseconds;
            }
            if (topple.maxBurstDelayMilliseconds <
                topple.minBurstDelayMilliseconds) {
                topple.maxBurstDelayMilliseconds =
                    topple.minBurstDelayMilliseconds;
            }
            if (const container::String* value =
                    moduleValue(module, "StructuralIntegrity")) {
                topple.structuralIntegrity = math::q32_32::clamp(
                    math::q32_32{parseFiniteFloat(*value)},
                    math::q32_32{}, math::q32_32{int32_t{1}});
            }
            if (const container::String* value =
                    moduleValue(module, "StructuralDecay")) {
                topple.structuralDecay = math::q32_32::clamp(
                    math::q32_32{parseFiniteFloat(*value)},
                    math::q32_32{}, math::q32_32{int32_t{1}});
            }
            if (const container::String* value =
                    moduleValue(module, "DamageFXTypes")) {
                topple.damageFxTypes = parseDamageTypeFlags(
                    *value, rule.filtersFullyResolved);
            }
            const auto resource = [&](container::StringView key,
                                      container::String& destination) {
                const container::String* value = moduleValue(module, key);
                if (!value) return;
                const container::StringView name = trimAsciiView(*value);
                if (!name.empty() && !asciiEqualIgnoreCase(name, "NONE")) {
                    destination = container::String{name};
                }
            };
            resource("TopplingFX", topple.topplingFx);
            resource("ToppleStartFX", topple.toppleStartFx);
            resource("ToppleDelayFX", topple.toppleDelayFx);
            resource("ToppleDoneFX", topple.toppleDoneFx);
            resource("CrushingFX", topple.crushingFx);
            resource("CrushingWeaponName", topple.crushingWeapon);
            appendStructurePhaseValues(
                module, "OCL", topple.ocls, parseTopplePhase);
            appendToppleAngleFx(module, topple);
            rule.structureTopple = std::move(topple);
        }
        if (kind == ObjectDeathReactionKind::StructureCollapse) {
            ObjectStructureCollapseParameters collapse;
            const auto duration = [&](container::StringView key,
                                      uint32_t& destination) {
                if (const container::String* value = moduleValue(module, key)) {
                    destination = parseMilliseconds(*value);
                }
            };
            duration("MinCollapseDelay", collapse.minCollapseDelayMilliseconds);
            duration("MaxCollapseDelay", collapse.maxCollapseDelayMilliseconds);
            duration("MinBurstDelay", collapse.minBurstDelayMilliseconds);
            duration("MaxBurstDelay", collapse.maxBurstDelayMilliseconds);
            if (collapse.maxCollapseDelayMilliseconds <
                collapse.minCollapseDelayMilliseconds) {
                collapse.maxCollapseDelayMilliseconds =
                    collapse.minCollapseDelayMilliseconds;
            }
            if (collapse.maxBurstDelayMilliseconds <
                collapse.minBurstDelayMilliseconds) {
                collapse.maxBurstDelayMilliseconds =
                    collapse.minBurstDelayMilliseconds;
            }
            if (const container::String* value =
                    moduleValue(module, "CollapseDamping")) {
                collapse.collapseDamping = math::q32_32::clamp(
                    math::q32_32{parseFiniteFloat(*value)},
                    math::q32_32{}, math::q32_32{int32_t{1}});
            }
            if (const container::String* value =
                    moduleValue(module, "MaxShudder")) {
                collapse.maxShudder = math::q32_32::max(
                    math::q32_32{},
                    math::q32_32{parseFiniteFloat(*value)});
            }
            if (const container::String* value =
                    moduleValue(module, "BigBurstFrequency")) {
                collapse.bigBurstFrequency = static_cast<uint32_t>(
                    std::max(0, parseSignedInt(*value, 0)));
            }
            appendStructurePhaseValues(
                module, "FXList", collapse.fx, parseCollapsePhase);
            appendStructurePhaseValues(
                module, "OCL", collapse.ocls, parseCollapsePhase);
            rule.structureCollapse = std::move(collapse);
        }
        if (kind == ObjectDeathReactionKind::FxList)
        {
            ObjectFxListDieParameters fxList;
            if (const container::String* value = moduleValue(module, "DeathFX"))
            {
                const container::StringView name = trimAsciiView(*value);
                // INI::parseFXList treats the conventional `None` spelling
                // as a null FXList. Keep that as an empty optional payload
                // boundary, never a fake renderer asset named "None".
                if (!asciiEqualIgnoreCase(name, "NONE"))
                    fxList.deathFx = container::String(name);
            }
            if (const container::String* value = moduleValue(module, "OrientToObject"))
            {
                fxList.orientToObject = parseBoolean(*value, fxList.orientToObject);
            }
            if (const container::String* value = moduleValue(module, "StartsActive"))
            {
                fxList.startsActive = parseBoolean(*value, fxList.startsActive);
            }
            if (const container::String* value = moduleValue(module, "RequiresAllTriggers"))
            {
                fxList.requiresAllTriggers = parseBoolean(*value, fxList.requiresAllTriggers);
            }
            if (const container::String* value = moduleValue(module, "FXListUpgrade"))
            {
                fxList.upgradeFx = container::String(trimAsciiView(*value));
            }
            appendUpgradeNames(module, "TriggeredBy", fxList.triggeredBy);
            appendUpgradeNames(module, "ConflictsWith", fxList.conflictsWith);
            appendUpgradeNames(module, "RemovesUpgrades", fxList.removesUpgrades);
            if (upgradeCatalog) {
                const auto compile = [upgradeCatalog](
                    container::Span<const container::String> names) {
                    engine::UpgradeMask result;
                    for (const container::String& name : names) {
                        if (const engine::UpgradeDefinition* definition =
                                upgradeCatalog->find(name)) {
                            engine::upgradeMaskSet(result, definition->id);
                        }
                    }
                    return result;
                };
                fxList.triggeredByMask = compile(fxList.triggeredBy);
                fxList.conflictsWithMask = compile(fxList.conflictsWith);
                fxList.removesUpgradesMask = compile(fxList.removesUpgrades);
                fxList.upgradeMasksCompiled = true;
            }
            rule.fxListDie = std::move(fxList);
        }
        if (kind == ObjectDeathReactionKind::Upgrade) {
            ObjectUpgradeDieParameters upgrade;
            if (const container::String* value = moduleValue(module, "UpgradeToRemove")) {
                // RefCode parses this field with INI::parseAsciiString, which
                // consumes one token. Shipped ZH data contains a stray
                // `ModuleTag_13` after several UpgradeToRemove values.
                const container::StringView trimmed = trimAsciiView(*value);
                const size_t end = trimmed.find_first_of(" \t\r\n");
                upgrade.upgradeToRemove = container::String(
                    trimmed.substr(0, end));
            }
            if (upgradeCatalog) {
                if (const engine::UpgradeDefinition* definition =
                        upgradeCatalog->find(upgrade.upgradeToRemove)) {
                    upgrade.upgradeToRemoveId = definition->id;
                }
            }
            rule.upgradeDie = std::move(upgrade);
        }
        if (kind == ObjectDeathReactionKind::Crush) {
            ObjectCrushDieParameters crush;
            constexpr container::Array<container::StringView, 3> soundKeys{
                "TotalCrushSound", "BackEndCrushSound", "FrontEndCrushSound"};
            constexpr container::Array<container::StringView, 3> percentKeys{
                "TotalCrushSoundPercent", "BackEndCrushSoundPercent",
                "FrontEndCrushSoundPercent"};
            for (size_t index = 0; index < soundKeys.size(); ++index) {
                if (const container::String* value = moduleValue(module, soundKeys[index])) {
                    const container::StringView name = trimAsciiView(*value);
                    if (!asciiEqualIgnoreCase(name, "NONE"))
                        crush.sounds[index] = container::String(name);
                }
                if (const container::String* value = moduleValue(module, percentKeys[index])) {
                    crush.soundPercents[index] =
                        parseSignedInt(*value, crush.soundPercents[index]);
                }
            }
            rule.crushDie = std::move(crush);
        }
        if (kind == ObjectDeathReactionKind::FireWeaponWhenDead) {
            rule.fireWeaponWhenDead =
                compileObjectFireWeaponWhenDeadParameters(
                    module, nullptr, upgradeCatalog);
        }
        plan->rules.push_back(std::move(rule));
    }
    if (plan->onDieBehaviors.empty()) return {};
    // Equal authored-order values are not expected from the resolved recipe,
    // but retaining source order here is cheap and keeps the Die interface
    // walk identical to Object::onDie even for malformed/modded content.
    std::stable_sort(
        plan->rules.begin(), plan->rules.end(),
        [](const ObjectDeathReactionRule& left,
           const ObjectDeathReactionRule& right) {
            return left.authoredOrder < right.authoredOrder;
        });
    for (ObjectOnDieBehaviorEntry& entry : plan->onDieBehaviors) {
        if (entry.handler != ObjectOnDieHandlerKind::DeathReaction) continue;
        const auto found = std::find_if(
            plan->rules.begin(), plan->rules.end(),
            [&entry](const ObjectDeathReactionRule& rule) {
                return rule.authoredOrder == entry.authoredOrder &&
                    rule.kind != ObjectDeathReactionKind::Unsupported;
            });
        if (found != plan->rules.end()) {
            entry.reactionRuleIndex = static_cast<uint32_t>(
                std::distance(plan->rules.begin(), found));
        }
    }
    return plan;
}

bool isObjectDeathReactionApplicable(const ObjectDeathReactionRule& rule, DeathType deathType,
                                     ObjectVeterancyLevel veterancy,
                                     ObjectStatusMask statuses) noexcept {
    if (!rule.filtersFullyResolved) return false;
    const uint8_t deathIndex = static_cast<uint8_t>(deathType);
    if (deathIndex >= static_cast<uint8_t>(DeathType::COUNT)) return false;
    if ((rule.deathTypeMask & (uint32_t{1} << deathIndex)) == 0) return false;
    if ((rule.veterancyMask & objectVeterancyBit(veterancy)) == 0) return false;
    if ((statuses & rule.exemptStatuses) != 0) return false;
    return (statuses & rule.requiredStatuses) == rule.requiredStatuses;
}

bool objectFxListDieUpgradeTriggersSatisfied(
    const ObjectFxListDieParameters& parameters,
    const engine::UpgradeMask& completedUpgrades,
    const engine::UpgradeCatalog* catalog) noexcept
{
    return objectFxListDieUpgradeTriggersSatisfied(
        parameters, completedUpgrades, {}, catalog);
}

bool objectFxListDieUpgradeTriggersSatisfied(
    const ObjectFxListDieParameters& parameters,
    const engine::UpgradeMask& playerCompletedUpgrades,
    const engine::UpgradeMask& objectCompletedUpgrades,
    const engine::UpgradeCatalog* catalog) noexcept
{
    static_cast<void>(catalog);
    if (!parameters.upgradeMasksCompiled ||
        parameters.triggeredByMask.none())
        return false;
    const engine::UpgradeMask completed =
        playerCompletedUpgrades | objectCompletedUpgrades;
    return parameters.requiresAllTriggers
        ? completed.test_for_all(parameters.triggeredByMask)
        : completed.test_for_any(parameters.triggeredByMask);
}

bool objectFxListDieHasUpgradeConflict(
    const ObjectFxListDieParameters& parameters,
    const engine::UpgradeMask& completedUpgrades,
    const engine::UpgradeCatalog* catalog) noexcept
{
    return objectFxListDieHasUpgradeConflict(
        parameters, completedUpgrades, {}, catalog);
}

bool objectFxListDieHasUpgradeConflict(
    const ObjectFxListDieParameters& parameters,
    const engine::UpgradeMask& playerCompletedUpgrades,
    const engine::UpgradeMask& objectCompletedUpgrades,
    const engine::UpgradeCatalog* catalog) noexcept
{
    static_cast<void>(catalog);
    return parameters.upgradeMasksCompiled &&
        (playerCompletedUpgrades | objectCompletedUpgrades)
            .test_for_any(parameters.conflictsWithMask);
}

bool isObjectOnDieBehaviorApplicable(
    const ObjectOnDieBehaviorEntry& entry, DeathType deathType,
    ObjectVeterancyLevel veterancy, ObjectStatusMask statuses) noexcept {
    if (!entry.filtersFullyResolved) return false;
    const uint8_t deathIndex = static_cast<uint8_t>(deathType);
    if (deathIndex >= static_cast<uint8_t>(DeathType::COUNT)) return false;
    if ((entry.deathTypeMask & (uint32_t{1} << deathIndex)) == 0)
        return false;
    if ((entry.veterancyMask & objectVeterancyBit(veterancy)) == 0)
        return false;
    if ((statuses & entry.exemptStatuses) != 0) return false;
    return (statuses & entry.requiredStatuses) == entry.requiredStatuses;
}

} // namespace game
