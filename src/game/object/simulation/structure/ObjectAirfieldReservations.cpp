#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/component/ObjectDirty.h"
#include "game/object/simulation/structure/ObjectAirfieldDetail.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/definition/ObjectArchetype.h"
#include "core/container/string_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <utility>

#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/base/SimulationRandom.h"
#include "game/object/simulation/combat/ObjectCombatSystem.h"
#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/world/ObjectRadiusDecal.h"
#include "game/object/runtime/ObjectStatus.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/presentation/ObjectModelConditionAuthority.h"
#include "game/object/definition/ModelConditionState.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/player/PlayerRegistry.h"
#include "game/terrain/MapVisibilityAuthority.h"
#include "game/terrain/TerrainLogic.h"
#include "math/fixed/q32_32_trig.h"

namespace engine
{
using namespace airfield_detail;

namespace {

[[nodiscard]] bool productionOwnsAirfieldDoor(
    const ecs::registry& registry, ecs::entity airfield,
    game::ObjectProductionExitKind kind, size_t doorIndex) noexcept {
    const ObjectProductionComponent* production =
        ecs::try_get<ObjectProductionComponent>(registry, airfield);
    if (kind != game::ObjectProductionExitKind::AirfieldParking ||
        !production || !production->exitPlan || production->jobs.empty() ||
        production->exitPlan->kind != kind) {
        return false;
    }
    const ObjectProductionJob& job = production->jobs.front();
    return job.kind == ObjectProductionJobKind::Unit &&
        job.constructionComplete && job.exitDoorAssigned &&
        job.exitDoorIndex == doorIndex && job.product &&
        !game::objectHasKind(job.product->kindOfMask,
                            game::ObjectKindOf::ProducedAtHelipad);
}

void holdAirfieldDoor(ecs::registry& registry, ecs::entity airfield,
                      size_t doorIndex, bool hold,
                      uint64_t confirmedTick) noexcept {
    ObjectProductionComponent* production =
        ecs::try_get<ObjectProductionComponent>(registry, airfield);
    if (production && setProductionDoorHoldOpen(
            *production, doorIndex, hold, confirmedTick)) {
        markObjectDirty(
            registry, airfield, ObjectDirtyDomain::ModelCondition);
    }
}

} // namespace

bool ObjectAirfieldSystem::reserveParkingSlot(ecs::registry& registry,
                                              const ObjectLifecycle& lifecycle,
                                              ObjectId airfield,
                                              ObjectId aircraft,
                                              uint64_t confirmedTick,
                                              ObjectAirfieldReservation& outReservation,
                                              container::Vector<ObjectAirfieldEvent>& outEvents) const
{
    if (!airfield || !aircraft || !objectAlive(registry, lifecycle, airfield) ||
        !objectAlive(registry, lifecycle, aircraft))
    {
        return false;
    }
    const std::optional<ecs::entity> airfieldEntity = lifecycle.entityFromId(airfield);
    if (!airfieldEntity)
        return false;
    ObjectAirfieldComponent* component = ecs::try_get<ObjectAirfieldComponent>(registry, *airfieldEntity);
    if (!component || !component->plan)
        return false;

    for (size_t index = 0; index < component->parkingPlaces.size() && index < component->plan->parkingPlaces.size();
         ++index)
    {
        ObjectAirfieldParkingRuntime& runtime = component->parkingPlaces[index];
        if (const std::optional<size_t> existing = findSlot(runtime.spaces, aircraft))
        {
            outReservation = {
                .airfield = airfield,
                .aircraft = aircraft,
                .slotKind = ObjectAirfieldSlotKind::ParkingPlace,
                .moduleIndex = index,
                .slotIndex = *existing,
            };
            rememberAircraftParkingReservation(registry, lifecycle, aircraft,
                                                outReservation, confirmedTick);
            return true;
        }
    }
    for (size_t index = 0; index < component->flightDecks.size() && index < component->plan->flightDecks.size();
         ++index)
    {
        ObjectAirfieldFlightDeckRuntime& runtime = component->flightDecks[index];
        if (const std::optional<size_t> existing = findSlot(runtime.spaces, aircraft))
        {
            outReservation = {
                .airfield = airfield,
                .aircraft = aircraft,
                .slotKind = ObjectAirfieldSlotKind::FlightDeck,
                .moduleIndex = index,
                .slotIndex = *existing,
            };
            rememberAircraftParkingReservation(registry, lifecycle, aircraft,
                                                outReservation, confirmedTick);
            return true;
        }
    }

    size_t parkingDoorIndex = 0;
    for (size_t index = 0; index < component->parkingPlaces.size() && index < component->plan->parkingPlaces.size();
         ++index)
    {
        ObjectAirfieldParkingRuntime& runtime = component->parkingPlaces[index];
        std::optional<size_t> freeSlot;
        size_t selectedDoor = 0;
        for (size_t slot = 0; slot < runtime.spaces.size(); ++slot) {
            const size_t door = parkingDoorIndex++;
            if (!runtime.spaces[slot] && !productionOwnsAirfieldDoor(
                    registry, *airfieldEntity,
                    game::ObjectProductionExitKind::AirfieldParking,
                    door)) {
                freeSlot = slot;
                selectedDoor = door;
                break;
            }
        }
        if (!freeSlot)
            continue;
        runtime.spaces[*freeSlot] = aircraft;
        holdAirfieldDoor(registry, *airfieldEntity, selectedDoor, true,
                         confirmedTick);
        const game::ObjectParkingPlaceRule& rule = component->plan->parkingPlaces[index];
        outReservation = {
            .airfield = airfield,
            .aircraft = aircraft,
            .slotKind = ObjectAirfieldSlotKind::ParkingPlace,
            .moduleIndex = index,
            .slotIndex = *freeSlot,
        };
        outEvents.push_back(makeSlotEvent(ObjectAirfieldEventKind::ParkingReserved,
                                          airfield,
                                          aircraft,
                                          ObjectAirfieldSlotKind::ParkingPlace,
                                          index,
                                          *freeSlot,
                                          rule.authoredOrder,
                                          "ParkingPlaceBehavior",
                                          static_cast<uint32_t>(runtime.spaces.size()),
                                          static_cast<uint32_t>(runtime.runwayUsers.size()),
                                          confirmedTick));
        rememberAircraftParkingReservation(registry, lifecycle, aircraft,
                                            outReservation, confirmedTick);
        return true;
    }
    for (size_t index = 0; index < component->flightDecks.size() && index < component->plan->flightDecks.size();
         ++index)
    {
        ObjectAirfieldFlightDeckRuntime& runtime = component->flightDecks[index];
        std::optional<size_t> freeSlot;
        for (size_t slot = 0; slot < runtime.spaces.size(); ++slot) {
            if (!runtime.spaces[slot]) {
                freeSlot = slot;
                break;
            }
        }
        if (!freeSlot)
            continue;
        runtime.spaces[*freeSlot] = aircraft;
        const game::ObjectFlightDeckRule& rule = component->plan->flightDecks[index];
        outReservation = {
            .airfield = airfield,
            .aircraft = aircraft,
            .slotKind = ObjectAirfieldSlotKind::FlightDeck,
            .moduleIndex = index,
            .slotIndex = *freeSlot,
        };
        outEvents.push_back(makeSlotEvent(ObjectAirfieldEventKind::ParkingReserved,
                                          airfield,
                                          aircraft,
                                          ObjectAirfieldSlotKind::FlightDeck,
                                          index,
                                          *freeSlot,
                                          rule.authoredOrder,
                                          "FlightDeckBehavior",
                                          static_cast<uint32_t>(runtime.spaces.size()),
                                          static_cast<uint32_t>(runtime.takeoffRunwayUsers.size()),
                                          confirmedTick));
        rememberAircraftParkingReservation(registry, lifecycle, aircraft,
                                            outReservation, confirmedTick);
        return true;
    }
    return false;
}

bool ObjectAirfieldSystem::reserveProducedAircraftParkingSlot(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId airfield, ObjectId aircraft, size_t doorIndex,
    uint64_t confirmedTick,
    ObjectAirfieldReservation& outReservation,
    container::Vector<ObjectAirfieldEvent>& outEvents) const {
    if (!airfield || !aircraft || !objectAlive(registry, lifecycle, airfield) ||
        !objectAlive(registry, lifecycle, aircraft)) {
        return false;
    }
    const std::optional<ecs::entity> airfieldEntity =
        lifecycle.entityFromId(airfield);
    ObjectAirfieldComponent* component = airfieldEntity
        ? ecs::try_get<ObjectAirfieldComponent>(registry, *airfieldEntity)
        : nullptr;
    if (!airfieldEntity || !component || !component->plan) return false;

    size_t flattened = 0;
    const size_t count = std::min(component->parkingPlaces.size(),
                                  component->plan->parkingPlaces.size());
    for (size_t moduleIndex = 0; moduleIndex < count; ++moduleIndex) {
        ObjectAirfieldParkingRuntime& runtime =
            component->parkingPlaces[moduleIndex];
        for (size_t slotIndex = 0; slotIndex < runtime.spaces.size();
             ++slotIndex, ++flattened) {
            if (flattened != doorIndex) continue;
            if (runtime.spaces[slotIndex] &&
                runtime.spaces[slotIndex] != aircraft) return false;
            runtime.spaces[slotIndex] = aircraft;
            const game::ObjectParkingPlaceRule& rule =
                component->plan->parkingPlaces[moduleIndex];
            outReservation = {
                .airfield = airfield,
                .aircraft = aircraft,
                .slotKind = ObjectAirfieldSlotKind::ParkingPlace,
                .moduleIndex = moduleIndex,
                .slotIndex = slotIndex,
            };
            holdAirfieldDoor(registry, *airfieldEntity, doorIndex, true,
                             confirmedTick);
            outEvents.push_back(makeSlotEvent(
                ObjectAirfieldEventKind::ParkingReserved, airfield, aircraft,
                ObjectAirfieldSlotKind::ParkingPlace, moduleIndex, slotIndex,
                rule.authoredOrder, "ParkingPlaceBehavior",
                static_cast<uint32_t>(runtime.spaces.size()),
                static_cast<uint32_t>(runtime.runwayUsers.size()),
                confirmedTick));
            rememberAircraftParkingReservation(
                registry, lifecycle, aircraft, outReservation,
                confirmedTick);
            return true;
        }
    }
    return false;
}

bool ObjectAirfieldSystem::reserveRunway(ecs::registry& registry,
                                         const ObjectLifecycle& lifecycle,
                                         ObjectId airfield,
                                         ObjectId aircraft,
                                         bool landing,
                                         uint64_t confirmedTick,
                                         uint32_t logicFramesPerSecond,
                                         ObjectAirfieldReservation& outReservation,
                                         container::Vector<ObjectAirfieldEvent>& outEvents) const
{
    if (!airfield || !aircraft || !objectAlive(registry, lifecycle, airfield) ||
        !objectAlive(registry, lifecycle, aircraft))
    {
        return false;
    }
    const std::optional<ecs::entity> airfieldEntity = lifecycle.entityFromId(airfield);
    if (!airfieldEntity)
        return false;
    ObjectAirfieldComponent* component = ecs::try_get<ObjectAirfieldComponent>(registry, *airfieldEntity);
    if (!component || !component->plan)
        return false;
    const ObjectAirfieldSlotKind slotKind =
        landing ? ObjectAirfieldSlotKind::LandingRunway : ObjectAirfieldSlotKind::TakeoffRunway;

    for (size_t index = 0; index < component->parkingPlaces.size() && index < component->plan->parkingPlaces.size();
         ++index)
    {
        ObjectAirfieldParkingRuntime& runtime = component->parkingPlaces[index];
        const std::optional<size_t> parkingSlot = findSlot(runtime.spaces, aircraft);
        if (!parkingSlot)
            continue;
        const game::ObjectParkingPlaceRule& rule = component->plan->parkingPlaces[index];
        const std::optional<size_t> runway = parkingRunwayForSlot(rule, *parkingSlot);
        if (!runway || *runway >= runtime.runwayUsers.size())
            return false;
        ObjectId& activeUser = runtime.runwayUsers[*runway];
        if (activeUser == aircraft)
        {
            outReservation = {
                .airfield = airfield,
                .aircraft = aircraft,
                .slotKind = slotKind,
                .moduleIndex = index,
                .slotIndex = *runway,
                .active = true,
            };
            rememberAircraftRunwayReservation(registry, lifecycle, aircraft, outReservation);
            return true;
        }
        if (!landing && *runway < runtime.nextTakeoffUsers.size() && runtime.nextTakeoffUsers[*runway] == aircraft)
        {
            outReservation = {
                .airfield = airfield,
                .aircraft = aircraft,
                .slotKind = slotKind,
                .moduleIndex = index,
                .slotIndex = *runway,
                .active = false,
            };
            rememberAircraftRunwayReservation(registry, lifecycle, aircraft, outReservation);
            return true;
        }
        if (!activeUser)
        {
            activeUser = aircraft;
            outReservation = {
                .airfield = airfield,
                .aircraft = aircraft,
                .slotKind = slotKind,
                .moduleIndex = index,
                .slotIndex = *runway,
                .active = true,
            };
            outEvents.push_back(makeSlotEvent(ObjectAirfieldEventKind::RunwayReserved,
                                              airfield,
                                              aircraft,
                                              slotKind,
                                              index,
                                              *runway,
                                              rule.authoredOrder,
                                              "ParkingPlaceBehavior",
                                              static_cast<uint32_t>(runtime.spaces.size()),
                                              static_cast<uint32_t>(runtime.runwayUsers.size()),
                                              confirmedTick));
            rememberAircraftRunwayReservation(registry, lifecycle, aircraft, outReservation);
            return true;
        }
        if (!landing && *runway < runtime.nextTakeoffUsers.size() && !runtime.nextTakeoffUsers[*runway])
        {
            runtime.nextTakeoffUsers[*runway] = aircraft;
            outReservation = {
                .airfield = airfield,
                .aircraft = aircraft,
                .slotKind = slotKind,
                .moduleIndex = index,
                .slotIndex = *runway,
                .active = false,
            };
            outEvents.push_back(makeSlotEvent(ObjectAirfieldEventKind::RunwayQueued,
                                              airfield,
                                              aircraft,
                                              slotKind,
                                              index,
                                              *runway,
                                              rule.authoredOrder,
                                              "ParkingPlaceBehavior",
                                              static_cast<uint32_t>(runtime.spaces.size()),
                                              static_cast<uint32_t>(runtime.runwayUsers.size()),
                                              confirmedTick));
            rememberAircraftRunwayReservation(registry, lifecycle, aircraft, outReservation);
            return true;
        }
        return false;
    }
    for (size_t index = 0; index < component->flightDecks.size() && index < component->plan->flightDecks.size();
         ++index)
    {
        ObjectAirfieldFlightDeckRuntime& runtime = component->flightDecks[index];
        const std::optional<size_t> parkingSlot = findSlot(runtime.spaces, aircraft);
        if (!parkingSlot)
            continue;
        const game::ObjectFlightDeckRule& rule = component->plan->flightDecks[index];
        const std::optional<size_t> runway = flightDeckRunwayForSlot(rule, *parkingSlot);
        if (!runway)
            return false;
        auto& runways = landing ? runtime.landingRunwayUsers : runtime.takeoffRunwayUsers;
        if (*runway >= runways.size())
            return false;
        if (runways[*runway] && runways[*runway] != aircraft)
            return false;
        const bool newlyReserved = !runways[*runway];
        runways[*runway] = aircraft;
        bool active = true;
        if (!landing &&
            *runway < runtime.nextLaunchWaveTicks.size() &&
            *runway < runtime.rampReadyTicks.size() &&
            *runway < runtime.catapultDueTicks.size() &&
            *runway < runtime.lowerRampTicks.size() &&
            *runway < runtime.rampRaised.size()) {
            if (confirmedTick < runtime.nextLaunchWaveTicks[*runway]) {
                active = false;
            } else if (!runtime.rampRaised[*runway]) {
                runtime.rampRaised[*runway] = 1u;
                runtime.rampReadyTicks[*runway] = saturatingAdd(
                    confirmedTick, millisecondsToFrames(
                        rule.launchRampMilliseconds,
                        logicFramesPerSecond));
                runtime.lowerRampTicks[*runway] =
                    std::numeric_limits<uint64_t>::max();
                publishObjectModelConditionDoor(
                    registry, *airfieldEntity,
                    ObjectModelConditionDoorSource::Airfield,
                    *runway + 1u,
                    ObjectModelConditionDoorPhase::Opening,
                    confirmedTick, rule.authoredOrder);
                active = false;
            } else if (confirmedTick < runtime.rampReadyTicks[*runway]) {
                active = false;
            } else if (runtime.catapultDueTicks[*runway] ==
                       std::numeric_limits<uint64_t>::max()) {
                runtime.nextLaunchWaveTicks[*runway] = saturatingAdd(
                    confirmedTick, millisecondsToFrames(
                        rule.launchWaveMilliseconds,
                        logicFramesPerSecond));
                runtime.catapultDueTicks[*runway] = saturatingAdd(
                    confirmedTick, millisecondsToFrames(
                        rule.catapultFireMilliseconds,
                        logicFramesPerSecond));
                runtime.lowerRampTicks[*runway] = saturatingAdd(
                    confirmedTick, millisecondsToFrames(
                        rule.lowerRampMilliseconds,
                        logicFramesPerSecond));
            }
        }
        outReservation = {
            .airfield = airfield,
            .aircraft = aircraft,
            .slotKind = slotKind,
            .moduleIndex = index,
            .slotIndex = *runway,
            .active = active,
        };
        if (newlyReserved)
        {
            outEvents.push_back(makeSlotEvent(ObjectAirfieldEventKind::RunwayReserved,
                                              airfield,
                                              aircraft,
                                              slotKind,
                                              index,
                                              *runway,
                                              rule.authoredOrder,
                                              "FlightDeckBehavior",
                                              static_cast<uint32_t>(runtime.spaces.size()),
                                              static_cast<uint32_t>(runtime.takeoffRunwayUsers.size()),
                                              confirmedTick));
        }
        rememberAircraftRunwayReservation(registry, lifecycle, aircraft, outReservation);
        return true;
    }
    return false;
}

bool ObjectAirfieldSystem::releaseAircraftReservations(ecs::registry& registry,
                                                       const ObjectLifecycle& lifecycle,
                                                       ObjectId airfield,
                                                       ObjectId aircraft,
                                                       uint64_t confirmedTick,
                                                       container::Vector<ObjectAirfieldEvent>& outEvents) const
{
    const bool runwayReleased = releaseRunway(registry, lifecycle, airfield, aircraft, confirmedTick, outEvents);
    const bool parkingReleased = releaseParkingSlot(registry, lifecycle, airfield, aircraft, confirmedTick, outEvents);
    return runwayReleased || parkingReleased;
}

container::Vector<ObjectAirfieldDefectionEntry>
ObjectAirfieldSystem::defectionEntries(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId airfield, PlayerId newOwner) const {
    container::Vector<ObjectAirfieldDefectionEntry> result;
    const std::optional<ecs::entity> airfieldEntity =
        lifecycle.entityFromId(airfield);
    if (!airfieldEntity || !newOwner) return result;
    const ObjectAirfieldComponent* component =
        ecs::try_get<ObjectAirfieldComponent>(registry, *airfieldEntity);
    if (!component) return result;

    container::Vector<ObjectId> parked;
    const auto appendSpaces = [&parked](const auto& modules) {
        for (const auto& module : modules) {
            for (const ObjectId aircraft : module.spaces) {
                if (aircraft) parked.push_back(aircraft);
            }
        }
    };
    appendSpaces(component->parkingPlaces);
    appendSpaces(component->flightDecks);
    std::sort(parked.begin(), parked.end());
    parked.erase(std::unique(parked.begin(), parked.end()), parked.end());
    result.reserve(parked.size());

    for (const ObjectId aircraft : parked) {
        const std::optional<ecs::entity> aircraftEntity =
            lifecycle.entityFromId(aircraft);
        if (!aircraftEntity) continue;
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, *aircraftEntity);
        if (health && health->effectivelyDead) continue;

        const ObjectAirborneComponent* airborne =
            ecs::try_get<ObjectAirborneComponent>(registry,
                                                   *aircraftEntity);
        bool takeoffOrLanding = false;
        bool airborneByJetState = false;
        if (const ObjectAirfieldComponent* aircraftRuntime =
                ecs::try_get<ObjectAirfieldComponent>(registry,
                                                       *aircraftEntity)) {
            for (const ObjectJetAiRuntime& jet : aircraftRuntime->jetAi) {
                if (jet.state == ObjectAircraftRuntimeState::Airborne ||
                    jet.state == ObjectAircraftRuntimeState::Attacking ||
                    jet.state == ObjectAircraftRuntimeState::ReturningToBase) {
                    airborneByJetState = true;
                }
                if (jet.state == ObjectAircraftRuntimeState::TakingOff ||
                    jet.state == ObjectAircraftRuntimeState::Landing) {
                    takeoffOrLanding = true;
                    break;
                }
            }
        }

        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, *aircraftEntity);
        // ParkingPlaceBehavior uses physical above-terrain state and only
        // exempts the takeoff/landing transition. ObjectAirborneComponent is
        // authoritative when materialized; the typed Jet state is the
        // compatibility fallback until every aircraft locomotor publishes
        // that sparse component in production.
        const bool aboveTerrain = airborne
            ? airborne->isAirborne
            : airborneByJetState;
        if (aboveTerrain && !takeoffOrLanding) {
            if (!owner || owner->player == newOwner) continue;
            const ObjectProducerComponent* producer =
                ecs::try_get<ObjectProducerComponent>(registry,
                                                       *aircraftEntity);
            result.push_back(ObjectAirfieldDefectionEntry{
                .aircraft = aircraft,
                .action =
                    ObjectAirfieldDefectionAction::ReleaseReservation,
                .clearProducer = producer && producer->producer == airfield,
            });
            continue;
        }

        if (!owner || owner->player == newOwner) continue;
        result.push_back(ObjectAirfieldDefectionEntry{
            .aircraft = aircraft,
            .action = ObjectAirfieldDefectionAction::Defect,
        });
    }
    return result;
}

bool ObjectAirfieldSystem::releaseParkingSlot(ecs::registry& registry,
                                              const ObjectLifecycle& lifecycle,
                                              ObjectId airfield,
                                              ObjectId aircraft,
                                              uint64_t confirmedTick,
                                              container::Vector<ObjectAirfieldEvent>& outEvents) const
{
    if (!airfield || !aircraft)
        return false;
    const std::optional<ecs::entity> airfieldEntity = lifecycle.entityFromId(airfield);
    if (!airfieldEntity)
        return false;
    ObjectAirfieldComponent* component = ecs::try_get<ObjectAirfieldComponent>(registry, *airfieldEntity);
    if (!component || !component->plan)
        return false;
    bool released = false;
    size_t parkingDoorIndex = 0;
    for (size_t index = 0; index < component->parkingPlaces.size() && index < component->plan->parkingPlaces.size();
         ++index)
    {
        ObjectAirfieldParkingRuntime& runtime = component->parkingPlaces[index];
        const game::ObjectParkingPlaceRule& rule = component->plan->parkingPlaces[index];
        if (const std::optional<size_t> slot = findSlot(runtime.spaces, aircraft))
        {
            const size_t doorIndex = parkingDoorIndex + *slot;
            runtime.spaces[*slot] = INVALID_OBJECT_ID;
            holdAirfieldDoor(registry, *airfieldEntity, doorIndex, false,
                             confirmedTick);
            released = true;
            outEvents.push_back(makeSlotEvent(ObjectAirfieldEventKind::ParkingReleased,
                                              airfield,
                                              aircraft,
                                              ObjectAirfieldSlotKind::ParkingPlace,
                                              index,
                                              *slot,
                                              rule.authoredOrder,
                                              "ParkingPlaceBehavior",
                                              static_cast<uint32_t>(runtime.spaces.size()),
                                              static_cast<uint32_t>(runtime.runwayUsers.size()),
                                              confirmedTick));
        }
        parkingDoorIndex += runtime.spaces.size();
    }
    for (size_t index = 0; index < component->flightDecks.size() && index < component->plan->flightDecks.size();
         ++index)
    {
        ObjectAirfieldFlightDeckRuntime& runtime = component->flightDecks[index];
        const game::ObjectFlightDeckRule& rule = component->plan->flightDecks[index];
        if (const std::optional<size_t> slot = findSlot(runtime.spaces, aircraft))
        {
            runtime.spaces[*slot] = INVALID_OBJECT_ID;
            released = true;
            outEvents.push_back(makeSlotEvent(ObjectAirfieldEventKind::ParkingReleased,
                                              airfield,
                                              aircraft,
                                              ObjectAirfieldSlotKind::FlightDeck,
                                              index,
                                              *slot,
                                              rule.authoredOrder,
                                              "FlightDeckBehavior",
                                              static_cast<uint32_t>(runtime.spaces.size()),
                                              static_cast<uint32_t>(runtime.takeoffRunwayUsers.size()),
                                              confirmedTick));
        }
    }
    if (released)
        clearAircraftParkingReservation(registry, lifecycle, airfield,
                                        aircraft, confirmedTick);
    return released;
}

bool ObjectAirfieldSystem::releaseRunway(ecs::registry& registry,
                                         const ObjectLifecycle& lifecycle,
                                         ObjectId airfield,
                                         ObjectId aircraft,
                                         uint64_t confirmedTick,
                                         container::Vector<ObjectAirfieldEvent>& outEvents) const
{
    if (!airfield || !aircraft)
        return false;
    const std::optional<ecs::entity> airfieldEntity = lifecycle.entityFromId(airfield);
    if (!airfieldEntity)
        return false;
    ObjectAirfieldComponent* component = ecs::try_get<ObjectAirfieldComponent>(registry, *airfieldEntity);
    if (!component || !component->plan)
        return false;
    bool released = false;
    for (size_t index = 0; index < component->parkingPlaces.size() && index < component->plan->parkingPlaces.size();
         ++index)
    {
        ObjectAirfieldParkingRuntime& runtime = component->parkingPlaces[index];
        const game::ObjectParkingPlaceRule& rule = component->plan->parkingPlaces[index];
        for (size_t runway = 0; runway < runtime.runwayUsers.size(); ++runway)
        {
            if (runtime.runwayUsers[runway] == aircraft)
            {
                runtime.runwayUsers[runway] = INVALID_OBJECT_ID;
                released = true;
                outEvents.push_back(makeSlotEvent(ObjectAirfieldEventKind::RunwayReleased,
                                                  airfield,
                                                  aircraft,
                                                  ObjectAirfieldSlotKind::TakeoffRunway,
                                                  index,
                                                  runway,
                                                  rule.authoredOrder,
                                                  "ParkingPlaceBehavior",
                                                  static_cast<uint32_t>(runtime.spaces.size()),
                                                  static_cast<uint32_t>(runtime.runwayUsers.size()),
                                                  confirmedTick));
                if (runway < runtime.nextTakeoffUsers.size() && runtime.nextTakeoffUsers[runway])
                {
                    const ObjectId advanced = runtime.nextTakeoffUsers[runway];
                    runtime.nextTakeoffUsers[runway] = INVALID_OBJECT_ID;
                    runtime.runwayUsers[runway] = advanced;
                    outEvents.push_back(makeSlotEvent(ObjectAirfieldEventKind::RunwayReservationAdvanced,
                                                      airfield,
                                                      advanced,
                                                      ObjectAirfieldSlotKind::TakeoffRunway,
                                                      index,
                                                      runway,
                                                      rule.authoredOrder,
                                                      "ParkingPlaceBehavior",
                                                      static_cast<uint32_t>(runtime.spaces.size()),
                                                      static_cast<uint32_t>(runtime.runwayUsers.size()),
                                                      confirmedTick));
                    ObjectAirfieldReservation reservation{
                        .airfield = airfield,
                        .aircraft = advanced,
                        .slotKind = ObjectAirfieldSlotKind::TakeoffRunway,
                        .moduleIndex = index,
                        .slotIndex = runway,
                        .active = true,
                    };
                    rememberAircraftRunwayReservation(registry, lifecycle, advanced, reservation);
                }
            }
            if (runway < runtime.nextTakeoffUsers.size() && runtime.nextTakeoffUsers[runway] == aircraft)
            {
                runtime.nextTakeoffUsers[runway] = INVALID_OBJECT_ID;
                released = true;
                outEvents.push_back(makeSlotEvent(ObjectAirfieldEventKind::RunwayReleased,
                                                  airfield,
                                                  aircraft,
                                                  ObjectAirfieldSlotKind::TakeoffRunway,
                                                  index,
                                                  runway,
                                                  rule.authoredOrder,
                                                  "ParkingPlaceBehavior",
                                                  static_cast<uint32_t>(runtime.spaces.size()),
                                                  static_cast<uint32_t>(runtime.runwayUsers.size()),
                                                  confirmedTick));
            }
        }
    }
    for (size_t index = 0; index < component->flightDecks.size() && index < component->plan->flightDecks.size();
         ++index)
    {
        ObjectAirfieldFlightDeckRuntime& runtime = component->flightDecks[index];
        const game::ObjectFlightDeckRule& rule = component->plan->flightDecks[index];
        const auto releaseFrom = [&](container::Vector<ObjectId>& runways, ObjectAirfieldSlotKind slotKind)
        {
            if (const std::optional<size_t> runway = findSlot(runways, aircraft))
            {
                runways[*runway] = INVALID_OBJECT_ID;
                released = true;
                outEvents.push_back(makeSlotEvent(ObjectAirfieldEventKind::RunwayReleased,
                                                  airfield,
                                                  aircraft,
                                                  slotKind,
                                                  index,
                                                  *runway,
                                                  rule.authoredOrder,
                                                  "FlightDeckBehavior",
                                                  static_cast<uint32_t>(runtime.spaces.size()),
                                                  static_cast<uint32_t>(runtime.takeoffRunwayUsers.size()),
                                                  confirmedTick));
            }
        };
        releaseFrom(runtime.takeoffRunwayUsers, ObjectAirfieldSlotKind::TakeoffRunway);
        releaseFrom(runtime.landingRunwayUsers, ObjectAirfieldSlotKind::LandingRunway);
    }
    if (released)
        clearAircraftRunwayReservation(registry, lifecycle, airfield, aircraft);
    return released;
}

bool ObjectAirfieldSystem::setAircraftState(ecs::registry& registry,
                                            const ObjectLifecycle& lifecycle,
                                            ObjectId aircraft,
                                            ObjectAircraftRuntimeState state,
                                            uint64_t confirmedTick,
                                            container::Vector<ObjectAirfieldEvent>& outEvents) const
{
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(aircraft);
    if (!entity)
        return false;
    ObjectAirfieldComponent* component = ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component || !component->plan)
        return false;
    bool changed = false;
    container::Vector<ObjectId> parkingToRelease;
    for (size_t index = 0; index < component->jetAi.size() && index < component->plan->jetAi.size(); ++index)
    {
        ObjectJetAiRuntime& runtime = component->jetAi[index];
        const game::ObjectJetAiRule& rule = component->plan->jetAi[index];
        if ((state == ObjectAircraftRuntimeState::Airborne ||
             state == ObjectAircraftRuntimeState::Attacking) &&
            !rule.keepsParkingSpaceWhenAirborne &&
            runtime.parkingReservation.airfield)
        {
            parkingToRelease.push_back(runtime.parkingReservation.airfield);
        }
        if (runtime.state == state)
            continue;
        runtime.state = state;
        runtime.phase = state == ObjectAircraftRuntimeState::Parked
            ? ObjectJetAirfieldPhase::Parked
            : state == ObjectAircraftRuntimeState::Taxiing
                ? ObjectJetAirfieldPhase::TaxiToTakeoff
            : state == ObjectAircraftRuntimeState::TakingOff
                ? ObjectJetAirfieldPhase::TakingOff
            : state == ObjectAircraftRuntimeState::Airborne ||
                    state == ObjectAircraftRuntimeState::Attacking
                ? ObjectJetAirfieldPhase::Airborne
            : state == ObjectAircraftRuntimeState::ReturningToBase
                ? ObjectJetAirfieldPhase::ReturningToBase
            : state == ObjectAircraftRuntimeState::Landing
                ? ObjectJetAirfieldPhase::Landing
            : state == ObjectAircraftRuntimeState::Reloading
                ? ObjectJetAirfieldPhase::Reloading
                : runtime.phase;
        runtime.phaseEnteredTick = confirmedTick;
        runtime.route.clear();
        runtime.nextRoutePoint = 0;
        changed = true;
        outEvents.push_back(makeRuntimeEvent(ObjectAirfieldEventKind::AircraftStateChanged,
                                             aircraft,
                                             component->plan->jetAi[index].authoredOrder,
                                             "JetAIUpdate",
                                             index,
                                             confirmedTick,
                                             state,
                                             runtime.phase));
    }
    for (size_t index = 0; index < component->spectreGunships.size() && index < component->plan->spectreGunships.size();
         ++index)
    {
        ObjectSpectreGunshipRuntime& runtime = component->spectreGunships[index];
        if (runtime.state == state)
            continue;
        runtime.state = state;
        changed = true;
        outEvents.push_back(makeRuntimeEvent(ObjectAirfieldEventKind::AircraftStateChanged,
                                             aircraft,
                                             component->plan->spectreGunships[index].authoredOrder,
                                             "SpectreGunshipUpdate",
                                             index,
                                             confirmedTick,
                                             state));
    }
    std::sort(parkingToRelease.begin(), parkingToRelease.end());
    parkingToRelease.erase(
        std::unique(parkingToRelease.begin(), parkingToRelease.end()),
        parkingToRelease.end());
    for (const ObjectId airfield : parkingToRelease)
    {
        static_cast<void>(releaseParkingSlot(
            registry, lifecycle, airfield, aircraft, confirmedTick,
            outEvents));
    }
    return changed;
}

} // namespace engine
