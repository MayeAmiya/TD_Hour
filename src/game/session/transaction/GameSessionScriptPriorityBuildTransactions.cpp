#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/lifecycle/ObjectRebuildHole.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/transaction/GameSessionScriptScenarioPlanTransactions.h"

namespace engine
{
namespace
{
[[nodiscard]] uint64_t fixedSecondsToTicks(
    math::q32_32 seconds, uint32_t framesPerSecond) noexcept
{
    if (seconds <= math::q32_32{})
        return 1;
    const uint32_t boundedFps = std::min<uint32_t>(
        framesPerSecond,
        static_cast<uint32_t>(std::numeric_limits<int32_t>::max()));
    const math::q32_32 ticks =
        seconds * math::q32_32{static_cast<int32_t>(boundedFps)};
    if (ticks.raw() <= 0)
        return 1;
    const uint64_t raw = static_cast<uint64_t>(ticks.raw());
    const uint64_t whole = raw >> 32u;
    return std::max<uint64_t>(
        1, whole + ((raw & 0xffffffffull) != 0 ? 1u : 0u));
}
} // namespace

bool GameSessionScriptScenarioPlanTransactions::tryPriorityBuildEntry(GameSessionPriorityBuildEntry& entry,
                                                                      uint64_t confirmedTick)
{
    if (entry.state != GameSessionPriorityBuildState::Unbuilt && entry.state != GameSessionPriorityBuildState::Reserved)
    {
        return true;
    }
    entry.state = GameSessionPriorityBuildState::Unbuilt;
    entry.reservedBuilder = INVALID_OBJECT_ID;
    const PlayerId player = entry.player;
    const container::StringView objectType = entry.objectType;
    const uint32_t sourceSequence = entry.sourceSequence;
    const container::StringView scriptName = entry.scriptName;
    const uint32_t sourceSideOrdinal = entry.sourceSideOrdinal;
    const uint32_t sourceBuildListOrdinal = entry.sourceBuildListOrdinal;
    const math::q32_32 anchorX = entry.anchorX;
    const math::q32_32 anchorY = entry.anchorY;
    const math::q32_32 placementYaw = entry.yawRadians;

    const PlayerState* playerState = m_content.m_players.get(player);
    if (!playerState || playerState->controller != PlayerControllerKind::Ai)
    {
        entry.state = GameSessionPriorityBuildState::Exhausted;
        return true;
    }
    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(objectType);
    if (!product)
    {
        entry.state = GameSessionPriorityBuildState::Exhausted;
        return true;
    }
    // These are planner gates, not one-shot Action admission. ZH retains the
    // priority BuildList entry while construction is disabled, prerequisites
    // are missing, builders are busy, or funds are insufficient.
    if (!playerState->constructionPolicy.baseConstructionEnabled)
        return false;
    bool ignoreBuildPrerequisites = false;
    if (!m_port.productionPolicy.admitsObjectBuildability(player, *product, ignoreBuildPrerequisites))
    {
        return false;
    }

    struct BuilderCandidate final
    {
        ObjectId object = INVALID_OBJECT_ID;
        ObjectTeamId primaryTeam = INVALID_OBJECT_TEAM_ID;
        math::q32_32 distanceSquared{};
    };
    container::Vector<BuilderCandidate> builders;
    const auto builderView = ecs::view<const ObjectIdentityComponent,
                                       const OwnerComponent,
                                       const ObjectLifecycleComponent,
                                       const TransformComponent,
                                       const ObjectBuilderComponent>(m_world.m_registry);
    builders.reserve(builderView.size_hint());
    for (const ecs::entity entity : builderView)
    {
        const ObjectIdentityComponent& identity = builderView.template get<const ObjectIdentityComponent>(entity);
        const OwnerComponent& owner = builderView.template get<const OwnerComponent>(entity);
        const ObjectLifecycleComponent& lifecycle = builderView.template get<const ObjectLifecycleComponent>(entity);
        const TransformComponent& transform = builderView.template get<const TransformComponent>(entity);
        const ObjectBuilderComponent& builder = builderView.template get<const ObjectBuilderComponent>(entity);
        const ObjectStatusComponent* status = ecs::try_get<ObjectStatusComponent>(m_world.m_registry, entity);
        const ObjectMapStatusComponent* mapStatus = ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, entity);
        if (!identity.id || owner.player != player || builder.runtimes.empty() ||
            lifecycle.phase != ObjectLifecyclePhase::Alive || m_world.m_objects.isPendingDestroy(identity.id) ||
            m_world.m_objectSimulation.isAnyObjectBuilderTaskPending(
                m_world.m_registry, m_world.m_objects, identity.id) ||
            (mapStatus && mapStatus->offMap) ||
            (status && status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction) |
                                      game::objectStatusBit(game::ObjectStatusFlag::Sold))))
        {
            continue;
        }
        if (!canObjectBuildTemplate(m_world.m_registry,
                                    entity,
                                    m_content.m_contentSnapshot,
                                    m_presentation.m_scriptCommandBarOverrides,
                                    m_content.m_players,
                                    player,
                                    *product,
                                    ignoreBuildPrerequisites))
        {
            continue;
        }
        const PrimaryTeamComponent* primary = ecs::try_get<PrimaryTeamComponent>(m_world.m_registry, entity);
        const ObjectTeamId builderTeam =
            primary && primary->team ? primary->team
                                     : m_world.m_objectTeams.defaultTeam(player).value_or(INVALID_OBJECT_TEAM_ID);
        if (!builderTeam)
            continue;
        const LogicFixedVec3 builderPosition = readAuthoritativeObjectPosition(m_world.m_registry, entity, transform);
        const math::q32_32 dx = builderPosition.x - anchorX;
        const math::q32_32 dy = builderPosition.y - anchorY;
        builders.push_back({
            .object = identity.id,
            .primaryTeam = builderTeam,
            .distanceSquared = dx * dx + dy * dy,
        });
    }
    std::sort(builders.begin(),
              builders.end(),
              [](const BuilderCandidate& left, const BuilderCandidate& right) noexcept
              {
                  if (left.distanceSquared != right.distanceSquared)
                      return left.distanceSquared < right.distanceSquared;
                  return left.object < right.object;
              });
    if (builders.empty())
        return false;

    enum class PlacementAttempt : uint8_t
    {
        Illegal,
        Retry,
        Completed,
        Deferred,
    };
    constexpr uint32_t kMaximumPlacementEvaluationsPerTick = 4;
    uint32_t placementOrdinal = 0;
    uint32_t placementEvaluations = 0;
    const auto tryPlacement = [&](math::q32_32 x, math::q32_32 y) -> PlacementAttempt
    {
        const uint32_t candidateOrdinal = placementOrdinal++;
        if (candidateOrdinal < entry.placementSearchOrdinal)
            return PlacementAttempt::Illegal;
        if (placementEvaluations >= kMaximumPlacementEvaluationsPerTick)
            return PlacementAttempt::Deferred;
        ++placementEvaluations;
        const math::q32_32 z = math::q32_32::from_raw(m_content.m_terrain.groundHeightRaw(x.raw(), y.raw()));
        for (const BuilderCandidate& builder : builders)
        {
            const GameSessionBuildPlacementLegalityEvaluation evaluation =
                m_port.evaluateBuildPlacement(builder.object, {x, y, z}, placementYaw, player, *product, true);
            if (!evaluation.evaluated || evaluation.legality != selection::LocalPlacementLegality::Legal)
            {
                continue;
            }

            const int64_t cost =
                calculateObjectBuildCost(*product, *playerState, m_world.m_registry, m_world.m_objects);
            entry.state = GameSessionPriorityBuildState::Reserved;
            entry.reservedBuilder = builder.object;
            if (cost > 0 && !m_content.m_players.trySpend(player, cost))
            {
                entry.state = GameSessionPriorityBuildState::Unbuilt;
                entry.reservedBuilder = INVALID_OBJECT_ID;
                return PlacementAttempt::Retry;
            }
            ObjectSpawnRequest request{
                .templateName = product->name,
                .owner = player,
                .primaryTeam = builder.primaryTeam,
                .transform =
                    ObjectFixedTransformComponent{
                        .position = {x, y, z},
                        .yawRadians = placementYaw,
                        .authoritative = true,
                    },
                .origin = ObjectCreationOrigin::Production,
                .confirmedTick = confirmedTick,
                .producer = builder.object,
                .constructedBy = builder.object,
                .scriptName = container::String{scriptName},
                .startsUnderConstruction = true,
                .flattenTerrainForStructure = true,
            };
            const GameSessionObjectSpawnResult spawned = m_port.lifecycle.spawnObject(std::move(request));
            const uint32_t requiredFrames =
                calculateObjectBuildFrames(*product,
                                           *playerState,
                                           static_cast<uint32_t>(std::max(1, m_content.m_startInfo.gameSpeedFPS)),
                                           m_content.m_objectSimulationRules.energy,
                                           confirmedTick);
            if (!spawned || !m_world.m_objectSimulation.beginObjectConstruction(m_world.m_registry,
                                                                                m_world.m_objects,
                                                                                spawned.object,
                                                                                builder.object,
                                                                                requiredFrames,
                                                                                false,
                                                                                confirmedTick))
            {
                if (spawned)
                {
                    // The spawn was already published. Roll it back through the
                    // lifecycle port so named/team/ownership indexes stop
                    // advertising the ObjectId before this planner frame
                    // continues; a raw requestDestroy would defer that
                    // publication to an unrelated lifecycle-consume boundary.
                    static_cast<void>(m_port.lifecycle.requestDestroyObject(
                        spawned.object, ObjectDestroyReason::System, confirmedTick));
                }
                if (cost > 0)
                {
                    static_cast<void>(m_content.m_players.adjustCash(player, cost));
                }
                entry.state = GameSessionPriorityBuildState::Unbuilt;
                entry.reservedBuilder = INVALID_OBJECT_ID;
                return PlacementAttempt::Retry;
            }
            ObjectConstructionSiteComponent* construction =
                spawned.entity ? ecs::try_get<ObjectConstructionSiteComponent>(m_world.m_registry, *spawned.entity)
                               : nullptr;
            if (construction)
            {
                construction->sourceSideOrdinal = sourceSideOrdinal;
                construction->sourceBuildListOrdinal = sourceBuildListOrdinal;
                ++construction->revision;
            }
            if (!m_world.m_objectSimulation.assignObjectConstruction(m_world.m_registry,
                                                                     m_world.m_objects,
                                                                     builder.object,
                                                                     spawned.object,
                                                                     confirmedTick,
                                                                     sourceSequence))
            {
                static_cast<void>(m_port.lifecycle.requestDestroyObject(
                    spawned.object, ObjectDestroyReason::System, confirmedTick));
                if (cost > 0)
                {
                    static_cast<void>(m_content.m_players.adjustCash(player, cost));
                }
                entry.state = GameSessionPriorityBuildState::Unbuilt;
                entry.reservedBuilder = INVALID_OBJECT_ID;
                return PlacementAttempt::Retry;
            }
            static_cast<void>(m_port.lifecycle.evacuateConstructionFootprint(
                spawned.object, builder.object, confirmedTick));
            entry.state = GameSessionPriorityBuildState::Constructing;
            entry.constructedObject = spawned.object;
            entry.placementSearchOrdinal = 0;
            return PlacementAttempt::Completed;
        }
        return PlacementAttempt::Illegal;
    };

    // Try the authored Team anchor first, then reproduce the old square-ring
    // "wiggle" search out to twice SUPPLY_CENTER_CLOSE_DIST. Navigation's
    // frozen cell size replaces the global PATHFIND_CELL_SIZE_F constant.
    const PlacementAttempt authoredAttempt = tryPlacement(anchorX, anchorY);
    if (authoredAttempt == PlacementAttempt::Completed)
        return true;
    if (authoredAttempt == PlacementAttempt::Retry)
        return false;
    if (authoredAttempt == PlacementAttempt::Deferred) {
        entry.placementSearchOrdinal = placementOrdinal - 1;
        return false;
    }
    const int64_t cellSizeRaw = m_content.m_navigation.grid().transform().cellSizeRaw;
    const math::q32_32 cellSize = cellSizeRaw > 0 ? math::q32_32::from_raw(cellSizeRaw) : math::q32_32{int32_t{10}};
    const math::q32_32 step = math::q32_32::max(math::q32_32{int32_t{1}}, cellSize * math::q32_32{int32_t{2}});
    const math::q32_32 maximumOffset{int32_t{400}};
    for (math::q32_32 diameter = step; diameter < maximumOffset; diameter += step)
    {
        const math::q32_32 offset = diameter / math::q32_32{int32_t{2}};
        for (math::q32_32 x = anchorX - offset; x <= anchorX + offset; x += step)
        {
            const PlacementAttempt lower = tryPlacement(x, anchorY - offset);
            if (lower == PlacementAttempt::Completed)
                return true;
            if (lower == PlacementAttempt::Retry)
                return false;
            if (lower == PlacementAttempt::Deferred) {
                entry.placementSearchOrdinal = placementOrdinal - 1;
                return false;
            }
            const PlacementAttempt upper = tryPlacement(x, anchorY + offset);
            if (upper == PlacementAttempt::Completed)
                return true;
            if (upper == PlacementAttempt::Retry)
                return false;
            if (upper == PlacementAttempt::Deferred) {
                entry.placementSearchOrdinal = placementOrdinal - 1;
                return false;
            }
        }
        for (math::q32_32 y = anchorY - offset; y <= anchorY + offset; y += step)
        {
            const PlacementAttempt left = tryPlacement(anchorX - offset, y);
            if (left == PlacementAttempt::Completed)
                return true;
            if (left == PlacementAttempt::Retry)
                return false;
            if (left == PlacementAttempt::Deferred) {
                entry.placementSearchOrdinal = placementOrdinal - 1;
                return false;
            }
            const PlacementAttempt right = tryPlacement(anchorX + offset, y);
            if (right == PlacementAttempt::Completed)
                return true;
            if (right == PlacementAttempt::Retry)
                return false;
            if (right == PlacementAttempt::Deferred) {
                entry.placementSearchOrdinal = placementOrdinal - 1;
                return false;
            }
        }
    }
    entry.state = GameSessionPriorityBuildState::Unbuilt;
    entry.reservedBuilder = INVALID_OBJECT_ID;
    entry.placementSearchOrdinal = 0;
    return false;
}

void GameSessionScriptScenarioPlanTransactions::processPriorityBuildEntries()
{
    auto& entries = m_ai.m_priorityBuildEntries;
    const uint64_t tick = m_presentation.m_confirmedTick;
    const uint64_t retryDelay = static_cast<uint64_t>(std::max(1, m_content.m_startInfo.gameSpeedFPS / 2));
    const uint64_t rebuildDelay = fixedSecondsToTicks(
        m_content.m_objectSimulationRules.ai.rebuildDelayTimeSeconds,
        static_cast<uint32_t>(std::max(
            1, m_content.m_startInfo.gameSpeedFPS)));
    const auto bindRebuildSuccessor =
        [&](GameSessionPriorityBuildEntry& entry,
            ObjectId priorObject) -> bool
    {
        ObjectId successor = INVALID_OBJECT_ID;
        bool successorIsHole = false;
        const auto holeView = ecs::view<
            const ObjectIdentityComponent,
            const OwnerComponent,
            const ObjectLifecycleComponent,
            const ObjectRebuildHoleComponent>(m_world.m_registry);
        for (const ecs::entity candidate : holeView)
        {
            const ObjectIdentityComponent& identity =
                holeView.template get<const ObjectIdentityComponent>(candidate);
            const OwnerComponent& owner =
                holeView.template get<const OwnerComponent>(candidate);
            const ObjectLifecycleComponent& lifecycle =
                holeView.template get<const ObjectLifecycleComponent>(candidate);
            const ObjectRebuildHoleComponent& hole =
                holeView.template get<const ObjectRebuildHoleComponent>(candidate);
            if (!identity.id || owner.player != entry.player ||
                lifecycle.phase != ObjectLifecyclePhase::Alive ||
                m_world.m_objects.isPendingDestroy(identity.id))
            {
                continue;
            }
            const bool matchesSpawner = std::any_of(
                hole.runtimes.begin(), hole.runtimes.end(),
                [priorObject](const ObjectRebuildHoleRuntime& runtime) noexcept
                {
                    return runtime.spawner == priorObject;
                });
            if (matchesSpawner && (!successor || identity.id < successor))
            {
                successor = identity.id;
                successorIsHole = true;
            }
        }

        if (!successor)
        {
            const auto reconstructionView = ecs::view<
                const ObjectIdentityComponent,
                const OwnerComponent,
                const ObjectLifecycleComponent,
                const ObjectProducerComponent,
                const ThingTemplateComponent>(m_world.m_registry);
            for (const ecs::entity candidate : reconstructionView)
            {
                const ObjectIdentityComponent& identity =
                    reconstructionView.template get<
                        const ObjectIdentityComponent>(candidate);
                const OwnerComponent& owner =
                    reconstructionView.template get<const OwnerComponent>(candidate);
                const ObjectLifecycleComponent& lifecycle =
                    reconstructionView.template get<
                        const ObjectLifecycleComponent>(candidate);
                const ObjectProducerComponent& producer =
                    reconstructionView.template get<
                        const ObjectProducerComponent>(candidate);
                const ThingTemplateComponent& type =
                    reconstructionView.template get<
                        const ThingTemplateComponent>(candidate);
                if (!identity.id || producer.producer != priorObject ||
                    owner.player != entry.player ||
                    lifecycle.phase != ObjectLifecyclePhase::Alive ||
                    m_world.m_objects.isPendingDestroy(identity.id) ||
                    !type.archetype ||
                    type.archetype->name != entry.objectType)
                {
                    continue;
                }
                if (!successor || identity.id < successor)
                    successor = identity.id;
            }
        }

        if (!successor)
            return false;
        entry.constructedObject = successor;
        entry.reservedBuilder = INVALID_OBJECT_ID;
        entry.placementSearchOrdinal = 0;
        entry.nextAttemptTick = tick;
        if (successorIsHole)
        {
            // RebuildHoleBehavior owns this reconstruction.  RefCode binds
            // the durable BuildList node to the hole and does not spend a
            // separate BuildList rebuild or start another foundation.
            entry.state = GameSessionPriorityBuildState::Completed;
            return true;
        }

        const std::optional<ecs::entity> successorEntity =
            m_world.m_objects.entityFromId(successor);
        const ObjectStatusComponent* status = successorEntity
            ? ecs::try_get<ObjectStatusComponent>(
                  m_world.m_registry, *successorEntity)
            : nullptr;
        const ObjectConstructionSiteComponent* site = successorEntity
            ? ecs::try_get<ObjectConstructionSiteComponent>(
                  m_world.m_registry, *successorEntity)
            : nullptr;
        const bool underConstruction = site != nullptr ||
            (status && status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::UnderConstruction)));
        entry.state = underConstruction
            ? GameSessionPriorityBuildState::Constructing
            : GameSessionPriorityBuildState::Completed;
        if (site)
            entry.reservedBuilder = site->builder;
        return true;
    };
    bool placementPlannerConsumed = false;
    size_t index = 0;
    while (index < entries.size())
    {
        GameSessionPriorityBuildEntry& entry = entries[index];
        const auto scheduleRebuildOrExhaust = [&]()
        {
            entry.reservedBuilder = INVALID_OBJECT_ID;
            entry.constructedObject = INVALID_OBJECT_ID;
            entry.placementSearchOrdinal = 0;
            if (entry.remainingRebuilds == 0)
            {
                entry.state = GameSessionPriorityBuildState::Exhausted;
                entry.nextAttemptTick = std::numeric_limits<uint64_t>::max();
                return;
            }
            if (entry.remainingRebuilds > 0)
                --entry.remainingRebuilds;
            entry.state = GameSessionPriorityBuildState::RebuildDelay;
            entry.nextAttemptTick = tick > std::numeric_limits<uint64_t>::max() - rebuildDelay
                                        ? std::numeric_limits<uint64_t>::max()
                                        : tick + rebuildDelay;
        };
        if (entry.state == GameSessionPriorityBuildState::Constructing ||
            entry.state == GameSessionPriorityBuildState::Completed)
        {
            const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(entry.constructedObject);
            const OwnerComponent* owner = entity ? ecs::try_get<OwnerComponent>(m_world.m_registry, *entity) : nullptr;
            if (!entity || !owner || owner->player != entry.player ||
                m_world.m_objects.isPendingDestroy(entry.constructedObject))
            {
                if (bindRebuildSuccessor(entry, entry.constructedObject))
                {
                    ++index;
                    continue;
                }
                scheduleRebuildOrExhaust();
                ++index;
                continue;
            }
            if (entry.state == GameSessionPriorityBuildState::Constructing)
            {
                const ObjectStatusComponent* status = ecs::try_get<ObjectStatusComponent>(m_world.m_registry, *entity);
                ObjectConstructionSiteComponent* site =
                    ecs::try_get<ObjectConstructionSiteComponent>(
                        m_world.m_registry, *entity);
                const bool underConstruction =
                    site != nullptr ||
                    (status && status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction)));
                if (!underConstruction)
                {
                    entry.state = GameSessionPriorityBuildState::Completed;
                    entry.reservedBuilder = INVALID_OBJECT_ID;
                }
                else if (site)
                {
                    // AIPlayer::processBaseBuilding calls
                    // aiResumeConstruction every pass and replaces a dead,
                    // captured or unmanned dozer.  The durable BuildList is
                    // the equivalent authority here: do not leave a valid
                    // construction site permanently stalled after its first
                    // worker disappears or accepts another command.
                    bool currentBuilderOwnsTask = false;
                    if (site->builder)
                    {
                        const std::optional<ecs::entity> builderEntity =
                            m_world.m_objects.entityFromId(site->builder);
                        const OwnerComponent* builderOwner = builderEntity
                            ? ecs::try_get<OwnerComponent>(
                                  m_world.m_registry, *builderEntity)
                            : nullptr;
                        const ObjectHealthComponent* builderHealth =
                            builderEntity
                            ? ecs::try_get<ObjectHealthComponent>(
                                  m_world.m_registry, *builderEntity)
                            : nullptr;
                        const ObjectBuilderTask task = builderEntity &&
                                builderOwner &&
                                builderOwner->player == entry.player &&
                                (!builderHealth ||
                                 !builderHealth->effectivelyDead) &&
                                !isObjectDisabledBy(
                                    m_world.m_registry, *builderEntity,
                                    ObjectDisabledReason::Unmanned, tick)
                            ? m_world.m_objectSimulation.objectBuilderTask(
                                  m_world.m_registry, m_world.m_objects,
                                  site->builder,
                                  ObjectBuilderTaskKind::Build, 0)
                            : ObjectBuilderTask{};
                        currentBuilderOwnsTask =
                            task.kind == ObjectBuilderTaskKind::Build &&
                            task.target == entry.constructedObject;
                    }

                    if (!currentBuilderOwnsTask)
                    {
                        site->builder = INVALID_OBJECT_ID;
                        ++site->revision;
                        entry.reservedBuilder = INVALID_OBJECT_ID;

                        const TransformComponent* siteTransform =
                            ecs::try_get<TransformComponent>(
                                m_world.m_registry, *entity);
                        const container::SharedPtr<const
                            game::ObjectArchetype> product =
                            m_content.m_contentSnapshot.findObjectArchetype(
                                entry.objectType);
                        ObjectId replacement = INVALID_OBJECT_ID;
                        math::q32_32 bestDistance{};
                        bool foundReplacement = false;
                        const auto builders = ecs::view<
                            const ObjectIdentityComponent,
                            const OwnerComponent,
                            const ObjectLifecycleComponent,
                            const TransformComponent,
                            const ObjectBuilderComponent>(
                                m_world.m_registry);
                        for (const ecs::entity builderEntity : builders)
                        {
                            const ObjectIdentityComponent& identity =
                                builders.template get<
                                    const ObjectIdentityComponent>(
                                        builderEntity);
                            const OwnerComponent& builderOwner =
                                builders.template get<const OwnerComponent>(
                                    builderEntity);
                            const ObjectLifecycleComponent& lifecycle =
                                builders.template get<
                                    const ObjectLifecycleComponent>(
                                        builderEntity);
                            const TransformComponent& transform =
                                builders.template get<
                                    const TransformComponent>(builderEntity);
                            const ObjectBuilderComponent& builder =
                                builders.template get<
                                    const ObjectBuilderComponent>(
                                        builderEntity);
                            const ObjectHealthComponent* health =
                                ecs::try_get<ObjectHealthComponent>(
                                    m_world.m_registry, builderEntity);
                            const ObjectStatusComponent* builderStatus =
                                ecs::try_get<ObjectStatusComponent>(
                                    m_world.m_registry, builderEntity);
                            const ObjectMapStatusComponent* mapStatus =
                                ecs::try_get<ObjectMapStatusComponent>(
                                    m_world.m_registry, builderEntity);
                            if (!identity.id ||
                                builderOwner.player != entry.player ||
                                builder.runtimes.empty() ||
                                lifecycle.phase !=
                                    ObjectLifecyclePhase::Alive ||
                                (health && health->effectivelyDead) ||
                                m_world.m_objects.isPendingDestroy(
                                    identity.id) ||
                                (mapStatus && mapStatus->offMap) ||
                                isObjectDisabledBy(
                                    m_world.m_registry, builderEntity,
                                    ObjectDisabledReason::Unmanned, tick) ||
                                (builderStatus && builderStatus->hasAny(
                                    game::objectStatusBit(
                                        game::ObjectStatusFlag::
                                            UnderConstruction) |
                                    game::objectStatusBit(
                                        game::ObjectStatusFlag::Sold))) ||
                                m_world.m_objectSimulation
                                    .isAnyObjectBuilderTaskPending(
                                        m_world.m_registry,
                                        m_world.m_objects, identity.id) ||
                                !product ||
                                !canObjectBuildTemplate(
                                    m_world.m_registry, builderEntity,
                                    m_content.m_contentSnapshot,
                                    m_presentation
                                        .m_scriptCommandBarOverrides,
                                    m_content.m_players, entry.player,
                                    *product, true))
                            {
                                continue;
                            }
                            math::q32_32 distance{};
                            if (siteTransform)
                            {
                                const LogicFixedVec3 builderPosition =
                                    readAuthoritativeObjectPosition(
                                        m_world.m_registry, builderEntity,
                                        transform);
                                const LogicFixedVec3 sitePosition =
                                    readAuthoritativeObjectPosition(
                                        m_world.m_registry, *entity,
                                        *siteTransform);
                                const math::q32_32 dx =
                                    builderPosition.x - sitePosition.x;
                                const math::q32_32 dy =
                                    builderPosition.y - sitePosition.y;
                                distance = dx * dx + dy * dy;
                            }
                            if (!foundReplacement ||
                                distance < bestDistance ||
                                (distance == bestDistance &&
                                 identity.id < replacement))
                            {
                                replacement = identity.id;
                                bestDistance = distance;
                                foundReplacement = true;
                            }
                        }
                        if (replacement &&
                            m_world.m_objectSimulation
                                .assignObjectConstruction(
                                    m_world.m_registry,
                                    m_world.m_objects, replacement,
                                    entry.constructedObject, tick,
                                    entry.sourceSequence))
                        {
                            entry.reservedBuilder = replacement;
                        }
                    }
                    else
                    {
                        entry.reservedBuilder = site->builder;
                    }
                }
            }
            ++index;
            continue;
        }
        if (entry.state == GameSessionPriorityBuildState::Exhausted)
        {
            ++index;
            continue;
        }
        if (entry.state == GameSessionPriorityBuildState::RebuildDelay)
        {
            if (entry.nextAttemptTick > tick)
            {
                ++index;
                continue;
            }
            entry.state = GameSessionPriorityBuildState::Unbuilt;
        }
        if (entry.nextAttemptTick > tick)
        {
            ++index;
            continue;
        }
        // BuildAssistant advances one priority plan at a time. Placement is
        // an expensive spatial query, so do not let every dormant BuildList
        // entry run its ring search in the same confirmed tick.
        if (placementPlannerConsumed) {
            ++index;
            continue;
        }
        placementPlannerConsumed = true;
        if (tryPriorityBuildEntry(entry, tick))
        {
            // BuildList nodes are durable. Construction completion and later
            // lifecycle loss, not a successful admission call, decide their
            // next state.
            ++index;
            continue;
        }
        if (entry.attemptCount != std::numeric_limits<uint32_t>::max())
            ++entry.attemptCount;
        entry.nextAttemptTick = tick > std::numeric_limits<uint64_t>::max() - retryDelay
                                    ? std::numeric_limits<uint64_t>::max()
                                    : tick + retryDelay;
        ++index;
    }
}


} // namespace engine
