#include "GameSessionScriptPortDetail.h"
#include "GameSessionScriptQueryPort.h"

#include "debug/debug.h"
#include "game/base/GameBalanceConstants.h"
#include "game/base/DamageTypes.h"
#include "game/base/GameCameraDirector.h"
#include "game/base/GameSettings.h"
#include "game/audio/GameAudioEvents.h"
#include "game/command/CommandButtonStore.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/session/state/GameSessionDomainState.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace engine::script {
namespace {
constexpr auto equalAsciiInsensitive = container::asciiEqualIgnoreCase;

// Mirror of the resolver in GameSessionScriptEffectPorts.cpp — both copies must
// stay in step, since this one is the terminal fallback of the authority
// resolver.  Empty is intentionally NOT a local-player reference (see the note
// there); it never reaches a resolver and means Neutral by convention.
[[nodiscard]] bool isLocalPlayerReference(container::StringView value) noexcept {
    return equalAsciiInsensitive(value, "LocalPlayer") ||
           equalAsciiInsensitive(value, "<Local Player>");
}
}
using detail::kindOfContains;

std::optional<PlayerId> GameSessionScriptQueryPort::findPlayer(container::StringView name) const {
    if (isLocalPlayerReference(name)) {
        // Per-client identity must not leak into lockstep or replay: this is the
        // terminal fallback of the authoritative resolver, so a differing
        // PlayerId per peer would desync.
        if (m_content.m_startInfo.network.enabled ||
            m_content.m_startInfo.mode == GameMode::Replay) {
            return std::nullopt;
        }
        const PlayerId local = m_content.m_players.localPlayerId();
        return m_content.m_players.get(local) ? std::optional<PlayerId>{local} : std::nullopt;
    }
    return resolvePlayerAlias(name);
}

std::optional<PlayerId> GameSessionScriptQueryPort::currentEnemyPlayer(
    PlayerId currentPlayer) const noexcept {
    return currentEnemyPlayerFor(currentPlayer);
}

std::optional<int64_t> GameSessionScriptQueryPort::playerCash(PlayerId player) const {
    const PlayerState* state = m_content.m_players.get(player);
    return state ? std::optional<int64_t>{state->cash} : std::nullopt;
}

std::optional<ScriptWorldPlayerEnergySnapshot> GameSessionScriptQueryPort::playerEnergy(
    PlayerId player) const {
    const PlayerState* state = m_content.m_players.get(player);
    if (!state) return std::nullopt;
    return ScriptWorldPlayerEnergySnapshot{
        .effectiveProduction = state->energy.effectiveProduction(m_confirmedTick),
        .consumption = state->energy.consumption,
        .sufficient = state->energy.hasSufficientPower(m_confirmedTick),
    };
}

std::optional<int32_t> GameSessionScriptQueryPort::playerSciencePurchasePoints(
    PlayerId player) const {
    const PlayerState* state = m_content.m_players.get(player);
    return state ? std::optional<int32_t>{state->sciences.purchasePoints} : std::nullopt;
}

bool GameSessionScriptQueryPort::consumePlayerScienceAcquired(
    PlayerId player, container::StringView science) const noexcept {
    return m_eventCursor.consumePlayerScienceAcquired(player, science);
}

bool GameSessionScriptQueryPort::playerCanPurchaseScience(
    PlayerId player, container::StringView science) const noexcept {
    // The catalog is frozen into GameContentSnapshot at session start; script
    // evaluation must never reach through to a reloadable global ScienceStore.
    const ScienceCatalog* catalog = m_content.m_contentSnapshot.scienceCatalog();
    if (!player || science.empty() || !catalog || !catalog->isLoaded()) return false;
    const ScienceDefinition* definition = catalog->find(science);
    return definition && m_content.m_players.canPurchaseScience(player, *definition);
}

ScriptWorldPlayerObjectSummary GameSessionScriptQueryPort::playerObjectSummary(
    PlayerId player) const {
    ScriptWorldPlayerObjectSummary summary;
    if (!m_content.m_players.get(player)) return summary;
    summary.playerExists = true;

    // ObjectOwnershipIndex is updated synchronously for Created,
    // OwnershipChanged and DestroyRequested lifecycle events.  It is the
    // stable live-object projection used by PLAYER_ALL_DESTROYED, whose
    // legacy hasAnyObjects semantics intentionally exclude dying objects.
    for (const ObjectId object : m_world.m_ownership.objects(player)) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
        if (!entity) continue;

        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
        if (health && health->effectivelyDead) continue;

        const ThingTemplateComponent* templateComponent =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        if (!kindOfContains(templateComponent ? templateComponent->archetype : nullptr,
                            game::ObjectKindOf::Projectile) &&
            !kindOfContains(templateComponent ? templateComponent->archetype : nullptr,
                            game::ObjectKindOf::Mine) &&
            !kindOfContains(templateComponent ? templateComponent->archetype : nullptr,
                            game::ObjectKindOf::Inert)) {
            summary.hasLegacyCountedObject = true;
        }
    }

    // RefCode's Player::countBuildings/countObjects intentionally keeps an
    // effectively-dead structure counted until its Team member is retired.
    // The modern ownership index removes DestroyRequested IDs earlier for
    // normal live-world queries, so preserve the count condition's separate
    // observation rule with an explicit lifecycle-component view.  Physical
    // reclamation removes the entity at the normal frame boundary; both Alive
    // and PendingDestroy are observable here, exactly as a legacy team list
    // still was during a death transition.
    const auto objects = ecs::view<const OwnerComponent,
                                   const ThingTemplateComponent,
                                   const ObjectLifecycleComponent>(m_world.m_registry);
    for (const ecs::entity entity : objects) {
        const auto& owner = objects.template get<const OwnerComponent>(entity);
        if (owner.player != player) continue;
        const auto& templateComponent =
            objects.template get<const ThingTemplateComponent>(entity);
        if (!templateComponent.archetype) continue;
        if (!kindOfContains(templateComponent.archetype,
                            game::ObjectKindOf::Structure)) continue;

        if (summary.structureCount != std::numeric_limits<uint32_t>::max()) {
            ++summary.structureCount;
        }
        if (kindOfContains(templateComponent.archetype,
                           game::ObjectKindOf::MpCountForVictory) &&
            summary.victoryStructureCount != std::numeric_limits<uint32_t>::max()) {
            ++summary.victoryStructureCount;
        }
    }
    return summary;
}

bool GameSessionScriptQueryPort::playerHasAnyBuildFacility(
    PlayerId player) const noexcept {
    if (!m_content.m_players.get(player)) return false;
    const auto objects = ecs::view<
        const OwnerComponent, const ThingTemplateComponent,
        const ObjectLifecycleComponent>(m_world.m_registry);
    for (const ecs::entity entity : objects) {
        const OwnerComponent& owner =
            objects.template get<const OwnerComponent>(entity);
        if (owner.player != player) continue;
        const ThingTemplateComponent& type =
            objects.template get<const ThingTemplateComponent>(entity);
        if (type.archetype && type.archetype->templateData.isBuildFacility)
            return true;
    }
    return false;
}

ScriptWorldPlayerAreaSummary GameSessionScriptQueryPort::playerAreaSummary(
    PlayerId player, container::StringView areaName,
    ScriptWorldPlayerAreaMetric metric, container::StringView requiredKind) const {
    ScriptWorldPlayerAreaSummary summary;
    if (!m_content.m_players.get(player)) return summary;
    summary.playerExists = true;

    const game::terrain::PolygonTriggerRecord* area =
        m_content.m_terrain.triggerByName(areaName);
    if (!area) return summary;
    summary.areaExists = true;

    for (const ObjectId object : m_world.m_ownership.objects(player)) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
        if (!entity) continue;
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
        if (!transform) continue;
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, *entity, *transform);
        if (!m_content.m_terrain.isInsideTriggerLegacyRaw(
                *area, position.x.raw(), position.y.raw()))
            continue;

        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
        if (health && health->effectivelyDead) continue;
        const ThingTemplateComponent* templateComponent =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        if (!templateComponent || !templateComponent->archetype) continue;
        const game::ThingTemplate& data = templateComponent->archetype->templateData;
        if (kindOfContains(templateComponent->archetype,
                           game::ObjectKindOf::Inert)) continue;
        const std::optional<game::ObjectKindOf> requiredKindId =
            requiredKind.empty() ? std::nullopt
                                  : game::parseObjectKindOf(requiredKind);

        switch (metric) {
        case ScriptWorldPlayerAreaMetric::MatchingKindCount:
            if (requiredKind.empty() || !requiredKindId ||
                !kindOfContains(templateComponent->archetype, *requiredKindId)) continue;
            if (summary.value != std::numeric_limits<int64_t>::max()) ++summary.value;
            break;
        case ScriptWorldPlayerAreaMetric::EligibleObjectCount:
            if (kindOfContains(templateComponent->archetype,
                               game::ObjectKindOf::Projectile)) continue;
            if (summary.value != std::numeric_limits<int64_t>::max()) ++summary.value;
            break;
        case ScriptWorldPlayerAreaMetric::BuildValue:
            const int64_t cost = data.buildCostFixed.raw() /
                (int64_t{1} << 32u);
            if (cost > 0 && summary.value > std::numeric_limits<int64_t>::max() - cost)
                summary.value = std::numeric_limits<int64_t>::max();
            else if (cost < 0 && summary.value < std::numeric_limits<int64_t>::min() - cost)
                summary.value = std::numeric_limits<int64_t>::min();
            else
                summary.value += cost;
            break;
        }
    }
    return summary;
}

std::optional<int64_t> GameSessionScriptQueryPort::playerObjectTypeCountInArea(
    PlayerId player, container::StringView areaName,
    container::Span<const container::String> objectTypes) const {
    if (!m_content.m_players.get(player)) return std::nullopt;
    const game::terrain::PolygonTriggerRecord* area =
        m_content.m_terrain.triggerByName(areaName);
    if (!area) return std::nullopt;

    int64_t count = 0;
    for (const ObjectId object : m_world.m_ownership.objects(player)) {
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
        if (!entity) continue;
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
        if (!transform) continue;
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, *entity, *transform);
        if (!m_content.m_terrain.isInsideTriggerLegacyRaw(
                *area, position.x.raw(), position.y.raw())) continue;
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        if (!type || !type->archetype) continue;
        const game::ThingTemplate& data = type->archetype->templateData;
        const bool matches = std::any_of(
            objectTypes.begin(), objectTypes.end(), [&data](container::StringView name) {
                return equalAsciiInsensitive(data.name, name);
            });
        if (!matches) continue;

        const bool crate = kindOfContains(type->archetype,
                                          game::ObjectKindOf::Crate);
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
        if (!crate && ((health && health->effectivelyDead) ||
                       kindOfContains(type->archetype,
                                      game::ObjectKindOf::Inert))) {
            continue;
        }
        if (count != std::numeric_limits<int64_t>::max()) ++count;
    }
    return count;
}

bool GameSessionScriptQueryPort::concreteObjectTypeExists(
    container::StringView objectType) const noexcept {
    return m_content.m_contentSnapshot.findObjectArchetype(objectType) != nullptr;
}

int64_t GameSessionScriptQueryPort::playerObjectTypeCount(
    PlayerId player, container::Span<const container::String> objectTypes,
    bool includeEffectivelyDead) const noexcept {
    if (!m_content.m_players.get(player)) return 0;
    container::Vector<container::SharedPtr<const game::ObjectArchetype>> sought;
    sought.reserve(objectTypes.size());
    for (const container::String& name : objectTypes) {
        const container::SharedPtr<const game::ObjectArchetype> archetype =
            m_content.m_contentSnapshot.findObjectArchetype(name);
        if (archetype && std::none_of(
                sought.begin(), sought.end(), [&archetype](const auto& existing) {
                    return existing->templateData.name == archetype->templateData.name;
                })) {
            sought.push_back(archetype);
        }
    }
    if (sought.empty()) return 0;

    int64_t count = 0;
    const auto objects = ecs::view<
        const OwnerComponent, const ThingTemplateComponent,
        const ObjectLifecycleComponent>(m_world.m_registry);
    for (const ecs::entity entity : objects) {
        const OwnerComponent& owner =
            objects.template get<const OwnerComponent>(entity);
        if (owner.player != player) continue;
        const ObjectLifecycleComponent& lifecycle =
            objects.template get<const ObjectLifecycleComponent>(entity);
        if (!includeEffectivelyDead &&
            lifecycle.phase != ObjectLifecyclePhase::Alive) continue;
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(m_world.m_registry, entity);
        if (status && status->hasAny(game::objectStatusBit(
                game::ObjectStatusFlag::UnderConstruction))) continue;
        if (!includeEffectivelyDead) {
            const ObjectHealthComponent* health =
                ecs::try_get<ObjectHealthComponent>(m_world.m_registry, entity);
            if (health && health->effectivelyDead) continue;
        }
        const ThingTemplateComponent& type =
            objects.template get<const ThingTemplateComponent>(entity);
        if (!type.archetype) continue;
        if (!std::any_of(sought.begin(), sought.end(), [&type](const auto& candidate) {
                return game::legacyThingTemplatesEquivalent(
                    type.archetype->templateData, candidate->templateData);
            })) continue;
        if (count != std::numeric_limits<int64_t>::max()) ++count;
    }
    return count;
}

bool GameSessionScriptQueryPort::techBuildingWithinDistance(
    PlayerId player, container::StringView areaName,
    math::q32_32 extraDistance) const {
    if (!m_content.m_players.get(player)) return false;
    const game::terrain::PolygonTriggerRecord* area =
        m_content.m_terrain.triggerByName(areaName);
    if (!area) return false;
    const std::optional<game::terrain::PolygonTriggerLegacyBounds> bounds =
        game::terrain::TerrainLogic::legacyTriggerBounds(*area);
    if (!bounds) return false;
    const math::q32_32 distance = bounds->radius + extraDistance;
    if (distance < math::q32_32{}) return false;
    const math::q32_32 distanceSquared = distance * distance;

    const auto objects = ecs::view<const OwnerComponent, const ThingTemplateComponent,
                                   const TransformComponent, const ObjectLifecycleComponent>(
        m_world.m_registry);
    for (const ecs::entity entity : objects) {
        const auto& lifecycle = objects.template get<const ObjectLifecycleComponent>(entity);
        if (lifecycle.phase != ObjectLifecyclePhase::Alive) continue;
        const auto& owner = objects.template get<const OwnerComponent>(entity);
        // RefCode uses PartitionFilterPlayerAffiliation(ALLOW_ALLIES, false)
        // followed by PartitionFilterPlayer(player, false): allied and own
        // objects are rejected; neutral/enemy tech buildings remain eligible.
        if (!owner.player || owner.player == player ||
            m_content.m_players.relationships().get(player, owner.player) ==
                PlayerRelationship::Allies) {
            continue;
        }
        const auto& templateComponent =
            objects.template get<const ThingTemplateComponent>(entity);
        if (!templateComponent.archetype ||
            !kindOfContains(templateComponent.archetype,
                            game::ObjectKindOf::TechBuilding)) {
            continue;
        }
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, entity);
        if (health && health->effectivelyDead) continue;
        const auto& transform = objects.template get<const TransformComponent>(entity);
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, entity, transform);
        const math::q32_32 dx = position.x - bounds->centerX;
        const math::q32_32 dy = position.y - bounds->centerY;
        if (dx * dx + dy * dy <= distanceSquared) return true;
    }
    return false;
}

uint32_t GameSessionScriptQueryPort::neutralUnmannedObjectCount() const noexcept {
    uint32_t count = 0;
    const auto objects = ecs::view<const OwnerComponent>(m_world.m_registry);
    for (const ecs::entity entity : objects) {
        const auto& owner = objects.template get<const OwnerComponent>(entity);
        if (owner.player != NEUTRAL_PLAYER_ID ||
            !isObjectDisabledBy(m_world.m_registry, entity,
                                ObjectDisabledReason::Unmanned, m_confirmedTick)) {
            continue;
        }
        if (count != std::numeric_limits<uint32_t>::max()) ++count;
    }
    return count;
}

std::optional<int32_t> GameSessionScriptQueryPort::playerStartPosition(
    PlayerId player) const {
    const PlayerState* state = m_content.m_players.get(player);
    return state ? std::optional<int32_t>{state->startPosition} : std::nullopt;
}

std::optional<container::StringView> GameSessionScriptQueryPort::playerFaction(
    PlayerId player) const {
    const PlayerState* state = m_content.m_players.get(player);
    if (!state || state->side.empty()) return std::nullopt;
    return container::StringView{state->side};
}

bool GameSessionScriptQueryPort::playerSpecialPowerReady(
    PlayerId player, container::StringView specialPower) const noexcept {
    if (!m_content.m_players.get(player) || specialPower.empty()) return false;
    const SpecialPowerDefinition* definition =
        m_content.m_contentSnapshot.findSpecialPower(specialPower);
    if (!definition) return false;

    const bool scienceAvailable = definition->requiredScience.empty() ||
        equalAsciiInsensitive(definition->requiredScience, "None") ||
        equalAsciiInsensitive(definition->requiredScience, "SCIENCE_INVALID") ||
        m_content.m_players.hasScience(player, definition->requiredScience);
    if (!scienceAvailable) return false;

    const auto objects = ecs::view<const OwnerComponent,
                                   const ObjectSpecialPowerComponent>(
        m_world.m_registry);
    for (const ecs::entity entity : objects) {
        if (objects.template get<const OwnerComponent>(entity).player != player)
            continue;

        const ObjectLifecycleComponent* lifecycle =
            ecs::try_get<ObjectLifecycleComponent>(m_world.m_registry, entity);
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(m_world.m_registry, entity);
        const ObjectMapStatusComponent* mapStatus =
            ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, entity);
        const ObjectIdentityComponent* identity =
            ecs::try_get<ObjectIdentityComponent>(m_world.m_registry, entity);
        if (!lifecycle || lifecycle->phase != ObjectLifecyclePhase::Alive ||
            !identity || !identity->id ||
            m_world.m_objects.isPendingDestroy(identity->id) ||
            (health && health->effectivelyDead) ||
            (mapStatus && mapStatus->offMap)) {
            continue;
        }

        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(m_world.m_registry, entity);
        if (status && status->hasAny(
                game::objectStatusBit(
                    game::ObjectStatusFlag::UnderConstruction))) {
            continue;
        }
        if (isObjectDisabled(m_world.m_registry, entity, m_confirmedTick))
            continue;

        const auto& powers =
            objects.template get<const ObjectSpecialPowerComponent>(entity);
        if (!powers.plan) continue;
        for (const ObjectSpecialPowerRuntime& runtime : powers.instances) {
            if (runtime.content == definition->id && runtime.pausedCount == 0 &&
                runtime.readyTick <= m_confirmedTick) {
                return true;
            }
        }
    }
    return false;
}

bool GameSessionScriptQueryPort::consumeSpecialPowerEvent(
    ScriptSpecialPowerEventPhase phase, PlayerId player,
    container::StringView specialPower, ObjectId source) const noexcept {
    return m_eventCursor.consumeSpecialPowerEvent(
        phase, player, specialPower, source);
}

bool GameSessionScriptQueryPort::consumeUpgradeEvent(
    PlayerId player, container::StringView upgrade,
    ObjectId source) const noexcept {
    return m_eventCursor.consumeUpgradeEvent(player, upgrade, source);
}

int64_t GameSessionScriptQueryPort::playerGarrisonedBuildingCount(
    PlayerId player) const noexcept {
    if (!m_content.m_players.get(player)) return 0;
    int64_t count = 0;
    const auto objects = ecs::view<const OwnerComponent,
                                   const ObjectContainmentRuntimeComponent>(
        m_world.m_registry);
    for (const ecs::entity entity : objects) {
        if (objects.template get<const OwnerComponent>(entity).player != player) continue;
        const auto& runtime =
            objects.template get<const ObjectContainmentRuntimeComponent>(entity);
        if (!runtime.plan || !std::any_of(
                runtime.plan->rules.begin(), runtime.plan->rules.end(),
                [](const ObjectContainmentRule& rule) {
                    return rule.kind == ObjectContainmentKind::Garrison;
                })) {
            continue;
        }
        const ObjectContainmentComponent* contents =
            ecs::try_get<ObjectContainmentComponent>(m_world.m_registry, entity);
        if (contents && !contents->objects.empty() &&
            count != std::numeric_limits<int64_t>::max()) {
            ++count;
        }
    }
    return count;
}

int64_t GameSessionScriptQueryPort::playerCapturedUnitCount(
    PlayerId player) const noexcept {
    if (!m_content.m_players.get(player)) return 0;

    int64_t count = 0;
    const auto objects = ecs::view<const OwnerComponent,
                                   const ObjectLifecycleComponent,
                                   const ObjectStatusComponent>(
        m_world.m_registry);
    const game::ObjectStatusMask capturedMask =
        game::objectStatusBit(game::ObjectStatusFlag::Hijacked);
    for (const ecs::entity entity : objects) {
        if (objects.template get<const OwnerComponent>(entity).player != player ||
            objects.template get<const ObjectLifecycleComponent>(entity).phase !=
                ObjectLifecyclePhase::Alive ||
            !objects.template get<const ObjectStatusComponent>(entity).hasAny(
                capturedMask)) {
            continue;
        }
        if (count != std::numeric_limits<int64_t>::max()) ++count;
    }
    return count;
}

bool GameSessionScriptQueryPort::playerCanBuildObjectType(
    PlayerId player, container::StringView objectType) const noexcept {
    const PlayerState* playerState = m_content.m_players.get(player);
    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(objectType);
    if (!playerState || !product) return false;

    const std::optional<game::ObjectBuildabilityStatus> buildability =
        effectiveObjectBuildability(product->templateData.name);
    if (!buildability) return false;
    bool ignorePrerequisites = false;
    switch (*buildability) {
    case game::ObjectBuildabilityStatus::Yes:
        break;
    case game::ObjectBuildabilityStatus::IgnorePrerequisites:
        ignorePrerequisites = true;
        break;
    case game::ObjectBuildabilityStatus::OnlyByAi:
        if (playerState->controller != PlayerControllerKind::Ai) return false;
        break;
    case game::ObjectBuildabilityStatus::No:
        return false;
    }

    const bool structure = kindOfContains(
        product.get(), game::ObjectKindOf::Structure);
    if ((structure && !playerState->constructionPolicy.baseConstructionEnabled) ||
        (!structure && !playerState->constructionPolicy.unitConstructionEnabled)) {
        return false;
    }

    // BSTATUS_IGNORE_PREREQUISITES bypasses both prerequisite and
    // MaxSimultaneous checks in Player::canBuild. All ordinary queries share
    // the same central admission helpers as a real queue command, including
    // ThingTemplate Science prerequisites and effectively-dead filtering.
    return ignorePrerequisites ||
        (playerSatisfiesObjectProductionPrerequisites(
             m_world.m_registry, m_content.m_players,
             m_content.m_contentSnapshot, player, *product) &&
         playerCanBuildMoreOfObjectType(
             m_world.m_registry, player, *product));
}

bool GameSessionScriptQueryPort::objectCompletedWaypointPath(
    ObjectId object, container::StringView pathName) const noexcept {
    if (!object || pathName.empty() || m_confirmedTick == 0) return false;
    const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(object);
    const ObjectWaypointCompletionComponent* completion = entity
        ? ecs::try_get<ObjectWaypointCompletionComponent>(
              m_world.m_registry, *entity)
        : nullptr;
    if (!completion || completion->waypointGraphRevision !=
            m_content.m_terrain.waypointGraphRevision() ||
        completion->completedAtTick == std::numeric_limits<uint64_t>::max() ||
        completion->completedAtTick + 1 != m_confirmedTick) {
        return false;
    }
    const game::terrain::WaypointRecord* terminal =
        m_content.m_terrain.waypointById(completion->terminalWaypointId);
    return terminal && std::any_of(
        terminal->pathLabels.begin(), terminal->pathLabels.end(),
        [pathName](container::StringView label) { return label == pathName; });
}

bool GameSessionScriptQueryPort::teamCompletedWaypointPath(
    ObjectTeamId team, container::StringView pathName) const noexcept {
    if (!m_world.m_objectTeams.find(team) || pathName.empty()) return false;
    // RefCode intentionally returns true when any member (normally the group
    // leader) reports completion; it does not require every team member.
    for (const ObjectId object : m_world.m_objectTeams.members(team)) {
        if (objectCompletedWaypointPath(object, pathName)) return true;
    }
    return false;
}

bool GameSessionScriptQueryPort::playerSupplySourceSafe(
    PlayerId player, int32_t minimumSupplies) const noexcept {
    const PlayerState* state = m_content.m_players.get(player);
    if (!state) return false;
    // Player::isSupplySourceSafe returns true when no AIPlayer is installed.
    if (state->controller != PlayerControllerKind::Ai) return true;

    struct SupplyCandidate final {
        ObjectId object = INVALID_OBJECT_ID;
        math::q32_32 x{};
        math::q32_32 y{};
        math::q32_32 radius{};
    };

    const auto worldObjects = ecs::view<const ObjectIdentityComponent,
                                        const OwnerComponent,
                                        const ObjectLifecycleComponent,
                                        const ThingTemplateComponent,
                                        const TransformComponent>(
        m_world.m_registry);
    math::q32_32 baseX{};
    math::q32_32 baseY{};
    uint64_t baseObjects = 0;
    for (const ecs::entity entity : worldObjects) {
        if (worldObjects.template get<const OwnerComponent>(entity).player != player ||
            worldObjects.template get<const ObjectLifecycleComponent>(entity).phase !=
                ObjectLifecyclePhase::Alive) {
            continue;
        }
        const auto& type =
            worldObjects.template get<const ThingTemplateComponent>(entity);
        if (!type.archetype || !kindOfContains(
                type.archetype, game::ObjectKindOf::Structure)) {
            continue;
        }
        const auto& transform =
            worldObjects.template get<const TransformComponent>(entity);
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, entity, transform);
        baseX += position.x;
        baseY += position.y;
        ++baseObjects;
    }
    if (baseObjects != 0) {
        const math::q32_32 count{
            static_cast<int32_t>(baseObjects)};
        baseX /= count;
        baseY /= count;
    }

    const uint64_t requiredValue = minimumSupplies <= 0
        ? 0u : static_cast<uint64_t>(minimumSupplies);
    std::optional<SupplyCandidate> best;
    math::q32_32 bestDistanceSquared{};
    for (const ecs::entity entity : worldObjects) {
        const auto& lifecycle =
            worldObjects.template get<const ObjectLifecycleComponent>(entity);
        const auto& type =
            worldObjects.template get<const ThingTemplateComponent>(entity);
        const auto& owner = worldObjects.template get<const OwnerComponent>(entity);
        if (lifecycle.phase != ObjectLifecyclePhase::Alive || !type.archetype ||
            !kindOfContains(type.archetype, game::ObjectKindOf::Structure) ||
            !kindOfContains(type.archetype, game::ObjectKindOf::SupplySource) ||
            m_content.m_players.relationships().get(player, owner.player) ==
                PlayerRelationship::Enemies) {
            continue;
        }
        const ObjectEconomyComponent* economy =
            ecs::try_get<ObjectEconomyComponent>(m_world.m_registry, entity);
        if (!economy || economy->supplyWarehouseDocks.empty()) continue;
        uint64_t boxes = 0;
        for (const ObjectSupplyWarehouseDockRuntime& warehouse :
             economy->supplyWarehouseDocks) {
            boxes = std::min<uint64_t>(
                std::numeric_limits<uint64_t>::max() - boxes,
                warehouse.boxesStored) + boxes;
        }
        const uint64_t perBox = static_cast<uint64_t>(std::max<int64_t>(
            0, m_content.m_objectSimulationRules.economy.valuePerSupplyBox));
        const uint64_t supplyValue = perBox != 0 && boxes >
                std::numeric_limits<uint64_t>::max() / perBox
            ? std::numeric_limits<uint64_t>::max()
            : boxes * perBox;
        if (supplyValue < requiredValue) continue;

        const auto& transform =
            worldObjects.template get<const TransformComponent>(entity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, entity);
        const math::q32_32 radius = geometry
            ? math::q32_32::max(
                  math::q32_32{},
                  geometry->boundingCircleRadiusFixed)
            : math::q32_32{};
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, entity, transform);

        // findSupplyCenter rejects sources already served by one of this
        // player's cash generators within 20 legacy path cells.
        bool alreadyServed = false;
        for (const ecs::entity candidate : worldObjects) {
            if (worldObjects.template get<const OwnerComponent>(candidate).player != player ||
                worldObjects.template get<const ObjectLifecycleComponent>(candidate).phase !=
                    ObjectLifecyclePhase::Alive) {
                continue;
            }
            const auto& candidateType =
                worldObjects.template get<const ThingTemplateComponent>(candidate);
            if (!candidateType.archetype || !kindOfContains(
                    candidateType.archetype,
                    game::ObjectKindOf::CashGenerator)) {
                continue;
            }
            const auto& candidateTransform =
                worldObjects.template get<const TransformComponent>(candidate);
            const ObjectGeometryComponent* candidateGeometry =
                ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, candidate);
            const math::q32_32 candidateRadius = candidateGeometry
                ? math::q32_32::max(
                      math::q32_32{},
                      candidateGeometry->boundingCircleRadiusFixed)
                : math::q32_32{};
            const LogicFixedVec3 candidatePosition =
                readAuthoritativeObjectPosition(
                    m_world.m_registry, candidate, candidateTransform);
            const math::q32_32 dx = candidatePosition.x - position.x;
            const math::q32_32 dy = candidatePosition.y - position.y;
            const math::q32_32 closeDistance =
                math::q32_32{int32_t{200}} + radius + candidateRadius;
            if (dx * dx + dy * dy <= closeDistance * closeDistance) {
                alreadyServed = true;
                break;
            }
        }
        if (alreadyServed) continue;

        const math::q32_32 dx = position.x - baseX;
        const math::q32_32 dy = position.y - baseY;
        const math::q32_32 distanceSquared = dx * dx + dy * dy;
        const ObjectId identity =
            worldObjects.template get<const ObjectIdentityComponent>(entity).id;
        if (!best || distanceSquared < bestDistanceSquared ||
            (distanceSquared == bestDistanceSquared && identity < best->object)) {
            best = SupplyCandidate{
                .object = identity,
                .x = position.x,
                .y = position.y,
                .radius = radius,
            };
            bestDistanceSquared = distanceSquared;
        }
    }
    if (!best) return true;

    for (const ecs::entity entity : worldObjects) {
        const auto& lifecycle =
            worldObjects.template get<const ObjectLifecycleComponent>(entity);
        const auto& owner = worldObjects.template get<const OwnerComponent>(entity);
        const auto& type =
            worldObjects.template get<const ThingTemplateComponent>(entity);
        if (lifecycle.phase != ObjectLifecyclePhase::Alive || !type.archetype ||
            m_content.m_players.relationships().get(player, owner.player) !=
                PlayerRelationship::Enemies ||
            kindOfContains(type.archetype, game::ObjectKindOf::Harvester) ||
            kindOfContains(type.archetype, game::ObjectKindOf::Dozer)) {
            continue;
        }
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(m_world.m_registry, entity);
        if (status && status->hasAny(
                game::objectStatusBit(game::ObjectStatusFlag::Stealthed)) &&
            !status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::Detected)) &&
            !status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::Disguised))) {
            continue;
        }
        const auto& transform =
            worldObjects.template get<const TransformComponent>(entity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, entity);
        const math::q32_32 enemyRadius = geometry
            ? math::q32_32::max(
                  math::q32_32{},
                  geometry->boundingCircleRadiusFixed)
            : math::q32_32{};
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, entity, transform);
        const math::q32_32 dx = position.x - best->x;
        const math::q32_32 dy = position.y - best->y;
        const math::q32_32 safeDistance =
            math::q32_32::max(
                math::q32_32{},
                m_content.m_objectSimulationRules.ai
                    .supplyCenterSafeRadius) +
            best->radius + enemyRadius;
        if (dx * dx + dy * dy <= safeDistance * safeDistance) return false;
    }
    return true;
}

bool GameSessionScriptQueryPort::playerSupplySourceAttacked(
    PlayerId player) const noexcept {
    const PlayerState* state = m_content.m_players.get(player);
    if (!state || state->controller != PlayerControllerKind::Ai ||
        m_confirmedTick == 0) {
        return false;
    }
    constexpr uint64_t kLegacyAttackWindow = 10;
    const auto objects = ecs::view<const OwnerComponent,
                                   const ObjectLifecycleComponent,
                                   const ThingTemplateComponent,
                                   const ObjectHealthComponent>(
        m_world.m_registry);
    for (const ecs::entity entity : objects) {
        if (objects.template get<const OwnerComponent>(entity).player != player ||
            objects.template get<const ObjectLifecycleComponent>(entity).phase !=
                ObjectLifecyclePhase::Alive) {
            continue;
        }
        const auto& type =
            objects.template get<const ThingTemplateComponent>(entity);
        if (!type.archetype ||
            (!kindOfContains(type.archetype, game::ObjectKindOf::CashGenerator) &&
             !kindOfContains(type.archetype, game::ObjectKindOf::Dozer) &&
             !kindOfContains(type.archetype, game::ObjectKindOf::Harvester))) {
            continue;
        }
        const auto& health =
            objects.template get<const ObjectHealthComponent>(entity);
        if (!health.hasLastDamageInfo ||
            health.lastDamageType == game::DamageType::HEALING ||
            health.lastDamageTick > m_confirmedTick) {
            continue;
        }
        if (m_confirmedTick - health.lastDamageTick < kLegacyAttackWindow)
            return true;
    }
    return false;
}

bool GameSessionScriptQueryPort::suppliesWithinDistance(
    PlayerId player, container::StringView areaName,
    math::q32_32 extraDistance,
    math::q32_32 minimumValue) const noexcept {
    if (!m_content.m_players.get(player)) return false;
    const game::terrain::PolygonTriggerRecord* area =
        m_content.m_terrain.triggerByName(areaName);
    if (!area) return false;
    const std::optional<game::terrain::PolygonTriggerLegacyBounds> bounds =
        game::terrain::TerrainLogic::legacyTriggerBounds(*area);
    if (!bounds) return false;
    const math::q32_32 distance = bounds->radius + extraDistance;
    if (distance < math::q32_32{}) return false;
    const math::q32_32 distanceSquared = distance * distance;

    uint64_t maximumValue = 0;
    const auto objects = ecs::view<const OwnerComponent,
                                   const ThingTemplateComponent,
                                   const TransformComponent,
                                   const ObjectEconomyComponent>(
        m_world.m_registry);
    for (const ecs::entity entity : objects) {
        const PlayerId owner = objects.template get<const OwnerComponent>(entity).player;
        // PartitionFilterPlayerAffiliation(ALLOW_NEUTRAL, true) admits self
        // plus every side currently neutral to the querying player; it is not
        // limited to the canonical Neutral player slot.
        if (owner != player &&
            m_content.m_players.relationships().get(player, owner) !=
                PlayerRelationship::Neutral) continue;
        const ObjectMapStatusComponent* mapStatus =
            ecs::try_get<ObjectMapStatusComponent>(m_world.m_registry, entity);
        if (mapStatus && mapStatus->offMap) continue;
        const auto& type = objects.template get<const ThingTemplateComponent>(entity);
        if (!type.archetype ||
            !kindOfContains(type.archetype, game::ObjectKindOf::Structure)) continue;
        const auto& transform = objects.template get<const TransformComponent>(entity);
        const LogicFixedVec3 position = readAuthoritativeObjectPosition(
            m_world.m_registry, entity, transform);
        const math::q32_32 dx = position.x - bounds->centerX;
        const math::q32_32 dy = position.y - bounds->centerY;
        if (dx * dx + dy * dy > distanceSquared) continue;

        const auto& economy = objects.template get<const ObjectEconomyComponent>(entity);
        // findUpdateModule(NameKey) returns the first authored matching
        // module. Multiple warehouse modules are legal but do not stack.
        if (economy.supplyWarehouseDocks.empty()) continue;
        const uint64_t perBox = static_cast<uint64_t>(std::max<int64_t>(
            0, m_content.m_objectSimulationRules.economy.valuePerSupplyBox));
        const uint64_t boxes =
            economy.supplyWarehouseDocks.front().boxesStored;
        const uint64_t value = perBox != 0 && boxes >
                std::numeric_limits<uint64_t>::max() / perBox
            ? std::numeric_limits<uint64_t>::max()
            : boxes * perBox;
        maximumValue = std::max(maximumValue, value);
    }
    // The WorldBuilder string says "at least", but RefCode is strictly >.
    if (maximumValue > static_cast<uint64_t>(
            std::numeric_limits<int32_t>::max())) {
        return true;
    }
    return math::q32_32{static_cast<int32_t>(maximumValue)} >
           minimumValue;
}


} // namespace engine::script
