#include "LegacyScriptCompilerInternal.h"

#include <optional>
#include <utility>

namespace engine::script::legacy::detail
{

[[nodiscard]] ScriptFixedVec3 freezeGameplayCoordinate(
    const math::vec3& value) noexcept
{
    return {
        .x = math::q32_32{value.x()},
        .y = math::q32_32{value.y()},
        .z = math::q32_32{value.z()},
    };
}

[[nodiscard]] std::optional<bool> compileObjectAndTeamAction(
    const LegacyScriptInstructionSource& instruction,
    container::Vector<ScriptAction>& output,
    CompileContext& context,
    container::StringView scriptName,
    const container::String& name)
{
    // These four opcodes are the two RefCode creation helpers expressed in
    // different UI forms. Keep one typed action so all of them later cross
    // the same ScriptRuntime -> GameSession::spawnObject boundary.
    if (name == "CREATE_OBJECT")
    {
        const auto templateName = textParameter(instruction, 0, context, scriptName, name);
        const auto team = textParameter(instruction, 1, context, scriptName, name);
        const auto position = coordinateParameter(instruction, 2, context, scriptName, name);
        const auto rotation = realParameter(instruction, 3, context, scriptName, name);
        if (!templateName || !team || !position || !rotation ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *team,
                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptCreateObjectAction{
            .templateName = *templateName,
            .teamName = *team,
            .position = freezeGameplayCoordinate(*position),
            .rotation = math::q32_32{*rotation},
        });
        return true;
    }
    if (name == "UNIT_SPAWN_NAMED_LOCATION_ORIENTATION")
    {
        const auto object = textParameter(instruction, 0, context, scriptName, name);
        const auto templateName = textParameter(instruction, 1, context, scriptName, name);
        const auto team = textParameter(instruction, 2, context, scriptName, name);
        const auto position = coordinateParameter(instruction, 3, context, scriptName, name);
        const auto rotation = realParameter(instruction, 4, context, scriptName, name);
        if (!object || !templateName || !team || !position || !rotation ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *object,
                                               instruction.serialized) ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *team,
                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptCreateObjectAction{
            .objectName = *object,
            .templateName = *templateName,
            .teamName = *team,
            .position = freezeGameplayCoordinate(*position),
            .rotation = math::q32_32{*rotation},
        });
        return true;
    }
    if (name == "CREATE_NAMED_ON_TEAM_AT_WAYPOINT")
    {
        const auto object = textParameter(instruction, 0, context, scriptName, name);
        const auto templateName = textParameter(instruction, 1, context, scriptName, name);
        const auto team = textParameter(instruction, 2, context, scriptName, name);
        const auto waypoint = textParameter(instruction, 3, context, scriptName, name);
        if (!object || !templateName || !team || !waypoint ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *object,
                                               instruction.serialized) ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *team,
                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptCreateObjectAction{
            .objectName = *object,
            .templateName = *templateName,
            .teamName = *team,
            .waypointName = *waypoint,
        });
        return true;
    }
    if (name == "CREATE_UNNAMED_ON_TEAM_AT_WAYPOINT")
    {
        const auto templateName = textParameter(instruction, 0, context, scriptName, name);
        const auto team = textParameter(instruction, 1, context, scriptName, name);
        const auto waypoint = textParameter(instruction, 2, context, scriptName, name);
        if (!templateName || !team || !waypoint ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *team,
                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptCreateObjectAction{
            .templateName = *templateName,
            .teamName = *team,
            .waypointName = *waypoint,
        });
        return true;
    }
    if (name == "DAMAGE_MEMBERS_OF_TEAM")
    {
        const auto team = textParameter(instruction, 0, context, scriptName, name);
        const auto amount = realParameter(instruction, 1, context, scriptName, name);
        if (!team || !amount ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *team,
                                               instruction.serialized)) {
            return false;
        }
        // Team::damageTeamMembers has a deliberately unusual legacy branch:
        // a negative amount calls Object::kill(), while zero and positive
        // values become UNRESISTABLE/NORMAL damage. Canonicalize that branch
        // explicitly so it cannot be mistaken for negative healing later.
        const bool forceKill = *amount < 0.0f;
        output.emplace_back(ScriptDamageAction{
            .targetSelector = ScriptDamageTargetSelector::ScenarioTeam,
            .teamName = *team,
            .amount = forceKill ? math::q32_32{} : math::q32_32{*amount},
            .forceKill = forceKill,
        });
        return true;
    }
    if (name == "NAMED_DAMAGE")
    {
        const auto object = textParameter(instruction, 0, context, scriptName, name);
        const auto amount = integerParameter(instruction, 1, context, scriptName, name);
        if (!object || !amount ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *object,
                                               instruction.serialized)) {
            return false;
        }
        // ScriptActions::doNamedDamage forwards the signed legacy Int
        // unchanged to attemptDamage; unlike DAMAGE_MEMBERS_OF_TEAM, it does
        // not interpret a negative value as Object::kill().
        output.emplace_back(ScriptDamageAction{
            .targetSelector = ScriptDamageTargetSelector::NamedObject,
            .objectName = *object,
            .amount = math::q32_32{*amount},
        });
        return true;
    }
    if (name == "MOVE_TEAM_TO")
    {
        const auto team = textParameter(instruction, 0, context, scriptName, name);
        const auto waypoint = textParameter(instruction, 1, context, scriptName, name);
        if (!team || !waypoint ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *team, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::Move,
            .actorSelector = ScriptOrderActorSelector::ScenarioTeam,
            .teamName = *team,
            .targetWaypointName = *waypoint,
        });
        return true;
    }
    if (name == "MOVE_NAMED_UNIT_TO")
    {
        const auto unit = textParameter(instruction, 0, context, scriptName, name);
        const auto waypoint = textParameter(instruction, 1, context, scriptName, name);
        if (!unit || !waypoint ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *unit, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::Move,
            .actorSelector = ScriptOrderActorSelector::NamedObjects,
            .actorNames = {*unit},
            .targetWaypointName = *waypoint,
        });
        return true;
    }
    if (name == "TEAM_FOLLOW_WAYPOINTS" ||
        name == "TEAM_FOLLOW_WAYPOINTS_EXACT")
    {
        const auto team = textParameter(instruction, 0, context, scriptName, name);
        const auto pathLabel = textParameter(instruction, 1, context, scriptName, name);
        const auto asTeam = integerParameter(instruction, 2, context, scriptName, name);
        if (!team || !pathLabel || !asTeam ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *team,
                                               instruction.serialized)) {
            return false;
        }
        const bool exact = name == "TEAM_FOLLOW_WAYPOINTS_EXACT";
        const ScriptMoveRouteSubtype subtype = exact
            ? (*asTeam != 0 ? ScriptMoveRouteSubtype::WaypointPathTeamExact
                            : ScriptMoveRouteSubtype::WaypointPathIndividualsExact)
            : (*asTeam != 0 ? ScriptMoveRouteSubtype::WaypointPathTeam
                            : ScriptMoveRouteSubtype::WaypointPathIndividuals);
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::Move,
            .moveRouteSubtype = subtype,
            .actorSelector = ScriptOrderActorSelector::ScenarioTeam,
            .teamName = *team,
            .targetWaypointName = *pathLabel,
            .queued = false,
        });
        return true;
    }
    if (name == "TEAM_WANDER" || name == "TEAM_PANIC")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        const auto path = textParameter(
            instruction, 1, context, scriptName, name);
        if (!team || !path || rejectDynamicScriptContextSelector(
                context, scriptName, name, *team, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::Move,
            .moveRouteSubtype = name == "TEAM_PANIC"
                ? ScriptMoveRouteSubtype::PanicWaypointPath
                : ScriptMoveRouteSubtype::WanderWaypointPath,
            .actorSelector = ScriptOrderActorSelector::ScenarioTeam,
            .teamName = *team,
            .targetWaypointName = *path,
        });
        return true;
    }
    if (name == "NAMED_FOLLOW_WAYPOINTS" ||
        name == "NAMED_FOLLOW_WAYPOINTS_EXACT")
    {
        const auto unit = textParameter(instruction, 0, context, scriptName, name);
        const auto pathLabel = textParameter(instruction, 1, context, scriptName, name);
        if (!unit || !pathLabel ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *unit,
                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::Move,
            .moveRouteSubtype = name == "NAMED_FOLLOW_WAYPOINTS_EXACT"
                ? ScriptMoveRouteSubtype::WaypointPathIndividualsExact
                : ScriptMoveRouteSubtype::WaypointPathIndividuals,
            .actorSelector = ScriptOrderActorSelector::NamedObjects,
            .actorNames = {*unit},
            .targetWaypointName = *pathLabel,
            .queued = false,
        });
        return true;
    }
    if (name == "TEAM_STOP" || name == "TEAM_STOP_AND_DISBAND")
    {
        const auto team = textParameter(instruction, 0, context, scriptName, name);
        if (!team ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *team, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::Stop,
            .actorSelector = ScriptOrderActorSelector::ScenarioTeam,
            .teamName = *team,
            .disbandAfterStop = name == "TEAM_STOP_AND_DISBAND",
        });
        return true;
    }
    if (name == "NAMED_STOP")
    {
        const auto unit = textParameter(instruction, 0, context, scriptName, name);
        if (!unit ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *unit, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::Stop,
            .actorSelector = ScriptOrderActorSelector::NamedObjects,
            .actorNames = {*unit},
        });
        return true;
    }
    if (name == "NAMED_FIRE_SPECIAL_POWER_AT_WAYPOINT" ||
        name == "NAMED_FIRE_SPECIAL_POWER_AT_NAMED")
    {
        const auto source = textParameter(instruction, 0, context, scriptName, name);
        const auto power = textParameter(instruction, 1, context, scriptName, name);
        const auto target = textParameter(instruction, 2, context, scriptName, name);
        if (!source || !power || !target ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *source,
                                               instruction.serialized) ||
            (name == "NAMED_FIRE_SPECIAL_POWER_AT_NAMED" &&
             rejectDynamicScriptContextSelector(context, scriptName, name, *target,
                                                instruction.serialized))) return false;
        ScriptIssueOrderAction action{
            .kind = ScriptOrderKind::SpecialPower,
            .actorSelector = ScriptOrderActorSelector::NamedObjects,
            .actorNames = {*source},
            .contentName = *power,
        };
        if (name == "NAMED_FIRE_SPECIAL_POWER_AT_WAYPOINT")
            action.targetWaypointName = *target;
        else
            action.targetObjectName = *target;
        output.emplace_back(std::move(action));
        return true;
    }
    if (name == "NAMED_FIRE_WEAPON_FOLLOWING_WAYPOINT_PATH")
    {
        const auto object = textParameter(
            instruction, 0, context, scriptName, name);
        const auto path = textParameter(
            instruction, 1, context, scriptName, name);
        if (!object || !path || rejectDynamicScriptContextSelector(
                context, scriptName, name, *object,
                instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptFireWeaponFollowingWaypointPathAction{
            .objectName = *object,
            .waypointPathName = *path,
        });
        return true;
    }
    if (name == "CREATE_REINFORCEMENT_TEAM")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        const auto destination = textParameter(
            instruction, 1, context, scriptName, name);
        if (!team || !destination || rejectDynamicScriptContextSelector(
                context, scriptName, name, *team, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptCreateReinforcementTeamAction{
            .teamName = *team,
            .destinationWaypointName = *destination,
        });
        return true;
    }
    if (name == "BUILD_TEAM")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        if (!team || rejectDynamicScriptContextSelector(
                context, scriptName, name, *team,
                instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptBuildTeamAction{.teamName = *team});
        return true;
    }
    if (name == "RECRUIT_TEAM")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        const auto radius = realParameter(
            instruction, 1, context, scriptName, name);
        if (!team || !radius || rejectDynamicScriptContextSelector(
                context, scriptName, name, *team, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptRecruitTeamAction{
            .teamName = *team,
            .radius = math::q32_32{*radius},
        });
        return true;
    }
    if (name == "NAMED_USE_COMMANDBUTTON_ABILITY" ||
        name == "NAMED_USE_COMMANDBUTTON_ABILITY_ON_NAMED" ||
        name == "NAMED_USE_COMMANDBUTTON_ABILITY_AT_WAYPOINT" ||
        name == "NAMED_USE_COMMANDBUTTON_ABILITY_USING_WAYPOINT_PATH" ||
        name == "TEAM_USE_COMMANDBUTTON_ABILITY" ||
        name == "TEAM_USE_COMMANDBUTTON_ABILITY_ON_NAMED" ||
        name == "TEAM_USE_COMMANDBUTTON_ABILITY_AT_WAYPOINT")
    {
        const auto actor = textParameter(
            instruction, 0, context, scriptName, name);
        const auto button = textParameter(
            instruction, 1, context, scriptName, name);
        const bool objectTarget =
            name == "NAMED_USE_COMMANDBUTTON_ABILITY_ON_NAMED" ||
            name == "TEAM_USE_COMMANDBUTTON_ABILITY_ON_NAMED";
        const bool waypointTarget =
            name == "NAMED_USE_COMMANDBUTTON_ABILITY_AT_WAYPOINT" ||
            name == "TEAM_USE_COMMANDBUTTON_ABILITY_AT_WAYPOINT";
        const bool waypointPathTarget =
            name == "NAMED_USE_COMMANDBUTTON_ABILITY_USING_WAYPOINT_PATH";
        std::optional<container::String> target;
        if (objectTarget || waypointTarget || waypointPathTarget) {
            target = textParameter(
                instruction, 2, context, scriptName, name);
        }
        if (!actor || !button ||
            ((objectTarget || waypointTarget || waypointPathTarget) && !target) ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *actor,
                instruction.serialized) ||
            (objectTarget && rejectDynamicScriptContextSelector(
                 context, scriptName, name, *target,
                 instruction.serialized))) {
            return false;
        }

        const bool teamActor =
            name.rfind("TEAM_USE_COMMANDBUTTON_ABILITY", 0) == 0;
        ScriptUseCommandButtonAction action{
            .actorSelector = teamActor
                ? ScriptOrderActorSelector::ScenarioTeam
                : ScriptOrderActorSelector::NamedObjects,
            .actorNames = teamActor
                ? container::Vector<container::String>{}
                : container::Vector<container::String>{*actor},
            .teamName = teamActor ? *actor : container::String{},
            .buttonName = *button,
            .targetKind = objectTarget
                ? ScriptCommandButtonTargetKind::NamedObject
                : waypointTarget
                    ? ScriptCommandButtonTargetKind::Waypoint
                    : waypointPathTarget
                        ? ScriptCommandButtonTargetKind::WaypointPath
                        : ScriptCommandButtonTargetKind::None,
        };
        if (objectTarget) action.targetObjectName = *target;
        if (waypointTarget || waypointPathTarget)
            action.targetWaypointName = *target;
        output.emplace_back(std::move(action));
        return true;
    }
    if (name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NAMED" ||
        name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_ENEMY_UNIT" ||
        name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_GARRISONED_BUILDING" ||
        name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_KINDOF" ||
        name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_ENEMY_BUILDING" ||
        name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_ENEMY_BUILDING_CLASS" ||
        name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_OBJECTTYPE")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        const auto button = textParameter(
            instruction, 1, context, scriptName, name);
        const bool hasFilter =
            name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NAMED" ||
            name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_KINDOF" ||
            name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_ENEMY_BUILDING_CLASS" ||
            name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_OBJECTTYPE";
        const bool kindOfFilter =
            name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_KINDOF" ||
            name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_ENEMY_BUILDING_CLASS";
        std::optional<container::String> filter;
        if (hasFilter) {
            if (kindOfFilter) {
                const auto ordinal = integerParameter(
                    instruction, 2, context, scriptName, name);
                const auto canonical = ordinal
                    ? legacyZeroHourKindOfName(*ordinal)
                    : std::nullopt;
                if (!canonical) {
                    context.diagnostic(
                        LegacyScriptCompileDiagnosticSeverity::Warning,
                        "script '" + container::String(scriptName) +
                            "' action '" + name +
                            "' has an invalid Zero Hour KindOf ordinal",
                        instruction.serialized);
                    return false;
                }
                filter = container::String{*canonical};
            } else {
                filter = textParameter(
                    instruction, 2, context, scriptName, name);
            }
        }
        if (!team || !button || (hasFilter && !filter) ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *team,
                instruction.serialized) ||
            (name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NAMED" &&
             rejectDynamicScriptContextSelector(
                 context, scriptName, name, *filter,
                 instruction.serialized))) {
            return false;
        }
        ScriptUseCommandButtonAction action{
            .actorSelector = ScriptOrderActorSelector::ScenarioTeam,
            .teamName = *team,
            .buttonName = *button,
            .preselectSourceAndTarget = true,
        };
        if (name == "TEAM_ALL_USE_COMMANDBUTTON_ON_NAMED") {
            action.targetKind = ScriptCommandButtonTargetKind::NamedObject;
            action.targetObjectName = *filter;
        } else if (name ==
                   "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_ENEMY_UNIT") {
            action.targetKind =
                ScriptCommandButtonTargetKind::NearestEnemyUnit;
        } else if (name ==
                   "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_GARRISONED_BUILDING") {
            action.targetKind =
                ScriptCommandButtonTargetKind::NearestGarrisonedBuilding;
        } else if (name ==
                   "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_KINDOF") {
            action.targetKind = ScriptCommandButtonTargetKind::NearestKindOf;
            action.targetFilter = *filter;
        } else if (name ==
                   "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_ENEMY_BUILDING") {
            action.targetKind =
                ScriptCommandButtonTargetKind::NearestEnemyBuilding;
        } else if (name ==
                   "TEAM_ALL_USE_COMMANDBUTTON_ON_NEAREST_ENEMY_BUILDING_CLASS") {
            action.targetKind =
                ScriptCommandButtonTargetKind::NearestEnemyBuildingClass;
            action.targetFilter = *filter;
        } else {
            action.targetKind =
                ScriptCommandButtonTargetKind::NearestObjectType;
            action.targetFilter = *filter;
        }
        output.emplace_back(std::move(action));
        return true;
    }
    if (name == "TEAM_PARTIAL_USE_COMMANDBUTTON")
    {
        const auto percentage = realParameter(
            instruction, 0, context, scriptName, name);
        const auto team = textParameter(
            instruction, 1, context, scriptName, name);
        const auto button = textParameter(
            instruction, 2, context, scriptName, name);
        if (!percentage || !team || !button ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *team,
                instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptUseCommandButtonAction{
            .actorSelector = ScriptOrderActorSelector::ScenarioTeam,
            .teamName = *team,
            .buttonName = *button,
            .actorPolicy =
                ScriptCommandButtonActorPolicy::PartialUsable,
            .actorPercentage = math::q32_32{*percentage},
            .preselectSourceAndTarget = true,
        });
        return true;
    }
    if (name == "NAMED_STOP_SPECIAL_POWER_COUNTDOWN" ||
        name == "NAMED_START_SPECIAL_POWER_COUNTDOWN" ||
        name == "NAMED_SET_SPECIAL_POWER_COUNTDOWN" ||
        name == "NAMED_ADD_SPECIAL_POWER_COUNTDOWN")
    {
        const auto object = textParameter(instruction, 0, context, scriptName, name);
        const auto power = textParameter(instruction, 1, context, scriptName, name);
        if (!object || !power || rejectDynamicScriptContextSelector(
                context, scriptName, name, *object, instruction.serialized)) return false;
        ScriptSpecialPowerCountdownAction action{
            .objectName = *object,
            .specialPower = *power,
        };
        if (name == "NAMED_STOP_SPECIAL_POWER_COUNTDOWN" ||
            name == "NAMED_START_SPECIAL_POWER_COUNTDOWN") {
            action.operation = ScriptSpecialPowerCountdownOperation::Pause;
            action.paused = name == "NAMED_STOP_SPECIAL_POWER_COUNTDOWN";
        } else {
            const auto seconds = integerParameter(instruction, 2, context, scriptName, name);
            if (!seconds) return false;
            action.operation = name == "NAMED_SET_SPECIAL_POWER_COUNTDOWN"
                ? ScriptSpecialPowerCountdownOperation::Set
                : ScriptSpecialPowerCountdownOperation::Add;
            action.seconds = *seconds;
        }
        output.emplace_back(std::move(action));
        return true;
    }
    if (name == "SET_STOPPING_DISTANCE" || name == "NAMED_SET_STOPPING_DISTANCE")
    {
        const auto target = textParameter(instruction, 0, context, scriptName, name);
        const auto distance = realParameter(instruction, 1, context, scriptName, name);
        if (!target || !distance || rejectDynamicScriptContextSelector(
                context, scriptName, name, *target, instruction.serialized)) return false;
        output.emplace_back(ScriptStoppingDistanceAction{
            .targetKind = name == "SET_STOPPING_DISTANCE"
                ? ScriptStoppingDistanceTargetKind::ScenarioTeam
                : ScriptStoppingDistanceTargetKind::NamedObject,
            .targetName = *target,
            .distance = math::q32_32{*distance},
        });
        return true;
    }
    if (name == "NAMED_SET_TOPPLE_DIRECTION")
    {
        const auto object = textParameter(instruction, 0, context, scriptName, name);
        const auto direction = coordinateParameter(
            instruction, 1, context, scriptName, name);
        if (!object || !direction || rejectDynamicScriptContextSelector(
                context, scriptName, name, *object, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptToppleDirectionAction{
            .objectName = *object,
            .direction = freezeGameplayCoordinate(*direction),
        });
        return true;
    }
    if (name == "UNIT_MOVE_TOWARDS_NEAREST_OBJECT_TYPE" ||
        name == "TEAM_MOVE_TOWARDS_NEAREST_OBJECT_TYPE")
    {
        const auto actor = textParameter(instruction, 0, context, scriptName, name);
        const auto objectType = textParameter(instruction, 1, context, scriptName, name);
        const auto trigger = textParameter(instruction, 2, context, scriptName, name);
        if (!actor || !objectType || !trigger ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *actor,
                                               instruction.serialized)) return false;
        const bool team = name == "TEAM_MOVE_TOWARDS_NEAREST_OBJECT_TYPE";
        output.emplace_back(ScriptMoveTowardsNearestAction{
            .actorSelector = team ? ScriptOrderActorSelector::ScenarioTeam
                                  : ScriptOrderActorSelector::NamedObjects,
            .actorName = team ? container::String{} : *actor,
            .teamName = team ? *actor : container::String{},
            .objectType = *objectType,
            .triggerArea = *trigger,
        });
        return true;
    }
    if (name == "WAREHOUSE_SET_VALUE")
    {
        const auto warehouse = textParameter(instruction, 0, context, scriptName, name);
        const auto cashValue = integerParameter(instruction, 1, context, scriptName, name);
        if (!warehouse || !cashValue || rejectDynamicScriptContextSelector(
                context, scriptName, name, *warehouse, instruction.serialized)) return false;
        output.emplace_back(ScriptWarehouseValueAction{
            .objectName = *warehouse,
            .cashValue = *cashValue,
        });
        return true;
    }
    if (name == "SET_CAVE_INDEX")
    {
        const auto cave = textParameter(instruction, 0, context, scriptName, name);
        const auto caveIndex = integerParameter(instruction, 1, context, scriptName, name);
        if (!cave || !caveIndex || *caveIndex < 0 ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *cave, instruction.serialized)) return false;
        output.emplace_back(ScriptCaveIndexAction{
            .objectName = *cave,
            .caveIndex = *caveIndex,
        });
        return true;
    }
    // RefCode routes both of these actions through the AI's object-target
    // attack path (`aiForceAttackObject` / `groupAttackObject`). Unlike
    // TEAM_ATTACK_TEAM and the area-target actions, their target is one
    // named object and therefore maps directly to the current typed Attack
    // intent consumed by ObjectCombatSystem. The intent preserves the
    // original target identity/source; pursuit/pathing remains an explicit
    // later locomotion stage rather than a compiler-side approximation.
    if (name == "NAMED_ATTACK_NAMED")
    {
        const auto attacker = textParameter(instruction, 0, context, scriptName, name);
        const auto target = textParameter(instruction, 1, context, scriptName, name);
        if (!attacker || !target ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *attacker,
                                               instruction.serialized) ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *target,
                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::Attack,
            .actorSelector = ScriptOrderActorSelector::NamedObjects,
            .actorNames = {*attacker},
            .targetObjectName = *target,
            .forceAttack = true,
        });
        return true;
    }
    if (name == "TEAM_ATTACK_NAMED")
    {
        const auto team = textParameter(instruction, 0, context, scriptName, name);
        const auto target = textParameter(instruction, 1, context, scriptName, name);
        if (!team || !target ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *team,
                                               instruction.serialized) ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *target,
                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::Attack,
            .actorSelector = ScriptOrderActorSelector::ScenarioTeam,
            .teamName = *team,
            .targetObjectName = *target,
        });
        return true;
    }
    if (name == "TEAM_ATTACK_TEAM" || name == "NAMED_ATTACK_TEAM")
    {
        const auto actor = textParameter(instruction, 0, context, scriptName, name);
        const auto targetTeam = textParameter(instruction, 1, context, scriptName, name);
        if (!actor || !targetTeam ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *actor,
                                               instruction.serialized) ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *targetTeam,
                                               instruction.serialized)) {
            return false;
        }
        const bool teamActor = name == "TEAM_ATTACK_TEAM";
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::TacticalAttack,
            .tacticalAttackSubtype = ScriptTacticalAttackSubtype::AttackSquad,
            .actorSelector = teamActor ? ScriptOrderActorSelector::ScenarioTeam
                                       : ScriptOrderActorSelector::NamedObjects,
            .actorNames = teamActor ? container::Vector<container::String>{}
                                    : container::Vector<container::String>{*actor},
            .teamName = teamActor ? *actor : container::String{},
            .targetTeamName = *targetTeam,
        });
        return true;
    }
    if (name == "NAMED_ATTACK_AREA" || name == "TEAM_ATTACK_AREA")
    {
        const auto actor = textParameter(instruction, 0, context, scriptName, name);
        const auto targetArea = textParameter(instruction, 1, context, scriptName, name);
        if (!actor || !targetArea ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *actor,
                                               instruction.serialized)) {
            return false;
        }
        const bool teamActor = name == "TEAM_ATTACK_AREA";
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::TacticalAttack,
            .tacticalAttackSubtype = ScriptTacticalAttackSubtype::AttackArea,
            .actorSelector = teamActor ? ScriptOrderActorSelector::ScenarioTeam
                                       : ScriptOrderActorSelector::NamedObjects,
            .actorNames = teamActor ? container::Vector<container::String>{}
                                    : container::Vector<container::String>{*actor},
            .teamName = teamActor ? *actor : container::String{},
            .targetAreaName = *targetArea,
        });
        return true;
    }
    if (name == "NAMED_HUNT" || name == "TEAM_HUNT")
    {
        const auto actor = textParameter(instruction, 0, context, scriptName, name);
        if (!actor || rejectDynamicScriptContextSelector(
                          context, scriptName, name, *actor, instruction.serialized)) {
            return false;
        }
        const bool team = name == "TEAM_HUNT";
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::TacticalAttack,
            .tacticalAttackSubtype = ScriptTacticalAttackSubtype::Hunt,
            .actorSelector = team ? ScriptOrderActorSelector::ScenarioTeam
                                  : ScriptOrderActorSelector::NamedObjects,
            .actorNames = team ? container::Vector<container::String>{}
                               : container::Vector<container::String>{*actor},
            .teamName = team ? *actor : container::String{},
        });
        return true;
    }
    if (name == "PLAYER_HUNT")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        if (!player || rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::TacticalAttack,
            .tacticalAttackSubtype = ScriptTacticalAttackSubtype::Hunt,
            .actorSelector = ScriptOrderActorSelector::PlayerAssets,
            .playerName = *player,
            // The bridge also stores Player::m_unitsShouldHunt persistently;
            // this first order carries the policy explicitly so the current
            // actor batch observes it without a second lookup boundary.
            .allArmyHunt = true,
        });
        return true;
    }
    if (name == "NAMED_GUARD" || name == "TEAM_GUARD")
    {
        const auto actor = textParameter(instruction, 0, context, scriptName, name);
        if (!actor || rejectDynamicScriptContextSelector(
                          context, scriptName, name, *actor, instruction.serialized)) {
            return false;
        }
        const bool team = name == "TEAM_GUARD";
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::TacticalAttack,
            .tacticalAttackSubtype = ScriptTacticalAttackSubtype::Guard,
            .actorSelector = team ? ScriptOrderActorSelector::ScenarioTeam
                                  : ScriptOrderActorSelector::NamedObjects,
            .actorNames = team ? container::Vector<container::String>{}
                               : container::Vector<container::String>{*actor},
            .teamName = team ? *actor : container::String{},
        });
        return true;
    }
    if (name == "TEAM_GUARD_IN_TUNNEL_NETWORK")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        if (!team || rejectDynamicScriptContextSelector(
                context, scriptName, name, *team, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::TacticalAttack,
            .tacticalAttackSubtype =
                ScriptTacticalAttackSubtype::GuardTunnelNetwork,
            .actorSelector = ScriptOrderActorSelector::ScenarioTeam,
            .teamName = *team,
        });
        return true;
    }
    if (name == "TEAM_GUARD_SUPPLY_CENTER")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        const auto supplies = integerParameter(
            instruction, 1, context, scriptName, name);
        if (!team || !supplies || rejectDynamicScriptContextSelector(
                context, scriptName, name, *team,
                instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptGuardSupplyCenterAction{
            .teamName = *team,
            .minimumSupplies = *supplies,
        });
        return true;
    }
    if (name == "TEAM_GUARD_POSITION")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        const auto waypoint = textParameter(
            instruction, 1, context, scriptName, name);
        if (!team || !waypoint ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *team,
                instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::TacticalAttack,
            .tacticalAttackSubtype = ScriptTacticalAttackSubtype::Guard,
            .actorSelector = ScriptOrderActorSelector::ScenarioTeam,
            .teamName = *team,
            .targetWaypointName = *waypoint,
        });
        return true;
    }
    if (name == "TEAM_GUARD_OBJECT")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        const auto target = textParameter(
            instruction, 1, context, scriptName, name);
        if (!team || !target ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *team,
                instruction.serialized) ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *target,
                instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::TacticalAttack,
            .tacticalAttackSubtype = ScriptTacticalAttackSubtype::Guard,
            .actorSelector = ScriptOrderActorSelector::ScenarioTeam,
            .teamName = *team,
            .targetObjectName = *target,
        });
        return true;
    }
    if (name == "TEAM_GUARD_AREA")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        const auto area = textParameter(
            instruction, 1, context, scriptName, name);
        if (!team || !area ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *team,
                instruction.serialized) ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *area,
                instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptIssueOrderAction{
            .kind = ScriptOrderKind::TacticalAttack,
            .tacticalAttackSubtype = ScriptTacticalAttackSubtype::Guard,
            .actorSelector = ScriptOrderActorSelector::ScenarioTeam,
            .teamName = *team,
            .targetAreaName = *area,
        });
        return true;
    }
    if (name == "TEAM_HUNT_WITH_COMMAND_BUTTON")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        const auto commandButton = textParameter(
            instruction, 1, context, scriptName, name);
        if (!team || !commandButton || rejectDynamicScriptContextSelector(
                context, scriptName, name, *team,
                instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptAIBehaviorMutationAction{
            .targetKind = ScriptAIBehaviorTargetKind::ScenarioTeam,
            .targetName = *team,
            .mutation =
                ScriptAIBehaviorMutationKind::SetCommandButtonHunt,
            .commandButton = *commandButton,
        });
        return true;
    }
    if (name == "TEAM_INCREASE_PRIORITY" ||
        name == "TEAM_DECREASE_PRIORITY")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        if (!team || rejectDynamicScriptContextSelector(
                context, scriptName, name, *team, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptAIBehaviorMutationAction{
            .targetKind = ScriptAIBehaviorTargetKind::ScenarioTeam,
            .targetName = *team,
            .mutation = name == "TEAM_INCREASE_PRIORITY"
                ? ScriptAIBehaviorMutationKind::IncreaseTeamProductionPriority
                : ScriptAIBehaviorMutationKind::DecreaseTeamProductionPriority,
        });
        return true;
    }
    if (name == "TEAM_WANDER_IN_PLACE")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        if (!team || rejectDynamicScriptContextSelector(
                context, scriptName, name, *team, instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptAIBehaviorMutationAction{
            .targetKind = ScriptAIBehaviorTargetKind::ScenarioTeam,
            .targetName = *team,
            .mutation = ScriptAIBehaviorMutationKind::WanderInPlace,
        });
        return true;
    }
    if (name == "NAMED_APPLY_ATTACK_PRIORITY_SET" ||
        name == "TEAM_APPLY_ATTACK_PRIORITY_SET" ||
        name == "NAMED_SET_ATTITUDE" ||
        name == "TEAM_SET_ATTITUDE")
    {
        const auto target = textParameter(
            instruction, 0, context, scriptName, name);
        if (!target || rejectDynamicScriptContextSelector(
                context, scriptName, name, *target,
                instruction.serialized)) {
            return false;
        }
        const bool attitudeAction =
            name == "NAMED_SET_ATTITUDE" ||
            name == "TEAM_SET_ATTITUDE";
        const bool teamAction =
            name == "TEAM_APPLY_ATTACK_PRIORITY_SET" ||
            name == "TEAM_SET_ATTITUDE";
        ScriptAIBehaviorMutationAction action{
            .targetKind = teamAction
                ? ScriptAIBehaviorTargetKind::ScenarioTeam
                : ScriptAIBehaviorTargetKind::NamedObject,
            .targetName = *target,
            .mutation = attitudeAction
                ? ScriptAIBehaviorMutationKind::SetAttitude
                : ScriptAIBehaviorMutationKind::ApplyAttackPrioritySet,
        };
        if (attitudeAction) {
            const auto attitude = integerParameter(
                instruction, 1, context, scriptName, name);
            if (!attitude || *attitude < -2 || *attitude > 2) {
                context.diagnostic(
                    LegacyScriptCompileDiagnosticSeverity::Warning,
                    "script '" + container::String(scriptName) +
                        "' action '" + name +
                        "' has an invalid attitude value",
                    instruction.serialized);
                return false;
            }
            action.attitude = *attitude;
        } else {
            const auto setName = textParameter(
                instruction, 1, context, scriptName, name);
            if (!setName) return false;
            action.attackPrioritySet = *setName;
        }
        output.emplace_back(std::move(action));
        return true;
    }
    if (name == "SET_ATTACK_PRIORITY_THING" ||
        name == "SET_ATTACK_PRIORITY_KIND_OF" ||
        name == "SET_DEFAULT_ATTACK_PRIORITY")
    {
        const auto setName = textParameter(
            instruction, 0, context, scriptName, name);
        if (!setName) return false;
        ScriptAttackPriorityMutationAction action{
            .mutation = name == "SET_ATTACK_PRIORITY_THING"
                ? ScriptAttackPriorityMutationKind::ObjectType
                : name == "SET_ATTACK_PRIORITY_KIND_OF"
                    ? ScriptAttackPriorityMutationKind::KindOf
                    : ScriptAttackPriorityMutationKind::Default,
            .setName = *setName,
        };
        const size_t priorityIndex =
            name == "SET_DEFAULT_ATTACK_PRIORITY" ? 1u : 2u;
        const auto priority = integerParameter(
            instruction, priorityIndex, context, scriptName, name);
        if (!priority) return false;
        action.priority = *priority;
        if (name != "SET_DEFAULT_ATTACK_PRIORITY") {
            // KIND_OF_PARAM serializes its canonical token alongside the
            // ordinal. Keeping that text avoids importing the old enum ABI.
            const auto selector = textParameter(
                instruction, 1, context, scriptName, name);
            if (!selector) return false;
            action.selector = *selector;
        }
        output.emplace_back(std::move(action));
        return true;
    }
    if (name == "TEAM_AVAILABLE_FOR_RECRUITMENT")
    {
        const auto team = textParameter(
            instruction, 0, context, scriptName, name);
        const auto available = integerParameter(
            instruction, 1, context, scriptName, name);
        if (!team || !available ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *team,
                instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptObjectStateMutationAction{
            .targetKind = ScriptObjectStateTargetKind::ScenarioTeam,
            .targetName = *team,
            .mutation = ScriptObjectStateMutationKind::TeamRecruitable,
            .enabled = *available != 0,
        });
        return true;
    }
    if (name == "NAMED_FACE_NAMED" ||
        name == "NAMED_FACE_WAYPOINT" ||
        name == "TEAM_FACE_NAMED" ||
        name == "TEAM_FACE_WAYPOINT")
    {
        const auto actor = textParameter(
            instruction, 0, context, scriptName, name);
        const auto target = textParameter(
            instruction, 1, context, scriptName, name);
        if (!actor || !target ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *actor,
                instruction.serialized) ||
            rejectDynamicScriptContextSelector(
                context, scriptName, name, *target,
                instruction.serialized)) {
            return false;
        }
        const bool teamActor = name.starts_with("TEAM_");
        const bool waypointTarget = name.ends_with("_WAYPOINT");
        ScriptFacingAction action{
            .actorSelector = teamActor
                ? ScriptOrderActorSelector::ScenarioTeam
                : ScriptOrderActorSelector::NamedObjects,
            .teamName = teamActor ? *actor : container::String{},
            .targetKind = waypointTarget
                ? ScriptFacingTargetKind::Waypoint
                : ScriptFacingTargetKind::NamedObject,
            .targetName = *target,
        };
        if (!teamActor) action.actorNames.push_back(*actor);
        output.emplace_back(std::move(action));
        return true;
    }
    if (name == "NAMED_SET_HELD" || name == "NAMED_SET_REPULSOR" ||
        name == "TEAM_SET_REPULSOR")
    {
        const auto targetName = textParameter(instruction, 0, context, scriptName, name);
        const auto enabled = integerParameter(instruction, 1, context, scriptName, name);
        if (!targetName || !enabled ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *targetName,
                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptObjectStateMutationAction{
            .targetKind = name == "TEAM_SET_REPULSOR"
                ? ScriptObjectStateTargetKind::ScenarioTeam
                : ScriptObjectStateTargetKind::NamedObject,
            .targetName = *targetName,
            .mutation = name == "NAMED_SET_HELD"
                ? ScriptObjectStateMutationKind::Held
                : ScriptObjectStateMutationKind::Repulsor,
            .enabled = *enabled != 0,
        });
        return true;
    }
    if (name == "NAMED_SET_UNMANNED_STATUS" || name == "TEAM_SET_UNMANNED_STATUS")
    {
        const auto target = textParameter(instruction, 0, context, scriptName, name);
        if (!target || rejectDynamicScriptContextSelector(
                context, scriptName, name, *target, instruction.serialized)) return false;
        output.emplace_back(ScriptObjectStateMutationAction{
            .targetKind = name == "TEAM_SET_UNMANNED_STATUS"
                ? ScriptObjectStateTargetKind::ScenarioTeam
                : ScriptObjectStateTargetKind::NamedObject,
            .targetName = *target,
            .mutation = ScriptObjectStateMutationKind::Unmanned,
            .enabled = true,
        });
        return true;
    }
    if (name == "NAMED_SET_STEALTH_ENABLED" || name == "TEAM_SET_STEALTH_ENABLED")
    {
        const auto target = textParameter(instruction, 0, context, scriptName, name);
        const auto enabled = integerParameter(instruction, 1, context, scriptName, name);
        if (!target || !enabled || rejectDynamicScriptContextSelector(
                context, scriptName, name, *target, instruction.serialized)) return false;
        output.emplace_back(ScriptObjectStateMutationAction{
            .targetKind = name == "TEAM_SET_STEALTH_ENABLED"
                ? ScriptObjectStateTargetKind::ScenarioTeam
                : ScriptObjectStateTargetKind::NamedObject,
            .targetName = *target,
            .mutation = ScriptObjectStateMutationKind::StealthEnabled,
            .enabled = *enabled != 0,
        });
        return true;
    }
    if (name == "UNIT_AFFECT_OBJECT_PANEL_FLAGS" ||
        name == "TEAM_AFFECT_OBJECT_PANEL_FLAGS")
    {
        const auto target = textParameter(instruction, 0, context, scriptName, name);
        const auto flag = textParameter(instruction, 1, context, scriptName, name);
        const auto enabled = integerParameter(instruction, 2, context, scriptName, name);
        if (!target || !flag || !enabled || rejectDynamicScriptContextSelector(
                context, scriptName, name, *target, instruction.serialized)) return false;
        std::optional<ScriptObjectStateMutationKind> mutation;
        if (*flag == "Enabled")
            mutation = ScriptObjectStateMutationKind::PanelEnabled;
        else if (*flag == "Powered")
            mutation = ScriptObjectStateMutationKind::PanelPowered;
        else if (*flag == "Indestructible")
            mutation = ScriptObjectStateMutationKind::PanelIndestructible;
        else if (*flag == "Unsellable")
            mutation = ScriptObjectStateMutationKind::PanelUnsellable;
        else if (*flag == "Selectable")
            mutation = ScriptObjectStateMutationKind::PanelSelectable;
        else if (*flag == "AI Recruitable")
            mutation = ScriptObjectStateMutationKind::PanelAiRecruitable;
        else if (*flag == "Player Targetable")
            mutation = ScriptObjectStateMutationKind::PanelPlayerTargetable;
        if (!mutation) {
            context.diagnostic(
                LegacyScriptCompileDiagnosticSeverity::Warning,
                "script '" + container::String(scriptName) + "' action '" + name +
                    "' has unknown Object Panel flag '" + *flag + "'",
                instruction.serialized);
            return false;
        }
        output.emplace_back(ScriptObjectStateMutationAction{
            .targetKind = name == "TEAM_AFFECT_OBJECT_PANEL_FLAGS"
                ? ScriptObjectStateTargetKind::ScenarioTeam
                : ScriptObjectStateTargetKind::NamedObject,
            .targetName = *target,
            .mutation = *mutation,
            .enabled = *enabled != 0,
        });
        return true;
    }
    if (name == "SET_TRAIN_HELD")
    {
        const auto train = textParameter(instruction, 0, context, scriptName, name);
        const auto held = integerParameter(instruction, 1, context, scriptName, name);
        if (!train || !held || rejectDynamicScriptContextSelector(
                context, scriptName, name, *train, instruction.serialized)) return false;
        output.emplace_back(ScriptObjectStateMutationAction{
            .targetKind = ScriptObjectStateTargetKind::NamedObject,
            .targetName = *train,
            .mutation = ScriptObjectStateMutationKind::RailroadHeld,
            .enabled = *held != 0,
        });
        return true;
    }
    if (name == "NAMED_SET_BOOBYTRAPPED" || name == "TEAM_SET_BOOBYTRAPPED")
    {
        const auto objectType = textParameter(instruction, 0, context, scriptName, name);
        const auto target = textParameter(instruction, 1, context, scriptName, name);
        if (!objectType || !target || rejectDynamicScriptContextSelector(
                context, scriptName, name, *target, instruction.serialized)) return false;
        output.emplace_back(ScriptBoobyTrapAction{
            .targetKind = name == "TEAM_SET_BOOBYTRAPPED"
                ? ScriptObjectStateTargetKind::ScenarioTeam
                : ScriptObjectStateTargetKind::NamedObject,
            .targetName = *target,
            .templateName = *objectType,
        });
        return true;
    }
    if (name == "IDLE_ALL_UNITS" || name == "RESUME_SUPPLY_TRUCKING" ||
        name == "DELETE_ALL_UNMANNED")
    {
        ScriptGlobalObjectOperation operation =
            ScriptGlobalObjectOperation::IdleHumanUnits;
        if (name == "RESUME_SUPPLY_TRUCKING")
            operation = ScriptGlobalObjectOperation::ResumeHumanSupplyTrucking;
        else if (name == "DELETE_ALL_UNMANNED")
            operation = ScriptGlobalObjectOperation::DeleteAllUnmanned;
        output.emplace_back(ScriptGlobalObjectAction{.operation = operation});
        return true;
    }
    if (name == "OBJECTLIST_ADDOBJECTTYPE" || name == "OBJECTLIST_REMOVEOBJECTTYPE")
    {
        const auto listName = textParameter(instruction, 0, context, scriptName, name);
        const auto objectType = textParameter(instruction, 1, context, scriptName, name);
        if (!listName || !objectType)
            return false;
        output.emplace_back(ScriptModifyObjectTypeListAction{
            .listName = *listName,
            .objectType = *objectType,
            .add = name == "OBJECTLIST_ADDOBJECTTYPE",
        });
        return true;
    }
    if (name == "NAMED_RECEIVE_UPGRADE")
    {
        const auto objectName = textParameter(instruction, 0, context, scriptName, name);
        const auto upgradeName = textParameter(instruction, 1, context, scriptName, name);
        if (!objectName || !upgradeName ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *objectName,
                                               instruction.serialized)) {
            return false;
        }
        output.emplace_back(ScriptGrantObjectUpgradeAction{
            .objectName = *objectName,
            .upgradeName = *upgradeName,
        });
        return true;
    }
    if (name == "NAMED_DELETE" || name == "NAMED_KILL")
    {
        const auto objectName = textParameter(instruction, 0, context, scriptName, name);
        if (!objectName ||
            rejectDynamicScriptContextSelector(context, scriptName, name, *objectName, instruction.serialized))
            return false;
        output.emplace_back(ScriptDestroyNamedObjectAction{
            .objectName = *objectName,
            .forceKill = name == "NAMED_KILL",
        });
        return true;
    }
    if (name == "TEAM_DELETE" || name == "TEAM_DELETE_LIVING" ||
        name == "TEAM_KILL")
    {
        const auto team = textParameter(instruction, 0, context, scriptName, name);
        if (!team || rejectDynamicScriptContextSelector(
                context, scriptName, name, *team, instruction.serialized)) return false;
        ScriptLifecycleOperation operation = ScriptLifecycleOperation::Delete;
        if (name == "TEAM_DELETE_LIVING")
            operation = ScriptLifecycleOperation::DeleteLiving;
        else if (name == "TEAM_KILL")
            operation = ScriptLifecycleOperation::Kill;
        output.emplace_back(ScriptLifecycleAction{
            .targetKind = ScriptLifecycleTargetKind::ScenarioTeam,
            .operation = operation,
            .targetName = *team,
        });
        return true;
    }
    if (name == "PLAYER_KILL")
    {
        const auto player = textParameter(instruction, 0, context, scriptName, name);
        if (!player || rejectUnsupportedDynamicPlayerSelector(
                context, scriptName, name, *player, instruction.serialized)) return false;
        output.emplace_back(ScriptLifecycleAction{
            .targetKind = ScriptLifecycleTargetKind::Player,
            .operation = ScriptLifecycleOperation::Kill,
            .targetName = *player,
        });
        return true;
    }
    if (name == "NAMED_SET_EVAC_LEFT_OR_RIGHT")
    {
        const auto object = textParameter(
            instruction, 0, context, scriptName, name);
        const auto disposition = integerParameter(
            instruction, 1, context, scriptName, name);
        if (!object || !disposition || rejectDynamicScriptContextSelector(
                context, scriptName, name, *object,
                instruction.serialized)) return false;
        output.emplace_back(ScriptContainmentAction{
            .kind = ScriptContainmentActionKind::SetEvacuationDisposition,
            .targetName = *object,
            .evacuationDisposition = *disposition,
        });
        return true;
    }
    return std::nullopt;
}

} // namespace engine::script::legacy::detail
