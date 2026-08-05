#include "game/object/simulation/economy/ObjectEconomy.h"

#include "game/object/simulation/economy/ObjectEconomyDetail.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/simulation/lifecycle/ObjectCreate.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/runtime/ObjectHackInternetOrderAdapter.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/structure/ObjectSupplyWarehouseCrippling.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/base/GameBalanceConstants.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>

namespace engine {

using namespace object_economy_detail;

ObjectRepairDockCommandResult ObjectEconomySystem::processRepairDockCommand(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules,
    const ObjectRepairDockCommand& command) const {
    ObjectRepairDockCommandResult result;
    const std::optional<ecs::entity> dockEntity =
        lifecycle.entityFromId(command.dock);
    if (!dockEntity || lifecycle.isPendingDestroy(command.dock)) {
        result.status = ObjectRepairDockCommandStatus::DockMissing;
        return result;
    }
    ObjectEconomyComponent* economy =
        ecs::try_get<ObjectEconomyComponent>(registry, *dockEntity);
    if (!economy || !economy->plan ||
        command.moduleIndex >= economy->repairDocks.size() ||
        command.moduleIndex >= economy->plan->repairDocks.size()) {
        result.status = ObjectRepairDockCommandStatus::DockMissing;
        return result;
    }
    ObjectRepairDockRuntime& runtime =
        economy->repairDocks[command.moduleIndex];
    const game::ObjectRepairDockRule& rule =
        economy->plan->repairDocks[command.moduleIndex];
    result.allowsPassthrough = rule.dock.allowsPassthrough;

    const auto clearDocker = [&](ObjectId docker) {
        bool changed = false;
        for (size_t index = 0; index < runtime.dock.approachOwners.size();
             ++index) {
            if (runtime.dock.approachOwners[index] != docker) continue;
            runtime.dock.approachOwners[index] = INVALID_OBJECT_ID;
            runtime.dock.approachReached[index] = false;
            runtime.approachOwnerPlayers[index] = INVALID_PLAYER_ID;
            changed = true;
        }
        if (runtime.dock.activeDocker == docker) {
            runtime.dock.activeDocker = INVALID_OBJECT_ID;
            runtime.dock.activeDockerInside = false;
            runtime.activeDockerAtActionPoint = false;
            runtime.repairSubject = INVALID_OBJECT_ID;
            runtime.pendingDrone = INVALID_OBJECT_ID;
            runtime.dockOwnerAtAdmission = INVALID_PLAYER_ID;
            runtime.dockerOwnerAtAdmission = INVALID_PLAYER_ID;
            runtime.healthToAddPerTick = {};
            runtime.actionPending = false;
            changed = true;
        }
        if (!runtime.dock.activeDocker &&
            std::none_of(runtime.dock.approachOwners.begin(),
                         runtime.dock.approachOwners.end(),
                         [](ObjectId value) {
                             return static_cast<bool>(value);
                         })) {
            runtime.reservationDockOwner = INVALID_PLAYER_ID;
        }
        if (changed) {
            ++runtime.dock.revision;
            ++runtime.revision;
        }
    };
    if (command.kind == ObjectRepairDockCommandKind::Cancel) {
        clearDocker(command.docker);
        result.status = ObjectRepairDockCommandStatus::Accepted;
        return result;
    }

    const std::optional<ecs::entity> dockerEntity =
        lifecycle.entityFromId(command.docker);
    if (!dockerEntity || lifecycle.isPendingDestroy(command.docker) ||
        !isAliveObject(registry, lifecycle, *dockerEntity, command.docker) ||
        hasBlockingStatus(registry, *dockerEntity, command.confirmedTick)) {
        clearDocker(command.docker);
        result.status = ObjectRepairDockCommandStatus::Denied;
        return result;
    }
    if (!isAliveObject(registry, lifecycle, *dockEntity, command.dock)) {
        clearDocker(command.docker);
        result.status = ObjectRepairDockCommandStatus::DockClosed;
        return result;
    }
    const bool exitOperation =
        command.kind == ObjectRepairDockCommandKind::QueryExitPosition ||
        command.kind == ObjectRepairDockCommandKind::NotifyExitReached;
    const bool activeInside = runtime.dock.activeDocker == command.docker &&
        runtime.dock.activeDockerInside;
    if ((!runtime.dock.open || hasBlockingStatus(
             registry, *dockEntity, command.confirmedTick)) &&
        !(exitOperation && activeInside)) {
        // Preserve the active inside relation so AIDock can observe DockClosed
        // and request the exit position. Approach owners have not crossed the
        // containment boundary and can be released immediately.
        if (!activeInside) clearDocker(command.docker);
        result.status = ObjectRepairDockCommandStatus::DockClosed;
        return result;
    }
    const OwnerComponent* dockOwner =
        ecs::try_get<OwnerComponent>(registry, *dockEntity);
    const OwnerComponent* dockerOwner =
        ecs::try_get<OwnerComponent>(registry, *dockerEntity);
    if (!dockOwner || !dockerOwner || !dockOwner->player ||
        !dockerOwner->player) {
        clearDocker(command.docker);
        result.status = ObjectRepairDockCommandStatus::Denied;
        return result;
    }

    enum class DockPoint : uint8_t { Approach, Enter, Action, Exit };
    const auto stagePosition = [&](DockPoint point, int32_t approachPosition) {
        const ObjectFixedTransformComponent* dockTransform =
            ecs::try_get<ObjectFixedTransformComponent>(registry,
                                                        *dockEntity);
        if (!dockTransform || !dockTransform->authoritative)
            return LogicFixedVec3{};
        const ObjectGeometryComponent* dockGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry, *dockEntity);
        const ObjectGeometryComponent* dockerGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry, *dockerEntity);
        const math::q32_32 one{int32_t{1}};
        const math::q32_32 dockRadius = dockGeometry
            ? math::q32_32::max(one,
                                dockGeometry->boundingCircleRadiusFixed)
            : math::q32_32{int32_t{10}};
        const math::q32_32 dockerRadius = dockerGeometry
            ? math::q32_32::max(one,
                                dockerGeometry->boundingCircleRadiusFixed)
            : math::q32_32{int32_t{10}};
        const math::q32_32 clearance = dockRadius + dockerRadius;
        const math::q32_32_sincos facing = math::fixed_sincos(
            dockTransform->yawRadians);
        const math::q32_32 forwardX = facing.cosine;
        const math::q32_32 forwardY = facing.sine;
        const math::q32_32 sideX = -forwardY;
        const math::q32_32 sideY = forwardX;
        const LogicFixedVec3* authoredLocal = nullptr;
        bool authoredLocalValid = false;
        switch (point) {
        case DockPoint::Approach:
            if (approachPosition >= 0 &&
                static_cast<size_t>(approachPosition) <
                    runtime.dock.approachPositionsLocal.size() &&
                static_cast<size_t>(approachPosition) <
                    runtime.dock.approachPositionValid.size()) {
                authoredLocal = &runtime.dock.approachPositionsLocal[
                    static_cast<size_t>(approachPosition)];
                authoredLocalValid = runtime.dock.approachPositionValid[
                    static_cast<size_t>(approachPosition)];
            }
            break;
        case DockPoint::Enter:
            authoredLocal = &runtime.dock.enterPositionLocal;
            authoredLocalValid = runtime.dock.enterPositionValid;
            break;
        case DockPoint::Action:
            authoredLocal = &runtime.dock.actionPositionLocal;
            authoredLocalValid = runtime.dock.actionPositionValid;
            break;
        case DockPoint::Exit:
            authoredLocal = &runtime.dock.exitPositionLocal;
            authoredLocalValid = runtime.dock.exitPositionValid;
            break;
        }
        if (authoredLocal && authoredLocalValid) {
            return LogicFixedVec3{
                .x = dockTransform->position.x +
                    facing.cosine * authoredLocal->x -
                    facing.sine * authoredLocal->y,
                .y = dockTransform->position.y +
                    facing.sine * authoredLocal->x +
                    facing.cosine * authoredLocal->y,
                .z = dockTransform->position.z + authoredLocal->z,
            };
        }
        const int32_t ordinal = std::max(0, approachPosition);
        const math::q32_32 lateral = ordinal == 0 ? math::q32_32{}
            : math::q32_32{static_cast<int32_t>((ordinal + 1) / 2 * 8)} *
                  ((ordinal & 1) ? one : -one);
        LogicFixedVec3 value = dockTransform->position;
        switch (point) {
        case DockPoint::Approach:
            value.x -= forwardX *
                (clearance + math::q32_32{int32_t{24}});
            value.y -= forwardY *
                (clearance + math::q32_32{int32_t{24}});
            value.x += sideX * lateral;
            value.y += sideY * lateral;
            break;
        case DockPoint::Enter:
            value.x -= forwardX * clearance;
            value.y -= forwardY * clearance;
            break;
        case DockPoint::Action:
            break;
        case DockPoint::Exit: {
            const math::q32_32 direction =
                rule.dock.allowsPassthrough ? one : -one;
            value.x += forwardX * direction *
                (clearance + math::q32_32{int32_t{16}});
            value.y += forwardY * direction *
                (clearance + math::q32_32{int32_t{16}});
            break;
        }
        }
        // Boneless or incomplete authored docks retain the deterministic
        // geometry fallback. Authored DockWaiting/DockStart/DockAction/
        // DockEnd points above are frozen from pristine model data at spawn.
        return value;
    };

    const auto reserveApproach = [&]() -> int32_t {
        if (!runtime.dock.open) return -1;
        for (size_t index = 0; index < runtime.dock.approachOwners.size();
             ++index) {
            if (runtime.dock.approachOwners[index] == command.docker)
                return static_cast<int32_t>(index);
        }
        size_t free = runtime.dock.approachOwners.size();
        for (size_t index = 0; index < runtime.dock.approachOwners.size();
             ++index) {
            if (!runtime.dock.approachOwners[index]) {
                free = index;
                break;
            }
        }
        if (free == runtime.dock.approachOwners.size()) {
            if (rule.dock.numberApproachPositions >= 0) return -1;
            runtime.dock.approachOwners.push_back(INVALID_OBJECT_ID);
            runtime.dock.approachReached.push_back(false);
            runtime.approachOwnerPlayers.push_back(INVALID_PLAYER_ID);
        }
        if (std::none_of(runtime.dock.approachOwners.begin(),
                         runtime.dock.approachOwners.end(),
                         [](ObjectId value) { return static_cast<bool>(value); }) &&
            !runtime.dock.activeDocker) {
            runtime.reservationDockOwner = dockOwner->player;
        }
        runtime.dock.approachOwners[free] = command.docker;
        runtime.dock.approachReached[free] = false;
        runtime.approachOwnerPlayers[free] = dockerOwner->player;
        ++runtime.dock.revision;
        ++runtime.revision;
        return static_cast<int32_t>(free);
    };

    switch (command.kind) {
    case ObjectRepairDockCommandKind::ReserveApproach: {
        const int32_t reserved = reserveApproach();
        if (reserved < 0) break;
        result.approachPosition = reserved;
        result.position = stagePosition(DockPoint::Approach, reserved);
        result.status = ObjectRepairDockCommandStatus::Accepted;
        return result;
    }
    case ObjectRepairDockCommandKind::PollClearance: {
        const ObjectDockCrippleComponent* cripple =
            ecs::try_get<ObjectDockCrippleComponent>(registry, *dockEntity);
        if (runtime.dock.open && !runtime.dock.activeDocker &&
            !(cripple && cripple->crippled())) {
            for (size_t index = 0; index < runtime.dock.approachOwners.size();
                 ++index) {
                if (!runtime.dock.approachOwners[index] ||
                    !runtime.dock.approachReached[index]) {
                    continue;
                }
                runtime.dock.activeDocker =
                    runtime.dock.approachOwners[index];
                const std::optional<ecs::entity> activeEntity =
                    lifecycle.entityFromId(runtime.dock.activeDocker);
                const OwnerComponent* activeOwner = activeEntity
                    ? ecs::try_get<OwnerComponent>(registry, *activeEntity)
                    : nullptr;
                runtime.dockOwnerAtAdmission = dockOwner->player;
                runtime.dockerOwnerAtAdmission = activeOwner
                    ? activeOwner->player : INVALID_PLAYER_ID;
                ++runtime.dock.revision;
                ++runtime.revision;
                break;
            }
        }
        if (runtime.dock.activeDocker == command.docker) {
            result.status = ObjectRepairDockCommandStatus::ClearToEnter;
            return result;
        }
        if (command.approachPosition > 0 &&
            static_cast<size_t>(command.approachPosition) <
                runtime.dock.approachOwners.size() &&
            runtime.dock.approachOwners[command.approachPosition] ==
                command.docker &&
            runtime.dock.approachReached[command.approachPosition] &&
            !runtime.dock.approachOwners[command.approachPosition - 1]) {
            result.status = ObjectRepairDockCommandStatus::ClearToAdvance;
            return result;
        }
        result.status = ObjectRepairDockCommandStatus::ClearanceWaiting;
        return result;
    }
    case ObjectRepairDockCommandKind::AdvanceApproach: {
        const int32_t current = command.approachPosition;
        if (current <= 0 ||
            static_cast<size_t>(current) >=
                runtime.dock.approachOwners.size() ||
            runtime.dock.approachOwners[current] != command.docker ||
            runtime.dock.approachOwners[current - 1]) {
            break;
        }
        runtime.dock.approachOwners[current - 1] = command.docker;
        runtime.dock.approachReached[current - 1] = false;
        runtime.approachOwnerPlayers[current - 1] =
            runtime.approachOwnerPlayers[current];
        runtime.dock.approachOwners[current] = INVALID_OBJECT_ID;
        runtime.dock.approachReached[current] = false;
        runtime.approachOwnerPlayers[current] = INVALID_PLAYER_ID;
        ++runtime.dock.revision;
        ++runtime.revision;
        result.approachPosition = current - 1;
        result.position = stagePosition(DockPoint::Approach, current - 1);
        result.status = ObjectRepairDockCommandStatus::Accepted;
        return result;
    }
    case ObjectRepairDockCommandKind::QueryEntryPosition:
        if (runtime.dock.activeDocker != command.docker) break;
        result.position = stagePosition(DockPoint::Enter,
                                        command.approachPosition);
        result.status = ObjectRepairDockCommandStatus::Accepted;
        return result;
    case ObjectRepairDockCommandKind::QueryDockPosition:
        if (runtime.dock.activeDocker != command.docker ||
            !runtime.dock.activeDockerInside) break;
        result.position = stagePosition(DockPoint::Action,
                                        command.approachPosition);
        result.status = ObjectRepairDockCommandStatus::Accepted;
        return result;
    case ObjectRepairDockCommandKind::QueryExitPosition:
        result.position = stagePosition(DockPoint::Exit,
                                        command.approachPosition);
        result.status = ObjectRepairDockCommandStatus::Accepted;
        return result;
    case ObjectRepairDockCommandKind::NotifyApproachReached: {
        size_t position = command.approachPosition >= 0
            ? static_cast<size_t>(command.approachPosition)
            : runtime.dock.approachOwners.size();
        if (position >= runtime.dock.approachOwners.size() ||
            runtime.dock.approachOwners[position] != command.docker) {
            const auto found = std::find(runtime.dock.approachOwners.begin(),
                                         runtime.dock.approachOwners.end(),
                                         command.docker);
            if (found == runtime.dock.approachOwners.end()) break;
            position = static_cast<size_t>(
                found - runtime.dock.approachOwners.begin());
        }
        runtime.dock.approachReached[position] = true;
        ++runtime.dock.revision;
        ++runtime.revision;
        result.status = ObjectRepairDockCommandStatus::Accepted;
        return result;
    }
    case ObjectRepairDockCommandKind::NotifyEnterReached:
        if (runtime.dock.activeDocker != command.docker) break;
        for (size_t index = 0; index < runtime.dock.approachOwners.size();
             ++index) {
            if (runtime.dock.approachOwners[index] != command.docker) continue;
            runtime.dock.approachOwners[index] = INVALID_OBJECT_ID;
            runtime.dock.approachReached[index] = false;
            runtime.approachOwnerPlayers[index] = INVALID_PLAYER_ID;
        }
        runtime.dock.activeDockerInside = true;
        ++runtime.dock.revision;
        ++runtime.revision;
        result.status = ObjectRepairDockCommandStatus::Accepted;
        return result;
    case ObjectRepairDockCommandKind::NotifyDockReached:
        if (runtime.dock.activeDocker != command.docker ||
            !runtime.dock.activeDockerInside) break;
        runtime.activeDockerAtActionPoint = true;
        ++runtime.revision;
        result.status = ObjectRepairDockCommandStatus::Accepted;
        return result;
    case ObjectRepairDockCommandKind::NotifyExitReached:
        if (runtime.dock.activeDocker != command.docker) break;
        clearDocker(command.docker);
        result.status = ObjectRepairDockCommandStatus::Accepted;
        return result;
    case ObjectRepairDockCommandKind::ProcessAction: {
        if (runtime.dock.activeDocker != command.docker ||
            !runtime.dock.activeDockerInside ||
            !runtime.activeDockerAtActionPoint) break;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, *dockerEntity);
        if (!health || health->maximumFixed <= math::q32_32{} ||
            health->currentFixed >= health->maximumFixed) {
            runtime.repairSubject = INVALID_OBJECT_ID;
            runtime.pendingDrone = INVALID_OBJECT_ID;
            runtime.healthToAddPerTick = {};
            runtime.actionPending = false;
            ++runtime.revision;
            result.status = ObjectRepairDockCommandStatus::ActionComplete;
            return result;
        }
        if (runtime.repairSubject != command.docker) {
            const uint64_t durationTicks = std::max<uint64_t>(
                1u, millisecondsToTicks(rule.timeForFullHealMilliseconds,
                                        rules.logicFramesPerSecond));
            const int32_t boundedTicks = static_cast<int32_t>(
                std::min<uint64_t>(durationTicks,
                    static_cast<uint64_t>(std::numeric_limits<int32_t>::max())));
            runtime.repairSubject = command.docker;
            runtime.healthToAddPerTick =
                (health->maximumFixed - health->currentFixed) /
                math::q32_32{boundedTicks};
            ++runtime.revision;
        }

        // RefCode heals the first producer-linked DRONE together with its
        // owner. Stable ObjectId ordering replaces Player::iterateObjects'
        // incidental container order and also lets a newly-built drone join
        // on a later action tick.
        ObjectId drone = INVALID_OBJECT_ID;
        const auto droneView = ecs::view<
            const ObjectIdentityComponent, const ObjectProducerComponent,
            const ObjectKindOfComponent>(registry);
        for (const ecs::entity candidate : droneView) {
            const ObjectProducerComponent& producer =
                droneView.template get<const ObjectProducerComponent>(candidate);
            if (producer.producer != command.docker) continue;
            const ObjectKindOfComponent& kinds =
                droneView.template get<const ObjectKindOfComponent>(candidate);
            if (!hasKind(&kinds, game::ObjectKindOf::Drone)) continue;
            const ObjectIdentityComponent& identity =
                droneView.template get<const ObjectIdentityComponent>(candidate);
            if (!isAliveObject(registry, lifecycle, candidate, identity.id) ||
                hasBlockingStatus(registry, candidate,
                                  command.confirmedTick)) {
                continue;
            }
            if (!drone || identity.id < drone) drone = identity.id;
        }
        runtime.pendingDrone = drone;
        runtime.pendingActionTick = command.confirmedTick;
        runtime.actionPending = true;
        ++runtime.revision;
        result.drone = drone;
        result.status = ObjectRepairDockCommandStatus::ActionContinue;
        return result;
    }
    case ObjectRepairDockCommandKind::Cancel:
        break;
    }
    result.status = ObjectRepairDockCommandStatus::Denied;
    return result;
}

void ObjectEconomySystem::updateRepairDocks(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSimulationRules& rules, uint64_t confirmedTick,
    container::Vector<ObjectDamageRequest>& outDamage) const {
    static_cast<void>(rules);
    // Snapshot and sort by ObjectId before emitting anything.  Every other
    // economy pass in this directory sorts its candidates explicitly, because
    // registry order is not part of the simulation contract: walking it raw made
    // the order of emitted heal requests depend on component-pool layout, which
    // a save/load or replay that rebuilds the registry differently would change.
    struct RepairDockSubject { ObjectId id; ecs::entity entity; };
    container::Vector<RepairDockSubject> dockSubjects;
    {
        const auto dockView =
            ecs::view<const ObjectIdentityComponent, ObjectEconomyComponent>(
                registry);
        dockSubjects.reserve(dockView.size_hint());
        for (const ecs::entity dockEntity : dockView) {
            dockSubjects.push_back(
                {dockView.template get<const ObjectIdentityComponent>(dockEntity).id,
                 dockEntity});
        }
    }
    std::sort(dockSubjects.begin(), dockSubjects.end(),
              [](const RepairDockSubject& a, const RepairDockSubject& b) {
                  return a.id < b.id;
              });

    for (const RepairDockSubject& dockSubject : dockSubjects) {
        const ecs::entity dockEntity = dockSubject.entity;
        const ObjectIdentityComponent* dockIdentityPtr =
            ecs::try_get<const ObjectIdentityComponent>(registry, dockEntity);
        ObjectEconomyComponent* economyPtr =
            ecs::try_get<ObjectEconomyComponent>(registry, dockEntity);
        if (!dockIdentityPtr || !economyPtr) continue;
        const ObjectIdentityComponent& dockIdentity = *dockIdentityPtr;
        ObjectEconomyComponent& economy = *economyPtr;
        if (!economy.plan) continue;
        const OwnerComponent* dockOwner =
            ecs::try_get<OwnerComponent>(registry, dockEntity);
        const bool dockValid =
            isAliveObject(registry, lifecycle, dockEntity, dockIdentity.id) &&
            !hasBlockingStatus(registry, dockEntity, confirmedTick) &&
            dockOwner && dockOwner->player;

        const size_t count = std::min(economy.plan->repairDocks.size(),
                                      economy.repairDocks.size());
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectRepairDockRule& rule =
                economy.plan->repairDocks[index];
            ObjectRepairDockRuntime& runtime = economy.repairDocks[index];
            bool changed = false;
            const auto clearActive = [&]() {
                changed = changed || static_cast<bool>(runtime.dock.activeDocker) ||
                    runtime.dock.activeDockerInside ||
                    runtime.activeDockerAtActionPoint || runtime.actionPending ||
                    static_cast<bool>(runtime.repairSubject) ||
                    static_cast<bool>(runtime.pendingDrone);
                runtime.dock.activeDocker = INVALID_OBJECT_ID;
                runtime.dock.activeDockerInside = false;
                runtime.activeDockerAtActionPoint = false;
                runtime.repairSubject = INVALID_OBJECT_ID;
                runtime.pendingDrone = INVALID_OBJECT_ID;
                runtime.dockOwnerAtAdmission = INVALID_PLAYER_ID;
                runtime.dockerOwnerAtAdmission = INVALID_PLAYER_ID;
                runtime.healthToAddPerTick = {};
                runtime.actionPending = false;
            };
            if (dockValid && runtime.reservationDockOwner &&
                runtime.reservationDockOwner != dockOwner->player) {
                for (size_t slot = 0;
                     slot < runtime.dock.approachOwners.size(); ++slot) {
                    changed = changed ||
                        static_cast<bool>(runtime.dock.approachOwners[slot]) ||
                        runtime.dock.approachReached[slot];
                    runtime.dock.approachOwners[slot] = INVALID_OBJECT_ID;
                    runtime.dock.approachReached[slot] = false;
                    runtime.approachOwnerPlayers[slot] = INVALID_PLAYER_ID;
                }
                runtime.reservationDockOwner = INVALID_PLAYER_ID;
                clearActive();
            }
            for (size_t slot = 0; slot < runtime.dock.approachOwners.size();
                 ++slot) {
                const ObjectId docker = runtime.dock.approachOwners[slot];
                if (!docker) continue;
                const std::optional<ecs::entity> dockerEntity =
                    lifecycle.entityFromId(docker);
                const OwnerComponent* dockerOwner = dockerEntity
                    ? ecs::try_get<OwnerComponent>(registry, *dockerEntity)
                    : nullptr;
                const bool valid = dockValid && dockerEntity &&
                    !lifecycle.isPendingDestroy(docker) &&
                    isAliveObject(registry, lifecycle, *dockerEntity, docker) &&
                    !hasBlockingStatus(registry, *dockerEntity,
                                       confirmedTick) &&
                    dockerOwner &&
                    dockerOwner->player == runtime.approachOwnerPlayers[slot];
                if (valid) continue;
                runtime.dock.approachOwners[slot] = INVALID_OBJECT_ID;
                runtime.dock.approachReached[slot] = false;
                runtime.approachOwnerPlayers[slot] = INVALID_PLAYER_ID;
                changed = true;
            }
            if (!dockValid) {
                clearActive();
            } else if (runtime.dock.activeDocker) {
                const std::optional<ecs::entity> dockerEntity =
                    lifecycle.entityFromId(runtime.dock.activeDocker);
                const OwnerComponent* dockerOwner = dockerEntity
                    ? ecs::try_get<OwnerComponent>(registry, *dockerEntity)
                    : nullptr;
                const bool activeValid = dockerEntity &&
                    !lifecycle.isPendingDestroy(runtime.dock.activeDocker) &&
                    isAliveObject(registry, lifecycle, *dockerEntity,
                                  runtime.dock.activeDocker) &&
                    !hasBlockingStatus(registry, *dockerEntity,
                                       confirmedTick) &&
                    dockerOwner &&
                    dockOwner->player == runtime.dockOwnerAtAdmission &&
                    dockerOwner->player == runtime.dockerOwnerAtAdmission;
                if (!activeValid) clearActive();
            }
            if (!runtime.dock.activeDocker &&
                std::none_of(runtime.dock.approachOwners.begin(),
                             runtime.dock.approachOwners.end(),
                             [](ObjectId value) {
                                 return static_cast<bool>(value);
                             })) {
                runtime.reservationDockOwner = INVALID_PLAYER_ID;
            }
            if (changed) {
                ++runtime.dock.revision;
                ++runtime.revision;
            }
            if (!runtime.actionPending ||
                runtime.pendingActionTick > confirmedTick ||
                !runtime.dock.activeDockerInside ||
                !runtime.activeDockerAtActionPoint ||
                runtime.repairSubject != runtime.dock.activeDocker) {
                continue;
            }

            const std::optional<ecs::entity> dockerEntity =
                lifecycle.entityFromId(runtime.repairSubject);
            const ObjectHealthComponent* health = dockerEntity
                ? ecs::try_get<ObjectHealthComponent>(registry, *dockerEntity)
                : nullptr;
            if (health && health->currentFixed < health->maximumFixed &&
                health->maximumFixed > math::q32_32{}) {
                math::q32_32 amount = runtime.healthToAddPerTick;
                if (amount <= math::q32_32{})
                    amount = health->maximumFixed - health->currentFixed;
                outDamage.push_back({
                    .target = runtime.repairSubject,
                    .source = dockIdentity.id,
                    .sourceSequence = rule.authoredOrder,
                    .amount = amount,
                    .damageType = game::DamageType::HEALING,
                    .confirmedTick = confirmedTick,
                });
            }
            if (runtime.pendingDrone) {
                const std::optional<ecs::entity> droneEntity =
                    lifecycle.entityFromId(runtime.pendingDrone);
                const ObjectProducerComponent* producer = droneEntity
                    ? ecs::try_get<ObjectProducerComponent>(registry,
                                                             *droneEntity)
                    : nullptr;
                const ObjectKindOfComponent* kinds = droneEntity
                    ? ecs::try_get<ObjectKindOfComponent>(registry,
                                                           *droneEntity)
                    : nullptr;
                const ObjectHealthComponent* droneHealth = droneEntity
                    ? ecs::try_get<ObjectHealthComponent>(registry, *droneEntity)
                    : nullptr;
                if (droneEntity &&
                    isAliveObject(registry, lifecycle, *droneEntity,
                                  runtime.pendingDrone) &&
                    !hasBlockingStatus(registry, *droneEntity,
                                       confirmedTick) &&
                    producer && producer->producer == runtime.repairSubject &&
                    hasKind(kinds, game::ObjectKindOf::Drone) && droneHealth &&
                    droneHealth->maximumFixed > math::q32_32{} &&
                    droneHealth->currentFixed < droneHealth->maximumFixed) {
                    outDamage.push_back({
                        .target = runtime.pendingDrone,
                        .source = dockIdentity.id,
                        .sourceSequence = rule.authoredOrder,
                        .amount = droneHealth->maximumFixed,
                        .damageType = game::DamageType::HEALING,
                        .confirmedTick = confirmedTick,
                    });
                }
            }
            runtime.pendingDrone = INVALID_OBJECT_ID;
            runtime.actionPending = false;
            ++runtime.revision;
        }
    }
}


} // namespace engine
