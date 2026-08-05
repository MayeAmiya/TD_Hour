#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include "game/player/PlayerRegistry.h"
#include "game/content/runtime/GameContentSnapshot.h"

#include <utility>

namespace engine {

namespace object_simulation_detail {

ObjectSimulationState& state(ObjectSimulation& simulation) noexcept {
    return *simulation.m_state;
}

const ObjectSimulationState& state(const ObjectSimulation& simulation) noexcept {
    return *simulation.m_state;
}

ObjectSimulationState& state(ObjectSimulationDamageDomain& domain) noexcept {
    return state(static_cast<ObjectSimulation&>(domain));
}
const ObjectSimulationState& state(const ObjectSimulationDamageDomain& domain) noexcept {
    return state(static_cast<const ObjectSimulation&>(domain));
}
ObjectSimulationState& state(ObjectSimulationMotionDomain& domain) noexcept {
    return state(static_cast<ObjectSimulation&>(domain));
}
const ObjectSimulationState& state(const ObjectSimulationMotionDomain& domain) noexcept {
    return state(static_cast<const ObjectSimulation&>(domain));
}
ObjectSimulationState& state(ObjectSimulationLifecycleDomain& domain) noexcept {
    return state(static_cast<ObjectSimulation&>(domain));
}
const ObjectSimulationState& state(const ObjectSimulationLifecycleDomain& domain) noexcept {
    return state(static_cast<const ObjectSimulation&>(domain));
}
ObjectSimulationState& state(ObjectSimulationProgressionDomain& domain) noexcept {
    return state(static_cast<ObjectSimulation&>(domain));
}
const ObjectSimulationState& state(const ObjectSimulationProgressionDomain& domain) noexcept {
    return state(static_cast<const ObjectSimulation&>(domain));
}
ObjectSimulationState& state(ObjectSimulationConstructionDomain& domain) noexcept {
    return state(static_cast<ObjectSimulation&>(domain));
}
const ObjectSimulationState& state(const ObjectSimulationConstructionDomain& domain) noexcept {
    return state(static_cast<const ObjectSimulation&>(domain));
}
ObjectSimulationState& state(ObjectSimulationAbilityDomain& domain) noexcept {
    return state(static_cast<ObjectSimulation&>(domain));
}
const ObjectSimulationState& state(const ObjectSimulationAbilityDomain& domain) noexcept {
    return state(static_cast<const ObjectSimulation&>(domain));
}
ObjectSimulationState& state(ObjectSimulationContainmentDomain& domain) noexcept {
    return state(static_cast<ObjectSimulation&>(domain));
}
const ObjectSimulationState& state(const ObjectSimulationContainmentDomain& domain) noexcept {
    return state(static_cast<const ObjectSimulation&>(domain));
}
ObjectSimulationState& state(ObjectSimulationAirOperationsDomain& domain) noexcept {
    return state(static_cast<ObjectSimulation&>(domain));
}
const ObjectSimulationState& state(const ObjectSimulationAirOperationsDomain& domain) noexcept {
    return state(static_cast<const ObjectSimulation&>(domain));
}

} // namespace object_simulation_detail

ObjectSimulation::ObjectSimulation()
    : m_state(std::make_unique<object_simulation_detail::ObjectSimulationState>()) {}

ObjectSimulation::~ObjectSimulation() = default;
ObjectSimulation::ObjectSimulation(ObjectSimulation&&) noexcept = default;
ObjectSimulation& ObjectSimulation::operator=(ObjectSimulation&&) noexcept = default;

const ObjectSimulationRules& ObjectSimulation::rules() const noexcept {
    return object_simulation_detail::state(*this).m_rules;
}

void ObjectSimulation::setSessionSeed(uint64_t seed) noexcept {
    object_simulation_detail::state(*this).m_sessionSeed = seed;
}

void ObjectSimulation::setHulkLifetimeOverrideFrames(
    std::optional<uint32_t> frames) noexcept {
    object_simulation_detail::state(*this).m_hulkLifetimeOverrideFrames = frames;
}

std::optional<uint32_t> ObjectSimulation::hulkLifetimeOverrideFrames() const noexcept {
    return object_simulation_detail::state(*this).m_hulkLifetimeOverrideFrames;
}

void ObjectSimulation::updateWeaponBonuses(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, const GameContentSnapshot& content,
    SimulationRandom& random, uint64_t confirmedTick) {
    object_simulation_detail::state(*this).m_weaponBonusUpdate.update(
        registry, lifecycle, players, content, random,
        object_simulation_detail::state(*this).m_rules.logicFramesPerSecond, confirmedTick,
        object_simulation_detail::state(*this).m_weaponBonusUpdateEvents);
}

void ObjectSimulation::updateRadarProviders(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    PlayerRegistry& players, uint64_t confirmedTick) const {
    object_simulation_detail::state(*this).m_upgrades.updateRadarProviders(
        registry, lifecycle, players, confirmedTick);
}

} // namespace engine
