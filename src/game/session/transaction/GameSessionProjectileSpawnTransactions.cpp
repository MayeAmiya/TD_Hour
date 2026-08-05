#include "game/session/transaction/GameSessionProjectileSpawnTransactions.h"

#include "debug/debug.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/session/state/GameSessionDomainState.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace engine {

GameSessionProjectileSpawnTransactions::GameSessionProjectileSpawnTransactions(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionObjectLifecycleTransactions lifecycle) noexcept
    : m_content(content),
      m_world(world),
      m_lifecycle(std::move(lifecycle)) {}

bool GameSessionProjectileSpawnTransactions::spawn(
    const ObjectProjectileSpawnRequest& request) {
    // Every rejection below silently deletes a projectile that the weapon layer
    // already reported as Fired.  For strategic missiles that is the difference
    // between "the superweapon has no effect" and a diagnosable content or
    // spawn failure, so name the reason instead of dropping it.
    const auto rejectUnsupported = [&](const char* reason) {
        TD_LOG_ERROR(
            "[GameSession] Projectile spawn rejected: launcher={} template='{}' tick={} reason={}",
            request.launcher.value, request.projectileTemplate,
            request.confirmedTick, reason);
        return false;
    };
    if (!m_content.m_active || !request.launcher ||
        request.projectileTemplate.empty() || !request.detonationWeapon) {
        return rejectUnsupported("missing launcher/template/detonation weapon");
    }

    // FireWeaponWhenDead may run after DestroyRequested, while the pending
    // launcher still owns its authoritative owner/team components.
    const std::optional<ecs::entity> launcher =
        m_world.m_objects.entityFromIdIncludingPending(request.launcher);
    const OwnerComponent* owner = launcher
        ? ecs::try_get<OwnerComponent>(m_world.m_registry, *launcher)
        : nullptr;
    const PrimaryTeamComponent* team = launcher
        ? ecs::try_get<PrimaryTeamComponent>(m_world.m_registry, *launcher)
        : nullptr;
    if (!launcher || !owner || !team || !owner->player || !team->team) {
        return rejectUnsupported("launcher has no owner/team snapshot");
    }

    ObjectSpawnRequest spawnRequest;
    spawnRequest.templateName = request.projectileTemplate;
    spawnRequest.owner = owner->player;
    spawnRequest.primaryTeam = team->team;
    spawnRequest.transform = ObjectFixedTransformComponent{
        .position = request.launchPosition,
        .authoritative = true,
    };
    spawnRequest.origin = ObjectCreationOrigin::System;
    spawnRequest.confirmedTick = request.confirmedTick;
    spawnRequest.producer = request.launcher;
    const bool launcherTracksSpecialPowerCompletion =
        m_world.m_objectSimulation.hasSpecialPowerCompletionDie(
            m_world.m_registry, m_world.m_objects, request.launcher);
    GameSessionObjectSpawnResult spawned =
        m_lifecycle.spawnObject(std::move(spawnRequest));
    if (!spawned) return rejectUnsupported("object spawn transaction failed");

    if (launcherTracksSpecialPowerCompletion) {
        static_cast<void>(
            m_world.m_objectSimulation.notifySpecialPowerCompletion(
                m_world.m_registry, m_world.m_objects, request.launcher,
                request.confirmedTick));
        static_cast<void>(
            m_world.m_objectSimulation.setSpecialPowerCompletionCreator(
                m_world.m_registry, m_world.m_objects, spawned.object,
                INVALID_OBJECT_ID));
    } else {
        static_cast<void>(
            m_world.m_objectSimulation.setSpecialPowerCompletionCreator(
                m_world.m_registry, m_world.m_objects, spawned.object,
                request.launcher));
    }

    const ThingTemplateComponent* projectileTemplate =
        ecs::try_get<ThingTemplateComponent>(
            m_world.m_registry, *spawned.entity);
    const bool neutronProjectile = projectileTemplate &&
        projectileTemplate->archetype &&
        projectileTemplate->archetype->projectilePlan &&
        projectileTemplate->archetype->projectilePlan->behaviorKind ==
            ObjectProjectileBehaviorKind::NeutronMissile;
    ObjectProjectileSpawnRequest effectiveRequest = request;
    if (neutronProjectile && request.intendedTarget) {
        const std::optional<ecs::entity> target =
            m_world.m_objects.entityFromId(request.intendedTarget);
        const TransformComponent* targetTransform = target
            ? ecs::try_get<TransformComponent>(m_world.m_registry, *target)
            : nullptr;
        if (targetTransform) {
            effectiveRequest.targetPosition = readAuthoritativeObjectPosition(
                m_world.m_registry, *target, *targetTransform);
        }
    }

    if (!projectileTemplate || !projectileTemplate->archetype ||
        !m_world.m_objectProjectiles.initializeObject(
            m_world.m_registry, *spawned.entity, spawned.object,
            *projectileTemplate->archetype, m_content.m_contentSnapshot,
            effectiveRequest, m_content.m_terrain,
            static_cast<uint32_t>(
                std::max(1, m_content.m_startInfo.gameSpeedFPS)),
            m_world.m_objectSimulation.rules()
                .gravityUnitsPerSecondSq)) {
        static_cast<void>(m_lifecycle.requestDestroyObject(
            spawned.object, ObjectDestroyReason::System,
            request.confirmedTick));
        return rejectUnsupported(
            "projectile flight initialization failed - missing projectile plan or unsupported behavior");
    }
    if (request.intendedTarget) {
        static_cast<void>(
            m_world.m_objectSimulation.reportIncomingSmallMissile(
                m_world.m_registry, m_world.m_objects,
                request.intendedTarget, spawned.object,
                m_content.m_simulationRandom, request.confirmedTick));
    }
    return true;
}

} // namespace engine
