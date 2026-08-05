#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

namespace engine {

bool ObjectSimulationAirOperationsDomain::reserveAirfieldParkingSlot(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId airfield, ObjectId aircraft, uint64_t confirmedTick,
    ObjectAirfieldReservation& outReservation) {
    return object_simulation_detail::state(*this).m_airfield.reserveParkingSlot(
        registry, lifecycle, airfield, aircraft, confirmedTick, outReservation,
        object_simulation_detail::state(*this).m_airfieldEvents);
}

bool ObjectSimulationAirOperationsDomain::reserveProducedAircraftParkingSlot(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId airfield, ObjectId aircraft, size_t doorIndex,
    uint64_t confirmedTick,
    ObjectAirfieldReservation& outReservation) {
    return object_simulation_detail::state(*this).m_airfield.
        reserveProducedAircraftParkingSlot(
            registry, lifecycle, airfield, aircraft, doorIndex,
            confirmedTick, outReservation,
            object_simulation_detail::state(*this).m_airfieldEvents);
}

bool ObjectSimulationAirOperationsDomain::reserveAirfieldRunway(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId airfield, ObjectId aircraft, bool landing,
    uint64_t confirmedTick,
    ObjectAirfieldReservation& outReservation) {
    return object_simulation_detail::state(*this).m_airfield.reserveRunway(
        registry, lifecycle, airfield, aircraft, landing, confirmedTick,
        object_simulation_detail::state(*this).m_rules.logicFramesPerSecond,
        outReservation,
        object_simulation_detail::state(*this).m_airfieldEvents);
}

bool ObjectSimulationAirOperationsDomain::releaseAirfieldParkingSlot(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId airfield, ObjectId aircraft, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_airfield.releaseParkingSlot(
        registry, lifecycle, airfield, aircraft, confirmedTick,
        object_simulation_detail::state(*this).m_airfieldEvents);
}

bool ObjectSimulationAirOperationsDomain::releaseAirfieldRunway(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId airfield, ObjectId aircraft, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_airfield.releaseRunway(
        registry, lifecycle, airfield, aircraft, confirmedTick,
        object_simulation_detail::state(*this).m_airfieldEvents);
}

bool ObjectSimulationAirOperationsDomain::releaseAirfieldReservations(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId airfield, ObjectId aircraft, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_airfield.releaseAircraftReservations(
        registry, lifecycle, airfield, aircraft, confirmedTick,
        object_simulation_detail::state(*this).m_airfieldEvents);
}

bool ObjectSimulationAirOperationsDomain::setAirfieldAircraftState(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId aircraft, ObjectAircraftRuntimeState state,
    uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_airfield.setAircraftState(
        registry, lifecycle, aircraft, state, confirmedTick,
        object_simulation_detail::state(*this).m_airfieldEvents);
}

bool ObjectSimulationAirOperationsDomain::beginProducedAircraftExit(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, ObjectId aircraft,
    uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_airfield.beginProducedAircraftExit(
        registry, lifecycle, content, aircraft, confirmedTick,
        object_simulation_detail::state(*this).m_airfieldEvents);
}

bool ObjectSimulationAirOperationsDomain::
    acknowledgeAirfieldAutomaticProduction(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        const ObjectAirfieldAutomaticProductionRequest& request) {
    auto& simulation = object_simulation_detail::state(*this);
    return simulation.m_airfield.acknowledgeAutomaticProduction(
        registry, lifecycle, simulation.m_rules, request);
}

bool ObjectSimulationAirOperationsDomain::requestAircraftRepairAtAirfield(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId aircraft, ObjectId airfield, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_airfield.
        requestAircraftRepairAtAirfield(
            registry, lifecycle, aircraft, airfield, confirmedTick,
            object_simulation_detail::state(*this).m_airfieldEvents);
}

bool ObjectSimulationAirOperationsDomain::beginSpectreGunshipTargeting(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, size_t moduleIndex, LogicFixedVec3 initialTarget,
    LogicFixedVec3 overrideTarget, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_airfield.beginSpectreGunshipTargeting(
        registry, lifecycle, object_simulation_detail::state(*this).m_rules, object, moduleIndex, initialTarget,
        overrideTarget, confirmedTick, object_simulation_detail::state(*this).m_radiusDecalEvents);
}

bool ObjectSimulationAirOperationsDomain::updateSpectreGunshipTargeting(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, size_t moduleIndex, LogicFixedVec3 overrideTarget,
    uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_airfield.updateSpectreGunshipTargeting(
        registry, lifecycle, object_simulation_detail::state(*this).m_rules, object, moduleIndex, overrideTarget,
        confirmedTick, object_simulation_detail::state(*this).m_radiusDecalEvents);
}

bool ObjectSimulationAirOperationsDomain::endSpectreGunshipTargeting(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, size_t moduleIndex, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_airfield.endSpectreGunshipTargeting(
        registry, lifecycle, object_simulation_detail::state(*this).m_rules, object, moduleIndex, confirmedTick,
        object_simulation_detail::state(*this).m_radiusDecalEvents);
}

bool ObjectSimulationAirOperationsDomain::beginChinookCombatDrop(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    SimulationRandom& random,
    const ObjectChinookCombatDropBeginRequest& request) {
    return object_simulation_detail::state(*this).m_airfield.beginChinookCombatDrop(
        registry, lifecycle, object_simulation_detail::state(*this).m_rules, random, request,
        object_simulation_detail::state(*this).m_chinookRopePresentationEvents);
}

bool ObjectSimulationAirOperationsDomain::notifyChinookRappellerStarted(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    SimulationRandom& random, ObjectId object, size_t moduleIndex,
    size_t ropeIndex, uint64_t confirmedTick, ObjectId rappeller) {
    return object_simulation_detail::state(*this).m_airfield.notifyChinookRappellerStarted(
        registry, lifecycle, object_simulation_detail::state(*this).m_rules, random, object, moduleIndex, ropeIndex,
        confirmedTick, rappeller);
}

bool ObjectSimulationAirOperationsDomain::endChinookCombatDrop(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, size_t moduleIndex, uint64_t confirmedTick,
    bool immediate) {
    return object_simulation_detail::state(*this).m_airfield.endChinookCombatDrop(
        registry, lifecycle, object_simulation_detail::state(*this).m_rules, object, moduleIndex, confirmedTick,
        immediate, object_simulation_detail::state(*this).m_chinookRopePresentationEvents);
}

container::Vector<ObjectAirfieldDefectionEntry>
ObjectSimulationAirOperationsDomain::airfieldDefectionEntries(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId airfield, PlayerId newOwner) const {
    return object_simulation_detail::state(*this).m_airfield.defectionEntries(
        registry, lifecycle, airfield, newOwner);
}

std::optional<ObjectChinookRopeReadyResult>
ObjectSimulationAirOperationsDomain::nextReadyChinookRope(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, size_t moduleIndex, uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_airfield.nextReadyChinookRope(
        registry, lifecycle, object, moduleIndex, confirmedTick);
}

} // namespace engine
