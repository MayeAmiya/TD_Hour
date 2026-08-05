#include "LegacyScriptCompilerInternal.h"

#include <limits>
#include <optional>
#include <utility>

namespace engine::script::legacy::detail
{

[[nodiscard]] std::optional<bool> compilePlayerAndSequentialAction(
    const LegacyScriptInstructionSource& instruction,
    container::Vector<ScriptAction>& output,
    CompileContext& context,
    container::StringView scriptName,
    const container::String& name)
{
    if (name == "TEAM_LOAD_TRANSPORTS" ||
        name == "TEAM_CAPTURE_NEAREST_UNOWNED_FACTION_UNIT" ||
        name == "NAMED_ENTER_NAMED" ||
        name == "TEAM_ENTER_NAMED" ||
        name == "TEAM_GARRISON_SPECIFIC_BUILDING" ||
        name == "TEAM_GARRISON_NEAREST_BUILDING" ||
        name == "NAMED_GARRISON_SPECIFIC_BUILDING" ||
        name == "NAMED_GARRISON_NEAREST_BUILDING" ||
        name == "PLAYER_GARRISON_ALL_BUILDINGS")
    {
        ScriptContainmentEnterAction action;
        if (name == "TEAM_LOAD_TRANSPORTS")
            action.kind = ScriptContainmentEnterActionKind::LoadTeamTransports;
        else if (name == "TEAM_CAPTURE_NEAREST_UNOWNED_FACTION_UNIT")
            action.kind = ScriptContainmentEnterActionKind::TeamCaptureNearestUnmanned;
        else if (name == "NAMED_ENTER_NAMED")
            action.kind = ScriptContainmentEnterActionKind::NamedEnterNamed;
        else if (name == "TEAM_ENTER_NAMED")
            action.kind = ScriptContainmentEnterActionKind::TeamEnterNamed;
        else if (name == "TEAM_GARRISON_SPECIFIC_BUILDING")
            action.kind = ScriptContainmentEnterActionKind::TeamGarrisonSpecific;
        else if (name == "TEAM_GARRISON_NEAREST_BUILDING")
            action.kind = ScriptContainmentEnterActionKind::TeamGarrisonNearest;
        else if (name == "NAMED_GARRISON_SPECIFIC_BUILDING")
            action.kind = ScriptContainmentEnterActionKind::NamedGarrisonSpecific;
        else if (name == "NAMED_GARRISON_NEAREST_BUILDING")
            action.kind = ScriptContainmentEnterActionKind::NamedGarrisonNearest;
        else
            action.kind = ScriptContainmentEnterActionKind::PlayerGarrisonAll;

        const bool named =
            action.kind == ScriptContainmentEnterActionKind::NamedEnterNamed ||
            action.kind == ScriptContainmentEnterActionKind::NamedGarrisonSpecific ||
            action.kind == ScriptContainmentEnterActionKind::NamedGarrisonNearest;
        const bool team =
            action.kind == ScriptContainmentEnterActionKind::LoadTeamTransports ||
            action.kind == ScriptContainmentEnterActionKind::TeamCaptureNearestUnmanned ||
            action.kind == ScriptContainmentEnterActionKind::TeamEnterNamed ||
            action.kind == ScriptContainmentEnterActionKind::TeamGarrisonSpecific ||
            action.kind == ScriptContainmentEnterActionKind::TeamGarrisonNearest;
        const bool player =
            action.kind == ScriptContainmentEnterActionKind::PlayerGarrisonAll;
        const bool specific =
            action.kind == ScriptContainmentEnterActionKind::NamedEnterNamed ||
            action.kind == ScriptContainmentEnterActionKind::TeamEnterNamed ||
            action.kind == ScriptContainmentEnterActionKind::TeamGarrisonSpecific ||
            action.kind == ScriptContainmentEnterActionKind::NamedGarrisonSpecific;
        const auto source = textParameter(
            instruction, 0, context, scriptName, name);
        const auto destination = specific
            ? textParameter(instruction, 1, context, scriptName, name)
            : std::optional<container::String>{};
        if (!source || (specific && !destination)) return false;
        if (named) action.object = objectSelector(*source);
        if (team) action.team = teamSelector(*source);
        if (player) {
            if (rejectUnsupportedDynamicPlayerSelector(
                    context, scriptName, name, *source,
                    instruction.serialized)) return false;
            action.player = *source;
        }
        if (destination) action.container = objectSelector(*destination);
        output.emplace_back(std::move(action));
        return true;
    }
    if (name == "NAMED_EXIT_ALL" || name == "EXIT_SPECIFIC_BUILDING" ||
        name == "NAMED_EXIT_BUILDING" || name == "UNIT_DESTROY_ALL_CONTAINED")
    {
        const auto object = textParameter(instruction, 0, context, scriptName, name);
        if (!object || rejectDynamicScriptContextSelector(
                context, scriptName, name, *object, instruction.serialized)) return false;
        ScriptContainmentActionKind kind =
            ScriptContainmentActionKind::EjectContainerContents;
        if (name == "EXIT_SPECIFIC_BUILDING")
            kind = ScriptContainmentActionKind::EjectSpecificStructure;
        else if (name == "NAMED_EXIT_BUILDING")
            kind = ScriptContainmentActionKind::DetachNamedOccupant;
        else if (name == "UNIT_DESTROY_ALL_CONTAINED")
            kind = ScriptContainmentActionKind::KillContainerContents;
        output.emplace_back(ScriptContainmentAction{
            .kind = kind,
            .targetName = *object,
        });
        return true;
    }
    if (name == "TEAM_EXIT_ALL" || name == "TEAM_EXIT_ALL_BUILDINGS")
    {
        const auto team = textParameter(instruction, 0, context, scriptName, name);
        if (!team || rejectDynamicScriptContextSelector(
                context, scriptName, name, *team, instruction.serialized)) return false;
        output.emplace_back(ScriptContainmentAction{
            .kind = name == "TEAM_EXIT_ALL"
                ? ScriptContainmentActionKind::EjectTeamContainerContents
                : ScriptContainmentActionKind::DetachTeamOccupants,
            .targetName = *team,
        });
        return true;
    }
    if (name == "PLAYER_EXIT_ALL_BUILDINGS")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        if (!player || rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) return false;
        output.emplace_back(ScriptContainmentAction{
            .kind = ScriptContainmentActionKind::EjectPlayerStructures,
            .targetName = *player,
        });
        return true;
    }
    if (name == "PLAYER_TRANSFER_OWNERSHIP_PLAYER")
    {
        const auto source = textParameter(instruction, 0, context, scriptName, name);
        const auto target = textParameter(instruction, 1, context, scriptName, name);
        if (!source || !target ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *source,
                                                   instruction.serialized) ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *target,
                                                   instruction.serialized)) return false;
        output.emplace_back(ScriptTransferOwnershipAction{
            .selector = ScriptOwnershipTransferSelector::PlayerAssets,
            .sourcePlayer = *source,
            .targetPlayer = *target,
        });
        return true;
    }
    if (name == "TEAM_MERGE_INTO_TEAM")
    {
        const auto source = textParameter(instruction, 0, context, scriptName, name);
        const auto target = textParameter(instruction, 1, context, scriptName, name);
        if (!source || !target ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *source,
                                               instruction.serialized) ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *target,
                                               instruction.serialized)) return false;
        output.emplace_back(ScriptTransferOwnershipAction{
            .selector = ScriptOwnershipTransferSelector::MergeScenarioTeam,
            .teamName = *source,
            .targetTeamName = *target,
        });
        return true;
    }
    if (name == "NAMED_TRANSFER_OWNERSHIP_PLAYER")
    {
        const auto objectName = textParameter(instruction, 0, context, scriptName, name);
        const auto targetPlayer = textParameter(instruction, 1, context, scriptName, name);
        if (!objectName || !targetPlayer ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *objectName,
                                               instruction.serialized) ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *targetPlayer,
                                                   instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptTransferOwnershipAction{
            .selector = ScriptOwnershipTransferSelector::NamedObject,
            .objectName = *objectName,
            .targetPlayer = *targetPlayer,
        });
        return true;
    }
    if (name == "TEAM_TRANSFER_TO_PLAYER")
    {
        const auto teamName = textParameter(instruction, 0, context, scriptName, name);
        const auto targetPlayer = textParameter(instruction, 1, context, scriptName, name);
        if (!teamName || !targetPlayer ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *teamName,
                                               instruction.serialized) ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *targetPlayer,
                                                   instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptTransferOwnershipAction{
            .selector = ScriptOwnershipTransferSelector::ScenarioTeam,
            .teamName = *teamName,
            .targetPlayer = *targetPlayer,
        });
        return true;
    }
    if (name == "PLAYER_DISABLE_BASE_CONSTRUCTION" ||
        name == "PLAYER_ENABLE_BASE_CONSTRUCTION" ||
        name == "PLAYER_DISABLE_UNIT_CONSTRUCTION" ||
        name == "PLAYER_ENABLE_UNIT_CONSTRUCTION")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        if (!player || rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) return false;
        const bool base = name == "PLAYER_DISABLE_BASE_CONSTRUCTION" ||
            name == "PLAYER_ENABLE_BASE_CONSTRUCTION";
        const bool enabled = name == "PLAYER_ENABLE_BASE_CONSTRUCTION" ||
            name == "PLAYER_ENABLE_UNIT_CONSTRUCTION";
        output.emplace_back(ScriptPlayerConstructionAction{
            .operation = base ? ScriptPlayerConstructionOperation::SetBaseEnabled
                              : ScriptPlayerConstructionOperation::SetUnitsEnabled,
            .player = *player,
            .enabled = enabled,
        });
        return true;
    }
    if (name == "PLAYER_DISABLE_FACTORIES" || name == "PLAYER_ENABLE_FACTORIES")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto factory = textParameter(instruction, 1, context, scriptName, name);
        if (!player || !factory || rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) return false;
        output.emplace_back(ScriptPlayerConstructionAction{
            .operation = ScriptPlayerConstructionOperation::SetFactoryTypeEnabled,
            .player = *player,
            .factoryType = *factory,
            .enabled = name == "PLAYER_ENABLE_FACTORIES",
        });
        return true;
    }
    if (name == "SET_BASE_CONSTRUCTION_SPEED")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto seconds = integerParameter(instruction, 1, context, scriptName, name);
        if (!player || !seconds || rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) return false;
        output.emplace_back(ScriptPlayerConstructionAction{
            .operation = ScriptPlayerConstructionOperation::SetTeamDelaySeconds,
            .player = *player,
            .value = *seconds,
        });
        return true;
    }
    if (name == "TECHTREE_MODIFY_BUILDABILITY_OBJECT")
    {
        const auto objectType = textParameter(instruction, 0, context, scriptName, name);
        const auto buildability = integerParameter(instruction, 1, context, scriptName, name);
        if (!objectType || !buildability) return false;
        if (*buildability < static_cast<int32_t>(ScriptObjectBuildability::Yes) ||
            *buildability > static_cast<int32_t>(ScriptObjectBuildability::OnlyByAi)) {
            context.diagnostic(
                LegacyScriptCompileDiagnosticSeverity::Warning,
                "script '" + container::String(scriptName) + "' action '" + name +
                    "' has an invalid Buildable ordinal " +
                    std::to_string(*buildability),
                instruction.serialized);
            return false;
        }
        output.emplace_back(ScriptObjectBuildabilityAction{
            .objectType = *objectType,
            .buildability = static_cast<ScriptObjectBuildability>(*buildability),
        });
        return true;
    }
    if (name == "PLAYER_SELL_EVERYTHING")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        if (!player || rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptPlayerSellEverythingAction{.player = *player});
        return true;
    }
    if (name == "PLAYER_REPAIR_NAMED_STRUCTURE")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto structure = textParameter(instruction, 1, context, scriptName, name);
        if (!player || !structure ||
            rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized) ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *structure, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptPlayerRepairStructureAction{
            .player = *player,
            .structure = objectSelector(*structure),
        });
        return true;
    }
    if (name == "AI_PLAYER_BUILD_UPGRADE")
    {
        const auto player = textParameter(
            instruction, 0, context, scriptName, name);
        const auto upgrade = textParameter(
            instruction, 1, context, scriptName, name);
        if (!player || !upgrade || rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptPlayerBuildUpgradeAction{
            .player = *player,
            .upgrade = *upgrade,
        });
        return true;
    }
    if (name == "AI_PLAYER_BUILD_TYPE_NEAREST_TEAM")
    {
        const auto player = textParameter(
            instruction, 0, context, scriptName, name);
        const auto objectType = textParameter(
            instruction, 1, context, scriptName, name);
        const auto team = textParameter(
            instruction, 2, context, scriptName, name);
        if (!player || !objectType || !team ||
            rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized) ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *team, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptPlayerBuildObjectNearTeamAction{
            .player = *player,
            .objectType = *objectType,
            .teamName = *team,
        });
        return true;
    }
    if (name == "AI_PLAYER_BUILD_SUPPLY_CENTER")
    {
        const auto player = textParameter(
            instruction, 0, context, scriptName, name);
        const auto objectType = textParameter(
            instruction, 1, context, scriptName, name);
        const auto minimumSupplies = integerParameter(
            instruction, 2, context, scriptName, name);
        if (!player || !objectType || !minimumSupplies ||
            rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptPlayerBuildSupplyCenterAction{
            .player = *player,
            .objectType = *objectType,
            .minimumSupplies = *minimumSupplies,
        });
        return true;
    }
    if (name == "SKIRMISH_BUILD_BUILDING")
    {
        const auto objectType = textParameter(
            instruction, 0, context, scriptName, name);
        if (!objectType) return false;
        output.emplace_back(ScriptSkirmishBuildBuildingAction{
            .objectType = *objectType,
        });
        return true;
    }
    if (name == "SKIRMISH_FOLLOW_APPROACH_PATH" ||
        name == "SKIRMISH_MOVE_TO_APPROACH_PATH")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        const auto pathPrefix = textParameter(
            instruction, 1, context, scriptName, name);
        std::optional<int32_t> asTeam = 0;
        if (name == "SKIRMISH_FOLLOW_APPROACH_PATH") {
            asTeam = integerParameter(
                instruction, 2, context, scriptName, name);
        }
        if (!team || !pathPrefix || !asTeam ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *team,
                instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptSkirmishApproachAction{
            .operation = name == "SKIRMISH_FOLLOW_APPROACH_PATH"
                ? ScriptSkirmishApproachOperation::FollowPath
                : ScriptSkirmishApproachOperation::MoveToPath,
            .teamName = *team,
            .pathPrefix = *pathPrefix,
            .asTeam = *asTeam != 0,
        });
        return true;
    }
    if (name == "SKIRMISH_BUILD_BASE_DEFENSE_FRONT" ||
        name == "SKIRMISH_BUILD_BASE_DEFENSE_FLANK" ||
        name == "SKIRMISH_BUILD_STRUCTURE_FRONT" ||
        name == "SKIRMISH_BUILD_STRUCTURE_FLANK")
    {
        const bool explicitType =
            name == "SKIRMISH_BUILD_STRUCTURE_FRONT" ||
            name == "SKIRMISH_BUILD_STRUCTURE_FLANK";
        std::optional<container::String> objectType = container::String{};
        if (explicitType) {
            objectType = textParameter(
                instruction, 0, context, scriptName, name);
        }
        if (!objectType) return false;
        bool flank = name == "SKIRMISH_BUILD_BASE_DEFENSE_FLANK" ||
                     name == "SKIRMISH_BUILD_STRUCTURE_FLANK";
        // RefCode heals the oldest BaseDefense form: the FRONT opcode carried
        // one Boolean which selected FRONT/FLANK, then the parameter was
        // removed. Preserve that map compatibility before typed validation.
        if (name == "SKIRMISH_BUILD_BASE_DEFENSE_FRONT" &&
            instruction.parameters.size() == 1u) {
            const auto legacyFlank = integerParameter(
                instruction, 0, context, scriptName, name);
            if (!legacyFlank) return false;
            flank = *legacyFlank != 0;
        }
        output.emplace_back(ScriptSkirmishPerimeterBuildAction{
            .objectType = *objectType,
            .flank = flank,
            .useFactionBaseDefense = !explicitType,
        });
        return true;
    }
    if (name == "SKIRMISH_FIRE_SPECIAL_POWER_AT_MOST_COST")
    {
        // RefCode ScriptAction::ReadAction heals old one-parameter chunks by
        // inserting SIDE=ThisPlayer and shifting SPECIAL_POWER to slot 1.
        const bool legacyOneParameter = instruction.parameters.size() == 1u;
        const std::optional<container::String> player = legacyOneParameter
            ? std::optional<container::String>{"ThisPlayer"}
            : textParameter(
                  instruction, 0, context, scriptName, name);
        const auto specialPower = textParameter(
            instruction, legacyOneParameter ? 0u : 1u,
            context, scriptName, name);
        if (!player || !specialPower ||
            rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player,
                instruction.serialized)) {
            return false;
        }
        output.emplace_back(
            ScriptSkirmishFireSpecialPowerAtMostCostAction{
                .player = *player,
                .specialPower = *specialPower,
            });
        return true;
    }
    if (name == "SKIRMISH_ATTACK_NEAREST_GROUP_WITH_VALUE")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        const auto comparison = comparisonParameter(
            instruction, 1, context, scriptName, name);
        const auto value = integerParameter(
            instruction, 2, context, scriptName, name);
        if (!team || !comparison || !value ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *team,
                instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptSkirmishAttackNearestValueGroupAction{
            .teamName = *team,
            .comparison = *comparison,
            .minimumValue = *value,
        });
        return true;
    }
    if (name ==
        "SKIRMISH_PERFORM_COMMANDBUTTON_ON_MOST_VALUABLE_OBJECT")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        const auto button = textParameter(
            instruction, 1, context, scriptName, name);
        const auto range = realParameter(
            instruction, 2, context, scriptName, name);
        const auto allTeamMembers = integerParameter(
            instruction, 3, context, scriptName, name);
        if (!team || !button || !range || !allTeamMembers ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *team,
                instruction.serialized)) {
            return false;
        }
        output.emplace_back(
            ScriptSkirmishMostValuableCommandButtonAction{
                .teamName = *team,
                .buttonName = *button,
                .range = math::q32_32{*range},
                .allTeamMembers = *allTeamMembers != 0,
            });
        return true;
    }
    if (name == "PLAYER_SET_MONEY" || name == "PLAYER_GIVE_MONEY")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto value = integerParameter(instruction, 1, context, scriptName, name);
        if (!player || !value ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized))
            return false;
        if (name == "PLAYER_SET_MONEY")
            output.emplace_back(ScriptSetPlayerCashAction{.player = *player, .value = *value});
        else
            output.emplace_back(ScriptAdjustPlayerCashAction{.player = *player, .delta = *value});
        return true;
    }
    if (name == "PLAYER_ADD_SKILLPOINTS" || name == "PLAYER_ADD_RANKLEVEL" ||
        name == "PLAYER_SET_RANKLEVEL" || name == "PLAYER_SELECT_SKILLSET")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto value = integerParameter(instruction, 1, context, scriptName, name);
        if (!player || !value ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return false;
        }
        ScriptPlayerProgressionOperation operation =
            ScriptPlayerProgressionOperation::AddSkillPoints;
        if (name == "PLAYER_ADD_RANKLEVEL") {
            operation = ScriptPlayerProgressionOperation::AdjustRankLevel;
        } else if (name == "PLAYER_SET_RANKLEVEL") {
            operation = ScriptPlayerProgressionOperation::SetRankLevel;
        } else if (name == "PLAYER_SELECT_SKILLSET") {
            operation = ScriptPlayerProgressionOperation::SelectSkillset;
        }
        output.emplace_back(ScriptPlayerProgressionAction{
            .operation = operation,
            .player = *player,
            // RefCode stores the selected skillset as the authored one-based
            // number minus one. A zero maps to its explicit "no skillset"
            // sentinel; do not clamp it into a fictitious first skillset.
            .integerValue = operation == ScriptPlayerProgressionOperation::SelectSkillset
                ? (*value == std::numeric_limits<int32_t>::min()
                    ? std::numeric_limits<int32_t>::min() : *value - 1)
                : *value,
        });
        return true;
    }
    if (name == "PLAYER_SET_RANKLEVELLIMIT")
    {
        const auto value = integerParameter(instruction, 0, context, scriptName, name);
        if (!value) return false;
        output.emplace_back(ScriptPlayerProgressionAction{
            .operation = ScriptPlayerProgressionOperation::SetRankLevelLimit,
            .integerValue = *value,
        });
        return true;
    }
    if (name == "PLAYER_GRANT_SCIENCE" || name == "PLAYER_PURCHASE_SCIENCE")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto science = textParameter(instruction, 1, context, scriptName, name);
        if (!player || !science ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptPlayerProgressionAction{
            .operation = name == "PLAYER_GRANT_SCIENCE"
                ? ScriptPlayerProgressionOperation::GrantScience
                : ScriptPlayerProgressionOperation::PurchaseScience,
            .player = *player,
            .science = *science,
        });
        return true;
    }
    if (name == "PLAYER_AFFECT_RECEIVING_EXPERIENCE")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto multiplier = realParameter(instruction, 1, context, scriptName, name);
        if (!player || !multiplier ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptPlayerProgressionAction{
            .operation = ScriptPlayerProgressionOperation::SetExperienceMultiplier,
            .player = *player,
            .realValue = math::q32_32{*multiplier},
        });
        return true;
    }
    if (name == "PLAYER_EXCLUDE_FROM_SCORE_SCREEN")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        if (!player || rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptPlayerProgressionAction{
            .operation = ScriptPlayerProgressionOperation::ExcludeFromScoreScreen,
            .player = *player,
        });
        return true;
    }
    if (name == "PLAYER_SCIENCE_AVAILABILITY")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        const auto science = textParameter(instruction, 1, context, scriptName, name);
        if (!player || !science ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *player,
                                                   instruction.serialized))
            return false;
        const auto availability = scienceAvailabilityParameter(
            instruction, 2, context, scriptName, name);
        if (!availability)
        {
            // Original ScriptActions treats an unknown availability spelling
            // as a no-op. A missing third parameter is malformed source and
            // remains a conservative compiler block.
            return instruction.parameters.size() >= 3;
        }
        output.emplace_back(ScriptSetPlayerScienceAvailabilityAction{
            .player = *player,
            .science = *science,
            .availability = *availability,
        });
        return true;
    }
    if (name == "TEAM_SET_OVERRIDE_RELATION_TO_TEAM" ||
        name == "TEAM_REMOVE_OVERRIDE_RELATION_TO_TEAM" ||
        name == "TEAM_REMOVE_ALL_OVERRIDE_RELATIONS" ||
        name == "TEAM_SET_OVERRIDE_RELATION_TO_PLAYER" ||
        name == "TEAM_REMOVE_OVERRIDE_RELATION_TO_PLAYER" ||
        name == "PLAYER_SET_OVERRIDE_RELATION_TO_TEAM" ||
        name == "PLAYER_REMOVE_OVERRIDE_RELATION_TO_TEAM")
    {
        const bool sourcePlayer = name.starts_with("PLAYER_");
        const bool targetPlayer = name.ends_with("_TO_PLAYER");
        const bool removeAll =
            name == "TEAM_REMOVE_ALL_OVERRIDE_RELATIONS";
        const bool set =
            name.find("_SET_") != container::StringView::npos;
        const auto source = textParameter(
            instruction, 0, context, scriptName, name);
        if (!source) return false;
        if (sourcePlayer) {
            if (rejectUnsupportedDynamicPlayerSelector(
                    context, scriptName, name, *source,
                    instruction.serialized)) return false;
        } else if (rejectDynamicScriptContextSelector(
                       context, scriptName, name, *source,
                       instruction.serialized)) {
            return false;
        }

        ScriptRelationshipOverrideAction action{
            .sourceKind = sourcePlayer
                ? ScriptRelationshipEndpointKind::Player
                : ScriptRelationshipEndpointKind::ScenarioTeam,
            .targetKind = targetPlayer
                ? ScriptRelationshipEndpointKind::Player
                : ScriptRelationshipEndpointKind::ScenarioTeam,
            .operation = removeAll
                ? ScriptRelationshipOverrideOperation::RemoveAllFromTeam
                : set
                    ? ScriptRelationshipOverrideOperation::Set
                    : ScriptRelationshipOverrideOperation::Remove,
            .sourceName = *source,
        };
        if (!removeAll) {
            const auto target = textParameter(
                instruction, 1, context, scriptName, name);
            if (!target) return false;
            if (targetPlayer) {
                if (rejectUnsupportedDynamicPlayerSelector(
                        context, scriptName, name, *target,
                        instruction.serialized)) return false;
            } else if (rejectDynamicScriptContextSelector(
                           context, scriptName, name, *target,
                           instruction.serialized)) {
                return false;
            }
            action.targetName = *target;
        }
        if (set) {
            const auto relationship = playerRelationshipParameter(
                instruction, 2, context, scriptName, name);
            if (!relationship) return false;
            action.relationship = *relationship;
        }
        output.emplace_back(std::move(action));
        return true;
    }
    if (name == "OBJECT_ALLOW_BONUSES" ||
        name == "CHOOSE_VICTIM_ALWAYS_USES_NORMAL")
    {
        const auto enabled = integerParameter(
            instruction, 0, context, scriptName, name);
        if (!enabled) return false;
        output.emplace_back(ScriptGlobalCombatPolicyAction{
            .policy = name == "OBJECT_ALLOW_BONUSES"
                ? ScriptGlobalCombatPolicy::ObjectDifficultyBonuses
                : ScriptGlobalCombatPolicy::ChooseVictimAlwaysNormal,
            .enabled = *enabled != 0,
        });
        return true;
    }
    if (name == "PLAYER_RELATES_PLAYER")
    {
        const auto sourcePlayer = textParameter(instruction, 0, context, scriptName, name);
        const auto targetPlayer = textParameter(instruction, 1, context, scriptName, name);
        const auto relationship = playerRelationshipParameter(
            instruction, 2, context, scriptName, name);
        if (!sourcePlayer || !targetPlayer || !relationship ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *sourcePlayer,
                                                   instruction.serialized) ||
            rejectUnsupportedDynamicPlayerSelector(context, scriptName, name, *targetPlayer,
                                                   instruction.serialized))
            return false;
        output.emplace_back(ScriptSetPlayerRelationshipAction{
            .sourcePlayer = *sourcePlayer,
            .targetPlayer = *targetPlayer,
            .relationship = *relationship,
        });
        return true;
    }
    if (name == "UNIT_EXECUTE_SEQUENTIAL_SCRIPT" ||
        name == "UNIT_EXECUTE_SEQUENTIAL_SCRIPT_LOOPING" ||
        name == "TEAM_EXECUTE_SEQUENTIAL_SCRIPT" ||
        name == "TEAM_EXECUTE_SEQUENTIAL_SCRIPT_LOOPING")
    {
        const bool teamTarget = name.starts_with("TEAM_");
        const bool looping = name.ends_with("_LOOPING");
        const auto targetName = textParameter(instruction, 0, context, scriptName, name);
        const auto targetScriptName = textParameter(instruction, 1, context, scriptName, name);
        const auto authoredLoopCount = looping
            ? integerParameter(instruction, 2, context, scriptName, name)
            : std::optional<int32_t>{0};
        if (!targetName || !targetScriptName || !authoredLoopCount)
            return false;
        const NamedScript* targetScript = findScriptByName(context, *targetScriptName);
        if (!targetScript)
        {
            context.diagnostic(
                LegacyScriptCompileDiagnosticSeverity::Warning,
                "script '" + container::String(scriptName) +
                    "' starts unknown sequential script '" + *targetScriptName + "'",
                instruction.serialized);
            return false;
        }
        const int32_t remainingRequeues = !looping
            ? 0
            : (*authoredLoopCount <= 0 ? -1 : *authoredLoopCount - 1);
        ScriptSequentialControlAction action{
            .operation = ScriptSequentialControlOperation::Start,
            .targetKind = teamTarget
                ? ScriptSequentialTargetKind::Team
                : ScriptSequentialTargetKind::Object,
            .script = targetScript->id,
            .remainingRequeues = remainingRequeues,
        };
        if (teamTarget)
            action.team = teamSelector(*targetName);
        else
            action.object = objectSelector(*targetName);
        output.emplace_back(std::move(action));
        return true;
    }
    if (name == "UNIT_STOP_SEQUENTIAL_SCRIPT" ||
        name == "TEAM_STOP_SEQUENTIAL_SCRIPT")
    {
        const bool teamTarget = name.starts_with("TEAM_");
        const auto targetName = textParameter(instruction, 0, context, scriptName, name);
        if (!targetName)
            return false;
        ScriptSequentialControlAction action{
            .operation = ScriptSequentialControlOperation::Stop,
            .targetKind = teamTarget
                ? ScriptSequentialTargetKind::Team
                : ScriptSequentialTargetKind::Object,
        };
        if (teamTarget)
            action.team = teamSelector(*targetName);
        else
            action.object = objectSelector(*targetName);
        output.emplace_back(std::move(action));
        return true;
    }
    if (name == "UNIT_IDLE_FOR_FRAMECOUNT" ||
        name == "UNIT_GUARD_FOR_FRAMECOUNT" ||
        name == "TEAM_IDLE_FOR_FRAMECOUNT" ||
        name == "TEAM_GUARD_FOR_FRAMECOUNT" ||
        name == "TEAM_SPIN_FOR_FRAMECOUNT")
    {
        const bool teamTarget = name.starts_with("TEAM_");
        const auto targetName = textParameter(instruction, 0, context, scriptName, name);
        const auto frames = integerParameter(instruction, 1, context, scriptName, name);
        if (!targetName || !frames)
            return false;
        ScriptSequentialTimedCommand command =
            ScriptSequentialTimedCommand::Idle;
        if (name == "UNIT_GUARD_FOR_FRAMECOUNT") {
            command = ScriptSequentialTimedCommand::GuardAtCurrentPosition;
        } else if (name == "TEAM_SPIN_FOR_FRAMECOUNT") {
            // Despite its editor name, the shipped handler only sets the
            // Team sequential timer; it never rotates or faces members.
            command = ScriptSequentialTimedCommand::DelayOnly;
        }
        ScriptSequentialTimedAction action{
            .targetKind = teamTarget
                ? ScriptSequentialTargetKind::Team
                : ScriptSequentialTargetKind::Object,
            .frames = *frames,
            .command = command,
        };
        if (teamTarget)
            action.team = teamSelector(*targetName);
        else
            action.object = objectSelector(*targetName);
        output.emplace_back(std::move(action));
        return true;
    }
    if (name == "SKIRMISH_WAIT_FOR_COMMANDBUTTON_AVAILABLE_ALL" ||
        name == "SKIRMISH_WAIT_FOR_COMMANDBUTTON_AVAILABLE_PARTIAL")
    {
        // SIDE is intentionally shape-checked but ignored, matching the
        // legacy sequential predicate implementation.
        const auto ignoredSide = textParameter(instruction, 0, context, scriptName, name);
        const auto team = textParameter(instruction, 1, context, scriptName, name);
        const auto commandButton = textParameter(instruction, 2, context, scriptName, name);
        if (!ignoredSide || !team || !commandButton)
            return false;
        output.emplace_back(ScriptSequentialWaitAction{
            .kind = name.ends_with("_ALL")
                ? ScriptSequentialWaitKind::CommandButtonAllReady
                : ScriptSequentialWaitKind::CommandButtonPartiallyReady,
            .team = teamSelector(*team),
            .commandButton = *commandButton,
        });
        return true;
    }
    if (name == "TEAM_WAIT_FOR_NOT_CONTAINED_ALL" ||
        name == "TEAM_WAIT_FOR_NOT_CONTAINED_PARTIAL")
    {
        const auto team = textParameter(instruction, 0, context, scriptName, name);
        if (!team)
            return false;
        output.emplace_back(ScriptSequentialWaitAction{
            .kind = name.ends_with("_ALL")
                ? ScriptSequentialWaitKind::TeamNotContainedAll
                : ScriptSequentialWaitKind::TeamNotContainedPartial,
            .team = teamSelector(*team),
        });
        return true;
    }
    return std::nullopt;
}

[[nodiscard]] bool compileAction(
    const LegacyScriptInstructionSource& instruction,
    container::Vector<ScriptAction>& output,
    CompileContext& context,
    container::StringView scriptName)
{
    const container::String name =
        instructionName(instruction, true, context, scriptName);
    if (const std::optional<bool> result =
            compileControlAndPresentationAction(
                instruction, output, context, scriptName, name))
        return *result;
    if (const std::optional<bool> result =
            compileObjectAndTeamAction(
                instruction, output, context, scriptName, name))
        return *result;
    if (const std::optional<bool> result =
            compilePlayerAndSequentialAction(
                instruction, output, context, scriptName, name))
        return *result;

    context.diagnostic(
        LegacyScriptCompileDiagnosticSeverity::Warning,
        "script '" + container::String(scriptName) +
            "' uses unsupported action '" +
            (name.empty()
                 ? container::String{"<unknown opcode "} +
                       std::to_string(instruction.opcode) + ">"
                 : name) +
            "'",
        instruction.serialized);
    return false;
}

[[nodiscard]] bool compileActions(const container::Vector<LegacyScriptInstructionSource>& sourceActions,
                                  container::Vector<ScriptAction>& output,
                                  CompileContext& context,
                                  container::StringView scriptName)
{
    output.clear();
    output.reserve(sourceActions.size());
    bool allSupported = true;
    for (const LegacyScriptInstructionSource& action : sourceActions)
    {
        const size_t checkpoint = output.size();
        if (compileAction(action, output, context, scriptName))
            continue;
        // A helper must not leak a partially assembled payload when a later
        // parameter fails. Preserve source ordinal with an explicit NO_OP so
        // subsequent actions still execute in their authored order.
        output.resize(checkpoint);
        output.emplace_back(ScriptNoOpAction{});
        allSupported = false;
    }
    return allSupported;
}
} // namespace engine::script::legacy::detail
