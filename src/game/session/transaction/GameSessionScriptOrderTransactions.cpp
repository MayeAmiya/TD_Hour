#include "game/session/transaction/GameSessionScriptOrderTransactions.h"

#include "game/object/ai/runtime/ObjectAIRuntime.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/base/SimulationRandom.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/terrain/TerrainLogic.h"

namespace engine {

GameSessionScriptOrderTransactions::GameSessionScriptOrderTransactions(
    ecs::registry& registry, ObjectLifecycle& objects,
    ai::ObjectAIRuntime& objectAI,
    const GameContentSnapshot& content,
    game::terrain::TerrainLogic& terrain,
    SimulationRandom& random,
    ObjectSimulation& simulation) noexcept
    : m_registry(registry),
      m_objects(objects),
      m_objectAI(objectAI),
      m_content(content),
      m_terrain(terrain),
      m_random(random),
      m_simulation(simulation) {}

bool GameSessionScriptOrderTransactions::face(
    ObjectId actor, ObjectId targetObject,
    const std::optional<LogicFixedVec3>& targetPosition,
    uint64_t confirmedTick) {
    if (!actor || static_cast<bool>(targetObject) == targetPosition.has_value() ||
        !m_objectAI.actorState(actor)) {
        return false;
    }
    const std::optional<ecs::entity> actorEntity =
        m_objects.entityFromId(actor);
    const TransformComponent* actorTransform = actorEntity
        ? ecs::try_get<TransformComponent>(m_registry, *actorEntity)
        : nullptr;
    if (!actorEntity || !actorTransform) return false;

    if (targetObject) {
        const std::optional<ecs::entity> targetEntity =
            m_objects.entityFromId(targetObject);
        const TransformComponent* targetTransform = targetEntity
            ? ecs::try_get<TransformComponent>(m_registry, *targetEntity)
            : nullptr;
        if (!targetEntity || !targetTransform) return false;
    }

    ObjectOrderQueueComponent* queue =
        ecs::try_get<ObjectOrderQueueComponent>(m_registry, *actorEntity);
    uint64_t externalRevision = queue ? queue->externalRevision + 1 : 1;
    if (externalRevision == 0) externalRevision = 1;
    std::optional<ai::AIFixedPosition> fixedTarget;
    if (targetPosition) {
        fixedTarget = ai::AIFixedPosition{
            targetPosition->x.raw(), targetPosition->y.raw(),
            targetPosition->z.raw()};
    }
    const ai::ObjectAIFacingTransitionResult staged =
        m_objectAI.stageFacingState(
            actor, targetObject, fixedTarget, confirmedTick,
            externalRevision);
    if (!staged.succeeded()) return false;
    if (!queue) {
        queue = &ecs::emplace<ObjectOrderQueueComponent>(
            m_registry, *actorEntity);
    }
    queue->orders.clear();
    ++queue->revision;
    if (queue->revision == 0) ++queue->revision;
    queue->externalRevision = externalRevision;
    queue->replacementExternalRevision = externalRevision;
    queue->replacementExternalSource = ObjectOrderSource::Script;
    queue->replacementExternalKind = ObjectOrderKind::Stop;
    if (ObjectLocomotionComponent* locomotion =
            ecs::try_get<ObjectLocomotionComponent>(
                m_registry, *actorEntity)) {
        locomotion->forwardSpeed = {};
        locomotion->hasActiveMove = false;
        locomotion->state = ObjectLocomotionState::Idle;
    }
    if (ObjectPhysicsComponent* physics =
            ecs::try_get<ObjectPhysicsComponent>(m_registry, *actorEntity)) {
        physics->yawRate = {};
    }
    return true;
}

bool GameSessionScriptOrderTransactions::fireWeaponFollowingWaypointPath(
    ObjectId object, container::StringView waypointPath,
    uint32_t authoredOrder, uint64_t confirmedTick) {
    if (!object || waypointPath.empty()) return false;
    const std::optional<ecs::entity> entity = m_objects.entityFromId(object);
    if (!entity || m_objects.isPendingDestroy(object)) return false;
    const ObjectFixedTransformComponent* fixedTransform =
        ecs::try_get<ObjectFixedTransformComponent>(m_registry, *entity);
    ObjectWeaponComponent* weapons =
        ecs::try_get<ObjectWeaponComponent>(m_registry, *entity);
    if (!fixedTransform || !fixedTransform->authoritative || !weapons ||
        !weapons->activeWeaponSetIndex ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return false;
    }

    const LogicFixedVec3& sourcePosition = fixedTransform->position;
    const game::terrain::WaypointRecord* start =
        m_terrain.closestWaypointOnPathRaw(
            sourcePosition.x.raw(), sourcePosition.y.raw(), waypointPath);
    if (!start) return false;

    const ObjectWeaponSetRuntime& active =
        weapons->sets[*weapons->activeWeaponSetIndex];
    std::optional<game::WeaponSlot> selected;
    for (size_t index = game::kWeaponSlotCount; index > 0; --index) {
        const size_t slotIndex = index - 1;
        const game::WeaponTemplate* definition =
            m_content.findWeapon(active.slots[slotIndex].content);
        if (!definition || !definition->capableOfFollowingWaypoints)
            continue;
        selected = static_cast<game::WeaponSlot>(slotIndex);
        break;
    }
    if (!selected) return false;

    container::Vector<ObjectSystemWeaponFireCommand> commands;
    if (!tryQueueObjectSlotWeaponFireAtPosition(
            m_registry, *entity, object, *selected, sourcePosition,
            m_content, m_random,
            std::max<uint32_t>(
                1u, m_simulation.rules().logicFramesPerSecond),
            authoredOrder,
            m_simulation.reserveGameplaySubmissionOrdinal(),
            confirmedTick, commands, false)) {
        return false;
    }
    const bool fired = !commands.empty();
    for (ObjectSystemWeaponFireCommand& command : commands) {
        command.waypointPathStartId = start->id;
        command.waypointGraphRevision = m_terrain.waypointGraphRevision();
        m_simulation.queueSystemWeaponFireCommand(std::move(command));
    }
    return fired;
}

} // namespace engine
