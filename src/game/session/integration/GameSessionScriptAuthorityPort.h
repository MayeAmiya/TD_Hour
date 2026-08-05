#pragma once

#include "core/ecs/registry.h"
#include "game/session/integration/GameSessionScriptQueryPort.h"
#include "game/session/transaction/GameSessionObjectStateTransactions.h"
#include "game/session/transaction/GameSessionObjectOwnershipTransactions.h"
#include "game/session/transaction/GameSessionObjectProgressionTransactions.h"
#include "game/session/transaction/GameSessionObjectDamageTransactions.h"
#include "game/session/transaction/GameSessionObjectLifecycleTransactions.h"
#include "game/session/transaction/GameSessionPlayerStateTransactions.h"
#include "game/session/transaction/GameSessionContainmentTransactions.h"
#include "game/session/transaction/GameSessionContainmentPlanTransactions.h"
#include "game/session/transaction/GameSessionScriptOrderTransactions.h"
#include "game/session/transaction/GameSessionScriptOrderAdmissionTransactions.h"
#include "game/session/transaction/GameSessionScriptScenarioPlanTransactions.h"
#include "game/script/runtime/ScriptRuntime.h"

namespace engine {
class GameContentSnapshot;
struct GameStartInfo;
class ObjectOwnershipIndex;
class ObjectTeamRegistry;
class PlayerRegistry;
namespace ai {
class ObjectAIRuntime;
}
namespace scenario {
class MissionState;
}
}

namespace game::terrain {
class TerrainLogic;
}

namespace engine::script {

class GameSessionScriptAuthorityPort;

namespace detail {
[[nodiscard]] bool applyObjectEffect(
    GameSessionScriptAuthorityPort& port, const ScriptEffect& effect);
[[nodiscard]] bool applyPlayerPolicyEffect(
    GameSessionScriptAuthorityPort& port, const ScriptEffect& effect);
[[nodiscard]] bool applyOrderAndAiEffect(
    GameSessionScriptAuthorityPort& port, const ScriptEffect& effect);
} // namespace detail

// Commits deterministic gameplay effects in source order. Presentation-only
// effects are deliberately outside this capability.
class GameSessionScriptAuthorityPort final {
public:
    GameSessionScriptAuthorityPort(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionAIState& ai,
        GameSessionScriptPresentationState& presentation,
        GameSessionLifecycleTransactionPort lifecycle,
        GameSessionScriptQueryPort& queries,
        uint64_t confirmedTick) noexcept;

    [[nodiscard]] bool applyObjectOrPlayerPolicy(const ScriptEffect& effect);
    [[nodiscard]] bool applyOrderAndAi(const ScriptEffect& effect);
    [[nodiscard]] int32_t integerInclusive(int32_t lo, int32_t hi) noexcept;
    void resolveQueuedObjectDamage();
    void emitDiagnostic(
        const ScriptEffectHeader& header, container::String text);

private:
    PlayerRegistry& players() noexcept;
    ecs::registry& registry() noexcept;
    [[nodiscard]] const GameContentSnapshot& contentSnapshot() const noexcept;
    game::terrain::TerrainLogic& terrain() noexcept;
    [[nodiscard]] const ObjectOwnershipIndex& ownership() const noexcept;
    ObjectTeamRegistry& objectTeams() noexcept;
    [[nodiscard]] const GameStartInfo& startInfo() const noexcept;
    scenario::MissionState& missionState() noexcept;
    ai::ObjectAIRuntime& objectAIRuntime() noexcept;
    void setTimeFrozen(bool frozen) noexcept;
    void setScoreAccumulationEnabled(bool enabled) noexcept;
    [[nodiscard]] bool setHulkLifetimeOverride(
        math::q32_32 seconds) noexcept;
    [[nodiscard]] bool setToppleDirection(
        container::StringView objectName, LogicFixedVec3 direction);
    [[nodiscard]] bool applyMapAuthority(
        const ScriptMapPresentationEffect& effect,
        uint64_t confirmedTick, uint32_t sourceScriptId, uint32_t ordinal,
        PlayerId currentPlayer, container::StringView currentPlayerAlias);
    [[nodiscard]] bool applyWaterAuthority(
        const ScriptWaterEffect& effect, const ScriptEffectHeader& header);
    [[nodiscard]] std::optional<ecs::entity> entityFromId(
        ObjectId object) const;

    friend bool detail::applyObjectEffect(
        GameSessionScriptAuthorityPort& port, const ScriptEffect& effect);
    friend bool detail::applyPlayerPolicyEffect(
        GameSessionScriptAuthorityPort& port, const ScriptEffect& effect);
    friend bool detail::applyOrderAndAiEffect(
        GameSessionScriptAuthorityPort& port, const ScriptEffect& effect);

    [[nodiscard]] std::optional<PlayerId> resolvePlayer(
        container::StringView name, PlayerId currentPlayer,
        container::StringView currentPlayerAlias) const noexcept;
    [[nodiscard]] std::optional<ObjectTeamId> resolveEffectTeam(
        container::StringView name,
        const ScriptEffectHeader& header) const noexcept;
    [[nodiscard]] bool acceptsAuthorityTick(uint64_t confirmedTick) const noexcept;
    [[nodiscard]] size_t setPlayerFactoryTypeEnabled(
        PlayerId player, container::StringView factoryType, bool enabled,
        uint64_t confirmedTick);
    [[nodiscard]] bool setObjectBuildability(
        container::StringView objectType,
        game::ObjectBuildabilityStatus buildability,
        uint64_t confirmedTick);
    [[nodiscard]] bool setTeamToTeamRelationship(
        ObjectTeamId source, ObjectTeamId target,
        std::optional<PlayerRelationship> relationship,
        uint64_t confirmedTick);
    [[nodiscard]] bool setTeamToPlayerRelationship(
        ObjectTeamId source, PlayerId target,
        std::optional<PlayerRelationship> relationship,
        uint64_t confirmedTick);
    [[nodiscard]] bool clearTeamRelationships(
        ObjectTeamId source, uint64_t confirmedTick);
    [[nodiscard]] bool setPlayerToTeamRelationship(
        PlayerId source, ObjectTeamId target,
        std::optional<PlayerRelationship> relationship,
        uint64_t confirmedTick);
    [[nodiscard]] bool canCreateNamedObject(
        container::StringView scriptName) const noexcept;
    [[nodiscard]] std::optional<ObjectTeamId> ensureScenarioTeamForCreate(
        container::StringView alias, uint64_t confirmedTick);
    [[nodiscard]] bool setTeamActive(
        ObjectTeamId team, bool active, uint64_t confirmedTick);
    [[nodiscard]] bool applyAttackPrioritySet(
        ObjectId object, container::StringView setName);
    [[nodiscard]] size_t applyTeamAttackPrioritySet(
        ObjectTeamId team, container::StringView setName);
    [[nodiscard]] size_t setTeamWanderInPlace(
        ObjectTeamId team, uint64_t confirmedTick);
    [[nodiscard]] size_t setTeamCommandButtonHunt(
        ObjectTeamId team, container::StringView commandButton,
        uint64_t confirmedTick);
    [[nodiscard]] bool mutateAttackPrioritySet(
        ScriptAttackPriorityMutationKind mutation,
        container::StringView setName,
        container::Span<const container::String> selectors,
        int32_t priority);
    [[nodiscard]] size_t destroyAllUnmanned(uint64_t confirmedTick);
    void projectTeamRelationshipPolicy(ObjectTeamId team);
    void setRankLevelLimit(int32_t limit) noexcept;
    [[nodiscard]] int32_t rankLevelLimit() const noexcept;
    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionAIState& m_ai;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionScriptQueryPort& m_queries;
    GameSessionObjectStateTransactions m_objectTransactions;
    GameSessionObjectOwnershipTransactions m_ownershipTransactions;
    GameSessionObjectProgressionTransactions m_progressionTransactions;
    GameSessionObjectDamageTransactions m_damageTransactions;
    GameSessionObjectLifecycleTransactions m_lifecycleTransactions;
    GameSessionPlayerStateTransactions m_playerTransactions;
    GameSessionContainmentTransactions m_containmentTransactions;
    GameSessionContainmentPlanTransactions m_containmentPlanTransactions;
    GameSessionScriptOrderTransactions m_orderTransactions;
    GameSessionScriptOrderAdmissionTransactions m_orderAdmissionTransactions;
    GameSessionScriptScenarioPlanTransactions m_scenarioPlanTransactions;
    uint64_t m_confirmedTick = 0;
};

} // namespace engine::script
