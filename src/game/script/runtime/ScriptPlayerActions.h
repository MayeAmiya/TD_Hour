#pragma once

#include "game/script/runtime/ScriptTypes.h"

namespace engine::script
{

struct ScriptSetPlayerCashAction final
{
    container::String player;
    int64_t value = 0;
};

struct ScriptAdjustPlayerCashAction final
{
    container::String player;
    int64_t delta = 0;
};

// PLAYER_SELL_EVERYTHING is a player-scoped request.  The runtime transports
// only the authored player selector; GameSession owns structure selection,
// refund settlement and delayed lifecycle destruction.
struct ScriptPlayerSellEverythingAction final
{
    container::String player;
};

// PLAYER_REPAIR_NAMED_STRUCTURE is a player-AI request in RefCode. Runtime
// carries only the authored operands; GameSession owns builder selection and
// ObjectBuilder repair admission.
struct ScriptPlayerRepairStructureAction final
{
    container::String player;
    ScriptObjectSelector structure;
};

// AI_PLAYER_BUILD_UPGRADE is a player-AI production request. The script layer
// retains authored names; GameSession resolves a stable owned producer and
// uses the ordinary production/upgrade transaction.
struct ScriptPlayerBuildUpgradeAction final
{
    container::String player;
    container::String upgrade;
};

struct ScriptPlayerBuildObjectNearTeamAction final
{
    container::String player;
    container::String objectType;
    container::String teamName;
};

struct ScriptPlayerBuildSupplyCenterAction final
{
    container::String player;
    container::String objectType;
    int32_t minimumSupplies = 0;
};

struct ScriptSkirmishBuildBuildingAction final
{
    container::String objectType;
};

enum class ScriptSkirmishApproachOperation : uint8_t
{
    FollowPath,
    MoveToPath,
};

struct ScriptSkirmishApproachAction final
{
    ScriptSkirmishApproachOperation operation =
        ScriptSkirmishApproachOperation::FollowPath;
    container::String teamName;
    container::String pathPrefix;
    bool asTeam = false;
};

struct ScriptSkirmishPerimeterBuildAction final
{
    container::String objectType;
    bool flank = false;
    bool useFactionBaseDefense = false;
};

struct ScriptSkirmishFireSpecialPowerAtMostCostAction final
{
    container::String player;
    container::String specialPower;
};

struct ScriptSkirmishAttackNearestValueGroupAction final
{
    container::String teamName;
    ScriptComparison comparison = ScriptComparison::GreaterEqual;
    int32_t minimumValue = 0;
};

struct ScriptSkirmishMostValuableCommandButtonAction final
{
    container::String teamName;
    container::String buttonName;
    math::q32_32 range{};
    bool allTeamMembers = false;
};

enum class ScriptPlayerConstructionOperation : uint8_t
{
    SetBaseEnabled,
    SetUnitsEnabled,
    SetFactoryTypeEnabled,
    SetTeamDelaySeconds,
};

struct ScriptPlayerConstructionAction final
{
    ScriptPlayerConstructionOperation operation =
        ScriptPlayerConstructionOperation::SetBaseEnabled;
    container::String player;
    container::String factoryType;
    int32_t value = 0;
    bool enabled = true;
};

enum class ScriptObjectBuildability : uint8_t
{
    Yes = 0,
    IgnorePrerequisites = 1,
    No = 2,
    OnlyByAi = 3,
};

struct ScriptObjectBuildabilityAction final
{
    container::String objectType;
    ScriptObjectBuildability buildability = ScriptObjectBuildability::Yes;
};

enum class ScriptScienceAvailability : uint8_t
{
    Available,
    Disabled,
    Hidden,
};

// Preserve RefCode's relation ordinal semantics independently of the modern
// PlayerRelationship enum: legacy 0=Enemies, 1=Neutral, 2=Allies whereas the
// modern enum has a different declaration order.
enum class ScriptPlayerRelationship : uint8_t
{
    Enemies,
    Neutral,
    Allies,
};

struct ScriptSetPlayerScienceAvailabilityAction final
{
    container::String player;
    container::String science;
    ScriptScienceAvailability availability = ScriptScienceAvailability::Available;
};

struct ScriptSetPlayerRelationshipAction final
{
    container::String sourcePlayer;
    container::String targetPlayer;
    ScriptPlayerRelationship relationship = ScriptPlayerRelationship::Enemies;
};

enum class ScriptRelationshipEndpointKind : uint8_t
{
    ScenarioTeam,
    Player,
};

enum class ScriptRelationshipOverrideOperation : uint8_t
{
    Set,
    Remove,
    RemoveAllFromTeam,
};

struct ScriptRelationshipOverrideAction final
{
    ScriptRelationshipEndpointKind sourceKind =
        ScriptRelationshipEndpointKind::ScenarioTeam;
    ScriptRelationshipEndpointKind targetKind =
        ScriptRelationshipEndpointKind::ScenarioTeam;
    ScriptRelationshipOverrideOperation operation =
        ScriptRelationshipOverrideOperation::Set;
    container::String sourceName;
    container::String targetName;
    ScriptPlayerRelationship relationship =
        ScriptPlayerRelationship::Neutral;
};

enum class ScriptGlobalCombatPolicy : uint8_t
{
    ObjectDifficultyBonuses,
    ChooseVictimAlwaysNormal,
};

struct ScriptGlobalCombatPolicyAction final
{
    ScriptGlobalCombatPolicy policy =
        ScriptGlobalCombatPolicy::ObjectDifficultyBonuses;
    bool enabled = false;
};

// Campaign/player progression is authoritative player data, but it has no
// dependency on a live Object, Team or production module.  Keep the legacy
// operation explicit rather than encoding it as a loosely-typed counter so
// the bridge can preserve the distinct rank-limit and score-screen rules.
enum class ScriptPlayerProgressionOperation : uint8_t
{
    AddSkillPoints,
    AdjustRankLevel,
    SetRankLevel,
    SetRankLevelLimit,
    GrantScience,
    // A science purchase is intentionally represented separately from a
    // grant.  The session may decline it until a frozen science catalog can
    // supply its cost/prerequisite policy; it must never silently become a
    // free grant.
    PurchaseScience,
    SelectSkillset,
    SetExperienceMultiplier,
    ExcludeFromScoreScreen,
};

struct ScriptPlayerProgressionAction final
{
    ScriptPlayerProgressionOperation operation = ScriptPlayerProgressionOperation::AddSkillPoints;
    container::String player;
    container::String science;
    int32_t integerValue = 0;
    math::q32_32 realValue{};
};

} // namespace engine::script
