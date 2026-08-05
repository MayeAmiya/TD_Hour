#include "game/object/simulation/economy/ObjectEconomy.h"

#include "game/object/simulation/economy/ObjectEconomyDetail.h"

#include "core/container/string_utils.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/lifecycle/ObjectCreate.h"
#include "game/object/simulation/runtime/ObjectDamage.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/object/simulation/runtime/ObjectHackInternetOrderAdapter.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/simulation/status/ObjectStealth.h"
#include "game/object/simulation/structure/ObjectSupplyWarehouseCrippling.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/weapon/WeaponTemplate.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "core/math/fixed/q32_32_trig.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <optional>

using namespace engine::object_economy_detail;

namespace {

[[nodiscard]] bool isSupplyTruckOrder(const engine::ObjectOrderIntent& order,
                                      size_t instance) noexcept {
    return order.source == engine::ObjectOrderSource::System &&
           order.systemPurpose == engine::ObjectOrderSystemPurpose::SupplyTruck &&
           order.systemPurposeInstance == instance;
}

void replaceSupplyTruckMove(engine::ObjectOrderQueueComponent& queue,
                            engine::ObjectSupplyTruckRuntime& runtime,
                            size_t instance, engine::PlayerId owner,
                            engine::ObjectId dock,
                            const engine::LogicFixedVec3& position,
                            uint64_t confirmedTick) {
    if (!queue.orders.empty() && isSupplyTruckOrder(queue.orders.front(), instance)) {
        const engine::ObjectOrderIntent& current = queue.orders.front();
        // NOTE: this test used to repeat `hasTargetPosition` twice.  Removing
        // the duplicate is behaviour-neutral, but it means whatever second
        // condition the author intended is still missing — if a future change
        // needs it (a kind/purpose check was the likely candidate), add it here.
        // The target identifies the obstacle ignored by the typed docking
        // move.  GameSessionAIOrders deliberately keeps targetPosition as
        // the stage goal instead of replacing it with the building origin.
        if (current.targetObject == dock && current.hasTargetPosition &&
            current.targetX == position.x &&
            current.targetY == position.y &&
            current.targetZ == position.z) return;
        queue.orders.erase(queue.orders.begin());
    } else if (!queue.orders.empty()) {
        return;
    }
    queue.orders.insert(queue.orders.begin(), engine::ObjectOrderIntent{
        .kind = engine::ObjectOrderKind::Move,
        .source = engine::ObjectOrderSource::System,
        .contextPlayer = owner,
        .issuedTick = confirmedTick,
        .sourceSequence = runtime.nextCommandSequence,
        .targetObject = dock,
        .targetX = position.x,
        .targetY = position.y,
        .targetZ = position.z,
        .hasTargetPosition = true,
        .systemPurpose = engine::ObjectOrderSystemPurpose::SupplyTruck,
        .systemPurposeInstance = static_cast<uint32_t>(instance),
    });
    ++queue.revision;
    ++runtime.nextCommandSequence;
    if (runtime.nextCommandSequence == 0) ++runtime.nextCommandSequence;
}

void cancelSupplyTruckMove(engine::ObjectOrderQueueComponent& queue,
                           size_t instance) {
    if (queue.orders.empty() ||
        !isSupplyTruckOrder(queue.orders.front(), instance)) {
        return;
    }
    queue.orders.erase(queue.orders.begin());
    ++queue.revision;
}

[[nodiscard]] bool isClearingMines(
    const ecs::registry& registry, ecs::entity entity,
    const engine::GameContentSnapshot* content) noexcept {
    if (!content) return false;
    const engine::ObjectStatusComponent* status =
        ecs::try_get<engine::ObjectStatusComponent>(registry, entity);
    if (!status || !status->hasAny(game::objectStatusBit(
                       game::ObjectStatusFlag::IsAttacking))) {
        return false;
    }
    const engine::ObjectWeaponComponent* weapons =
        ecs::try_get<engine::ObjectWeaponComponent>(registry, entity);
    if (!weapons || !weapons->activeWeaponSetIndex ||
        !weapons->currentSlot ||
        *weapons->activeWeaponSetIndex >= weapons->sets.size()) {
        return false;
    }
    const size_t slot = static_cast<size_t>(*weapons->currentSlot);
    if (slot >= game::kWeaponSlotCount) return false;
    const game::WeaponTemplate* weapon = content->findWeapon(
        weapons->sets[*weapons->activeWeaponSetIndex].slots[slot].content);
    return weapon && (weapon->antiMask & game::weaponAntiBit(
        game::WeaponAntiTarget::Mine)) != 0;
}


} // namespace

namespace engine {

void ObjectEconomySystem::updateSupplyTrucks(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    PlayerRegistry& players, const GameContentSnapshot* content,
    const ObjectSimulationRules& rules,
    uint64_t confirmedTick,
    container::Vector<ObjectSupplyEvent>& outEvents,
    container::Vector<ObjectDamageRequest>& outDamage) const {
    struct Dock final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
        LogicFixedVec3 position;
        math::q32_32 yawRadians{};
        PlayerId owner = INVALID_PLAYER_ID;
        bool center = false;
        bool warehouse = false;
        bool open = true;
        bool crippled = false;
        uint32_t moduleIndex = 0;
    };

    container::Vector<Dock> centers;
    container::Vector<Dock> warehouses;
    const auto dockView =
        ecs::view<const ObjectIdentityComponent, const ObjectSupplyAnchorComponent,
                  const TransformComponent>(registry);
    for (const ecs::entity entity : dockView) {
        const ObjectIdentityComponent& identity =
            dockView.template get<const ObjectIdentityComponent>(entity);
        if (!isAliveObject(registry, lifecycle, entity, identity.id)) {
            continue;
        }
        const ObjectSupplyAnchorComponent& anchor =
            dockView.template get<const ObjectSupplyAnchorComponent>(entity);
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, entity);
        const ObjectDockCrippleComponent* cripple =
            ecs::try_get<ObjectDockCrippleComponent>(registry, entity);
        const TransformComponent& transform =
            dockView.template get<const TransformComponent>(entity);
        Dock dock{.id = identity.id,
                  .entity = entity,
                  .position = readAuthoritativeObjectPosition(
                      registry, entity, transform),
                  .yawRadians = readAuthoritativeObjectYaw(
                      registry, entity, transform),
                  .owner = owner ? owner->player : INVALID_PLAYER_ID,
                  .center = anchor.supplyCenterReady,
                  .warehouse = anchor.supplyWarehouseReady,
                  .open = !hasBlockingStatus(
                      registry, entity, confirmedTick),
                  .crippled = cripple && cripple->crippled()};
        ObjectEconomyComponent* economy =
            ecs::try_get<ObjectEconomyComponent>(registry, entity);
        if (dock.center && economy && economy->plan &&
            !economy->supplyCenterDocks.empty()) {
            for (size_t index = 0;
                 index < std::min(economy->supplyCenterDocks.size(),
                                  economy->plan->supplyCenterDocks.size());
                 ++index) {
                Dock occurrence = dock;
                occurrence.moduleIndex = static_cast<uint32_t>(index);
                occurrence.open = occurrence.open &&
                    economy->supplyCenterDocks[index].dock.open;
                centers.push_back(occurrence);
            }
        }
        if (dock.warehouse && economy && economy->plan &&
            !economy->supplyWarehouseDocks.empty()) {
            for (size_t index = 0;
                 index < std::min(economy->supplyWarehouseDocks.size(),
                                  economy->plan->supplyWarehouseDocks.size());
                 ++index) {
                Dock occurrence = dock;
                occurrence.moduleIndex = static_cast<uint32_t>(index);
                occurrence.open = occurrence.open &&
                    economy->supplyWarehouseDocks[index].dock.open;
                warehouses.push_back(occurrence);
            }
        }
    }
    const auto byId = [](const Dock& left, const Dock& right) {
        return left.id != right.id ? left.id < right.id
                                   : left.moduleIndex < right.moduleIndex;
    };
    std::sort(centers.begin(), centers.end(), byId);
    std::sort(warehouses.begin(), warehouses.end(), byId);

    // SupplyTruckAIUpdate::isAvailableForSupplying is further narrowed by
    // ChinookAIUpdate: a Chinook with passengers wanting to enter or exit is
    // unavailable even before the containment edge itself exists.  Freeze
    // the pending host IDs once so each airborne supply occurrence can query
    // the same deterministic fact without scanning the registry repeatedly.
    container::Vector<ObjectId> pendingContainmentEntryHosts;
    const auto pendingEntries =
        ecs::view<const ObjectScriptContainmentEnterComponent>(registry);
    for (const ecs::entity entity : pendingEntries) {
        const ObjectScriptContainmentEnterComponent& pending =
            pendingEntries.template get<
                const ObjectScriptContainmentEnterComponent>(entity);
        if (pending.target)
            pendingContainmentEntryHosts.push_back(pending.target);
    }
    std::sort(pendingContainmentEntryHosts.begin(),
              pendingContainmentEntryHosts.end());
    pendingContainmentEntryHosts.erase(
        std::unique(pendingContainmentEntryHosts.begin(),
                    pendingContainmentEntryHosts.end()),
        pendingContainmentEntryHosts.end());

    const auto dockRuntime = [&](const Dock& dock)
            -> ObjectSupplyDockRuntime* {
        ObjectEconomyComponent* economy =
            ecs::try_get<ObjectEconomyComponent>(registry, dock.entity);
        if (!economy) return nullptr;
        if (dock.center) {
            return dock.moduleIndex < economy->supplyCenterDocks.size()
                ? &economy->supplyCenterDocks[dock.moduleIndex].dock : nullptr;
        }
        return dock.moduleIndex < economy->supplyWarehouseDocks.size()
            ? &economy->supplyWarehouseDocks[dock.moduleIndex].dock : nullptr;
    };
    const auto dockRule = [&](const Dock& dock)
            -> const game::ObjectSupplyDockRule* {
        const ObjectEconomyComponent* economy =
            ecs::try_get<ObjectEconomyComponent>(registry, dock.entity);
        if (!economy || !economy->plan) return nullptr;
        if (dock.center) {
            return dock.moduleIndex < economy->plan->supplyCenterDocks.size()
                ? &economy->plan->supplyCenterDocks[dock.moduleIndex].dock
                : nullptr;
        }
        return dock.moduleIndex < economy->plan->supplyWarehouseDocks.size()
            ? &economy->plan->supplyWarehouseDocks[dock.moduleIndex].dock
            : nullptr;
    };
    const auto validDocker = [&](ObjectId object) {
        return object && lifecycle.entityFromId(object) &&
               !lifecycle.isPendingDestroy(object);
    };
    const auto scrubDockRuntime = [&](ObjectSupplyDockRuntime& runtime) {
        bool changed = false;
        for (size_t index = 0; index < runtime.approachOwners.size(); ++index) {
            if (validDocker(runtime.approachOwners[index])) continue;
            changed = changed || static_cast<bool>(runtime.approachOwners[index]) ||
                (index < runtime.approachReached.size() &&
                 runtime.approachReached[index]);
            runtime.approachOwners[index] = INVALID_OBJECT_ID;
            if (index < runtime.approachReached.size())
                runtime.approachReached[index] = false;
        }
        if (!validDocker(runtime.activeDocker)) {
            changed = changed || static_cast<bool>(runtime.activeDocker) ||
                runtime.activeDockerInside;
            runtime.activeDocker = INVALID_OBJECT_ID;
            runtime.activeDockerInside = false;
        }
        if (changed) ++runtime.revision;
    };
    // Reservation ownership outlives a Dock's current eligibility. Scrub the
    // complete runtime storage, including crippled/disabled/unfinished docks
    // omitted from this frame's candidate lists.
    const auto allEconomy = ecs::view<ObjectEconomyComponent>(registry);
    for (const ecs::entity entity : allEconomy) {
        ObjectEconomyComponent& economy =
            allEconomy.template get<ObjectEconomyComponent>(entity);
        for (ObjectSupplyCenterDockRuntime& center :
             economy.supplyCenterDocks) {
            scrubDockRuntime(center.dock);
        }
        for (ObjectSupplyWarehouseDockRuntime& warehouse :
             economy.supplyWarehouseDocks) {
            scrubDockRuntime(warehouse.dock);
        }
    }

    const auto reserveApproach = [&](const Dock& dock, ObjectId truck)
            -> int32_t {
        ObjectSupplyDockRuntime* runtime = dockRuntime(dock);
        const game::ObjectSupplyDockRule* rule = dockRule(dock);
        if (!runtime || !rule || !runtime->open || !dock.open || dock.crippled)
            return -1;
        for (size_t index = 0; index < runtime->approachOwners.size(); ++index)
            if (runtime->approachOwners[index] == truck)
                return static_cast<int32_t>(index);
        size_t slot = runtime->approachOwners.size();
        for (size_t index = 0; index < runtime->approachOwners.size(); ++index) {
            if (!runtime->approachOwners[index]) {
                slot = index;
                break;
            }
        }
        if (slot == runtime->approachOwners.size()) {
            if (rule->numberApproachPositions >= 0) return -1;
            runtime->approachOwners.push_back(INVALID_OBJECT_ID);
            runtime->approachReached.push_back(false);
        }
        runtime->approachOwners[slot] = truck;
        runtime->approachReached[slot] = false;
        ++runtime->revision;
        return static_cast<int32_t>(slot);
    };
    const auto canReserveApproach = [&](const Dock& dock,
                                        ObjectId truck) noexcept {
        const ObjectSupplyDockRuntime* runtime = dockRuntime(dock);
        const game::ObjectSupplyDockRule* rule = dockRule(dock);
        if (!runtime || !rule || !runtime->open || !dock.open || dock.crippled)
            return false;
        if (runtime->activeDocker == truck) return true;
        for (const ObjectId owner : runtime->approachOwners) {
            if (!owner || owner == truck) return true;
        }
        return rule->numberApproachPositions < 0;
    };

    const auto releaseDockRuntime = [&](ObjectSupplyDockRuntime& runtime,
                                        ObjectId truck) {
        bool changed = false;
        for (size_t index = 0; index < runtime.approachOwners.size(); ++index) {
            if (runtime.approachOwners[index] != truck) continue;
            runtime.approachOwners[index] = INVALID_OBJECT_ID;
            if (index < runtime.approachReached.size())
                runtime.approachReached[index] = false;
            changed = true;
        }
        if (runtime.activeDocker == truck) {
            runtime.activeDocker = INVALID_OBJECT_ID;
            runtime.activeDockerInside = false;
            changed = true;
        }
        if (changed) ++runtime.revision;
    };
    const auto releaseDock = [&](const Dock& dock, ObjectId truck) {
        ObjectSupplyDockRuntime* runtime = dockRuntime(dock);
        if (runtime) releaseDockRuntime(*runtime, truck);
    };
    const auto releaseAllDocks = [&](ObjectId truck) {
        const auto economyView = ecs::view<ObjectEconomyComponent>(registry);
        for (const ecs::entity entity : economyView) {
            ObjectEconomyComponent& economy =
                economyView.template get<ObjectEconomyComponent>(entity);
            for (ObjectSupplyCenterDockRuntime& center :
                 economy.supplyCenterDocks) {
                releaseDockRuntime(center.dock, truck);
            }
            for (ObjectSupplyWarehouseDockRuntime& warehouse :
                 economy.supplyWarehouseDocks) {
                releaseDockRuntime(warehouse.dock, truck);
            }
        }
    };

    enum class DockPoint : uint8_t { Approach, Enter, Action, Exit };
    const auto stagePosition = [&](const Dock& dock,
                                   const ObjectGeometryComponent* truckGeometry,
                                   DockPoint point, int32_t approachIndex) {
        LogicFixedVec3 result = dock.position;
        const ObjectSupplyDockRuntime* runtime = dockRuntime(dock);
        const LogicFixedVec3* authoredLocal = nullptr;
        bool authoredLocalValid = false;
        switch (point) {
        case DockPoint::Approach:
            if (runtime && approachIndex >= 0 &&
                static_cast<size_t>(approachIndex) <
                    runtime->approachPositionsLocal.size() &&
                static_cast<size_t>(approachIndex) <
                    runtime->approachPositionValid.size()) {
                authoredLocal = &runtime->approachPositionsLocal[
                    static_cast<size_t>(approachIndex)];
                authoredLocalValid = runtime->approachPositionValid[
                    static_cast<size_t>(approachIndex)];
            }
            break;
        case DockPoint::Enter:
            if (runtime) {
                authoredLocal = &runtime->enterPositionLocal;
                authoredLocalValid = runtime->enterPositionValid;
            }
            break;
        case DockPoint::Action:
            if (runtime) {
                authoredLocal = &runtime->actionPositionLocal;
                authoredLocalValid = runtime->actionPositionValid;
            }
            break;
        case DockPoint::Exit:
            if (runtime) {
                authoredLocal = &runtime->exitPositionLocal;
                authoredLocalValid = runtime->exitPositionValid;
            }
            break;
        }
        const math::q32_32_sincos facing = math::fixed_sincos(
            dock.yawRadians);
        if (authoredLocal && authoredLocalValid) {
            result.x += facing.cosine * authoredLocal->x -
                facing.sine * authoredLocal->y;
            result.y += facing.sine * authoredLocal->x +
                facing.cosine * authoredLocal->y;
            result.z += authoredLocal->z;
            return result;
        }
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, dock.entity);
        const math::q32_32 one{int32_t{1}};
        const math::q32_32 truckRadius = truckGeometry
            ? math::q32_32::max(one,
                                truckGeometry->boundingCircleRadiusFixed)
            : math::q32_32{int32_t{10}};
        const math::q32_32 dockRadius = geometry
            ? math::q32_32::max(one, geometry->boundingCircleRadiusFixed)
            : math::q32_32{int32_t{10}};
        const math::q32_32 clearance = truckRadius + dockRadius;
        const math::q32_32 forwardX = facing.cosine;
        const math::q32_32 forwardY = facing.sine;
        const math::q32_32 sideX = -forwardY;
        const math::q32_32 sideY = forwardX;
        const int32_t ordinal = std::max(0, approachIndex);
        const math::q32_32 lateral = ordinal == 0 ? math::q32_32{}
            : math::q32_32{static_cast<int32_t>((ordinal + 1) / 2 * 8)} *
                  ((ordinal & 1) ? one : -one);
        const game::ObjectSupplyDockRule* rule = dockRule(dock);
        switch (point) {
        case DockPoint::Approach:
            result.x -= forwardX * (clearance + math::q32_32{int32_t{24}});
            result.y -= forwardY * (clearance + math::q32_32{int32_t{24}});
            result.x += sideX * lateral;
            result.y += sideY * lateral;
            break;
        case DockPoint::Enter:
            result.x -= forwardX * clearance;
            result.y -= forwardY * clearance;
            break;
        case DockPoint::Action:
            break;
        case DockPoint::Exit: {
            const math::q32_32 direction =
                rule && rule->allowsPassthrough ? one : -one;
            result.x += forwardX * direction *
                (clearance + math::q32_32{int32_t{16}});
            result.y += forwardY * direction *
                (clearance + math::q32_32{int32_t{16}});
            break;
        }
        }
        return result;
    };

    struct Truck final {
        ObjectId id = INVALID_OBJECT_ID;
        ecs::entity entity = ecs::null;
    };
    container::Vector<Truck> trucks;
    const auto truckView =
        ecs::view<const ObjectIdentityComponent, ObjectEconomyComponent>(
            registry);
    for (const ecs::entity entity : truckView) {
        const ObjectIdentityComponent& identity =
            truckView.template get<const ObjectIdentityComponent>(entity);
        const ObjectEconomyComponent& economy =
            truckView.template get<const ObjectEconomyComponent>(entity);
        if (economy.supplyTrucks.empty() ||
            !isAliveObject(registry, lifecycle, entity, identity.id)) {
            continue;
        }
        trucks.push_back({.id = identity.id, .entity = entity});
    }
    std::sort(trucks.begin(), trucks.end(),
              [](const Truck& left, const Truck& right) {
                  return left.id < right.id;
              });

    for (const Truck& truck : trucks) {
        ObjectEconomyComponent& economy =
            ecs::get<ObjectEconomyComponent>(registry, truck.entity);
        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, truck.entity);
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, truck.entity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(registry, truck.entity);
        ObjectOrderQueueComponent* queue =
            ecs::try_get<ObjectOrderQueueComponent>(registry, truck.entity);
        if (!owner || !owner->player || !transform || !economy.plan) {
            continue;
        }
        if (!queue) {
            queue = &ecs::emplace<ObjectOrderQueueComponent>(
                registry, truck.entity);
        }
        const LogicFixedVec3 truckPosition = readAuthoritativeObjectPosition(
            registry, truck.entity, *transform);
        const auto distanceSquaredFromTruck = [&](const LogicFixedVec3& point) {
            const math::q32_32 dx = truckPosition.x - point.x;
            const math::q32_32 dy = truckPosition.y - point.y;
            return dx * dx + dy * dy;
        };
        const size_t count = std::min(economy.plan->supplyTrucks.size(),
                                      economy.supplyTrucks.size());
        const bool supplyUnavailable = hasBlockingStatus(
            registry, truck.entity, confirmedTick);
        for (size_t index = 0; index < count; ++index) {
            const game::ObjectSupplyTruckRule& rule =
                economy.plan->supplyTrucks[index];
            ObjectSupplyTruckRuntime& runtime = economy.supplyTrucks[index];
            const uint32_t maxBoxes = rule.maxBoxes;

            if (supplyUnavailable) {
                // A Held/disabled docker no longer owns a usable AI docking
                // path. RefCode cancels the DockUpdate reservation when the
                // AI state is displaced; otherwise one contained or EMP'd
                // gatherer can permanently occupy an approach/active slot.
                releaseAllDocks(truck.id);
                runtime.targetDock = INVALID_OBJECT_ID;
                runtime.targetDockModule = 0;
                runtime.targetIsCenter = false;
                runtime.approachPosition = -1;
                runtime.state = ObjectSupplyTruckRuntimeState::SeekingWarehouse;
                cancelSupplyTruckMove(*queue, index);
                continue;
            }

            if (runtime.scriptIdleSuppressed) {
                releaseAllDocks(truck.id);
                runtime.targetDock = INVALID_OBJECT_ID;
                runtime.targetDockModule = 0;
                runtime.targetIsCenter = false;
                runtime.approachPosition = -1;
                cancelSupplyTruckMove(*queue, index);
                continue;
            }

            if (rule.airborneTransport) {
                const ObjectContainmentComponent* passengers =
                    ecs::try_get<ObjectContainmentComponent>(registry,
                                                             truck.entity);
                const bool carryingPassengers =
                    passengers && !passengers->objects.empty();
                const bool passengerEntering = std::binary_search(
                    pendingContainmentEntryHosts.begin(),
                    pendingContainmentEntryHosts.end(), truck.id);
                const ObjectContainmentRuntimeComponent* containmentRuntime =
                    ecs::try_get<ObjectContainmentRuntimeComponent>(
                        registry, truck.entity);
                const bool missingContainment = !containmentRuntime ||
                    !containmentRuntime->plan;
                const bool passengerExitPending =
                    ecs::try_get<ObjectPendingPlayerEvacuationComponent>(
                        registry, truck.entity) != nullptr ||
                    (containmentRuntime &&
                     containmentRuntime->ownerChangeEvacuationPending);
                const ObjectContainmentKindMask specialContainerKinds =
                    objectContainmentKindBit(ObjectContainmentKind::Overlord) |
                    objectContainmentKindBit(ObjectContainmentKind::Helix);
                const bool specialOverlordStyleContainer =
                    containmentRuntime && containmentRuntime->plan &&
                    (containmentRuntime->plan->kindMask &
                     specialContainerKinds) != 0;
                const bool insertionActive =
                    ecs::try_get<ObjectCombatDropOrderRuntimeComponent>(
                        registry, truck.entity) != nullptr ||
                    ecs::try_get<ObjectRappellingComponent>(
                        registry, truck.entity) != nullptr;
                if (missingContainment || carryingPassengers ||
                    passengerEntering ||
                    passengerExitPending || specialOverlordStyleContainer ||
                    insertionActive) {
                    releaseAllDocks(truck.id);
                    runtime.targetDock = INVALID_OBJECT_ID;
                    runtime.targetDockModule = 0;
                    runtime.targetIsCenter = false;
                    runtime.approachPosition = -1;
                    runtime.state =
                        ObjectSupplyTruckRuntimeState::SeekingWarehouse;
                    cancelSupplyTruckMove(*queue, index);
                    continue;
                }
            }

            if (rule.workerMode) {
                if (runtime.boxes != 0 &&
                    isClearingMines(registry, truck.entity, content)) {
                    runtime.boxes = 0;
                    markObjectDirty(
                        registry, truck.entity,
                        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
                        objectDirtyBit(ObjectDirtyDomain::RenderExtraction));
                }
                const ObjectBuilderComponent* builder =
                    ecs::try_get<ObjectBuilderComponent>(registry, truck.entity);
                const bool builderBusy = builder && std::any_of(
                    builder->runtimes.begin(), builder->runtimes.end(),
                    [](const ObjectBuilderRuntime& state) {
                        if (state.current.kind !=
                            ObjectBuilderTaskKind::None) {
                            return true;
                        }
                        return std::any_of(
                            state.taskSlots.begin(), state.taskSlots.end(),
                            [](const ObjectBuilderTask& task) {
                                return task.kind !=
                                        ObjectBuilderTaskKind::None &&
                                    static_cast<bool>(task.target);
                            });
                    });
                if (builderBusy) {
                    // WorkerAIUpdate leaves AS_SUPPLY_TRUCK when a builder
                    // task is assigned and resets that sub-machine. Finishing
                    // the task alone does not force supply mode back on.
                    runtime.workerSupplyActive = false;
                    releaseAllDocks(truck.id);
                    runtime.targetDock = INVALID_OBJECT_ID;
                    runtime.targetDockModule = 0;
                    runtime.targetIsCenter = false;
                    runtime.approachPosition = -1;
                    runtime.state =
                        ObjectSupplyTruckRuntimeState::SeekingWarehouse;
                    cancelSupplyTruckMove(*queue, index);
                    continue;
                }
            }

            // The outer SupplyTruck state chooses by empty/non-empty cargo.
            // Once docked, the warehouse Dock action itself repeats until the
            // truck is full or the source is empty.
            const bool needsWarehouse = runtime.boxes == 0;
            const auto findDock = [&](ObjectId id, uint32_t module,
                                      bool center) -> const Dock* {
                const container::Vector<Dock>& list = center ? centers : warehouses;
                const auto found = std::lower_bound(
                    list.begin(), list.end(), Dock{.id = id, .moduleIndex = module}, byId);
                return found != list.end() && found->id == id &&
                       found->moduleIndex == module ? &*found : nullptr;
            };
            const auto findAnyDock = [&](ObjectId id) -> const Dock* {
                for (const Dock& dock : centers) if (dock.id == id) return &dock;
                for (const Dock& dock : warehouses) if (dock.id == id) return &dock;
                return nullptr;
            };
            const auto canTransferSuppliesAt = [&](const Dock& dock) {
                // RefCode ActionManager::canTransferSuppliesAt permits neutral
                // and allied warehouses, but never an enemy warehouse. Supply
                // centers remain private to their owning player.
                return dock.center
                    ? dock.owner == owner->player
                    : players.relationship(owner->player, dock.owner) !=
                          PlayerRelationship::Enemies;
            };
            const Dock* currentDock = runtime.targetDock
                ? findDock(runtime.targetDock, runtime.targetDockModule,
                           runtime.targetIsCenter)
                : nullptr;
            if (currentDock && !canTransferSuppliesAt(*currentDock)) {
                releaseDock(*currentDock, truck.id);
                runtime.targetDock = INVALID_OBJECT_ID;
                runtime.targetDockModule = 0;
                runtime.targetIsCenter = false;
                runtime.approachPosition = -1;
                runtime.state = ObjectSupplyTruckRuntimeState::SeekingWarehouse;
                currentDock = nullptr;
            }

            const ObjectOrderIntent* externalOrder =
                !queue->orders.empty() &&
                !isSupplyTruckOrder(queue->orders.front(), index)
                    ? &queue->orders.front() : nullptr;
            const Dock* requestedDock = externalOrder &&
                    externalOrder->kind == ObjectOrderKind::Move
                ? findAnyDock(externalOrder->targetObject) : nullptr;
            const Dock* commandedDock =
                requestedDock && canTransferSuppliesAt(*requestedDock)
                    ? requestedDock : nullptr;
            if (runtime.observedExternalOrderRevision != queue->externalRevision) {
                runtime.observedExternalOrderRevision = queue->externalRevision;
                if (commandedDock) {
                    runtime.externalIdleSuppressed = false;
                    if (externalOrder->source == ObjectOrderSource::Player) {
                        // One remembered dock is intentional legacy semantics:
                        // a player can override either half of the route, not
                        // define a separate warehouse/center pair through the
                        // old UI.
                        runtime.preferredDock = commandedDock->id;
                    }
                } else if (queue->replacementExternalRevision ==
                               queue->externalRevision) {
                    const bool directIdle =
                        queue->replacementExternalKind ==
                        ObjectOrderKind::Stop;
                    const bool playerStoppedRegularTruck = directIdle &&
                        !rule.workerMode &&
                        queue->replacementExternalSource ==
                            ObjectOrderSource::Player;
                    // A non-Dock replacement drives the supply sub-machine
                    // through Busy and leaves it in Idle.  Direct Idle is the
                    // exception: SupplyTruckAIUpdate latches Busy only for a
                    // player Stop, while WorkerAIUpdate deliberately does not
                    // latch for either Player or Script Stop.
                    if (!directIdle || playerStoppedRegularTruck)
                        runtime.externalIdleSuppressed = true;
                }
                if (!commandedDock && currentDock) releaseDock(*currentDock, truck.id);
                if (!commandedDock) {
                    runtime.state = ObjectSupplyTruckRuntimeState::SeekingWarehouse;
                    runtime.targetDock = INVALID_OBJECT_ID;
                    runtime.targetDockModule = 0;
                    runtime.targetIsCenter = false;
                    runtime.approachPosition = -1;
                    currentDock = nullptr;
                }
            }
            if (rule.workerMode && commandedDock)
                runtime.workerSupplyActive = true;
            if (rule.workerMode && !runtime.workerSupplyActive) {
                releaseAllDocks(truck.id);
                runtime.targetDock = INVALID_OBJECT_ID;
                runtime.targetDockModule = 0;
                runtime.targetIsCenter = false;
                runtime.approachPosition = -1;
                runtime.state = ObjectSupplyTruckRuntimeState::SeekingWarehouse;
                cancelSupplyTruckMove(*queue, index);
                continue;
            }
            if (runtime.externalIdleSuppressed) {
                releaseAllDocks(truck.id);
                runtime.targetDock = INVALID_OBJECT_ID;
                runtime.targetDockModule = 0;
                runtime.targetIsCenter = false;
                runtime.approachPosition = -1;
                cancelSupplyTruckMove(*queue, index);
                continue;
            }
            if (externalOrder &&
                (!commandedDock || commandedDock->warehouse != needsWarehouse ||
                 commandedDock->center == needsWarehouse)) {
                // Any foreign AI command makes the inherited AIUpdate state
                // non-idle, so RefCode's SupplyTruckStateMachine leaves its
                // docking/wanting branch for Busy. StrategicAI, MoveAside and
                // special-ability commands deliberately do not advance the
                // player/script external revision, but they must still release
                // this truck's current dock reservation while they own the
                // foreground command. This is temporary: unlike a player Stop
                // it does not latch externalIdleSuppressed, and autopilot will
                // seek a new dock once the foreign order completes.
                releaseAllDocks(truck.id);
                runtime.targetDock = INVALID_OBJECT_ID;
                runtime.targetDockModule = 0;
                runtime.targetIsCenter = false;
                runtime.approachPosition = -1;
                runtime.state = ObjectSupplyTruckRuntimeState::SeekingWarehouse;
                continue;
            }

            const PlayerState* owningPlayer = players.get(owner->player);
            math::q32_32 scanRange = rule.supplyWarehouseScanDistance;
            if (owningPlayer && owningPlayer->controller == PlayerControllerKind::Ai)
                scanRange *= math::q32_32{int32_t{2}};
            const math::q32_32 scanRangeSq = scanRange * scanRange;

            struct RegroupChoice final {
                ObjectId object = INVALID_OBJECT_ID;
                LogicFixedVec3 destination{};
                math::q32_32 surfaceDistanceSquared{};
                uint8_t priority = std::numeric_limits<uint8_t>::max();
                bool reached = false;
            };
            const auto chooseRegroup = [&]() -> RegroupChoice {
                RegroupChoice best;
                const auto candidates = ecs::view<
                    const ObjectIdentityComponent, const OwnerComponent,
                    const TransformComponent,
                    const ObjectKindOfComponent>(registry);
                const math::q32_32 truckRadius = geometry
                    ? math::q32_32::max(
                          math::q32_32{},
                          geometry->boundingCircleRadiusFixed)
                    : math::q32_32{};
                for (const ecs::entity candidate : candidates) {
                    const ObjectIdentityComponent& identity =
                        candidates.template get<
                            const ObjectIdentityComponent>(candidate);
                    const OwnerComponent& candidateOwner =
                        candidates.template get<const OwnerComponent>(
                            candidate);
                    const ObjectKindOfComponent& kinds =
                        candidates.template get<
                            const ObjectKindOfComponent>(candidate);
                    if (!identity.id || identity.id == truck.id ||
                        candidateOwner.player != owner->player ||
                        !isAliveObject(
                            registry, lifecycle, candidate, identity.id) ||
                        !game::objectHasKind(
                            kinds.mask, game::ObjectKindOf::Structure)) {
                        continue;
                    }
                    const uint8_t priority = game::objectHasKind(
                            kinds.mask, game::ObjectKindOf::CashGenerator)
                        ? 0u
                        : game::objectHasKind(
                              kinds.mask,
                              game::ObjectKindOf::CommandCenter)
                            ? 1u : 2u;
                    if (priority > best.priority) continue;
                    const TransformComponent& candidateTransform =
                        candidates.template get<const TransformComponent>(
                            candidate);
                    const LogicFixedVec3 candidatePosition =
                        readAuthoritativeObjectPosition(
                            registry, candidate, candidateTransform);
                    const ObjectGeometryComponent* candidateGeometry =
                        ecs::try_get<ObjectGeometryComponent>(
                            registry, candidate);
                    const math::q32_32 candidateRadius = candidateGeometry
                        ? math::q32_32::max(
                              math::q32_32{},
                              candidateGeometry->boundingCircleRadiusFixed)
                        : math::q32_32{};
                    const math::q32_32 dx =
                        truckPosition.x - candidatePosition.x;
                    const math::q32_32 dy =
                        truckPosition.y - candidatePosition.y;
                    const math::q32_32 centerDistance =
                        math::q32_32::sqrt(dx * dx + dy * dy);
                    const math::q32_32 surfaceDistance =
                        math::q32_32::max(
                            math::q32_32{},
                            centerDistance - truckRadius - candidateRadius);
                    const math::q32_32 surfaceDistanceSquared =
                        surfaceDistance * surfaceDistance;
                    if (priority == best.priority && best.object &&
                        (surfaceDistanceSquared >
                             best.surfaceDistanceSquared ||
                         (surfaceDistanceSquared ==
                              best.surfaceDistanceSquared &&
                          identity.id > best.object))) {
                        continue;
                    }

                    const math::q32_32 standoff = truckRadius +
                        candidateRadius + math::q32_32{int32_t{10}};
                    LogicFixedVec3 destination{
                        candidatePosition.x + standoff,
                        candidatePosition.y,
                        candidatePosition.z};
                    if (centerDistance > math::q32_32{}) {
                        destination.x = candidatePosition.x +
                            dx * standoff / centerDistance;
                        destination.y = candidatePosition.y +
                            dy * standoff / centerDistance;
                    }
                    best = {
                        .object = identity.id,
                        .destination = destination,
                        .surfaceDistanceSquared = surfaceDistanceSquared,
                        .priority = priority,
                        .reached = surfaceDistanceSquared <
                            math::q32_32{int32_t{225}},
                    };
                }
                return best;
            };

            if (runtime.state ==
                    ObjectSupplyTruckRuntimeState::Regrouping) {
                // RefCode's RegroupingState chooses one building and one
                // nearby position in onEnter(), then merely waits for that
                // AI move to become idle.  Do not re-rank buildings or
                // rebuild the destination while the confirmed system order
                // is still active: doing so makes the standoff point follow
                // the moving truck and can switch the chosen base structure
                // halfway through the route.
                if (!queue->orders.empty() &&
                    isSupplyTruckOrder(queue->orders.front(), index)) {
                    continue;
                }
                if (runtime.regroupMoveIssued) {
                    runtime.regroupMoveIssued = false;
                    runtime.state =
                        ObjectSupplyTruckRuntimeState::SeekingWarehouse;
                    continue;
                }
                const RegroupChoice regroup = chooseRegroup();
                if (!regroup.object) {
                    // RegroupingState::onEnter fails into Busy/Idle when the
                    // player owns no cash generator, command center, or other
                    // structure. It stays there until an explicit resume.
                    runtime.externalIdleSuppressed = true;
                    runtime.regroupMoveIssued = false;
                    cancelSupplyTruckMove(*queue, index);
                    continue;
                }
                if (regroup.reached) {
                    cancelSupplyTruckMove(*queue, index);
                    runtime.regroupMoveIssued = false;
                    runtime.state =
                        ObjectSupplyTruckRuntimeState::SeekingWarehouse;
                    continue;
                }
                replaceSupplyTruckMove(
                    *queue, runtime, index, owner->player, regroup.object,
                    regroup.destination, confirmedTick);
                runtime.regroupMoveIssued = !queue->orders.empty() &&
                    isSupplyTruckOrder(queue->orders.front(), index);
                continue;
            }

            if (!currentDock && runtime.targetDock) {
                runtime.targetDock = INVALID_OBJECT_ID;
                runtime.targetDockModule = 0;
                runtime.targetIsCenter = false;
                runtime.approachPosition = -1;
                runtime.state = ObjectSupplyTruckRuntimeState::SeekingWarehouse;
            }
            if (runtime.state == ObjectSupplyTruckRuntimeState::SeekingWarehouse) {
                const bool seekWarehouse = needsWarehouse;
                const container::Vector<Dock>& searchList =
                    seekWarehouse ? warehouses : centers;

                const Dock* bestDock = nullptr;
                math::q32_32 bestDistance = math::q32_32::from_raw(
                    std::numeric_limits<int64_t>::max());
                for (const Dock& dock : searchList) {
                    if (!dock.open || dock.crippled) continue;
                    if (!canTransferSuppliesAt(dock)) continue;
                    // ResourceGatheringManager::computeRelativeCost excludes
                    // a DockUpdate whose approach queue is full. Selecting
                    // the nearest full dock and attempting reservation only
                    // after the search makes every truck retry that same dock
                    // forever instead of choosing the next clear candidate.
                    if (!canReserveApproach(dock, truck.id)) continue;
                    if (seekWarehouse) {
                        const ObjectEconomyComponent* dockEconomy =
                            ecs::try_get<ObjectEconomyComponent>(registry,
                                                                 dock.entity);
                        if (!dockEconomy || dock.moduleIndex >=
                                dockEconomy->supplyWarehouseDocks.size() ||
                            dockEconomy->supplyWarehouseDocks[
                                dock.moduleIndex].boxesStored == 0)
                            continue;
                    }
                    const bool explicitChoice = commandedDock == &dock ||
                        (runtime.preferredDock && dock.id == runtime.preferredDock);
                    const math::q32_32 distance =
                        distanceSquaredFromTruck(dock.position);
                    if (seekWarehouse && !explicitChoice &&
                        distance >= scanRangeSq)
                        continue;
                    if (runtime.preferredDock && dock.id == runtime.preferredDock) {
                        bestDock = &dock;
                        break;
                    }
                    if (!bestDock || distance < bestDistance ||
                        (distance == bestDistance && byId(dock, *bestDock))) {
                        bestDock = &dock;
                        bestDistance = distance;
                    }
                }
                if (!bestDock) {
                    runtime.state =
                        ObjectSupplyTruckRuntimeState::Regrouping;
                    runtime.regroupMoveIssued = false;
                    cancelSupplyTruckMove(*queue, index);
                    continue;
                }
                const int32_t reservation = reserveApproach(*bestDock, truck.id);
                if (reservation < 0) continue;
                runtime.targetDock = bestDock->id;
                runtime.targetDockModule = bestDock->moduleIndex;
                runtime.targetIsCenter = bestDock->center;
                runtime.approachPosition = reservation;
                runtime.state = ObjectSupplyTruckRuntimeState::MovingToApproach;
                currentDock = bestDock;
                outEvents.push_back({
                    .kind = ObjectSupplyEventKind::DockReserved,
                    .truck = truck.id,
                    .dock = bestDock->id,
                    .confirmedTick = confirmedTick,
                });
            }
            if (!currentDock) continue;
            const auto reached = [&](const LogicFixedVec3& point) {
                const ObjectLocomotionComponent* locomotion =
                    ecs::try_get<ObjectLocomotionComponent>(registry, truck.entity);
                const math::q32_32 threshold = locomotion
                    ? math::q32_32::max(
                          math::q32_32::from_raw(int64_t{1} << 31),
                          locomotion->closeEnough)
                    : math::q32_32{int32_t{1}};
                const math::q32_32 dx = truckPosition.x - point.x;
                const math::q32_32 dy = truckPosition.y - point.y;
                return dx * dx + dy * dy <= threshold * threshold;
            };
            const auto issueStage = [&](DockPoint point) {
                const LogicFixedVec3 destination = stagePosition(
                    *currentDock, geometry, point, runtime.approachPosition);
                if (!externalOrder) replaceSupplyTruckMove(
                    *queue, runtime, index, owner->player, currentDock->id,
                    destination, confirmedTick);
                return externalOrder
                    ? distanceSquaredFromTruck(currentDock->position) <=
                          dockingDistanceSquaredLimit(
                              geometry, ecs::try_get<ObjectGeometryComponent>(
                                  registry, currentDock->entity))
                    : reached(destination);
            };

            ObjectSupplyDockRuntime* commonDock = dockRuntime(*currentDock);
            if (!commonDock) continue;
            if (currentDock->crippled &&
                runtime.state != ObjectSupplyTruckRuntimeState::MovingToExit) {
                // SupplyWarehouseDockUpdate::setDockCrippled kills a
                // non-airborne docker already inside. A docker still in the
                // approach queue is released and retries later. Preserve that
                // distinction through the central Body barrier instead of
                // letting an occupied crippled warehouse complete its action.
                const bool inside = !currentDock->center &&
                    commonDock->activeDocker == truck.id &&
                    commonDock->activeDockerInside;
                const ObjectAirborneComponent* airborne =
                    ecs::try_get<ObjectAirborneComponent>(registry,
                                                          truck.entity);
                if (inside && !(airborne && airborne->isAirborne)) {
                    outDamage.push_back({
                        .target = truck.id,
                        .source = currentDock->id,
                        .sourceSequence = currentDock->moduleIndex,
                        .damageType = game::DamageType::UNRESISTABLE,
                        .deathType = game::DeathType::NORMAL,
                        .forceKill = true,
                        .confirmedTick = confirmedTick,
                    });
                }
                releaseDock(*currentDock, truck.id);
                runtime.targetDock = INVALID_OBJECT_ID;
                runtime.targetDockModule = 0;
                runtime.targetIsCenter = false;
                runtime.approachPosition = -1;
                runtime.state = ObjectSupplyTruckRuntimeState::SeekingWarehouse;
                continue;
            }
            if (!currentDock->open &&
                runtime.state != ObjectSupplyTruckRuntimeState::MovingToExit) {
                // DockUpdate::setDockOpen(FALSE) does not destroy an inside
                // docker. It only closes admission; an active inside truck is
                // moved to the exit leg, while approach/waiting reservations
                // are released to retry another dock.
                const bool inside = commonDock->activeDocker == truck.id &&
                    commonDock->activeDockerInside;
                if (inside) {
                    runtime.state = ObjectSupplyTruckRuntimeState::MovingToExit;
                } else {
                    releaseDock(*currentDock, truck.id);
                    runtime.targetDock = INVALID_OBJECT_ID;
                    runtime.targetDockModule = 0;
                    runtime.targetIsCenter = false;
                    runtime.approachPosition = -1;
                    runtime.state =
                        ObjectSupplyTruckRuntimeState::SeekingWarehouse;
                    continue;
                }
            }
            switch (runtime.state) {
            case ObjectSupplyTruckRuntimeState::SeekingWarehouse:
                runtime.state = ObjectSupplyTruckRuntimeState::MovingToApproach;
                break;
            case ObjectSupplyTruckRuntimeState::MovingToApproach:
                if (distanceSquaredFromTruck(currentDock->position) >
                        dockingDistanceSquaredLimit(
                            geometry,
                            ecs::try_get<ObjectGeometryComponent>(
                                registry, currentDock->entity)) &&
                    !issueStage(DockPoint::Approach)) break;
                if (runtime.approachPosition >= 0 &&
                    static_cast<size_t>(runtime.approachPosition) <
                        commonDock->approachReached.size()) {
                    commonDock->approachReached[
                        static_cast<size_t>(runtime.approachPosition)] = true;
                    ++commonDock->revision;
                }
                runtime.state = ObjectSupplyTruckRuntimeState::WaitingToEnter;
                [[fallthrough]];
            case ObjectSupplyTruckRuntimeState::WaitingToEnter: {
                if (!commonDock->activeDocker) {
                    for (size_t slot = 0; slot < commonDock->approachOwners.size(); ++slot) {
                        if (slot < commonDock->approachReached.size() &&
                            commonDock->approachReached[slot] &&
                            commonDock->approachOwners[slot]) {
                            commonDock->activeDocker = commonDock->approachOwners[slot];
                            ++commonDock->revision;
                            break;
                        }
                    }
                }
                if (commonDock->activeDocker != truck.id) break;
                runtime.state = ObjectSupplyTruckRuntimeState::MovingToEnter;
                break;
            }
            case ObjectSupplyTruckRuntimeState::MovingToEnter:
                if (!issueStage(DockPoint::Enter)) break;
                for (size_t slot = 0; slot < commonDock->approachOwners.size(); ++slot) {
                    if (commonDock->approachOwners[slot] != truck.id) continue;
                    commonDock->approachOwners[slot] = INVALID_OBJECT_ID;
                    commonDock->approachReached[slot] = false;
                }
                commonDock->activeDockerInside = true;
                ++commonDock->revision;
                runtime.state = ObjectSupplyTruckRuntimeState::MovingToAction;
                outEvents.push_back({
                    .kind = ObjectSupplyEventKind::DockEntered,
                    .truck = truck.id, .dock = currentDock->id,
                    .confirmedTick = confirmedTick,
                });
                break;
            case ObjectSupplyTruckRuntimeState::MovingToAction:
                if (!issueStage(DockPoint::Action)) break;
                runtime.state = ObjectSupplyTruckRuntimeState::Acting;
                runtime.nextActionTick = confirmedTick;
                [[fallthrough]];
            case ObjectSupplyTruckRuntimeState::Acting: {
                if (runtime.nextActionTick > confirmedTick) break;
                if (currentDock->warehouse) {
                    ObjectEconomyComponent* dockEconomy =
                        ecs::try_get<ObjectEconomyComponent>(registry,
                                                             currentDock->entity);
                    if (!dockEconomy || !dockEconomy->plan ||
                        currentDock->moduleIndex >=
                            dockEconomy->supplyWarehouseDocks.size() ||
                        currentDock->moduleIndex >=
                            dockEconomy->plan->supplyWarehouseDocks.size()) {
                        runtime.state = ObjectSupplyTruckRuntimeState::MovingToExit;
                        break;
                    }
                    ObjectSupplyWarehouseDockRuntime& warehouse =
                        dockEconomy->supplyWarehouseDocks[currentDock->moduleIndex];
                    if (warehouse.boxesStored == 0 || maxBoxes == 0 ||
                        runtime.boxes >= maxBoxes) {
                        runtime.state = ObjectSupplyTruckRuntimeState::MovingToExit;
                        break;
                    }
                    --warehouse.boxesStored;
                    ++warehouse.revision;
                    ++runtime.boxes;
                    markObjectDirty(
                        registry, currentDock->entity,
                        ObjectDirtyDomain::RenderExtraction);
                    markObjectDirty(
                        registry, truck.entity,
                        objectDirtyBit(ObjectDirtyDomain::ModelCondition) |
                            objectDirtyBit(
                                ObjectDirtyDomain::RenderExtraction));
                    outEvents.push_back({
                        .kind = ObjectSupplyEventKind::BoxTransferred,
                        .truck = truck.id, .dock = currentDock->id,
                        .boxes = runtime.boxes, .confirmedTick = confirmedTick,
                    });
                    runtime.nextActionTick = saturatingAdd(
                        confirmedTick, millisecondsToTicks(
                            rule.supplyWarehouseActionDelayMilliseconds,
                            rules.logicFramesPerSecond));
                    if (warehouse.boxesStored == 0) {
                        bool distantAlternative = true;
                        math::q32_32 nearest = math::q32_32::from_raw(
                            std::numeric_limits<int64_t>::max());
                        for (const Dock& alternative : warehouses) {
                            const ObjectEconomyComponent* alternativeEconomy =
                                ecs::try_get<ObjectEconomyComponent>(
                                    registry, alternative.entity);
                            if (!alternativeEconomy || alternative.moduleIndex >=
                                    alternativeEconomy->supplyWarehouseDocks.size() ||
                                alternativeEconomy->supplyWarehouseDocks[
                                    alternative.moduleIndex].boxesStored == 0)
                                continue;
                            nearest = std::min(nearest,
                                distanceSquaredFromTruck(alternative.position));
                        }
                        const math::q32_32 quarter = scanRange /
                            math::q32_32{int32_t{4}};
                        if (nearest <= quarter * quarter) distantAlternative = false;
                        if (distantAlternative && !rule.suppliesDepletedVoice.empty()) {
                            outEvents.push_back({
                                .kind = ObjectSupplyEventKind::SuppliesDepletedVoice,
                                .truck = truck.id, .dock = currentDock->id,
                                .resource = rule.suppliesDepletedVoice,
                                .boxes = runtime.boxes,
                                .confirmedTick = confirmedTick,
                            });
                        }
                        if (dockEconomy->plan->supplyWarehouseDocks[
                                currentDock->moduleIndex].deleteWhenEmpty) {
                            static_cast<void>(lifecycle.requestDestroy(
                                currentDock->id, ObjectDestroyReason::System,
                                confirmedTick));
                        }
                    }
                    if (runtime.boxes >= maxBoxes || warehouse.boxesStored == 0)
                        runtime.state = ObjectSupplyTruckRuntimeState::MovingToExit;
                } else {
                    if (runtime.boxes == 0) {
                        runtime.state = ObjectSupplyTruckRuntimeState::MovingToExit;
                        break;
                    }
                    const uint32_t deliveredBoxes = runtime.boxes;
                    const int64_t perBox = std::max<int64_t>(
                        0, rules.economy.valuePerSupplyBox);
                    const int64_t deliveredValueBase =
                        deliveredBoxes != 0 && perBox >
                                std::numeric_limits<int64_t>::max() /
                                    static_cast<int64_t>(deliveredBoxes)
                            ? std::numeric_limits<int64_t>::max()
                            : perBox * static_cast<int64_t>(deliveredBoxes);
                    int64_t deliveredValue = deliveredValueBase;
                    const PlayerState* player = players.get(owner->player);
                    if (rule.usesUpgradedSupplyBoost && maxBoxes != 0 &&
                        rule.upgradedSupplyBoost != 0 && player &&
                        upgradeMaskTest(
                            player->upgrades.completed,
                            rule.upgradedSupplyBoostUpgrade)) {
                        deliveredValue +=
                            static_cast<int64_t>(rule.upgradedSupplyBoost) *
                            static_cast<int64_t>(deliveredBoxes) /
                            static_cast<int64_t>(maxBoxes);
                    }
                    if (deliveredValue > 0 &&
                        players.adjustCash(owner->player, deliveredValue)) {
                        static_cast<void>(players.recordMoneyEarned(
                            owner->player,
                            static_cast<uint64_t>(deliveredValue),
                            confirmedTick));
                        runtime.boxes = 0;
                        markObjectDirty(
                            registry, truck.entity,
                            objectDirtyBit(
                                ObjectDirtyDomain::ModelCondition) |
                                objectDirtyBit(
                                    ObjectDirtyDomain::RenderExtraction));
                        outEvents.push_back({
                            .kind = ObjectSupplyEventKind::CashDelivered,
                            .truck = truck.id, .dock = currentDock->id,
                            .boxes = deliveredBoxes, .cash = deliveredValue,
                            .confirmedTick = confirmedTick,
                        });
                        const ObjectEconomyComponent* centerEconomy =
                            ecs::try_get<ObjectEconomyComponent>(
                                registry, currentDock->entity);
                        if (centerEconomy && centerEconomy->plan &&
                            currentDock->moduleIndex <
                                centerEconomy->plan->supplyCenterDocks.size()) {
                            const uint32_t grantMilliseconds =
                                centerEconomy->plan->supplyCenterDocks[
                                    currentDock->moduleIndex]
                                    .grantTemporaryStealthMilliseconds;
                            if (grantMilliseconds != 0) {
                                const ObjectStatusComponent* centerStatus =
                                    ecs::try_get<ObjectStatusComponent>(
                                        registry, currentDock->entity);
                                const ObjectStatusComponent* truckStatus =
                                    ecs::try_get<ObjectStatusComponent>(
                                        registry, truck.entity);
                                const ObjectStealthComponent* truckStealth =
                                    ecs::try_get<ObjectStealthComponent>(
                                        registry, truck.entity);
                                const bool centerIsStealthed = centerStatus &&
                                    centerStatus->hasAny(game::objectStatusBit(
                                        game::ObjectStatusFlag::Stealthed));
                                const bool truckHasPermanentStealth =
                                    truckStatus && truckStatus->hasAny(
                                        game::objectStatusBit(
                                            game::ObjectStatusFlag::CanStealth)) &&
                                    truckStealth &&
                                    truckStealth->temporaryGrantExpiresTick == 0;
                                // RefCode refreshes an existing temporary
                                // grant but never replaces a stronger innate/
                                // GPS-style permanent stealth state.
                                if (centerIsStealthed &&
                                    !truckHasPermanentStealth) {
                                    ObjectStealthSystem stealth;
                                    static_cast<void>(stealth.receiveGrant(
                                        registry, lifecycle, truck.id, true,
                                        static_cast<uint32_t>(millisecondsToTicks(
                                            grantMilliseconds,
                                            rules.logicFramesPerSecond)),
                                        confirmedTick));
                                }
                            }
                        }
                        runtime.nextActionTick = saturatingAdd(
                            confirmedTick, millisecondsToTicks(
                                rule.supplyCenterActionDelayMilliseconds,
                                rules.logicFramesPerSecond));
                    }
                    runtime.state = ObjectSupplyTruckRuntimeState::MovingToExit;
                }
                break;
            }
            case ObjectSupplyTruckRuntimeState::MovingToExit:
                if (runtime.nextActionTick > confirmedTick) break;
                if (!issueStage(DockPoint::Exit)) break;
                releaseDock(*currentDock, truck.id);
                outEvents.push_back({
                    .kind = ObjectSupplyEventKind::DockExited,
                    .truck = truck.id, .dock = currentDock->id,
                    .confirmedTick = confirmedTick,
                });
                runtime.targetDock = INVALID_OBJECT_ID;
                runtime.targetDockModule = 0;
                runtime.targetIsCenter = false;
                runtime.approachPosition = -1;
                runtime.state = ObjectSupplyTruckRuntimeState::SeekingWarehouse;
                break;
            case ObjectSupplyTruckRuntimeState::Regrouping:
                break;
            }
        }
    }
}

} // namespace engine
