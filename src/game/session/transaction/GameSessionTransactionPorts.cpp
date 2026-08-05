#include "game/session/transaction/GameSessionTransactionPorts.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/command/CommandSetStore.h"

#include "game/session/frame/GameSessionObjectEventPublisher.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/session/transaction/GameSessionObjectProductionTransactions.h"
#include "game/session/transaction/GameSessionObjectOwnershipTransactions.h"
#include "game/session/transaction/GameSessionBuildPlacementEvaluator.h"
#include "game/session/transaction/GameSessionScriptOrderAdmissionTransactions.h"
#include "game/session/transaction/GameSessionObjectLifecycleTransactions.h"
#include "game/session/transaction/GameSessionObjectDamageTransactions.h"
#include "game/session/transaction/GameSessionLifecycleCascadeTransactions.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/object/simulation/economy/ObjectUpgrade.h"
#include "game/session/weapon/GameSessionGameplayTransactionDrain.h"

namespace engine {

void GameSessionLifecycleTransactionPort::publishObjectFxEvents() const {
    if (!*this) return;
    GameSessionGameplayPublicationPort publication{
        *content, *world, *presentation, *frame};
    GameSessionObjectEventPublisher{
        *content, *world, *presentation, publication}
        .publishFx();
}

void GameSessionLifecycleTransactionPort::publishTechBuildingEvents() const {
    if (!*this) return;
    GameSessionGameplayPublicationPort publication{
        *content, *world, *presentation, *frame};
    GameSessionObjectEventPublisher{
        *content, *world, *presentation, publication}
        .publishTechAndBeacon();
}

bool GameSessionLifecycleTransactionPort::consumeObjectLifecycleEvents()
    const {
    if (!*this) return false;
    return GameSessionLifecycleCascadeTransactions{
        *content, *world, *ai, *presentation, *objectEvents, *frame}
        .consume();
}

const FrameCommitResult*
GameSessionLifecycleTransactionPort::frameCommitResult() const {
    return frame ? &frame->m_result : nullptr;
}

bool GameSessionLifecycleTransactionPort::canCreateScriptObjectNamed(
    container::StringView name) const {
    if (!presentation || !world || name.empty()) return true;
    const std::optional<ObjectId> existing =
        presentation->m_scriptObjects.liveNamedObject(name);
    if (!existing) return true;
    const std::optional<ecs::entity> entity =
        world->m_objects.entityFromId(*existing);
    if (!entity) return true;
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(world->m_registry, *entity);
    return health && health->effectivelyDead;
}

bool GameSessionLifecycleTransactionPort::applyObjectDifficultyBonusPolicy(
    ObjectId object, bool receiving, uint64_t tick) const {
    if (!content || !world || !ai || !presentation) return false;
    return GameSessionObjectOwnershipTransactions{
        *content, *world, *ai, *presentation, *this}
        .applyDifficultyBonusPolicy(object, receiving, tick);
}

void GameSessionLifecycleTransactionPort::resolveQueuedObjectDamage() const {
    if (!content || !world || !presentation) return;
    GameSessionObjectDamageTransactions{
        *content, *world, *presentation, *this}
        .resolveQueuedObjectDamage();
}

void GameSessionLifecycleTransactionPort::drainGameplayTransactions() const {
    if (!*this) return;
    detail::GameSessionGameplayTransactionDrain::run(
        *content, *world, *ai, *presentation, *objectEvents, *frame, *this);
}

bool GameSessionLifecycleTransactionPort::objectAIOwnsMoveStop(
    ObjectId object) const noexcept {
    if (!ai || !object) return false;
    const std::optional<ai::AIActorHandle> actor =
        ai->m_objectAI.find(object);
    const ai::ObjectAIOrderAdmissionStorage* admission = actor
        ? ai->m_objectAI.orderAdmission(*actor) : nullptr;
    ai::ObjectAIOrderAdmissionSlotView slot;
    return actor && admission &&
        admission->readSlot(actor->slot, slot) ==
            ai::ObjectAIOrderAdmissionStatus::Success &&
        ai::hasObjectAIOrderCapability(
            slot.capabilities, ai::ObjectAIOrderCapability::MoveStop);
}

GameSessionObjectSpawnResult GameSessionLifecycleTransactionPort::spawnObject(
    ObjectSpawnRequest request) const {
    if (!content || !world || !presentation) return {};
    return GameSessionObjectLifecycleTransactions{
        *content, *world, *presentation, *this, objectEvents}
        .spawnObject(std::move(request));
}

bool GameSessionLifecycleTransactionPort::destroyObject(
    ObjectId object) const {
    if (!content || !world || !presentation) return false;
    return GameSessionObjectLifecycleTransactions{
        *content, *world, *presentation, *this, objectEvents}
        .destroyObject(object);
}

bool GameSessionLifecycleTransactionPort::requestDestroyObject(
    ObjectId object, ObjectDestroyReason reason, uint64_t tick) const {
    if (!content || !world || !presentation) return false;
    return GameSessionObjectLifecycleTransactions{
        *content, *world, *presentation, *this, objectEvents}
        .requestDestroyObject(object, reason, tick);
}

size_t GameSessionLifecycleTransactionPort::evacuateConstructionFootprint(
    ObjectId structure, ObjectId builder, uint64_t tick) const {
    if (!content || !world || !presentation) return 0;
    return GameSessionObjectLifecycleTransactions{
        *content, *world, *presentation, *this, objectEvents}
        .evacuateConstructionFootprint(structure, builder, tick);
}

bool GameSessionLifecycleTransactionPort::raiseSimulationFault(
    SimulationFault fault) const {
    if (!content || !world || !presentation || !frame) return false;
    return GameSessionGameplayPublicationPort{
        *content, *world, *presentation, *frame}
        .raiseSimulationFault(fault);
}

GameSessionLifecycleTransactionPort makeLifecycleTransactionPort(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation,
    GameSessionObjectEventState& objectEvents,
    GameSessionFrameCommitState& frame) noexcept {
    return GameSessionLifecycleTransactionPort{
        content, world, ai, presentation, objectEvents, frame};
}

GameSessionProductionPolicyPort makeProductionPolicyPort(
    const GameSessionContentStartState& content,
    const GameSessionScriptPresentationState& presentation) noexcept {
    return GameSessionProductionPolicyPort{content, presentation};
}

bool GameSessionProductionPolicyPort::admitsObjectBuildability(
    PlayerId player, const game::ObjectArchetype& product,
    bool& ignorePrerequisites) const noexcept {
    ignorePrerequisites = false;
    if (!content || !presentation) return false;
    game::ObjectBuildabilityStatus status = product.templateData.buildability;
    const auto found = presentation->m_scriptObjectBuildabilityOverrides.find(
        product.templateData.name);
    if (found != presentation->m_scriptObjectBuildabilityOverrides.end())
        status = found->second;
    switch (status) {
    case game::ObjectBuildabilityStatus::Yes:
        return true;
    case game::ObjectBuildabilityStatus::IgnorePrerequisites:
        ignorePrerequisites = true;
        return true;
    case game::ObjectBuildabilityStatus::No:
        return false;
    case game::ObjectBuildabilityStatus::OnlyByAi: {
        const PlayerState* owner = content->m_players.get(player);
        return owner && owner->controller == PlayerControllerKind::Ai;
    }
    }
    return false;
}

container::StringView
GameSessionOrderAdmissionPolicyPort::effectiveCommandBarButton(
    ObjectId object, size_t slot) const {
    if (!content || !world || !presentation ||
        slot >= game::COMMAND_SET_SLOT_COUNT) return {};
    const std::optional<ecs::entity> entity =
        world->m_objects.entityFromId(object);
    if (!entity) return {};
    const container::StringView commandSetName =
        effectiveObjectCommandSetName(world->m_registry, *entity);
    const game::CommandSetTemplate* commandSet =
        content->m_contentSnapshot.findCommandSet(commandSetName);
    if (!commandSet) return {};
    return presentation->m_scriptCommandBarOverrides.effectiveButtonName(
        commandSet->name, slot, commandSet->commands[slot]);
}

bool GameSessionOrderAdmissionPolicyPort::canReceiveUpgrade(
    ObjectId object, container::StringView upgrade) const {
    if (!content || !world || !content->m_active || !object ||
        upgrade.empty() || world->m_objects.isPendingDestroy(object)) {
        return false;
    }
    const UpgradeCatalog* catalog =
        content->m_contentSnapshot.upgradeCatalog();
    const UpgradeDefinition* definition = catalog ? catalog->find(upgrade) : nullptr;
    const std::optional<ecs::entity> entity =
        world->m_objects.entityFromId(object);
    if (!definition || !entity) return false;
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(world->m_registry, *entity);
    const PlayerState* player =
        owner ? content->m_players.get(owner->player) : nullptr;
    return player && world->m_objectSimulation.canObjectReceiveUpgrade(
        world->m_registry, *entity, player->upgrades.completed,
        definition->id);
}

GameSessionProductionCommandResult
GameSessionOrderAdmissionPolicyPort::queuePlayerUpgrade(
    ObjectId producer, PlayerId player, container::StringView upgrade,
    uint32_t sequence, uint64_t tick,
    ObjectUpgradeProductionAdmission admission) const {
    if (!content || !world || !presentation) return {};
    return GameSessionObjectProductionTransactions{
        *content, *world, *presentation,
        makeProductionPolicyPort(*content, *presentation), lifecycle}
        .queuePlayerUpgrade(
            producer, player, upgrade, sequence, tick, admission);
}

GameSessionProductionCommandResult
GameSessionOrderAdmissionPolicyPort::queueProduction(
    ObjectId producer, PlayerId player, container::StringView product,
    uint32_t sequence, uint64_t tick) const {
    if (!content || !world || !presentation) return {};
    return GameSessionObjectProductionTransactions{
        *content, *world, *presentation,
        makeProductionPolicyPort(*content, *presentation), lifecycle}
        .queueProduction(producer, player, product, sequence, tick);
}

GameSessionOrderAdmissionPolicyPort makeOrderAdmissionPolicyPort(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionScriptPresentationState& presentation,
    GameSessionLifecycleTransactionPort lifecycle) noexcept {
    return GameSessionOrderAdmissionPolicyPort{
        content, world, presentation, std::move(lifecycle),
        GameSessionAIOrderPolicy{content, world, presentation}};
}

const scenario::ScriptTeamDefinition*
GameSessionScenarioTransactionPort::findTeam(
    container::StringView alias) const {
    if (!presentation || !presentation->m_scenarioDefinition || alias.empty())
        return nullptr;
    const scenario::OwnerReference reference =
        presentation->m_scenarioDefinition->resolveOwner(alias);
    if (reference.kind != scenario::OwnerReferenceKind::ScriptTeam ||
        !reference.scriptTeam) return nullptr;
    return presentation->m_scenarioDefinition->findScriptTeam(
        reference.scriptTeam);
}

bool GameSessionScenarioTransactionPort::transferObjectToTeam(
    ObjectId object, ObjectTeamId team, uint64_t tick) const {
    if (!content || !world || !ai || !presentation) return false;
    return GameSessionObjectOwnershipTransactions{
        *content, *world, *ai, *presentation, lifecycle}
        .transferObjectToTeam(object, team, tick);
}

OrderExecutionResult GameSessionScenarioTransactionPort::executeScriptOrder(
    const ScriptOrderIntent& order) const {
    if (!content || !world || !ai || !presentation) return {};
    return GameSessionScriptOrderAdmissionTransactions{
        *content, *world, *ai, *presentation, orderPolicy}
        .executeScriptOrder(order);
}

std::optional<PlayerId>
GameSessionScenarioTransactionPort::currentEnemyPlayer(
    PlayerId player) const {
    if (!content || !content->m_players.get(player)) return std::nullopt;
    const auto eligible = [player](const PlayerState* candidate) noexcept {
        return candidate && candidate->id != player &&
            candidate->isPlayableSide() &&
            candidate->life != PlayerLifeState::Defeated;
    };
    if (const StrategicAIPlayerBrain* brain =
            ai->m_strategicAI.findBrain(player)) {
        const PlayerState* candidate =
            content->m_players.get(brain->currentEnemy);
        if (eligible(candidate) &&
            content->m_players.relationships().get(
                player, brain->currentEnemy) ==
                PlayerRelationship::Enemies) {
            return brain->currentEnemy;
        }
    }
    for (const PlayerId candidateId : content->m_players.activePlayerIds()) {
        const PlayerState* candidate = content->m_players.get(candidateId);
        if (eligible(candidate) &&
            content->m_players.relationships().get(player, candidateId) ==
                PlayerRelationship::Enemies) return candidateId;
    }
    for (const PlayerId candidateId : content->m_players.activePlayerIds()) {
        const PlayerState* candidate = content->m_players.get(candidateId);
        if (eligible(candidate) &&
            candidate->controller == PlayerControllerKind::Human)
            return candidateId;
    }
    return std::nullopt;
}

GameSessionBuildPlacementLegalityEvaluation
GameSessionScenarioTransactionPort::evaluateBuildPlacement(
    ObjectId source, const LogicFixedVec3& position, math::q32_32 yaw,
    PlayerId player, const game::ObjectArchetype& product,
    bool finalConfirmation) const {
    if (!content || !world || !ai || !presentation) return {};
    return GameSessionBuildPlacementEvaluator{
        *content, *ai, *presentation, *world}
        .evaluateFixed(
            source, position, yaw, player, product, finalConfirmation);
}

std::optional<ScriptCommandButtonSelectionResult>
GameSessionScenarioTransactionPort::selectCommandButton(
    container::Span<const ObjectId> actors, container::StringView button,
    script::ScriptCommandButtonActorPolicy actorPolicy,
    math::q32_32 percentage,
    script::ScriptCommandButtonTargetKind targetKind, ObjectId target,
    container::StringView filter,
    container::Span<const container::String> targetTypes,
    std::optional<math::q32_32> range) const {
    if (!content || !world || !ai || !presentation) return std::nullopt;
    return GameSessionScriptOrderAdmissionTransactions{
        *content, *world, *ai, *presentation, orderPolicy}
        .selectScriptCommandButtonExecution(
            actors, button, actorPolicy, percentage, targetKind, target,
            filter, targetTypes, range);
}

OrderExecutionResult GameSessionScenarioTransactionPort::executeCommandButton(
    const ScriptOrderIntent& order, bool requireInSet) const {
    if (!content || !world || !ai || !presentation) return {};
    return GameSessionScriptOrderAdmissionTransactions{
        *content, *world, *ai, *presentation, orderPolicy}
        .executeScriptCommandButton(order, requireInSet);
}

bool GameSessionScenarioTransactionPort::raiseSimulationFault(
    SimulationFault fault) const {
    return lifecycle.raiseSimulationFault(std::move(fault));
}

GameSessionScenarioTransactionPort makeScenarioTransactionPort(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation,
    GameSessionLifecycleTransactionPort lifecycle) noexcept {
    GameSessionProductionPolicyPort productionPolicy =
        makeProductionPolicyPort(content, presentation);
    GameSessionOrderAdmissionPolicyPort orderPolicy =
        makeOrderAdmissionPolicyPort(
            content, world, presentation, lifecycle);
    return GameSessionScenarioTransactionPort{
        content, world, ai, presentation, std::move(lifecycle),
        std::move(productionPolicy), std::move(orderPolicy)};
}

} // namespace engine
