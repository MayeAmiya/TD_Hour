#include "game/object/simulation/runtime/ObjectSimulation.h"
#include "game/object/simulation/runtime/ObjectSimulationState.h"

#include <utility>

namespace engine {

bool ObjectSimulationConstructionDomain::beginObjectConstruction(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId site, ObjectId builder, uint32_t requiredFrames,
    bool rebuild, uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_builder.beginConstruction(
        registry, lifecycle, site, builder, requiredFrames, rebuild,
        confirmedTick);
}

bool ObjectSimulationConstructionDomain::requestObjectRepair(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, ObjectId target, uint64_t confirmedTick,
    uint32_t sourceSequence) const {
    return object_simulation_detail::state(*this).m_builder.requestRepair(
        registry, lifecycle, builder, target, confirmedTick,
        sourceSequence);
}

bool ObjectSimulationConstructionDomain::canObjectRepair(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ObjectId builder, ObjectId target) const {
    return object_simulation_detail::state(*this).m_builder.canRepair(registry, lifecycle, players, builder, target);
}

bool ObjectSimulationConstructionDomain::requestObjectRepair(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ObjectId builder, ObjectId target,
    uint64_t confirmedTick, uint32_t sourceSequence,
    bool replaceExternalOrders, bool requireClearTarget) const {
    return object_simulation_detail::state(*this).m_builder.requestRepair(
        registry, lifecycle, players, builder, target, confirmedTick,
        sourceSequence, replaceExternalOrders, requireClearTarget);
}

bool ObjectSimulationConstructionDomain::canObjectResumeConstruction(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ObjectId builder, ObjectId site) const {
    return object_simulation_detail::state(*this).m_builder
        .canResumeConstruction(registry, lifecycle, players, builder, site);
}

bool ObjectSimulationConstructionDomain::resumeObjectConstruction(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const PlayerRegistry& players, ObjectId builder, ObjectId site,
    uint64_t confirmedTick, uint32_t sourceSequence,
    bool replaceExternalOrders) const {
    return object_simulation_detail::state(*this).m_builder
        .resumeConstruction(
            registry, lifecycle, players, builder, site, confirmedTick,
            sourceSequence, replaceExternalOrders);
}

bool ObjectSimulationConstructionDomain::assignObjectConstruction(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, ObjectId site, uint64_t confirmedTick,
    uint32_t sourceSequence) const {
    return object_simulation_detail::state(*this).m_builder.assignConstruction(
        registry, lifecycle, builder, site, confirmedTick, sourceSequence);
}

ObjectBuilderTask ObjectSimulationConstructionDomain::objectBuilderTask(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, ObjectBuilderTaskKind kind, size_t moduleIndex) const {
    return object_simulation_detail::state(*this).m_builder.task(registry, lifecycle, builder, kind, moduleIndex);
}

ObjectBuilderTask ObjectSimulationConstructionDomain::mostRecentObjectBuilderTask(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, size_t moduleIndex) const {
    return object_simulation_detail::state(*this).m_builder.mostRecentTask(
        registry, lifecycle, builder, moduleIndex);
}

ObjectBuilderTask ObjectSimulationConstructionDomain::currentObjectBuilderTask(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, size_t moduleIndex) const {
    return object_simulation_detail::state(*this).m_builder.currentTask(registry, lifecycle, builder, moduleIndex);
}

bool ObjectSimulationConstructionDomain::isObjectBuilderTaskPending(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, ObjectBuilderTaskKind kind, size_t moduleIndex) const {
    return object_simulation_detail::state(*this).m_builder.isTaskPending(
        registry, lifecycle, builder, kind, moduleIndex);
}

bool ObjectSimulationConstructionDomain::isAnyObjectBuilderTaskPending(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, size_t moduleIndex) const {
    return object_simulation_detail::state(*this).m_builder.isAnyTaskPending(
        registry, lifecycle, builder, moduleIndex);
}

bool ObjectSimulationConstructionDomain::cancelAllObjectBuilderTasks(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId builder, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_builder.cancelAllTasks(
        registry, lifecycle, builder, confirmedTick,
        object_simulation_detail::state(*this).m_bridgeRepairScaffoldIntents);
}

ObjectRepairDockCommandResult ObjectSimulationConstructionDomain::processRepairDockCommand(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectRepairDockCommand& command) const {
    return object_simulation_detail::state(*this).m_economy.processRepairDockCommand(
        registry, lifecycle, object_simulation_detail::state(*this).m_rules, command);
}

bool ObjectSimulationConstructionDomain::startRebuildHole(
    ecs::registry& registry, ObjectLifecycle& lifecycle, ObjectId hole,
    container::StringView rebuildTemplate, ObjectId spawner,
    uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_rebuildHole.startHole(
        registry, lifecycle, hole, rebuildTemplate, spawner, object_simulation_detail::state(*this).m_rules,
        confirmedTick);
}

bool ObjectSimulationConstructionDomain::acknowledgeRebuildWorker(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId hole, ObjectId worker, ObjectId reconstruction,
    uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_rebuildHole.acknowledgeWorker(
        registry, lifecycle, hole, worker, reconstruction, confirmedTick);
}

bool ObjectSimulationConstructionDomain::rejectRebuildWorker(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId hole, uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_rebuildHole.rejectWorker(
        registry, lifecycle, hole, object_simulation_detail::state(*this).m_rules, confirmedTick);
}

bool ObjectSimulation::reportIncomingSmallMissile(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId victim, ObjectId projectile, SimulationRandom& random,
    uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_countermeasures.reportIncomingMissile(
        registry, lifecycle, victim, projectile, random, confirmedTick);
}

void ObjectSimulation::updateCountermeasures(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    uint64_t confirmedTick) {
    object_simulation_detail::state(*this).m_countermeasures.update(registry, lifecycle, confirmedTick);
}

void ObjectSimulation::acknowledgeCountermeasureFlareSpawn(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId source, uint32_t ruleIndex, ObjectId flare, bool created,
    uint64_t confirmedTick) {
    object_simulation_detail::state(*this).m_countermeasures.acknowledgeFlareSpawn(
        registry, lifecycle, source, ruleIndex, flare, created,
        confirmedTick);
}

void ObjectSimulation::resolveCountermeasureDiversions(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    uint64_t confirmedTick) {
    object_simulation_detail::state(*this).m_countermeasures.resolveMissileDiversions(
        registry, lifecycle, confirmedTick);
}

bool ObjectSimulation::reloadObjectCountermeasures(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_countermeasures.reload(
        registry, lifecycle, object, confirmedTick);
}

bool ObjectSimulation::setSmartBombTarget(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, const LogicFixedVec3& target) const {
    return object_simulation_detail::state(*this).m_smartBomb.setTarget(registry, lifecycle, object, target);
}

bool ObjectSimulation::attachStickyBomb(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain,
    const ObjectStickyBombAttachRequest& request) {
    const std::optional<uint64_t> dueTick = object_simulation_detail::state(*this).m_lifetime.nextDueTick(
        registry, lifecycle, request.bomb,
        game::ObjectLifetimeAction::Kill);
    const bool attached = object_simulation_detail::state(*this).m_stickyBomb.attach(
        registry, lifecycle, terrain, request, dueTick,
        object_simulation_detail::state(*this).m_rules.logicFramesPerSecond, object_simulation_detail::state(*this).m_stickyBombPresentationEvents);
    if (attached && request.bomber) {
        static_cast<void>(object_simulation_detail::state(*this).m_experience.setSink(
            registry, lifecycle, request.bomb, request.bomber,
            request.confirmedTick));
    }
    return attached;
}

bool ObjectSimulation::acknowledgeSpecialPowerSpawn(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSpecialPowerSpawnRequest& request, ObjectId spawned,
    bool accepted) {
    switch (request.completion) {
    case ObjectSpecialPowerSpawnCompletionKind::None:
        return false;
    case ObjectSpecialPowerSpawnCompletionKind::SpecialAbility: {
        const bool acknowledged =
            object_simulation_detail::state(*this)
                .m_tactical.acknowledgeSpecialObjectSpawn(
                    registry, lifecycle, request.source,
                    request.specialAbilityRuleIndex,
                    request.activationSequence, request.target,
                    request.emissionSequence, spawned, accepted);
        if (acknowledged && accepted && spawned && request.source) {
            static_cast<void>(
                object_simulation_detail::state(*this).m_experience.setSink(
                    registry, lifecycle, spawned, request.source,
                    request.confirmedTick));
        }
        return acknowledged;
    }
    case ObjectSpecialPowerSpawnCompletionKind::AirfieldCapabilityChild:
        if (!accepted || !spawned) return true;
        return object_simulation_detail::state(*this)
            .m_airfield.assignSpectreGunshipGattling(
                registry, lifecycle, request.source,
                request.capabilityRuleIndex, spawned,
                request.confirmedTick);
    }
    return false;
}

bool ObjectSimulation::acceptsSpecialAbilityFacingRequest(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSpecialAbilityFacingRequest& request) const {
    return object_simulation_detail::state(*this)
        .m_tactical.acceptsSpecialAbilityFacingRequest(
            registry, lifecycle, request);
}

bool ObjectSimulation::acknowledgeSpecialAbilityFacingRequest(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSpecialAbilityFacingRequest& request, bool accepted,
    bool terminalFailure, uint64_t requestIssuedTick,
    uint32_t requestSequence) {
    return object_simulation_detail::state(*this)
        .m_tactical.acknowledgeSpecialAbilityFacingRequest(
            registry, lifecycle, request, accepted, terminalFailure,
            requestIssuedTick, requestSequence);
}

bool ObjectSimulation::acknowledgeSpecialAbilityFacingFeedback(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId source, uint64_t requestIssuedTick,
    uint32_t requestSequence, bool completed) {
    return object_simulation_detail::state(*this)
        .m_tactical.acknowledgeSpecialAbilityFacingFeedback(
            registry, lifecycle, source, requestIssuedTick,
            requestSequence, completed);
}

bool ObjectSimulation::retargetStickyBomb(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId bomb, ObjectId target) const {
    return object_simulation_detail::state(*this).m_stickyBomb.retarget(registry, lifecycle, bomb, target);
}

bool ObjectSimulation::detonateStickyBomb(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const GameContentSnapshot& content, ObjectId bomb,
    sticky_bomb::DetonationTrigger trigger, uint64_t confirmedTick) {
    container::Vector<ObjectDamageRequest> damage;
    if (!object_simulation_detail::state(*this).m_stickyBomb.detonate(
            registry, lifecycle, content, bomb, trigger, confirmedTick,
            damage, object_simulation_detail::state(*this).m_stickyBombPresentationEvents)) {
        return false;
    }
    for (ObjectDamageRequest& request : damage) {
        queueDamage(std::move(request));
    }
    return true;
}

std::optional<ObjectStickyBombState> ObjectSimulation::stickyBombState(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId bomb) const noexcept {
    return object_simulation_detail::state(*this).m_stickyBomb.state(registry, lifecycle, bomb);
}

bool ObjectSimulation::resetObjectOclTimers(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, SimulationRandom& random,
    uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_oclUpdate.resetTimers(
        registry, lifecycle, object, random,
        object_simulation_detail::state(*this).m_rules.logicFramesPerSecond, confirmedTick);
}

bool ObjectSimulation::setPlayerSpyVisionDisabledUntil(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    PlayerId player, uint64_t untilTick,
    uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_spyVision.setPlayerDisabledUntil(
        registry, lifecycle, player, untilTick, confirmedTick);
}

bool ObjectSimulation::restartAllSpecialPowerRecharge(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, const GameContentSnapshot& content,
    uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_specialPower.restartAllRecharge(
        registry, lifecycle, object, content, object_simulation_detail::state(*this).m_rules, confirmedTick);
}

void ObjectSimulation::queuePhysicsRequest(ObjectPhysicsRequest request) {
    if (!request.target) return;
    const uint64_t ordinal = object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal++;
    if (object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal == 0) ++object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal;
    object_simulation_detail::state(*this).m_physicsRequests.push_back({.request = std::move(request), .submissionOrdinal = ordinal});
}

bool ObjectSimulation::setPhysicsIgnoreCollisionWith(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, ObjectId ignored) const noexcept {
    const std::optional<ecs::entity> entity = lifecycle.entityFromId(object);
    if (!entity || lifecycle.isPendingDestroy(object)) return false;
    ObjectPhysicsComponent* physics =
        ecs::try_get<ObjectPhysicsComponent>(registry, *entity);
    if (!physics) return false;
    physics->ignoreCollisionWith = ignored;
    return true;
}

bool ObjectSimulation::rescheduleLifetime(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectLifetimeRescheduleRequest& request) const {
    return object_simulation_detail::state(*this).m_lifetime.reschedule(registry, lifecycle, request, object_simulation_detail::state(*this).m_sessionSeed);
}

bool ObjectSimulation::setFloatEnabled(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectFloatEnableRequest& request) const {
    return object_simulation_detail::state(*this).m_float.setEnabled(registry, lifecycle, request);
}

bool ObjectSimulation::stopAllBoneFx(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectBoneFxStopRequest& request) {
    return object_simulation_detail::state(*this).m_boneFx.stopAll(
        registry, lifecycle, request, object_simulation_detail::state(*this).m_nextGameplaySubmissionOrdinal,
        object_simulation_detail::state(*this).m_transitionDamageFxEvents);
}

bool ObjectSimulation::markObjectDetected(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint32_t frames, uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_stealth.markDetected(
        registry, lifecycle, object, frames, object_simulation_detail::state(*this).m_rules, confirmedTick);
}

bool ObjectSimulation::grantObjectStealth(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, bool active, uint32_t frames,
    uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_stealth.receiveGrant(
        registry, lifecycle, object, active, frames, confirmedTick);
}

bool ObjectSimulation::setStealthDetectorEnabled(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, bool enabled, uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_stealth.setDetectorEnabled(
        registry, lifecycle, object, enabled, confirmedTick);
}

bool ObjectSimulation::setCommandButtonHunt(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, container::String commandButton,
    uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_tactical.setCommandButtonHunt(
        registry, lifecycle, object, std::move(commandButton),
        confirmedTick);
}

bool ObjectSimulation::setWanderInPlace(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_tactical.setWanderInPlace(
        registry, lifecycle, object, confirmedTick);
}

bool ObjectSimulation::setOverchargeActive(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, bool active, uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_overcharge.setActive(
        registry, lifecycle, object, active, object_simulation_detail::state(*this).m_rules, confirmedTick);
}

bool ObjectSimulation::toggleOvercharge(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_overcharge.toggle(
        registry, lifecycle, object, object_simulation_detail::state(*this).m_rules, confirmedTick);
}

bool ObjectSimulation::isOverchargeActive(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object) const noexcept {
    return object_simulation_detail::state(*this).m_overcharge.isActive(registry, lifecycle, object);
}

bool ObjectSimulation::createRadiusDecal(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectRadiusDecalRequest& request) {
    return object_simulation_detail::state(*this).m_radiusDecal.createRadiusDecal(
        registry, lifecycle, request, object_simulation_detail::state(*this).m_radiusDecalEvents);
}

bool ObjectSimulation::killRadiusDecal(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, uint64_t confirmedTick) {
    return object_simulation_detail::state(*this).m_radiusDecal.killRadiusDecal(
        registry, lifecycle, object, confirmedTick, object_simulation_detail::state(*this).m_radiusDecalEvents);
}

bool ObjectSimulationAbilityDomain::setParticleUplinkDestination(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, const LogicFixedVec3& destination,
    uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_particleUplinkCannon.setOverridableDestination(
        registry, lifecycle, object, destination, confirmedTick);
}

bool ObjectSimulationAbilityDomain::setParticleUplinkWaypoint(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const game::terrain::TerrainLogic& terrain, ObjectId object,
    SpecialPowerContentId specialPower, uint32_t waypointId,
    uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_particleUplinkCannon.setWaypointDestination(
        registry, lifecycle, terrain, object, specialPower,
        waypointId, confirmedTick);
}

bool ObjectSimulationAbilityDomain::setMinefieldTarget(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, const LogicFixedVec3* target) const {
    return object_simulation_detail::state(*this).m_minefield.setGeneratorTarget(registry, lifecycle, object, target);
}

bool ObjectSimulationAbilityDomain::configureMineScoot(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId mine, const LogicFixedVec3& start,
    const LogicFixedVec3& target, uint64_t confirmedTick) const {
    return object_simulation_detail::state(*this).m_minefield.configureMineScoot(
        registry, lifecycle, mine, start, target, object_simulation_detail::state(*this).m_rules, confirmedTick);
}

bool ObjectSimulationAbilityDomain::setDemoTrapMode(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId trap, bool proximityMode) const {
    return object_simulation_detail::state(*this).m_minefield.setDemoTrapMode(
        registry, lifecycle, trap, proximityMode);
}

bool ObjectSimulationAbilityDomain::triggerDemoTrap(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId trap) const {
    return object_simulation_detail::state(*this).m_minefield.triggerDemoTrap(registry, lifecycle, trap);
}

bool ObjectSimulationAbilityDomain::disarmMine(
    ecs::registry& registry, ObjectLifecycle& lifecycle,
    ObjectId mine, uint64_t confirmedTick) {
    container::Vector<ObjectDamageRequest> damage;
    if (!object_simulation_detail::state(*this).m_minefield.disarmMine(
            registry, lifecycle, mine, confirmedTick, damage)) return false;
    auto& simulation = static_cast<ObjectSimulation&>(*this);
    for (ObjectDamageRequest& request : damage) {
        simulation.queueDamage(std::move(request));
    }
    return true;
}

bool ObjectSimulationAbilityDomain::applyBridgeScaffoldMotionRequest(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectBridgeScaffoldMotionRequest& request) const {
    return object_simulation_detail::state(*this).m_bridge.applyScaffoldMotionRequest(registry, lifecycle, request);
}

bool ObjectSimulationConstructionDomain::acknowledgeRailroadCarriageSpawn(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectRailroadCarriageSpawnRequest& request,
    ObjectId spawnedCarriage, bool accepted) const {
    return object_simulation_detail::state(*this).m_bridge.acknowledgeCarriageSpawn(
        registry, lifecycle, request, spawnedCarriage, accepted);
}

bool ObjectSimulationConstructionDomain::acknowledgeSpawnSlave(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    const ObjectSpawnSlaveRequest& request, ObjectId spawned,
    bool accepted) const {
    return object_simulation_detail::state(*this).m_spawnSlave.acknowledgeSpawn(
        registry, lifecycle, request, spawned, accepted);
}

bool ObjectSimulationConstructionDomain::bindOclSlaveMaster(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId object, ObjectId master) const noexcept {
    return object_simulation_detail::state(*this).m_spawnSlave.bindOclSlaveMaster(
        registry, lifecycle, object, master);
}

ObjectId ObjectSimulationConstructionDomain::closestSpawnChild(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId spawner, const LogicFixedVec3& position) const noexcept {
    return object_simulation_detail::state(*this).m_spawnSlave.closestSpawnChild(
        registry, lifecycle, spawner, position);
}

container::Vector<ObjectId> ObjectSimulationConstructionDomain::spawnChildren(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId spawner) const {
    return object_simulation_detail::state(*this).m_spawnSlave.spawnChildren(registry, lifecycle, spawner);
}

bool ObjectSimulationAbilityDomain::maySpawnSelfTaskAI(
    const ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId spawner, uint32_t ruleIndex,
    math::q32_32 maximumSelfTaskersRatio) const noexcept {
    return object_simulation_detail::state(*this).m_spawnSlave.maySpawnSelfTaskAI(
        registry, lifecycle, spawner, ruleIndex,
        maximumSelfTaskersRatio);
}

bool ObjectSimulationConstructionDomain::setSpawnChildSelfTasking(
    ecs::registry& registry, const ObjectLifecycle& lifecycle,
    ObjectId spawner, uint32_t ruleIndex, ObjectId child,
    bool selfTasking) const noexcept {
    return object_simulation_detail::state(*this).m_spawnSlave.setSpawnChildSelfTasking(
        registry, lifecycle, spawner, ruleIndex, child, selfTasking);
}


} // namespace engine
