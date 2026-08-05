#pragma once

#include "core/container/container_types.h"

#include "math/fixed/q32_32.h"

#include <cstdint>

namespace engine {

struct AISideInfoRule final {
    container::String side;
    container::Array<uint32_t, 3> resourceGatherers{2, 3, 4};
    container::String baseDefenseStructure;
    container::Array<container::Vector<container::String>, 5> skillSets;
};

struct AISkirmishBuildStructureRule final {
    container::String objectType;
    math::q32_32 x{};
    math::q32_32 y{};
    math::q32_32 yawRadians{};
    int32_t rebuilds = 0;
    bool initiallyBuilt = false;
    bool automaticallyBuild = false;
};

struct AISkirmishBuildListRule final {
    container::String side;
    container::Vector<AISkirmishBuildStructureRule> structures;
};

// Immutable projection of the object-AI fields authored by AIData.ini.
// Durations remain milliseconds until GameLogic knows the match's confirmed
// tick rate; spatial values are compiled to Q32.32 at the content boundary.
struct AISimulationRules final {
    using Scalar = math::q32_32;

    Scalar guardInnerModifierAI{};
    Scalar guardOuterModifierAI{};
    Scalar guardInnerModifierHuman{};
    Scalar guardOuterModifierHuman{};
    // TAiData constructs this as one 30 Hz logic frame. 33 authored
    // milliseconds converts back to one frame with the legacy ceil rule.
    uint32_t forceIdleMilliseconds = 33;
    uint32_t guardChaseDurationMilliseconds = 0;
    uint32_t guardEnemyScanMilliseconds = 500;
    uint32_t guardEnemyReturnScanMilliseconds = 1000;
    Scalar maximumRetaliationDistance{210.0};
    Scalar retaliationFriendsRadius{120.0};
    Scalar alertRangeModifier{};
    Scalar aggressiveRangeModifier{};
    Scalar attackPriorityDistanceModifier{};
    // AIData.MaxRecruitRadius bounds BUILD_TEAM's recruit-before-produce
    // pass. Explicit RECRUIT_TEAM keeps its authored action radius.
    Scalar maximumRecruitDistance{};
    // How far a unit continues past its vision range when fleeing a repulsor.
    Scalar repulsedDistance{};
    // AIData.AttackUsesLineOfSight. TAiData's global switch that enables the
    // KINDOF_ATTACK_NEEDS_LINE_OF_SIGHT obstruction test inside
    // Pathfinder::isAttackViewBlockedByObstacle. Shipped ZH authors Yes; the
    // default keeps a mod that omits the field on the authored behaviour.
    bool attackUsesLineOfSight = true;
    Scalar structureSeconds{};
    Scalar teamSeconds{10};
    Scalar wealthy{7000};
    Scalar poor{2000};
    Scalar structuresWealthyRate{2};
    Scalar structuresPoorRate{0.6};
    Scalar teamsWealthyRate{2};
    Scalar teamsPoorRate{0.6};
    Scalar teamResourcesToStart{0.1};
    Scalar skirmishBaseDefenseExtraDistance{150};
    Scalar wallHeight{43};
    Scalar skirmishGroupFudgeDistance{5};
    Scalar minimumDistanceForGroup{100};
    Scalar distanceRequiresGroup{500};
    Scalar supplyCenterSafeRadius{300};
    Scalar rebuildDelayTimeSeconds{30};
    Scalar aiDozerBoredRadiusModifier{2};
    uint32_t minimumInfantryForGroup = 3;
    uint32_t minimumVehiclesForGroup = 3;
    uint32_t infantryPathfindDiameter = 6;
    uint32_t vehiclePathfindDiameter = 6;
    bool forceSkirmishAI = false;
    bool rotateSkirmishBases = false;
    bool enableRepulsors = true;
    bool attackIgnoreInsignificantBuildings = true;
    bool aiCrushesInfantry = true;
    container::Vector<AISideInfoRule> sides;
    container::Vector<AISkirmishBuildListRule> skirmishBuildLists;

    [[nodiscard]] const AISideInfoRule* sideInfo(
        container::StringView side) const noexcept;
    [[nodiscard]] const AISkirmishBuildListRule* skirmishBuildList(
        container::StringView side) const noexcept;

    [[nodiscard]] Scalar guardInnerModifier(bool humanControlled) const noexcept {
        return humanControlled ? guardInnerModifierHuman : guardInnerModifierAI;
    }
    [[nodiscard]] Scalar guardOuterModifier(bool humanControlled) const noexcept {
        return humanControlled ? guardOuterModifierHuman : guardOuterModifierAI;
    }

    void canonicalize() noexcept;

    [[nodiscard]] bool applyLegacyAIDataOverrides(
        container::StringView content, container::StringView sourceName,
        container::String* error = nullptr);

    // Applies the winning VFS instance and uses last-value-wins within that
    // file, matching the legacy AIData block parser without retaining its
    // global mutable TAiData singleton.
    [[nodiscard]] static bool loadFromLegacyAIData(
        container::StringView path, AISimulationRules& rules,
        container::String* error = nullptr);

    // Same winning-VFS-instance/last-value-wins rules, but layered on top of
    // an already-compiled result instead of restarting from the C++ defaults.
    // RefCode's GameEngine loads `Data\INI\Default\AIData` and then
    // `Data\INI\AIData`; those are two distinct logical files, so the second
    // one must override per key rather than reset the first.
    [[nodiscard]] static bool applyLegacyAIDataFile(
        container::StringView path, AISimulationRules& rules,
        container::String* error = nullptr);
};

} // namespace engine
