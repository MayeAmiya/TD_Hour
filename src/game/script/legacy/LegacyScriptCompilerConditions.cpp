#include "LegacyScriptCompilerInternal.h"
#include "game/object/contracts/ObjectDeathReaction.h"

#include <optional>
#include <utility>

namespace engine::script::legacy::detail
{

[[nodiscard]] std::optional<ScriptCondition> compileCondition(const LegacyScriptInstructionSource& instruction,
                                                              CompileContext& context,
                                                              container::StringView scriptName)
{
    const container::String name = instructionName(instruction, false, context, scriptName);
    if (name == "CONDITION_TRUE")
        return ScriptAlwaysTrueCondition{};
    if (name == "CONDITION_FALSE")
        return ScriptAlwaysFalseCondition{};
    if (name == "CAMERA_MOVEMENT_FINISHED")
        return ScriptCameraMovementFinishedCondition{};
    if (name == "HAS_FINISHED_VIDEO" || name == "HAS_FINISHED_SPEECH" ||
        name == "HAS_FINISHED_AUDIO")
    {
        const auto mediaName = textParameter(instruction, 0, context, scriptName, name);
        if (!mediaName) return std::nullopt;
        ScriptPresentationCompletionKind kind = ScriptPresentationCompletionKind::Video;
        if (name == "HAS_FINISHED_SPEECH") {
            kind = ScriptPresentationCompletionKind::Speech;
        } else if (name == "HAS_FINISHED_AUDIO") {
            kind = ScriptPresentationCompletionKind::Audio;
        }
        return ScriptPresentationCompletionCondition{
            .kind = kind,
            .mediaName = *mediaName,
        };
    }
    if (name == "MUSIC_TRACK_HAS_COMPLETED")
    {
        const auto trackName = textParameter(instruction, 0, context, scriptName, name);
        const auto minimumLoops = integerParameter(instruction, 1, context, scriptName, name);
        if (!trackName || !minimumLoops) return std::nullopt;
        return ScriptMusicTrackCompletedCondition{
            .trackName = *trackName,
            .minimumCompletedLoops = *minimumLoops,
        };
    }
    if (name == "COUNTER")
    {
        const auto counter = textParameter(instruction, 0, context, scriptName, name);
        const auto comparison = comparisonParameter(instruction, 1, context, scriptName, name);
        const auto value = integerParameter(instruction, 2, context, scriptName, name);
        if (!counter || !comparison || !value)
            return std::nullopt;
        return ScriptCounterCondition{.counter = *counter, .comparison = *comparison, .value = *value};
    }
    if (name == "FLAG")
    {
        const auto flag = textParameter(instruction, 0, context, scriptName, name);
        const auto value = integerParameter(instruction, 1, context, scriptName, name);
        if (!flag || !value)
            return std::nullopt;
        return ScriptFlagCondition{.flag = *flag, .expectedValue = *value != 0};
    }
    if (name == "TIMER_EXPIRED")
    {
        const auto timer = textParameter(instruction, 0, context, scriptName, name);
        if (!timer)
            return std::nullopt;
        return ScriptTimerExpiredCondition{.timer = *timer};
    }
    if (name == "NAMED_DESTROYED" || name == "NAMED_NOT_DESTROYED" ||
        name == "NAMED_CREATED" || name == "NAMED_DYING" ||
        name == "NAMED_TOTALLY_DEAD")
    {
        const auto objectName = textParameter(instruction, 0, context, scriptName, name);
        if (!objectName ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *objectName, instruction.serialized))
            return std::nullopt;
        ScriptNamedObjectExpectation expectation = ScriptNamedObjectExpectation::Alive;
        if (name == "NAMED_DESTROYED")
            expectation = ScriptNamedObjectExpectation::Destroyed;
        else if (name == "NAMED_CREATED")
            expectation = ScriptNamedObjectExpectation::Present;
        else if (name == "NAMED_DYING")
            expectation = ScriptNamedObjectExpectation::Dying;
        else if (name == "NAMED_TOTALLY_DEAD")
            expectation = ScriptNamedObjectExpectation::TotallyDead;
        return ScriptNamedObjectStateCondition{
            .objectName = *objectName,
            .expected = expectation,
        };
    }
    if (name == "UNIT_HEALTH")
    {
        const auto objectName = textParameter(instruction, 0, context, scriptName, name);
        const auto comparison = comparisonParameter(instruction, 1, context, scriptName, name);
        const auto percent = integerParameter(instruction, 2, context, scriptName, name);
        if (!objectName || !comparison || !percent ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *objectName,
                                               instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptUnitHealthCondition{
            .objectName = *objectName,
            .comparison = *comparison,
            .percent = *percent,
        };
    }
    if (name == "UNIT_HAS_OBJECT_STATUS" || name == "TEAM_ALL_HAS_OBJECT_STATUS" ||
        name == "TEAM_SOME_HAVE_OBJECT_STATUS")
    {
        const auto targetName = textParameter(instruction, 0, context, scriptName, name);
        const auto statusName = textParameter(instruction, 1, context, scriptName, name);
        if (!targetName || !statusName ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *targetName,
                                               instruction.serialized)) {
            return std::nullopt;
        }
        const game::ObjectStatusMaskParseResult parsed = game::parseObjectStatusMask(*statusName);
        if (!parsed.resolved) {
            context.diagnostic(
                LegacyScriptCompileDiagnosticSeverity::Warning,
                "script '" + container::String(scriptName) + "' " + name +
                    " has an unresolved OBJECT_STATUS parameter '" + *statusName + "'",
                instruction.serialized);
            return std::nullopt;
        }
        return ScriptObjectStatusCondition{
            .targetName = *targetName,
            .statusMask = parsed.mask,
            .team = name != "UNIT_HAS_OBJECT_STATUS",
            .entireTeam = name == "TEAM_ALL_HAS_OBJECT_STATUS",
        };
    }
    if (name == "NAMED_BUILDING_IS_EMPTY" || name == "NAMED_HAS_FREE_CONTAINER_SLOTS")
    {
        const auto objectName = textParameter(instruction, 0, context, scriptName, name);
        if (!objectName ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *objectName,
                                               instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptNamedContainmentCondition{
            .objectName = *objectName,
            .expected = name == "NAMED_BUILDING_IS_EMPTY"
                ? ScriptNamedContainmentExpectation::Empty
                : ScriptNamedContainmentExpectation::HasFreeSlots,
        };
    }
    if (name == "SKIRMISH_SPECIAL_POWER_READY")
    {
        // RefCode heals the oldest one-parameter chunk form by inserting
        // SIDE=ThisPlayer and shifting SPECIAL_POWER to parameter 1.
        const bool legacyOneParameter = instruction.parameters.size() == 1;
        const std::optional<container::String> player = legacyOneParameter
            ? std::optional<container::String>{"ThisPlayer"}
            : textParameter(instruction, 0, context, scriptName, name);
        const auto specialPower = textParameter(
            instruction, legacyOneParameter ? 0u : 1u, context, scriptName, name);
        if (!player || !specialPower ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerSpecialPowerReadyCondition{
            .player = *player,
            .specialPower = *specialPower,
        };
    }
    if (name == "PLAYER_TRIGGERED_SPECIAL_POWER" ||
        name == "PLAYER_MIDWAY_SPECIAL_POWER" ||
        name == "PLAYER_COMPLETED_SPECIAL_POWER" ||
        name == "PLAYER_TRIGGERED_SPECIAL_POWER_FROM_NAMED" ||
        name == "PLAYER_MIDWAY_SPECIAL_POWER_FROM_NAMED" ||
        name == "PLAYER_COMPLETED_SPECIAL_POWER_FROM_NAMED")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto specialPower = textParameter(instruction, 1, context, scriptName, name);
        const bool fromNamed = name.ends_with("_FROM_NAMED");
        std::optional<container::String> sourceObject;
        if (fromNamed) {
            sourceObject = textParameter(instruction, 2, context, scriptName, name);
        }
        if (!player || !specialPower || (fromNamed && !sourceObject) ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized) ||
            (sourceObject && rejectDynamicScriptContextSelector(
                context, scriptName, name, *sourceObject, instruction.serialized))) {
            return std::nullopt;
        }
        return ScriptSpecialPowerEventCondition{
            .player = *player,
            .specialPower = *specialPower,
            .sourceObject = sourceObject.value_or(container::String{}),
            .phase = name.starts_with("PLAYER_TRIGGERED_")
                ? ScriptSpecialPowerEventPhase::Triggered
                : name.starts_with("PLAYER_MIDWAY_")
                    ? ScriptSpecialPowerEventPhase::Midway
                    : ScriptSpecialPowerEventPhase::Completed,
        };
    }
    if (name == "PLAYER_BUILT_UPGRADE" ||
        name == "PLAYER_BUILT_UPGRADE_FROM_NAMED")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto upgrade = textParameter(instruction, 1, context, scriptName, name);
        const bool fromNamed = name == "PLAYER_BUILT_UPGRADE_FROM_NAMED";
        std::optional<container::String> sourceObject;
        if (fromNamed) {
            sourceObject = textParameter(instruction, 2, context, scriptName, name);
        }
        if (!player || !upgrade || (fromNamed && !sourceObject) ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized) ||
            (sourceObject && rejectDynamicScriptContextSelector(
                context, scriptName, name, *sourceObject, instruction.serialized))) {
            return std::nullopt;
        }
        return ScriptUpgradeEventCondition{
            .player = *player,
            .upgrade = *upgrade,
            .sourceObject = sourceObject.value_or(container::String{}),
        };
    }
    if (name == "SKIRMISH_PLAYER_HAS_COMPARISON_GARRISONED")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto comparison = comparisonParameter(instruction, 1, context, scriptName, name);
        const auto count = integerParameter(instruction, 2, context, scriptName, name);
        if (!player || !comparison || !count ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerGarrisonedCountCondition{
            .player = *player,
            .comparison = *comparison,
            .count = *count,
        };
    }
    if (name == "SKIRMISH_PLAYER_HAS_COMPARISON_CAPTURED_UNITS")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto comparison = comparisonParameter(instruction, 1, context, scriptName, name);
        const auto count = integerParameter(instruction, 2, context, scriptName, name);
        if (!player || !comparison || !count ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerCapturedUnitCountCondition{
            .player = *player,
            .comparison = *comparison,
            .count = *count,
        };
    }
    if (name == "SKIRMISH_PLAYER_HAS_PREREQUISITE_TO_BUILD")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto objectType = textParameter(instruction, 1, context, scriptName, name);
        if (!player || !objectType ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerCanBuildObjectCondition{
            .player = *player,
            .objectType = *objectType,
        };
    }
    if (name == "SUPPLY_SOURCE_SAFE")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto minimumSupplies = integerParameter(
            instruction, 1, context, scriptName, name);
        if (!player || !minimumSupplies ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptSupplySourceSafeCondition{
            .player = *player,
            .minimumSupplies = *minimumSupplies,
        };
    }
    if (name == "SUPPLY_SOURCE_ATTACKED")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        if (!player || rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptSupplySourceAttackedCondition{.player = *player};
    }
    if (name == "SKIRMISH_SUPPLIES_VALUE_WITHIN_DISTANCE")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto extraDistance = realParameter(instruction, 1, context, scriptName, name);
        const auto areaName = textParameter(instruction, 2, context, scriptName, name);
        const auto minimumValue = realParameter(instruction, 3, context, scriptName, name);
        if (!player || !extraDistance || !areaName || !minimumValue ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptSuppliesWithinDistanceCondition{
            .player = *player,
            .areaName = *areaName,
            .extraDistance = math::q32_32{*extraDistance},
            .minimumValue = math::q32_32{*minimumValue},
        };
    }
    if (name == "NAMED_DISCOVERED" || name == "TEAM_DISCOVERED" ||
        name == "SKIRMISH_PLAYER_HAS_DISCOVERED_PLAYER")
    {
        const auto subject = textParameter(instruction, 0, context, scriptName, name);
        const auto observer = textParameter(instruction, 1, context, scriptName, name);
        if (!subject || !observer ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *observer,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        if (name != "SKIRMISH_PLAYER_HAS_DISCOVERED_PLAYER" &&
            rejectDynamicScriptContextSelector(context, scriptName, name, *subject,
                                               instruction.serialized)) {
            return std::nullopt;
        }
        if (name == "SKIRMISH_PLAYER_HAS_DISCOVERED_PLAYER" &&
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *subject,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        ScriptDiscoverySubjectKind kind = ScriptDiscoverySubjectKind::NamedObject;
        if (name == "TEAM_DISCOVERED") kind = ScriptDiscoverySubjectKind::Team;
        else if (name == "SKIRMISH_PLAYER_HAS_DISCOVERED_PLAYER")
            kind = ScriptDiscoverySubjectKind::Player;
        return ScriptDiscoveryCondition{
            .subject = *subject,
            .observer = *observer,
            .kind = kind,
        };
    }
    if (name == "ENEMY_SIGHTED")
    {
        const auto sourceObject = textParameter(instruction, 0, context, scriptName, name);
        const auto relation = integerParameter(instruction, 1, context, scriptName, name);
        const auto targetPlayer = textParameter(instruction, 2, context, scriptName, name);
        if (!sourceObject || !relation || !targetPlayer || *relation < 0 || *relation > 2 ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *sourceObject,
                                               instruction.serialized) ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *targetPlayer,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        ScriptSightRelationship relationship = ScriptSightRelationship::Enemies;
        if (*relation == 1) relationship = ScriptSightRelationship::Neutral;
        else if (*relation == 2) relationship = ScriptSightRelationship::Allies;
        return ScriptSightedRelationshipCondition{
            .sourceObject = *sourceObject,
            .targetPlayer = *targetPlayer,
            .relationship = relationship,
        };
    }
    if (name == "TYPE_SIGHTED")
    {
        const auto sourceObject = textParameter(instruction, 0, context, scriptName, name);
        const auto objectType = textParameter(instruction, 1, context, scriptName, name);
        const auto targetPlayer = textParameter(instruction, 2, context, scriptName, name);
        if (!sourceObject || !objectType || !targetPlayer ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *sourceObject,
                                               instruction.serialized) ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *targetPlayer,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptSightedObjectTypeCondition{
            .sourceObject = *sourceObject,
            .targetPlayer = *targetPlayer,
            .objectType = *objectType,
        };
    }
    if (name == "NAMED_ATTACKED_BY_OBJECTTYPE" ||
        name == "TEAM_ATTACKED_BY_OBJECTTYPE" ||
        name == "NAMED_ATTACKED_BY_PLAYER" ||
        name == "TEAM_ATTACKED_BY_PLAYER")
    {
        const auto target = textParameter(instruction, 0, context, scriptName, name);
        const auto matcher = textParameter(instruction, 1, context, scriptName, name);
        const bool byPlayer = name.ends_with("_BY_PLAYER");
        if (!target || !matcher ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *target,
                                               instruction.serialized) ||
            (byPlayer && rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *matcher, instruction.serialized))) {
            return std::nullopt;
        }
        return ScriptAttackedCondition{
            .target = *target,
            .matcher = *matcher,
            .matcherKind = byPlayer
                ? ScriptAttackedMatcherKind::Player
                : ScriptAttackedMatcherKind::ObjectType,
            .team = name.starts_with("TEAM_"),
        };
    }
    if (name == "SKIRMISH_PLAYER_HAS_BEEN_ATTACKED_BY_PLAYER")
    {
        const auto victim = textParameter(instruction, 0, context, scriptName, name);
        const auto attacker = textParameter(instruction, 1, context, scriptName, name);
        if (!victim || !attacker ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *victim,
                                                   instruction.serialized) ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *attacker,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerAttackedByPlayerCondition{
            .victimPlayer = *victim,
            .attackerPlayer = *attacker,
        };
    }
    if (name == "BRIDGE_REPAIRED" || name == "BRIDGE_BROKEN")
    {
        const auto bridge = textParameter(instruction, 0, context, scriptName, name);
        if (!bridge || rejectDynamicScriptContextSelector(
                context, scriptName, name, *bridge, instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptBridgeTransitionCondition{
            .bridgeObject = *bridge,
            .broken = name == "BRIDGE_BROKEN",
        };
    }
    if (name == "UNIT_EMPTIED")
    {
        const auto objectName = textParameter(instruction, 0, context, scriptName, name);
        if (!objectName || rejectDynamicScriptContextSelector(
                context, scriptName, name, *objectName, instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptUnitEmptiedCondition{.objectName = *objectName};
    }
    if (name == "BUILDING_ENTERED_BY_PLAYER")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto building = textParameter(instruction, 1, context, scriptName, name);
        if (!player || !building ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized) ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *building,
                                               instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptBuildingEnteredCondition{
            .player = *player,
            .buildingObject = *building,
        };
    }
    if (name == "NAMED_ENTERED_AREA" || name == "NAMED_EXITED_AREA" ||
        name == "TEAM_ENTERED_AREA_ENTIRELY" ||
        name == "TEAM_ENTERED_AREA_PARTIALLY" ||
        name == "TEAM_EXITED_AREA_ENTIRELY" ||
        name == "TEAM_EXITED_AREA_PARTIALLY")
    {
        const auto target = textParameter(instruction, 0, context, scriptName, name);
        const auto areaName = textParameter(instruction, 1, context, scriptName, name);
        const bool team = name.starts_with("TEAM_");
        const std::optional<uint8_t> surfaces = team
            ? teamAreaSurfacesParameter(instruction, context, scriptName, name)
            : std::optional<uint8_t>{3};
        if (!target || !areaName || !surfaces ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *target,
                                               instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptAreaTransitionCondition{
            .target = *target,
            .areaName = *areaName,
            .kind = name.find("EXITED") != container::String::npos
                ? ScriptAreaTransitionKind::Exited
                : ScriptAreaTransitionKind::Entered,
            .allowedSurfaces = *surfaces,
            .team = team,
            .entireTeam = name.find("ENTIRELY") != container::String::npos,
        };
    }
    if (name == "PLAYER_HAS_CREDITS")
    {
        const auto value = integerParameter(instruction, 0, context, scriptName, name);
        const auto comparison = comparisonParameter(instruction, 1, context, scriptName, name);
        const auto player = textParameter(instruction, 2, context, scriptName, name);
        if (!value || !comparison || !player ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized))
            return std::nullopt;
        return ScriptPlayerCashCondition{.player = *player, .comparison = *comparison, .value = *value};
    }
    if (name == "PLAYER_HAS_POWER" || name == "PLAYER_HAS_NO_POWER")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        if (!player ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerPowerCondition{
            .player = *player,
            .kind = name == "PLAYER_HAS_POWER"
                ? ScriptPlayerPowerConditionKind::HasSufficientPower
                : ScriptPlayerPowerConditionKind::HasInsufficientPower,
        };
    }
    if (name == "PLAYER_POWER_COMPARE_PERCENT" ||
        name == "PLAYER_EXCESS_POWER_COMPARE_VALUE")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto comparison = comparisonParameter(instruction, 1, context, scriptName, name);
        const auto value = integerParameter(instruction, 2, context, scriptName, name);
        if (!player || !comparison || !value ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerPowerCondition{
            .player = *player,
            .kind = name == "PLAYER_POWER_COMPARE_PERCENT"
                ? ScriptPlayerPowerConditionKind::SupplyPercent
                : ScriptPlayerPowerConditionKind::ExcessValue,
            .comparison = *comparison,
            .value = *value,
        };
    }
    if (name == "PLAYER_HAS_SCIENCEPURCHASEPOINTS")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto minimumPoints = integerParameter(instruction, 1, context, scriptName, name);
        if (!player || !minimumPoints ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized))
            return std::nullopt;
        return ScriptPlayerSciencePurchasePointsCondition{
            .player = *player,
            .minimumPoints = *minimumPoints,
        };
    }
    if (name == "PLAYER_ACQUIRED_SCIENCE" || name == "PLAYER_CAN_PURCHASE_SCIENCE")
    {
        // RefCode uses the exact same SIDE, SCIENCE parameter order for the
        // two conditions.  Keep the science identity as authored text: its
        // validation belongs to the session's frozen ScienceCatalog, because
        // a ScriptProgram is intentionally reusable across loaded content.
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto science = textParameter(instruction, 1, context, scriptName, name);
        if (!player || !science ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        if (name == "PLAYER_ACQUIRED_SCIENCE") {
            return ScriptPlayerScienceAcquiredCondition{
                .player = *player,
                .science = *science,
            };
        }
        return ScriptPlayerCanPurchaseScienceCondition{
            .player = *player,
            .science = *science,
        };
    }
    if (name == "PLAYER_ALL_DESTROYED")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        if (!player ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerAllDestroyedCondition{.player = *player};
    }
    if (name == "PLAYER_ALL_BUILDFACILITIES_DESTROYED")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        if (!player || rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerAllBuildFacilitiesDestroyedCondition{
            .player = *player,
        };
    }
    if (name == "BUILT_BY_PLAYER")
    {
        const auto objectType = textParameter(instruction, 0, context, scriptName, name);
        const auto player = textParameter(instruction, 1, context, scriptName, name);
        if (!objectType || !player || rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerObjectTypeCountCondition{
            .player = *player,
            .objectType = *objectType,
            .kind = ScriptPlayerObjectTypeCountKind::BuiltByPlayer,
        };
    }
    if (name == "PLAYER_HAS_OBJECT_COMPARISON")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto comparison = comparisonParameter(
            instruction, 1, context, scriptName, name);
        const auto value = integerParameter(instruction, 2, context, scriptName, name);
        const auto objectType = textParameter(instruction, 3, context, scriptName, name);
        if (!player || !comparison || !value || !objectType ||
            rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerObjectTypeCountCondition{
            .player = *player,
            .objectType = *objectType,
            .kind = ScriptPlayerObjectTypeCountKind::CurrentComparison,
            .comparison = *comparison,
            .value = *value,
        };
    }
    if (name == "PLAYER_LOST_OBJECT_TYPE")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto objectType = textParameter(instruction, 1, context, scriptName, name);
        if (!player || !objectType || rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerObjectTypeCountCondition{
            .player = *player,
            .objectType = *objectType,
            .kind = ScriptPlayerObjectTypeCountKind::LostSincePreviousEvaluation,
        };
    }
    if (name == "PLAYER_HAS_N_OR_FEWER_BUILDINGS" ||
        name == "PLAYER_HAS_N_OR_FEWER_FACTION_BUILDINGS")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto maximum = integerParameter(instruction, 1, context, scriptName, name);
        if (!player || !maximum ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerBuildingCountCondition{
            .player = *player,
            .maximumCount = *maximum,
            .kind = name == "PLAYER_HAS_N_OR_FEWER_FACTION_BUILDINGS"
                ? ScriptPlayerBuildingCountKind::VictoryStructures
                : ScriptPlayerBuildingCountKind::AllStructures,
        };
    }
    if (name == "START_POSITION_IS")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto authoredPosition = integerParameter(instruction, 1, context, scriptName, name);
        if (!player || !authoredPosition ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerStartPositionCondition{
            .player = *player,
            .authoredPosition = *authoredPosition,
        };
    }
    if (name == "SKIRMISH_PLAYER_FACTION")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto faction = textParameter(instruction, 1, context, scriptName, name);
        if (!player || !faction ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerFactionCondition{.player = *player, .faction = *faction};
    }
    if (name == "SKIRMISH_NAMED_AREA_EXIST")
    {
        // RefCode declares a SIDE parameter but deliberately ignores it;
        // retain source-shape validation without making an irrelevant player
        // alias a dependency of the trigger lookup.
        const auto ignoredPlayer = textParameter(instruction, 0, context, scriptName, name);
        const auto areaName = textParameter(instruction, 1, context, scriptName, name);
        if (!ignoredPlayer || !areaName)
            return std::nullopt;
        return ScriptTriggerAreaExistsCondition{.areaName = *areaName};
    }
    if (name == "PLAYER_HAS_COMPARISON_UNIT_KIND_IN_TRIGGER_AREA")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto comparison = comparisonParameter(instruction, 1, context, scriptName, name);
        const auto count = integerParameter(instruction, 2, context, scriptName, name);
        // KIND_OF_PARAM writes both its ordinal and canonical KindOf name.
        // Retain the name so the modern ECS never depends on the old enum layout.
        const auto requiredKind = textParameter(instruction, 3, context, scriptName, name);
        const auto areaName = textParameter(instruction, 4, context, scriptName, name);
        if (!player || !comparison || !count || !requiredKind || !areaName ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerAreaCondition{
            .player = *player,
            .areaName = *areaName,
            .requiredKind = *requiredKind,
            .kind = ScriptPlayerAreaConditionKind::MatchingKindCount,
            .comparison = *comparison,
            .value = *count,
        };
    }
    if (name == "SKIRMISH_VALUE_IN_AREA")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto comparison = comparisonParameter(instruction, 1, context, scriptName, name);
        const auto value = integerParameter(instruction, 2, context, scriptName, name);
        const auto areaName = textParameter(instruction, 3, context, scriptName, name);
        if (!player || !comparison || !value || !areaName ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerAreaCondition{
            .player = *player,
            .areaName = *areaName,
            .kind = ScriptPlayerAreaConditionKind::BuildValue,
            .comparison = *comparison,
            .value = *value,
        };
    }
    if (name == "SKIRMISH_PLAYER_HAS_UNITS_IN_AREA" ||
        name == "SKIRMISH_PLAYER_IS_OUTSIDE_AREA")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto areaName = textParameter(instruction, 1, context, scriptName, name);
        if (!player || !areaName ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerAreaCondition{
            .player = *player,
            .areaName = *areaName,
            .kind = name == "SKIRMISH_PLAYER_HAS_UNITS_IN_AREA"
                ? ScriptPlayerAreaConditionKind::HasEligibleObjects
                : ScriptPlayerAreaConditionKind::HasNoEligibleObjects,
        };
    }
    if (name == "PLAYER_HAS_COMPARISON_UNIT_TYPE_IN_TRIGGER_AREA")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto comparison = comparisonParameter(
            instruction, 1, context, scriptName, name);
        const auto value = integerParameter(instruction, 2, context, scriptName, name);
        const auto objectType = textParameter(instruction, 3, context, scriptName, name);
        const auto areaName = textParameter(instruction, 4, context, scriptName, name);
        if (!player || !comparison || !value || !objectType || !areaName ||
            rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptPlayerObjectTypeAreaCountCondition{
            .player = *player,
            .objectType = *objectType,
            .areaName = *areaName,
            .comparison = *comparison,
            .value = *value,
        };
    }
    if (name == "SKIRMISH_TECH_BUILDING_WITHIN_DISTANCE")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto extraDistance = realParameter(instruction, 1, context, scriptName, name);
        const auto areaName = textParameter(instruction, 2, context, scriptName, name);
        if (!player || !extraDistance || !areaName ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptTechBuildingWithinDistanceCondition{
            .player = *player,
            .areaName = *areaName,
            .extraDistance = math::q32_32{*extraDistance},
        };
    }
    if (name == "SKIRMISH_UNOWNED_FACTION_UNIT_EXISTS")
    {
        // RefCode declares SIDE but ignores it and always scans Neutral.
        const auto ignoredPlayer = textParameter(instruction, 0, context, scriptName, name);
        const auto comparison = comparisonParameter(instruction, 1, context, scriptName, name);
        const auto count = integerParameter(instruction, 2, context, scriptName, name);
        if (!ignoredPlayer || !comparison || !count)
            return std::nullopt;
        return ScriptNeutralUnmannedCountCondition{
            .comparison = *comparison,
            .value = *count,
        };
    }
    if (name == "PLAYER_DESTROYED_N_BUILDINGS_PLAYER")
    {
        // This predicate is an explicit RefCode stub: it validates its
        // player/count/opponent wire shape, then always returns FALSE. Keep
        // that useful compatibility behavior instead of needlessly blocking
        // a whole map script while waiting for a feature the original never
        // implemented either.
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto count = integerParameter(instruction, 1, context, scriptName, name);
        const auto opponent = textParameter(instruction, 2, context, scriptName, name);
        if (!player || !count || !opponent)
            return std::nullopt;
        return ScriptAlwaysFalseCondition{};
    }
    if (name == "MISSION_ATTEMPTS")
    {
        // RefCode carries all three authored parameters but its evaluator is
        // an unconditional false stub. Validate the wire shape so malformed
        // content still reports a diagnostic, then preserve that exact false
        // result without inventing campaign-profile state.
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto comparison = comparisonParameter(instruction, 1, context, scriptName, name);
        const auto attempts = integerParameter(instruction, 2, context, scriptName, name);
        if (!player || !comparison || !attempts)
            return std::nullopt;
        return ScriptAlwaysFalseCondition{};
    }
    if (name == "UNIT_COMPLETED_SEQUENTIAL_EXECUTION" ||
        name == "TEAM_COMPLETED_SEQUENTIAL_EXECUTION")
    {
        // RefCode declares these predicates and even exposes ScriptEngine
        // helpers for them, but both helpers return FALSE unconditionally.
        // Keep the authored wire shape useful while preserving that result.
        const auto subject = textParameter(instruction, 0, context, scriptName, name);
        const auto sequentialScript =
            textParameter(instruction, 1, context, scriptName, name);
        if (!subject || !sequentialScript)
            return std::nullopt;
        context.diagnostic(
            LegacyScriptCompileDiagnosticSeverity::Info,
            "script '" + container::String(scriptName) + "' uses legacy condition '" +
                name + "', which is permanently false",
            instruction.serialized);
        return ScriptAlwaysFalseCondition{};
    }
    if (name == "NAMED_SELECTED")
    {
        const auto objectName = textParameter(
            instruction, 0, context, scriptName, name);
        if (!objectName || rejectDynamicScriptContextSelector(
                context, scriptName, name, *objectName,
                instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptNamedSelectedCondition{.objectName = *objectName};
    }
    if (name == "MULTIPLAYER_ALLIED_VICTORY" ||
        name == "MULTIPLAYER_ALLIED_DEFEAT" ||
        name == "MULTIPLAYER_PLAYER_DEFEAT")
    {
        ScriptMultiplayerOutcomeKind kind =
            ScriptMultiplayerOutcomeKind::AlliedVictory;
        if (name == "MULTIPLAYER_ALLIED_DEFEAT") {
            kind = ScriptMultiplayerOutcomeKind::AlliedDefeat;
        } else if (name == "MULTIPLAYER_PLAYER_DEFEAT") {
            kind = ScriptMultiplayerOutcomeKind::PlayerDefeat;
        }
        return ScriptMultiplayerOutcomeCondition{.kind = kind};
    }
    if (name == "NAMED_REACHED_WAYPOINTS_END" ||
        name == "TEAM_REACHED_WAYPOINTS_END")
    {
        const auto target = textParameter(instruction, 0, context, scriptName, name);
        const auto pathName = textParameter(instruction, 1, context, scriptName, name);
        if (!target || !pathName || rejectDynamicScriptContextSelector(
                context, scriptName, name, *target, instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptWaypointPathCompletedCondition{
            .target = *target,
            .pathName = *pathName,
            .team = name == "TEAM_REACHED_WAYPOINTS_END",
        };
    }
    if (name == "SKIRMISH_COMMAND_BUTTON_READY_ALL" ||
        name == "SKIRMISH_COMMAND_BUTTON_READY_PARTIAL")
    {
        // RefCode intentionally ignores the SIDE operand in this condition.
        const auto ignoredPlayer = textParameter(
            instruction, 0, context, scriptName, name);
        const auto teamName = textParameter(
            instruction, 1, context, scriptName, name);
        const auto commandButton = textParameter(
            instruction, 2, context, scriptName, name);
        if (!ignoredPlayer || !teamName || !commandButton ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *teamName,
                instruction.serialized)) {
            return std::nullopt;
        }
        return ScriptTeamCommandButtonReadyCondition{
            .teamName = *teamName,
            .commandButton = *commandButton,
            .allReady = name == "SKIRMISH_COMMAND_BUTTON_READY_ALL",
        };
    }
    if (name == "NAMED_OWNED_BY_PLAYER")
    {
        const auto objectName = textParameter(instruction, 0, context, scriptName, name);
        const auto player = textParameter(instruction, 1, context, scriptName, name);
        if (!objectName || !player ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *objectName, instruction.serialized))
            return std::nullopt;
        if (rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized))
            return std::nullopt;
        return ScriptNamedObjectOwnerCondition{.objectName = *objectName, .player = *player};
    }
    if (name == "TEAM_OWNED_BY_PLAYER")
    {
        const auto teamName = textParameter(instruction, 0, context, scriptName, name);
        const auto player = textParameter(instruction, 1, context, scriptName, name);
        if (!teamName || !player ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *teamName, instruction.serialized))
            return std::nullopt;
        if (rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized))
            return std::nullopt;
        return ScriptTeamOwnerCondition{.teamName = *teamName, .player = *player};
    }
    if (name == "TEAM_STATE_IS" || name == "TEAM_STATE_IS_NOT")
    {
        const auto teamName = textParameter(
            instruction, 0, context, scriptName, name);
        const auto state = textParameter(
            instruction, 1, context, scriptName, name);
        if (!teamName || !state) return std::nullopt;
        return ScriptTeamCustomStateCondition{
            .team = teamSelector(*teamName),
            .state = *state,
            .negated = name == "TEAM_STATE_IS_NOT",
        };
    }
    if (name == "TEAM_HAS_UNITS" || name == "TEAM_DESTROYED" || name == "TEAM_CREATED")
    {
        const auto teamName = textParameter(instruction, 0, context, scriptName, name);
        if (!teamName ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *teamName, instruction.serialized))
            return std::nullopt;
        ScriptTeamStateExpectation expectation = ScriptTeamStateExpectation::HasUnits;
        if (name == "TEAM_DESTROYED")
            expectation = ScriptTeamStateExpectation::Destroyed;
        else if (name == "TEAM_CREATED")
            expectation = ScriptTeamStateExpectation::Created;
        return ScriptTeamStateCondition{.teamName = *teamName, .expected = expectation};
    }
    if (name == "NAMED_INSIDE_AREA" || name == "NAMED_OUTSIDE_AREA")
    {
        const auto objectName = textParameter(instruction, 0, context, scriptName, name);
        const auto areaName = textParameter(instruction, 1, context, scriptName, name);
        if (!objectName || !areaName ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *objectName, instruction.serialized))
            return std::nullopt;
        return ScriptNamedAreaCondition{
            .objectName = *objectName,
            .areaName = *areaName,
            .expected = name == "NAMED_INSIDE_AREA" ? ScriptAreaExpectation::Inside
                                                      : ScriptAreaExpectation::Outside,
        };
    }
    if (name == "TEAM_INSIDE_AREA_PARTIALLY" || name == "TEAM_INSIDE_AREA_ENTIRELY" ||
        name == "TEAM_OUTSIDE_AREA_ENTIRELY")
    {
        const auto teamName = textParameter(instruction, 0, context, scriptName, name);
        const auto areaName = textParameter(instruction, 1, context, scriptName, name);
        const auto surfaces = teamAreaSurfacesParameter(instruction, context, scriptName, name);
        if (!teamName || !areaName || !surfaces ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *teamName, instruction.serialized))
            return std::nullopt;
        ScriptTeamAreaExpectation expectation = ScriptTeamAreaExpectation::AnyInside;
        if (name == "TEAM_INSIDE_AREA_ENTIRELY")
            expectation = ScriptTeamAreaExpectation::EntirelyInside;
        else if (name == "TEAM_OUTSIDE_AREA_ENTIRELY")
            expectation = ScriptTeamAreaExpectation::EntirelyOutside;
        return ScriptTeamAreaCondition{
            .teamName = *teamName,
            .areaName = *areaName,
            .allowedSurfaces = *surfaces,
            .expected = expectation,
        };
    }
    if (name == "OBSOLETE_SCRIPT_1" || name == "OBSOLETE_SCRIPT_2" ||
        name == "DEFUNCT_PLAYER_SELECTED_GENERAL" ||
        name == "DEFUNCT_PLAYER_SELECTED_GENERAL_FROM_NAMED")
    {
        // RefCode either falls through its unknown-condition assertion
        // (the two obsolete opcodes) or explicitly asserts that the selected
        // general predicates are defunct, then returns false.  Preserve the
        // release-build result without rejecting every other condition and
        // action in the containing script.
        context.diagnostic(
            LegacyScriptCompileDiagnosticSeverity::Info,
            "script '" + container::String(scriptName) + "' uses legacy condition '" +
                name + "', which is permanently false",
            instruction.serialized);
        return ScriptAlwaysFalseCondition{};
    }

    context.diagnostic(
        LegacyScriptCompileDiagnosticSeverity::Warning,
        "script '" + container::String(scriptName) + "' uses unsupported condition '" +
            (name.empty() ? container::String{"<unknown opcode "} + std::to_string(instruction.opcode) + ">" : name) + "'",
        instruction.serialized);
    return std::nullopt;
}

[[nodiscard]] bool compileConditions(const LegacyScriptSource& source,
                                     container::Vector<ScriptAndClause>& output,
                                     CompileContext& context,
                                     container::StringView scriptName)
{
    output.clear();
    output.reserve(source.conditions.size());
    bool allSupported = true;
    for (const LegacyOrConditionSource& sourceClause : source.conditions)
    {
        ScriptAndClause clause;
        clause.allOf.reserve(sourceClause.conditions.size());
        for (const LegacyScriptInstructionSource& instruction : sourceClause.conditions)
        {
            const std::optional<ScriptCondition> condition = compileCondition(instruction, context, scriptName);
            if (condition) {
                clause.allOf.push_back(*condition);
            } else {
                // Keep the OR/AND topology intact. A malformed condition may
                // not make its clause accidentally true, and it must not
                // erase unrelated clauses or block the whole Script.
                clause.allOf.push_back(ScriptAlwaysFalseCondition{});
                allSupported = false;
            }
        }
        output.push_back(std::move(clause));
    }
    return allSupported;
}

[[nodiscard]] container::Vector<ScriptTeamSelector> conditionTeamCandidates(
    const LegacyScriptSource& source)
{
    constexpr int32_t kLegacyTeamParameterType = 3;
    container::Vector<ScriptTeamSelector> result;
    for (const LegacyOrConditionSource& clause : source.conditions)
    {
        for (const LegacyScriptInstructionSource& condition : clause.conditions)
        {
            for (const LegacyScriptParameter& parameter : condition.parameters)
            {
                if (parameter.type == kLegacyTeamParameterType && !parameter.text.empty())
                    result.push_back(teamSelector(parameter.text));
            }
        }
    }
    return result;
}
} // namespace engine::script::legacy::detail
