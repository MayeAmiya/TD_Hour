#pragma once

#include "core/container/container_types.h"
#include "core/ecs/ObjectId.h"
#include "game/base/FrameCommitResult.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/session/object/GameSessionObjectContracts.h"
#include "game/session/command/OrderContracts.h"
#include "game/session/script/GameSessionScriptContracts.h"
#include "game/session/transaction/GameSessionBuildPlacementContracts.h"
#include "game/session/query/GameSessionAIOrderPolicy.h"
#include "game/script/runtime/ScriptRuntime.h"
#include "game/scenario/runtime/ScenarioDefinition.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace game {
struct ObjectArchetype;
}

namespace engine {

class GameSessionAIState;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;
class GameSessionObjectEventState;
class GameSessionScriptOrderAdmissionTransactions;
class GameSessionScriptPriorityBuildTransactions;
class GameSessionScriptScenarioPlanTransactions;
class GameSessionFrameCommitState;

// Explicit lifecycle barrier capability over stable state partitions.
class GameSessionLifecycleTransactionPort final {
public:
    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return content != nullptr && world != nullptr && ai != nullptr &&
            presentation != nullptr && objectEvents != nullptr &&
            frame != nullptr;
    }
    void publishObjectFxEvents() const;
    void publishTechBuildingEvents() const;
    [[nodiscard]] bool consumeObjectLifecycleEvents() const;
    [[nodiscard]] const FrameCommitResult* frameCommitResult() const;
    [[nodiscard]] bool canCreateScriptObjectNamed(
        container::StringView name) const;
    [[nodiscard]] bool applyObjectDifficultyBonusPolicy(
        ObjectId object, bool receiving, uint64_t tick) const;
    void resolveQueuedObjectDamage() const;
    void drainGameplayTransactions() const;
    [[nodiscard]] bool objectAIOwnsMoveStop(ObjectId object) const noexcept;
    [[nodiscard]] GameSessionObjectSpawnResult spawnObject(
        ObjectSpawnRequest request) const;
    [[nodiscard]] bool destroyObject(ObjectId object) const;
    [[nodiscard]] bool requestDestroyObject(
        ObjectId object, ObjectDestroyReason reason, uint64_t tick) const;
    [[nodiscard]] size_t evacuateConstructionFootprint(
        ObjectId structure, ObjectId builder, uint64_t tick) const;
    [[nodiscard]] bool raiseSimulationFault(SimulationFault fault) const;

private:
    friend GameSessionLifecycleTransactionPort makeLifecycleTransactionPort(
        GameSessionContentStartState&, GameSessionWorldState&,
        GameSessionAIState&, GameSessionScriptPresentationState&,
        GameSessionObjectEventState&, GameSessionFrameCommitState&) noexcept;

    GameSessionLifecycleTransactionPort(
        GameSessionContentStartState& contentValue,
        GameSessionWorldState& worldValue,
        GameSessionAIState& aiValue,
        GameSessionScriptPresentationState& presentationValue,
        GameSessionObjectEventState& objectEventsValue,
        GameSessionFrameCommitState& frameValue) noexcept
        : content(&contentValue), world(&worldValue), ai(&aiValue),
          presentation(&presentationValue), objectEvents(&objectEventsValue),
          frame(&frameValue) {}

    GameSessionContentStartState* content = nullptr;
    GameSessionWorldState* world = nullptr;
    GameSessionAIState* ai = nullptr;
    GameSessionScriptPresentationState* presentation = nullptr;
    GameSessionObjectEventState* objectEvents = nullptr;
    GameSessionFrameCommitState* frame = nullptr;
};

[[nodiscard]] GameSessionLifecycleTransactionPort
makeLifecycleTransactionPort(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation,
    GameSessionObjectEventState& objectEvents,
    GameSessionFrameCommitState& frame) noexcept;

class GameSessionProductionPolicyPort final {
public:
    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return content != nullptr && presentation != nullptr;
    }
    [[nodiscard]] bool admitsObjectBuildability(
        PlayerId player, const game::ObjectArchetype& product,
        bool& ignorePrerequisites) const noexcept;

private:
    friend GameSessionProductionPolicyPort makeProductionPolicyPort(
        const GameSessionContentStartState&,
        const GameSessionScriptPresentationState&) noexcept;

    GameSessionProductionPolicyPort(
        const GameSessionContentStartState& contentValue,
        const GameSessionScriptPresentationState& presentationValue) noexcept
        : content(&contentValue), presentation(&presentationValue) {}

    const GameSessionContentStartState* content = nullptr;
    const GameSessionScriptPresentationState* presentation = nullptr;
};

[[nodiscard]] GameSessionProductionPolicyPort
makeProductionPolicyPort(
    const GameSessionContentStartState& content,
    const GameSessionScriptPresentationState& presentation) noexcept;

class GameSessionOrderAdmissionPolicyPort final {
public:
    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return content != nullptr && world != nullptr &&
            presentation != nullptr;
    }
    [[nodiscard]] bool rejectsOrdersWhileSleeping(ObjectId object) const {
        return aiPolicy && aiPolicy->rejectsOrdersWhileSleeping(object);
    }
    [[nodiscard]] container::StringView effectiveCommandBarButton(
        ObjectId object, size_t slot) const;
    [[nodiscard]] bool canReceiveUpgrade(
        ObjectId object, container::StringView upgrade) const;
    [[nodiscard]] GameSessionProductionCommandResult queuePlayerUpgrade(
        ObjectId producer, PlayerId player, container::StringView upgrade,
        uint32_t sequence, uint64_t tick,
        ObjectUpgradeProductionAdmission admission =
            ObjectUpgradeProductionAdmission::PlayerCommand) const;
    [[nodiscard]] GameSessionProductionCommandResult queueProduction(
        ObjectId producer, PlayerId player, container::StringView product,
        uint32_t sequence, uint64_t tick) const;
    [[nodiscard]] bool attitudePromotesMove(ObjectId object) const {
        return aiPolicy && aiPolicy->attitudePromotesMove(object);
    }

private:
    friend class GameSessionScriptOrderAdmissionTransactions;
    friend GameSessionOrderAdmissionPolicyPort makeOrderAdmissionPolicyPort(
        GameSessionContentStartState&, GameSessionWorldState&,
        GameSessionScriptPresentationState&,
        GameSessionLifecycleTransactionPort) noexcept;

    GameSessionOrderAdmissionPolicyPort(
        GameSessionContentStartState& contentValue,
        GameSessionWorldState& worldValue,
        GameSessionScriptPresentationState& presentationValue,
        GameSessionLifecycleTransactionPort lifecycleValue,
        GameSessionAIOrderPolicy aiPolicyValue) noexcept
        : content(&contentValue), world(&worldValue),
          presentation(&presentationValue),
          lifecycle(std::move(lifecycleValue)),
          aiPolicy(std::move(aiPolicyValue)) {}

    GameSessionContentStartState* content = nullptr;
    GameSessionWorldState* world = nullptr;
    GameSessionScriptPresentationState* presentation = nullptr;
    GameSessionLifecycleTransactionPort lifecycle;
    std::optional<GameSessionAIOrderPolicy> aiPolicy;
};

[[nodiscard]] GameSessionOrderAdmissionPolicyPort
makeOrderAdmissionPolicyPort(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionScriptPresentationState& presentation,
    GameSessionLifecycleTransactionPort lifecycle) noexcept;

class GameSessionScenarioTransactionPort final {
public:
    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return content != nullptr && world != nullptr && ai != nullptr &&
            presentation != nullptr;
    }
    [[nodiscard]] const scenario::ScriptTeamDefinition* findTeam(
        container::StringView alias) const;
    [[nodiscard]] bool transferObjectToTeam(
        ObjectId object, ObjectTeamId team, uint64_t tick) const;
    [[nodiscard]] OrderExecutionResult executeScriptOrder(
        const ScriptOrderIntent& order) const;
    [[nodiscard]] std::optional<PlayerId> currentEnemyPlayer(
        PlayerId player) const;
    [[nodiscard]] GameSessionBuildPlacementLegalityEvaluation
    evaluateBuildPlacement(
        ObjectId source, const LogicFixedVec3& position,
        math::q32_32 yaw, PlayerId player,
        const game::ObjectArchetype& product,
        bool finalConfirmation) const;
    [[nodiscard]] std::optional<ScriptCommandButtonSelectionResult>
    selectCommandButton(
        container::Span<const ObjectId> actors,
        container::StringView button,
        script::ScriptCommandButtonActorPolicy actorPolicy,
        math::q32_32 percentage,
        script::ScriptCommandButtonTargetKind targetKind,
        ObjectId target, container::StringView filter,
        container::Span<const container::String> targetTypes,
        std::optional<math::q32_32> range) const;
    [[nodiscard]] OrderExecutionResult executeCommandButton(
        const ScriptOrderIntent& order, bool requireInSet) const;
    [[nodiscard]] bool raiseSimulationFault(SimulationFault fault) const;

private:
    friend class GameSessionScriptPriorityBuildTransactions;
    friend class GameSessionScriptScenarioPlanTransactions;
    friend GameSessionScenarioTransactionPort makeScenarioTransactionPort(
        GameSessionContentStartState&, GameSessionWorldState&,
        GameSessionAIState&, GameSessionScriptPresentationState&,
        GameSessionLifecycleTransactionPort) noexcept;

    GameSessionScenarioTransactionPort(
        GameSessionContentStartState& contentValue,
        GameSessionWorldState& worldValue,
        GameSessionAIState& aiValue,
        GameSessionScriptPresentationState& presentationValue,
        GameSessionLifecycleTransactionPort lifecycleValue,
        GameSessionProductionPolicyPort productionPolicyValue,
        GameSessionOrderAdmissionPolicyPort orderPolicyValue) noexcept
        : content(&contentValue), world(&worldValue), ai(&aiValue),
          presentation(&presentationValue),
          lifecycle(std::move(lifecycleValue)),
          productionPolicy(std::move(productionPolicyValue)),
          orderPolicy(std::move(orderPolicyValue)) {}

    GameSessionContentStartState* content = nullptr;
    GameSessionWorldState* world = nullptr;
    GameSessionAIState* ai = nullptr;
    GameSessionScriptPresentationState* presentation = nullptr;
    GameSessionLifecycleTransactionPort lifecycle;
    GameSessionProductionPolicyPort productionPolicy;
    GameSessionOrderAdmissionPolicyPort orderPolicy;
};

[[nodiscard]] GameSessionScenarioTransactionPort
makeScenarioTransactionPort(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation,
    GameSessionLifecycleTransactionPort lifecycle) noexcept;

} // namespace engine
