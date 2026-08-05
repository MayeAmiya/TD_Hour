#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"

#include "math/fixed/q32_32_trig.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace engine::detail {
namespace {

using Fixed = math::q32_32;
using Participant = ObjectAIMovementObstructionParticipant;

const Fixed kZero{};
const Fixed kOne{int32_t{1}};
const Fixed kTwo{int32_t{2}};
constexpr Fixed kPi = Fixed::from_raw(13'493'037'705ll);
const Fixed kHalfPi = kPi / kTwo;
const Fixed kQuarterPi = kPi / Fixed{int32_t{4}};
constexpr Fixed kInfantryDirectionThreshold =
    Fixed::from_fraction(1, 4);
constexpr Fixed kCoincidentScale = Fixed::from_fraction(1, 10000);

[[nodiscard]] Fixed normalizeAngle(Fixed value) noexcept {
    const Fixed fullTurn = kPi * kTwo;
    int64_t raw = value.raw() % fullTurn.raw();
    if (raw > kPi.raw()) raw -= fullTurn.raw();
    if (raw < -kPi.raw()) raw += fullTurn.raw();
    return Fixed::from_raw(raw);
}

[[nodiscard]] Fixed dotDirection(const Participant& left,
                                 const Participant& right) noexcept {
    return left.direction.x * right.direction.x +
        left.direction.y * right.direction.y;
}

// AIUpdateInterface::hasHigherPathPriority(), expressed only in frozen
// fixed-point facts. Locomotor GroupMovementPriority is deliberately absent:
// ZH applies Dozer, Vehicle-vs-Infantry, front-most and ObjectId tie-breaks.
[[nodiscard]] bool hasHigherPathPriority(
    const Participant& self, const Participant& other) noexcept {
    if (self.dozer != other.dozer) return self.dozer;
    if (self.vehicle && other.infantry) return true;
    if (self.infantry && other.vehicle) return false;

    if (dotDirection(self, other) <= kZero)
        return self.object < other.object;

    const Fixed combinedX = self.direction.x + other.direction.x;
    const Fixed combinedY = self.direction.y + other.direction.y;
    const Fixed toOtherX = other.position.x - self.position.x;
    const Fixed toOtherY = other.position.y - self.position.y;
    const Fixed ahead = combinedX * toOtherX + combinedY * toOtherY;
    if (ahead > kZero) return false;
    if (ahead < kZero) return true;
    return self.object < other.object;
}

[[nodiscard]] Fixed relativeAngleTo(
    const Participant& self, const Participant& other) noexcept {
    const Fixed dx = other.position.x - self.position.x;
    const Fixed dy = other.position.y - self.position.y;
    return normalizeAngle(
        math::fixed_atan2(dy, dx) -
        math::fixed_atan2(self.direction.y, self.direction.x));
}

// Frozen equivalent of AIUpdateInterface::blockedBy(). The contact itself is
// already established by Physics; this function preserves the gameplay gates
// that decide whether the overlap is an AI obstruction.
[[nodiscard]] bool blockedBy(
    const ObjectAIMovementObstructionEvent& event,
    uint32_t ticksPerSecond) noexcept {
    const Participant& self = event.mover;
    const Participant& other = event.blocker;
    if (!self.moving || self.nearFinalGoal ||
        event.moverCanCrushBlocker || !self.doingGroundMovement ||
        !other.doingGroundMovement || self.movingBackward ||
        other.effectivelyDead) {
        return false;
    }

    const Fixed directionDot = dotDirection(self, other);
    if (self.infantry && other.infantry &&
        directionDot <= kInfantryDirectionThreshold) {
        return false;
    }

    const Fixed dx = self.position.x - other.position.x;
    const Fixed dy = self.position.y - other.position.y;
    const Fixed distanceSquared = dx * dx + dy * dy;
    const Fixed cell = Fixed::max(kZero, event.pathfindCellSize);
    if (distanceSquared < cell * cell * kCoincidentScale)
        return hasHigherPathPriority(self, other);

    if (self.blockedTicks > std::max<uint32_t>(1, ticksPerSecond) &&
        directionDot <= kZero) {
        return false;
    }

    const Fixed collisionAngle = relativeAngleTo(self, other);
    if (Fixed::abs(collisionAngle) > kHalfPi) return false;

    Fixed angleLimit = kQuarterPi;
    if (!other.moving)
        angleLimit *= Fixed::from_fraction(3, 4);
    if (Fixed::abs(collisionAngle) > angleLimit) {
        if (directionDot <= kZero) return false;
        if (!other.moving) return false;

        const Fixed otherAngle = relativeAngleTo(other, self);
        if (Fixed::abs(otherAngle) <= angleLimit) return false;

        const Fixed projectedX =
            dx + self.direction.x - other.direction.x;
        const Fixed projectedY =
            dy + self.direction.y - other.direction.y;
        if (distanceSquared <=
            projectedX * projectedX + projectedY * projectedY) {
            return false;
        }
        if (hasHigherPathPriority(self, other)) return false;
    }
    return true;
}

[[nodiscard]] bool isMoveAsideFor(
    const ObjectOrderIntent& order, ObjectId obstacle) noexcept {
    return order.kind == ObjectOrderKind::Move &&
        order.source == ObjectOrderSource::System &&
        order.moveRouteSubtype == ObjectMoveRouteSubtype::MoveAside &&
        order.systemPurpose == ObjectOrderSystemPurpose::MoveAside &&
        order.systemPurposeInstance == obstacle.value;
}

void consumeGroupPathOptimization(ObjectOrderIntent& order) noexcept {
    if (order.groupPathId != 0 && order.hasTargetPosition) {
        order.targetX += order.groupPathOffsetX;
        order.targetY += order.groupPathOffsetY;
    }
    order.groupPathId = 0;
    order.groupPathMemberOrdinal = 0;
    order.groupPathMemberCount = 0;
    order.groupPathStartX = {};
    order.groupPathStartY = {};
    order.groupPathStartZ = {};
    order.groupPathOffsetX = {};
    order.groupPathOffsetY = {};
}

[[nodiscard]] const ObjectAIMovementObstructionEvent* findReverse(
    container::Span<const ObjectAIMovementObstructionEvent> events,
    const ObjectAIMovementObstructionEvent& event) noexcept {
    // Physics emits the two directed records consecutively in stable contact
    // order, not in (mover, blocker) key order. A lower_bound with a different
    // comparator therefore missed valid reverse records depending on ObjectId.
    for (const ObjectAIMovementObstructionEvent& candidate : events) {
        if (candidate.mover.object == event.blocker.object &&
            candidate.blocker.object == event.mover.object) {
            return &candidate;
        }
    }
    return nullptr;
}

void rememberMoveAside(
    ObjectAIMovementObstructionStateComponent& state,
    ObjectId blocker, uint64_t tick) noexcept {
    if (state.blocker != blocker) {
        state.previousBlocker = state.blocker;
        state.blocker = blocker;
        state.consecutiveTicks = 1;
    } else if (state.lastContactTick != tick) {
        const bool consecutive = state.lastContactTick < tick &&
            tick - state.lastContactTick == 1u;
        if (!consecutive) {
            state.consecutiveTicks = 1;
        } else if (state.consecutiveTicks !=
                   std::numeric_limits<uint32_t>::max()) {
            ++state.consecutiveTicks;
        }
    }
    state.lastContactTick = tick;
}

// AIUpdateInterface::calculateMaxBlockedSpeed(). Physics discovers the
// contact after this tick's locomotion step, so clamp the retained locomotor
// speed for the next step. Repeated contact keeps the cap active without
// introducing a second movement owner or physically pushing either unit.
void applyBlockedSpeedLimit(
    ecs::registry& registry, ecs::entity moverEntity,
    ecs::entity blockerEntity,
    const ObjectAIMovementObstructionEvent& event) noexcept {
    ObjectLocomotionComponent* mover =
        ecs::try_get<ObjectLocomotionComponent>(registry, moverEntity);
    const ObjectLocomotionComponent* blocker =
        ecs::try_get<ObjectLocomotionComponent>(registry, blockerEntity);
    if (!mover || !blocker || mover->forwardSpeed <= kZero) return;

    Fixed toBlockerX = event.blocker.position.x - event.mover.position.x;
    Fixed toBlockerY = event.blocker.position.y - event.mover.position.y;
    const Fixed distance = Fixed::sqrt(
        toBlockerX * toBlockerX + toBlockerY * toBlockerY);
    if (distance <= kZero) {
        mover->forwardSpeed = kZero;
        return;
    }
    toBlockerX /= distance;
    toBlockerY /= distance;

    const Fixed blockerProjection =
        toBlockerX * event.blocker.direction.x +
        toBlockerY * event.blocker.direction.y;
    Fixed maximum = kZero;
    if (blockerProjection >= kZero) {
        const Fixed approachProjection =
            toBlockerX * event.mover.direction.x +
            toBlockerY * event.mover.direction.y;
        if (approachProjection <= kZero) return;
        const Fixed blockerSpeed = Fixed::max(
            kZero, blocker->forwardSpeed);
        maximum = blockerSpeed * blockerProjection /
            approachProjection;
    }

    const ObjectPlayerFormationComponent* moverFormation =
        ecs::try_get<ObjectPlayerFormationComponent>(registry, moverEntity);
    const ObjectPlayerFormationComponent* blockerFormation =
        ecs::try_get<ObjectPlayerFormationComponent>(registry, blockerEntity);
    if (moverFormation && blockerFormation && moverFormation->id != 0 &&
        moverFormation->id == blockerFormation->id) {
        maximum *= Fixed::from_fraction(55, 100);
    }
    mover->forwardSpeed = Fixed::min(
        mover->forwardSpeed, Fixed::max(kZero, maximum));
}

// Physics samples contacts after locomotion has published this tick's final
// pose. RefCode's processCollision lowers the mover's speed before it can pass
// through the blocker on subsequent updates; our detached transaction also
// restores the swept contact pose so the already-published current step cannot
// tunnel through before that speed/MoveAside decision takes effect. A previous
// contact in the same tick wins because it has the earlier submission ordinal.
[[nodiscard]] bool clampBlockedMoverToContact(
    ecs::registry& registry, ecs::entity moverEntity,
    const ObjectAIMovementObstructionEvent& event) {
    const ObjectFixedTransformComponent* transform =
        ecs::try_get<ObjectFixedTransformComponent>(registry, moverEntity);
    if (!transform || !transform->authoritative) return false;
    const LogicFixedVec3 current = transform->position;
    if (current.x != event.mover.position.x ||
        current.y != event.mover.position.y ||
        current.z != event.mover.position.z) {
        return false;
    }
    writeAuthoritativeObjectPosition(
        registry, moverEntity, event.moverContactPosition);
    return true;
}

} // namespace

bool GameSessionWeaponEventDrain::handleAIMovementObstructionBatch(
    WorkItem item) {
    auto& world = m_world;
    auto& content = m_content;
    const uint64_t confirmedTick =
        m_presentation.m_confirmedTick;
    const uint32_t ticksPerSecond = std::max<uint32_t>(
        1, content.m_objectSimulationRules.logicFramesPerSecond);
    const container::Span<const ObjectAIMovementObstructionEvent> events{
        item.aiMovementObstructionBatch.events};
    size_t blockedCount = 0;
    size_t clampedCount = 0;
    size_t moveAsideCount = 0;

    const auto setCollisionIgnore = [&](ecs::entity entity,
                                        ObjectId blocker) {
        const uint64_t duration = static_cast<uint64_t>(ticksPerSecond) * 2u;
        const uint64_t untilTick = duration >
                std::numeric_limits<uint64_t>::max() - confirmedTick
            ? std::numeric_limits<uint64_t>::max()
            : confirmedTick + duration;
        ObjectTemporaryCollisionIgnoreComponent* collisionIgnore =
            ecs::try_get<ObjectTemporaryCollisionIgnoreComponent>(
                world.m_registry, entity);
        if (!collisionIgnore) {
            ecs::emplace<ObjectTemporaryCollisionIgnoreComponent>(
                world.m_registry, entity,
                ObjectTemporaryCollisionIgnoreComponent{
                    .untilTick = untilTick,
                    .other = blocker,
                });
        } else if (!collisionIgnore->other ||
                   collisionIgnore->other == blocker ||
                   collisionIgnore->untilTick <= confirmedTick) {
            collisionIgnore->untilTick =
                std::max(collisionIgnore->untilTick, untilTick);
            collisionIgnore->other = blocker;
        }
    };

    const auto hasMoveAsideOrderFor = [&](ecs::entity entity,
                                          ObjectId obstacle) {
        const ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(
                world.m_registry, entity);
        return queue && !queue->orders.empty() &&
            isMoveAsideFor(queue->orders.front(), obstacle);
    };

    const auto rememberContact = [&](ecs::entity entity, ObjectId blocker) {
        ObjectAIMovementObstructionStateComponent* state =
            ecs::try_get<ObjectAIMovementObstructionStateComponent>(
                world.m_registry, entity);
        const bool continuous = state && state->blocker == blocker &&
            (state->lastContactTick == confirmedTick ||
             (state->lastContactTick < confirmedTick &&
              confirmedTick - state->lastContactTick == 1u));
        if (!state) {
            state = &ecs::emplace<
                ObjectAIMovementObstructionStateComponent>(
                    world.m_registry, entity);
        }
        rememberMoveAside(*state, blocker, confirmedTick);
        return std::pair{state, !continuous};
    };

    const auto repathDirectMoverOnce = [&](ecs::entity moverEntity,
                                           ObjectId blocker) {
        const auto [state, firstContact] =
            rememberContact(moverEntity, blocker);
        static_cast<void>(state);
        if (!firstContact) return false;
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(
                world.m_registry, moverEntity);
        if (!queue || queue->orders.empty()) return false;
        ObjectOrderIntent& order = queue->orders.front();
        if (order.kind != ObjectOrderKind::Move ||
            order.moveRouteSubtype != ObjectMoveRouteSubtype::Direct) {
            return false;
        }
        consumeGroupPathOptimization(order);
        ++queue->revision;
        return true;
    };

    const auto issueMoveAside = [&](ecs::entity subjectEntity,
                                    const Participant& subject,
                                    const Participant& obstacle) {
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(
                world.m_registry, subjectEntity);
        if (!queue) {
            queue = &ecs::emplace<ObjectOrderQueueComponent>(
                world.m_registry, subjectEntity);
        }
        if (!queue->orders.empty() &&
            isMoveAsideFor(queue->orders.front(), obstacle.object)) {
            // The order may have been inserted by the opposite directed
            // collision record earlier in this same batch, before ObjectAI
            // observes it. It is already the one temporary MoveAway request.
            return false;
        }
        if (queue->orders.size() >=
            ObjectOrderQueueComponent::MaximumQueuedOrders) {
            return false;
        }

        Fixed awayX = subject.position.x - obstacle.position.x;
        Fixed awayY = subject.position.y - obstacle.position.y;
        const Fixed length = Fixed::sqrt(
            awayX * awayX + awayY * awayY);
        if (length.raw() == 0) {
            awayX = subject.object < obstacle.object ? -kOne : kOne;
            awayY = kZero;
        } else {
            awayX /= length;
            awayY /= length;
        }

        const ObjectGeometryComponent* subjectGeometry =
            ecs::try_get<ObjectGeometryComponent>(
                world.m_registry, subjectEntity);
        const std::optional<ecs::entity> obstacleEntity =
            world.m_objects.entityFromId(obstacle.object);
        const ObjectGeometryComponent* obstacleGeometry = obstacleEntity
            ? ecs::try_get<ObjectGeometryComponent>(
                  world.m_registry, *obstacleEntity)
            : nullptr;
        const Fixed distance = Fixed{int32_t{40}} +
            (subjectGeometry
                ? Fixed::max(kZero,
                      subjectGeometry->boundingCircleRadiusFixed)
                : kZero) +
            (obstacleGeometry
                ? Fixed::max(kZero,
                      obstacleGeometry->boundingCircleRadiusFixed)
                : kZero);
        const Fixed targetX = subject.position.x + awayX * distance;
        const Fixed targetY = subject.position.y + awayY * distance;
        const Fixed targetZ = Fixed::from_raw(
            content.m_terrain.groundHeightRaw(
                targetX.raw(), targetY.raw()));
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(
            world.m_registry, subjectEntity);
        ObjectOrderIntent moveAside{
            .kind = ObjectOrderKind::Move,
            .source = ObjectOrderSource::System,
            .contextPlayer = owner ? owner->player : INVALID_PLAYER_ID,
            .issuedTick = confirmedTick,
            .sourceSequence = obstacle.object.value,
            .targetX = targetX,
            .targetY = targetY,
            .targetZ = targetZ,
            .hasTargetPosition = true,
            .moveRouteSubtype = ObjectMoveRouteSubtype::MoveAside,
            .systemPurpose = ObjectOrderSystemPurpose::MoveAside,
            .systemPurposeInstance = obstacle.object.value,
        };
        // ZH uses a temporary AI state over the current command. In the
        // queue-backed runtime, a front insertion preserves that parent
        // command and resumes it after MoveAside completes.
        // The group centerline is intentionally one-shot.  It has already
        // been consumed by the interrupted Move state and its Navigation
        // cache is released after the initial batch; retaining the handle
        // metadata here made the resumed parent fail with NoPath.
        if (!queue->orders.empty()) {
            consumeGroupPathOptimization(queue->orders.front());
        }
        queue->orders.insert(queue->orders.begin(), std::move(moveAside));
        ++queue->revision;

        ObjectAIMovementObstructionStateComponent* state =
            ecs::try_get<ObjectAIMovementObstructionStateComponent>(
                world.m_registry, subjectEntity);
        if (!state) {
            state = &ecs::emplace<
                ObjectAIMovementObstructionStateComponent>(
                    world.m_registry, subjectEntity);
        }
        rememberMoveAside(*state, obstacle.object, confirmedTick);
        ++moveAsideCount;
        return true;
    };

    for (const ObjectAIMovementObstructionEvent& event : events) {
        if (!event.mover.object || !event.blocker.object ||
            event.confirmedTick != confirmedTick) {
            continue;
        }
        const std::optional<ecs::entity> mover =
            world.m_objects.entityFromId(event.mover.object);
        const std::optional<ecs::entity> blocker =
            world.m_objects.entityFromId(event.blocker.object);
        if (!mover || !blocker) {
            continue;
        }
        const bool blockerUnavailable = event.blocker.effectivelyDead ||
            isObjectDisabled(
                world.m_registry, *blocker, confirmedTick);
        if (blockerUnavailable) {
            // A dead/EMP/paralyzed member cannot execute MoveAside.  Keeping
            // the rear actor on the old shared centerline makes every later
            // member inherit a permanent zero-speed blocker.  Re-admit the
            // mover's existing order as an individual path instead; the
            // frozen path-object field then treats the stationary ally as a
            // cost and routes around it.  One continuous contact causes one
            // revision only, avoiding a repath storm while the replacement
            // path is being solved.
            if (ObjectLocomotionComponent* locomotion =
                    ecs::try_get<ObjectLocomotionComponent>(
                        world.m_registry, *mover)) {
                locomotion->forwardSpeed = kZero;
            }
            static_cast<void>(repathDirectMoverOnce(
                *mover, event.blocker.object));
            continue;
        }
        if (!blockedBy(event, ticksPerSecond)) continue;
        ++blockedCount;
        clampedCount += clampBlockedMoverToContact(
            world.m_registry, *mover, event) ? 1u : 0u;
        applyBlockedSpeedLimit(
            world.m_registry, *mover, *blocker, event);

        // RefCode's moveAllies() is deliberately relationship-gated.
        // Neutral/enemy actors remain navigation/physics obstacles (or valid
        // Crusher/Squish victims); they must not be rewritten into a
        // temporary friendly MoveAside order.
        if (relationshipBetweenObjects(
                world.m_registry, content.m_players, *mover, *blocker) !=
            PlayerRelationship::Allies) {
            // Enemy and neutral path units never receive MoveAside and must
            // not be displaced by Physics. Re-admit the current direct move
            // once for each continuous blocker contact so navigation can use
            // its frozen EnemyBlock/NeutralBlock occupancy and route around
            // the actor without generating a per-tick repath storm.
            static_cast<void>(repathDirectMoverOnce(
                *mover, event.blocker.object));
            continue;
        }

        // privateMoveAwayFromUnit remembers the two requesters. Re-entering
        // the same temporary MoveAway while still blocked grants a pair-local
        // two-second collision ignore; the first request never phases.
        if (hasMoveAsideOrderFor(*mover, event.blocker.object)) {
            const auto [obstruction, firstContact] = rememberContact(
                *mover, event.blocker.object);
            if (!firstContact && obstruction->consecutiveTicks >= 2u) {
                setCollisionIgnore(*mover, event.blocker.object);
            }
            continue;
        }

        // The opposite directed record can observe the yielding order before
        // ObjectAI enters MoveOutOfTheWay. Once the blocker accepted that
        // request, this mover retains its path and must never be selected as
        // the loser by the bilateral deadlock branch.
        if (hasMoveAsideOrderFor(*blocker, event.mover.object)) {
            continue;
        }

        const ObjectAIMovementObstructionEvent* reverse =
            findReverse(events, event);
        if (reverse && blockedBy(*reverse, ticksPerSecond) &&
            event.mover.hasPath && event.blocker.hasPath &&
            event.mover.moving && event.blocker.moving &&
            !event.mover.needsRotation &&
            !event.blocker.needsRotation) {
            if (!hasHigherPathPriority(event.mover, event.blocker)) {
                static_cast<void>(issueMoveAside(
                    *mover, event.mover, event.blocker));
            }
            continue;
        }

        // Patch 1.01's unilateral branch: a commanded ground mover may ask
        // an idle, non-busy friendly locomotion unit to step aside. The
        // relationship gate above is intentional; this is not a generic
        // neutral/enemy avoidance mechanism.
        if (event.blocker.moving || event.blocker.effectivelyDead) {
            continue;
        }
        const ObjectOrderQueueComponent* blockerQueue =
            ecs::try_get<ObjectOrderQueueComponent>(
                world.m_registry, *blocker);
        const ObjectLocomotionComponent* blockerLocomotion =
            ecs::try_get<ObjectLocomotionComponent>(
                world.m_registry, *blocker);
        const ObjectContainedByComponent* blockerContained =
            ecs::try_get<ObjectContainedByComponent>(
                world.m_registry, *blocker);
        const std::optional<ai::ObjectAIActorStateView> blockerState =
            m_ai.m_objectAI.actorState(
                event.blocker.object);
        if ((blockerQueue && !blockerQueue->orders.empty()) ||
            !blockerLocomotion ||
            (blockerContained && blockerContained->enclosing) ||
            !blockerState || blockerState->state != ai::AIStateId::Idle) {
            static_cast<void>(repathDirectMoverOnce(
                *mover, event.blocker.object));
            continue;
        }
        // DozerAIUpdate/WorkerAIUpdate intercept MOVE_AWAY_FROM_UNIT before
        // the inherited idle AI sees it.  A builder with a live dozer task
        // ignores an ordinary unit's request; only another dozer may force
        // it to make room.  The specialized builder state is independent of
        // the generic ObjectAI idle state, so both facts are required here.
        if (event.blocker.dozer && !event.mover.dozer &&
            ObjectBuilderSystem{}.isAnyTaskPending(
                world.m_registry, world.m_objects,
                event.blocker.object)) {
            static_cast<void>(repathDirectMoverOnce(
                *mover, event.blocker.object));
            continue;
        }
        if (!issueMoveAside(*blocker, event.blocker, event.mover)) {
            static_cast<void>(repathDirectMoverOnce(
                *mover, event.blocker.object));
        }
    }
    if (!events.empty() && confirmedTick % ticksPerSecond == 0u) {
        const ObjectAIMovementObstructionEvent& sample = events.front();
        const std::optional<ecs::entity> sampleMover =
            world.m_objects.entityFromId(sample.mover.object);
        const std::optional<ecs::entity> sampleBlocker =
            world.m_objects.entityFromId(sample.blocker.object);
        const ObjectOrderQueueComponent* sampleBlockerQueue = sampleBlocker
            ? ecs::try_get<ObjectOrderQueueComponent>(
                  world.m_registry, *sampleBlocker)
            : nullptr;
        const std::optional<ai::ObjectAIActorStateView> sampleBlockerState =
            m_ai.m_objectAI.actorState(sample.blocker.object);
        const int relationship = sampleMover && sampleBlocker
            ? static_cast<int>(relationshipBetweenObjects(
                  world.m_registry, content.m_players,
                  *sampleMover, *sampleBlocker))
            : -1;
        TD_LOG_INFO(
            "[MovementObstruction] tick={} events={} blocked={} clamped={} moveAside={} sample={}->{} relation={} facts[moving={}/{} path={}/{} ground={}/{} nearGoal={} backward={} dead={} infantry={}/{} rotate={}/{} blockedTicks={} dotRaw={} angleRaw={} blockerQueue={} blockerState={}]",
            confirmedTick, events.size(), blockedCount, clampedCount,
            moveAsideCount, sample.mover.object.value,
            sample.blocker.object.value, relationship,
            sample.mover.moving, sample.blocker.moving,
            sample.mover.hasPath, sample.blocker.hasPath,
            sample.mover.doingGroundMovement,
            sample.blocker.doingGroundMovement,
            sample.mover.nearFinalGoal, sample.mover.movingBackward,
            sample.blocker.effectivelyDead,
            sample.mover.infantry, sample.blocker.infantry,
            sample.mover.needsRotation, sample.blocker.needsRotation,
            sample.mover.blockedTicks,
            dotDirection(sample.mover, sample.blocker).raw(),
            relativeAngleTo(sample.mover, sample.blocker).raw(),
            sampleBlockerQueue ? sampleBlockerQueue->orders.size() : 0u,
            sampleBlockerState
                ? static_cast<int>(sampleBlockerState->state)
                : -1);
    }
    return true;
}

} // namespace engine::detail
