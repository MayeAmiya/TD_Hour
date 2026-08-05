#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/structure/ObjectAirfieldDetail.h"
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
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
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

bool ObjectAirfieldSystem::beginProducedAircraftExit(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, ObjectId aircraft,
    uint64_t confirmedTick,
    container::Vector<ObjectAirfieldEvent>& outEvents) const
{
    const std::optional<ecs::entity> entity =
        lifecycle.entityFromId(aircraft);
    if (!entity) return false;
    ObjectAirfieldComponent* component =
        ecs::try_get<ObjectAirfieldComponent>(registry, *entity);
    if (!component || !component->plan) return false;
    const ObjectKindOfComponent* kinds =
        ecs::try_get<ObjectKindOfComponent>(registry, *entity);
    const bool producedAtHelipad = kinds && game::objectHasKind(
        kinds->mask, game::ObjectKindOf::ProducedAtHelipad);
    math::q32_32 helipadApproachHeight{int32_t{30}};
    if (const ObjectProducerComponent* producer =
            ecs::try_get<ObjectProducerComponent>(registry, *entity)) {
        const std::optional<ecs::entity> producerEntity =
            lifecycle.entityFromId(producer->producer);
        const ObjectAirfieldComponent* producerAirfield = producerEntity
            ? ecs::try_get<ObjectAirfieldComponent>(registry,
                                                     *producerEntity)
            : nullptr;
        if (producerAirfield && producerAirfield->plan &&
            !producerAirfield->plan->parkingPlaces.empty()) {
            helipadApproachHeight = math::q32_32::max(
                math::q32_32{int32_t{1}},
                producerAirfield->plan->parkingPlaces.front().
                    approachHeightFixed);
        }
    }
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, *entity);
    const LogicFixedVec3 current = transform
        ? readAuthoritativeObjectPosition(registry, *entity, *transform)
        : LogicFixedVec3{};
    bool initialized = false;
    const size_t count = std::min(component->jetAi.size(),
                                  component->plan->jetAi.size());
    for (size_t index = 0; index < count; ++index) {
        ObjectJetAiRuntime& runtime = component->jetAi[index];
        const game::ObjectJetAiRule& rule = component->plan->jetAi[index];
        if (producedAtHelipad) {
            runtime.route = {{
                .x = current.x,
                .y = current.y,
                .z = current.z + helipadApproachHeight,
            }};
            runtime.nextRoutePoint = 0;
            runtime.productionExitCompleted = true;
            runtime.phase = ObjectJetAirfieldPhase::TakingOff;
            runtime.state = ObjectAircraftRuntimeState::TakingOff;
            runtime.phaseEnteredTick = confirmedTick;
            runtime.helipadLandingPositionValid = false;
            runtime.helipadHealingRegistered = false;
            outEvents.push_back(makeRuntimeEvent(
                ObjectAirfieldEventKind::AircraftStateChanged, aircraft,
                rule.authoredOrder, "JetAIUpdate", index, confirmedTick,
                runtime.state, runtime.phase));
            initialized = true;
            continue;
        }
        if (!runtime.parkingReservation.airfield) continue;
        const JetParkingGeometry geometry = resolveJetParkingGeometry(
            registry, lifecycle, &content, runtime.parkingReservation,
            rule.parkingOffsetFixed);
        if (!geometry.valid) continue;

        runtime.route.clear();
        if (!geometry.creation.empty()) {
            publishJetPosition(registry, *entity,
                               geometry.creation.front(),
                               geometry.creationOrientationRadians);
            runtime.route.insert(runtime.route.end(),
                                 geometry.creation.begin() + 1,
                                 geometry.creation.end());
        }
        runtime.route.push_back(geometry.parking);
        runtime.nextRoutePoint = 0;
        runtime.parkingOrientationRadians =
            geometry.parkingOrientationRadians;
        runtime.productionExitCompleted = true;
        runtime.phase = ObjectJetAirfieldPhase::TaxiToParking;
        runtime.state = ObjectAircraftRuntimeState::Taxiing;
        runtime.phaseEnteredTick = confirmedTick;
        outEvents.push_back(makeRuntimeEvent(
            ObjectAirfieldEventKind::AircraftStateChanged, aircraft,
            rule.authoredOrder, "JetAIUpdate", index, confirmedTick,
            runtime.state, runtime.phase));
        initialized = true;
    }
    if (producedAtHelipad) {
        const size_t chinookCount = std::min(
            component->chinookAi.size(), component->plan->chinookAi.size());
        for (size_t index = 0; index < chinookCount; ++index) {
            ObjectChinookAiRuntime& runtime = component->chinookAi[index];
            runtime.flightRoute = {{
                .x = current.x,
                .y = current.y,
                .z = current.z + helipadApproachHeight,
            }};
            runtime.nextFlightRoutePoint = 0;
            runtime.flightPhase = ObjectHelicopterFlightPhase::TakingOff;
            runtime.flightPhaseEnteredTick = confirmedTick;
            runtime.productionExitCompleted = true;
            runtime.landingPositionValid = false;
            runtime.healingRegistered = false;
            outEvents.push_back({
                .kind = ObjectAirfieldEventKind::AircraftStateChanged,
                .object = aircraft,
                .moduleIndex = index,
                .authoredOrder =
                    component->plan->chinookAi[index].authoredOrder,
                .moduleClass = "ChinookAIUpdate",
                .aircraftState = ObjectAircraftRuntimeState::TakingOff,
                .confirmedTick = confirmedTick,
            });
            initialized = true;
        }
    }
    if (initialized) {
        if (ObjectAirborneComponent* airborne =
                ecs::try_get<ObjectAirborneComponent>(registry, *entity)) {
            airborne->isAirborne = false;
        } else {
            ecs::emplace<ObjectAirborneComponent>(
                registry, *entity, ObjectAirborneComponent{false});
        }
    }
    return initialized;
}

bool ObjectAirfieldSystem::requestAircraftRepairAtAirfield(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId aircraft, ObjectId airfield, uint64_t confirmedTick,
    container::Vector<ObjectAirfieldEvent>& outEvents) const
{
    const std::optional<ecs::entity> aircraftEntity =
        lifecycle.entityFromId(aircraft);
    const std::optional<ecs::entity> airfieldEntity =
        lifecycle.entityFromId(airfield);
    if (!aircraftEntity || !airfieldEntity || aircraft == airfield)
        return false;
    ObjectAirfieldComponent* aircraftRuntime =
        ecs::try_get<ObjectAirfieldComponent>(registry, *aircraftEntity);
    const ObjectAirfieldComponent* destination =
        ecs::try_get<ObjectAirfieldComponent>(registry, *airfieldEntity);
    const ObjectKindOfComponent* aircraftKinds =
        ecs::try_get<ObjectKindOfComponent>(registry, *aircraftEntity);
    if (!aircraftRuntime || !aircraftRuntime->plan || !destination ||
        !destination->plan ||
        (destination->parkingPlaces.empty() &&
         destination->flightDecks.empty()) ||
        !aircraftKinds || !game::objectHasKind(
            aircraftKinds->mask, game::ObjectKindOf::Aircraft)) {
        return false;
    }
    const size_t supportedJetCount = std::min(
        aircraftRuntime->jetAi.size(), aircraftRuntime->plan->jetAi.size());
    const size_t supportedChinookCount = std::min(
        aircraftRuntime->chinookAi.size(),
        aircraftRuntime->plan->chinookAi.size());
    if (supportedJetCount == 0 && supportedChinookCount == 0) return false;

    const bool producedAtHelipad = game::objectHasKind(
        aircraftKinds->mask, game::ObjectKindOf::ProducedAtHelipad);
    if (producedAtHelipad && destination->parkingPlaces.empty()) {
        // FlightDeck providers expose fixed-wing parking/runway geometry but
        // no HeliPark landing service. Preserve the provider abstraction
        // without inventing a helicopter landing point at the deck origin.
        return false;
    }
    ObjectId previousParkingAirfield = INVALID_OBJECT_ID;
    for (const ObjectJetAiRuntime& runtime : aircraftRuntime->jetAi) {
        if (runtime.parkingReservation.airfield) {
            previousParkingAirfield = runtime.parkingReservation.airfield;
            break;
        }
    }
    if (!producedAtHelipad) {
        ObjectAirfieldReservation reservation;
        if (!reserveParkingSlot(
                registry, lifecycle, airfield, aircraft, confirmedTick,
                reservation, outEvents)) {
            return false;
        }
        if (previousParkingAirfield &&
            previousParkingAirfield != airfield) {
            static_cast<void>(releaseParkingSlot(
                registry, lifecycle, previousParkingAirfield, aircraft,
                confirmedTick, outEvents));
        }
    }
    if (ObjectProducerComponent* producer =
            ecs::try_get<ObjectProducerComponent>(registry,
                                                   *aircraftEntity)) {
        producer->producer = airfield;
    } else {
        ecs::emplace<ObjectProducerComponent>(
            registry, *aircraftEntity, ObjectProducerComponent{airfield});
    }

    bool accepted = false;
    const size_t jetCount = supportedJetCount;
    for (size_t index = 0; index < jetCount; ++index) {
        ObjectJetAiRuntime& runtime = aircraftRuntime->jetAi[index];
        if (runtime.helipadHealingRegistered && runtime.reservedAirfield) {
            static_cast<void>(setAirfieldHealee(
                registry, lifecycle, runtime.reservedAirfield, aircraft,
                false));
        }
        runtime.reservedAirfield = airfield;
        runtime.route.clear();
        runtime.nextRoutePoint = 0;
        runtime.pendingOrder.reset();
        runtime.pendingOrderTail.clear();
        runtime.returnToBaseIdleDueTick = 0;
        runtime.helipadLandingPositionValid = false;
        runtime.helipadHealingRegistered = false;
        runtime.phase = ObjectJetAirfieldPhase::ReturningToBase;
        runtime.state = ObjectAircraftRuntimeState::ReturningToBase;
        runtime.phaseEnteredTick = confirmedTick;
        outEvents.push_back(makeRuntimeEvent(
            ObjectAirfieldEventKind::AircraftStateChanged, aircraft,
            aircraftRuntime->plan->jetAi[index].authoredOrder,
            "JetAIUpdate", index, confirmedTick, runtime.state,
            runtime.phase));
        accepted = true;
    }

    const size_t chinookCount = supportedChinookCount;
    for (size_t index = 0; index < chinookCount; ++index) {
        ObjectChinookAiRuntime& runtime = aircraftRuntime->chinookAi[index];
        if (runtime.healingRegistered && runtime.healingAirfield) {
            static_cast<void>(setAirfieldHealee(
                registry, lifecycle, runtime.healingAirfield, aircraft,
                false));
        }
        runtime.healingAirfield = airfield;
        runtime.flightRoute.clear();
        runtime.nextFlightRoutePoint = 0;
        runtime.pendingOrder.reset();
        runtime.pendingOrderTail.clear();
        runtime.landingPositionValid = false;
        runtime.healingRegistered = false;
        runtime.flightPhase =
            ObjectHelicopterFlightPhase::ReturningForLanding;
        runtime.flightPhaseEnteredTick = confirmedTick;
        outEvents.push_back({
            .kind = ObjectAirfieldEventKind::AircraftStateChanged,
            .object = aircraft,
            .moduleIndex = index,
            .authoredOrder =
                aircraftRuntime->plan->chinookAi[index].authoredOrder,
            .moduleClass = "ChinookAIUpdate",
            .aircraftState = ObjectAircraftRuntimeState::ReturningToBase,
            .confirmedTick = confirmedTick,
        });
        accepted = true;
    }
    return accepted;
}

} // namespace engine
