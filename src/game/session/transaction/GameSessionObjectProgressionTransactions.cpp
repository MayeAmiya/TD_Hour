#include "game/session/transaction/GameSessionObjectProgressionTransactions.h"

#include "game/session/state/GameSessionDomainState.h"

#include "game/data/base/UpgradeCatalog.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/simulation/economy/ObjectEnergy.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/player/PlayerRegistry.h"
#include "game/session/object/GameSessionObjectContracts.h"

#include <optional>
#include <utility>

namespace engine {

GameSessionObjectProgressionTransactions::GameSessionObjectProgressionTransactions(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionScriptPresentationState& presentation,
    GameSessionLifecycleTransactionPort lifecyclePublisher) noexcept
    : m_content(content),
      m_world(world),
      m_presentation(presentation),
      m_lifecyclePublisher(lifecyclePublisher) {}

void GameSessionObjectProgressionTransactions::refreshDerivedAggregates(
    uint64_t confirmedTick) {
    m_world.m_objectEnergy.update(
        m_world.m_registry, m_content.m_players, confirmedTick);
    m_world.m_objectSimulation.updateRadarProviders(
        m_world.m_registry, m_world.m_objects, m_content.m_players,
        confirmedTick);
}

bool GameSessionObjectProgressionTransactions::completeObjectUpgrade(
    ObjectId object, container::StringView upgrade) {
    if (!m_content.m_active || !object || upgrade.empty() ||
        m_world.m_objects.isPendingDestroy(object)) {
        return false;
    }
    const UpgradeCatalog* catalog =
        m_content.m_contentSnapshot.upgradeCatalog();
    const UpgradeDefinition* definition =
        catalog ? catalog->find(upgrade) : nullptr;
    if (!definition || definition->type != UpgradeDefinitionType::Object) {
        return false;
    }
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(object);
    if (!entity) return false;
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
    const PlayerState* player =
        owner ? m_content.m_players.get(owner->player) : nullptr;
    if (!player) return false;
    if (!m_world.m_objectSimulation.completeObjectUpgrade(
            m_world.m_registry,
            m_world.m_objects,
            object,
            definition->id,
            player->upgrades.completed,
            m_presentation.m_confirmedTick,
            {.players = &m_content.m_players,
             .scienceCatalog = m_content.m_contentSnapshot.scienceCatalog(),
             .content = &m_content.m_contentSnapshot,
             .random = &m_content.m_simulationRandom,
             .effects = &m_world.m_objectSimulation})) {
        return false;
    }
    refreshDerivedAggregates(m_presentation.m_confirmedTick);
    return true;
}

bool GameSessionObjectProgressionTransactions::completePlayerUpgrade(
    PlayerId player, container::String upgrade) {
    if (!m_content.m_active || !player || upgrade.empty()) return false;
    const UpgradeCatalog* catalog = m_content.m_contentSnapshot.upgradeCatalog();
    if (!catalog ||
        !m_content.m_players.markUpgradeComplete(player, upgrade, *catalog)) {
        return false;
    }
    fanOutPlayerUpgradeCompletion(player);
    return true;
}

bool GameSessionObjectProgressionTransactions::commitQueuedPlayerUpgrade(
    PlayerId player, UpgradeContentId upgrade) {
    if (!m_content.m_active || !player || !upgrade) return false;
    const UpgradeCatalog* catalog = m_content.m_contentSnapshot.upgradeCatalog();
    const UpgradeDefinition* definition =
        catalog ? catalog->find(upgrade) : nullptr;
    if (!definition || definition->type != UpgradeDefinitionType::Player) {
        return false;
    }
    if (!m_content.m_players.commitQueuedPlayerUpgrade(
            player, definition->id)) {
        return false;
    }
    fanOutPlayerUpgradeCompletion(player);
    return true;
}

void GameSessionObjectProgressionTransactions::fanOutPlayerUpgradeCompletion(
    PlayerId player) {
    const PlayerState* state = m_content.m_players.get(player);
    if (!state) return;
    m_world.m_objectSimulation.onPlayerUpgradeCompleted(
        m_world.m_registry, m_world.m_objects, m_world.m_ownership, player,
        state->upgrades.completed, m_presentation.m_confirmedTick,
        {.players = &m_content.m_players,
         .scienceCatalog = m_content.m_contentSnapshot.scienceCatalog(),
         .content = &m_content.m_contentSnapshot,
         .random = &m_content.m_simulationRandom,
         .effects = &m_world.m_objectSimulation});
    // A PowerPlantUpgrade changes a per-object source bit. Publish the small
    // aggregate immediately so later confirmed commands in this frame observe
    // the same power total rather than a one-frame stale construction speed.
    refreshDerivedAggregates(m_presentation.m_confirmedTick);
}

bool GameSessionObjectProgressionTransactions::attachScriptBoobyTrap(
    ObjectId target, container::StringView templateName,
    uint64_t confirmedTick) {
    if (!m_lifecyclePublisher || !m_content.m_active ||
        !m_presentation.m_hasConfirmedFrame ||
        confirmedTick != m_presentation.m_confirmedTick || !target ||
        templateName.empty() ||
        m_world.m_objects.isPendingDestroy(target)) {
        return false;
    }
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(target);
    const OwnerComponent* owner = entity
        ? ecs::try_get<OwnerComponent>(m_world.m_registry, *entity)
        : nullptr;
    const PrimaryTeamComponent* team = entity
        ? ecs::try_get<PrimaryTeamComponent>(m_world.m_registry, *entity)
        : nullptr;
    const ObjectFixedTransformComponent* transform = entity
        ? ecs::try_get<ObjectFixedTransformComponent>(
              m_world.m_registry, *entity)
        : nullptr;
    if (!entity || !owner || !owner->player || !team || !team->team ||
        !transform) {
        return false;
    }

    ObjectSpawnRequest request;
    request.templateName = container::String{templateName};
    request.owner = owner->player;
    request.primaryTeam = team->team;
    request.transform = *transform;
    request.origin = ObjectCreationOrigin::Script;
    request.confirmedTick = confirmedTick;
    const GameSessionObjectSpawnResult bomb =
        m_lifecyclePublisher.spawnObject(std::move(request));
    if (!bomb) return false;
    if (!m_world.m_objectSimulation.attachStickyBomb(
            m_world.m_registry,
            m_world.m_objects,
            m_content.m_terrain,
            {.bomb = bomb.object,
             .target = target,
             .confirmedTick = confirmedTick})) {
        static_cast<void>(m_lifecyclePublisher.destroyObject(bomb.object));
        return false;
    }
    return true;
}

} // namespace engine
