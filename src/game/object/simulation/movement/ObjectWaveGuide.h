#pragma once

#include "core/container/container_types.h"
#include "core/ecs/registry.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/combat/ObjectFireUpdates.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

#include "game/object/plan/movement/ObjectWaveGuidePlanTypes.h"
namespace engine {
class GameContentSnapshot;
class ObjectLifecycle;
class SimulationRandom;
struct ObjectDamageRequest;
struct ObjectSimulationRules;

enum class ObjectWaveGuideEventKind : uint8_t {
    Started,
    FrontSpray,
    FrontAdvanced,
    ShoreSplashLeft,
    ShoreSplashRight,
    ObjectHit,
    BridgeHit,
    RandomSplashSound,
    Finished,
    InvalidPath,
};

// Detached gameplay/presentation fact.  Particle/audio consumers receive
// names and transforms by value; no particle, renderer, waypoint or ECS
// handle survives the confirmed-frame boundary.
struct ObjectWaveGuideEvent final {
    ObjectWaveGuideEventKind kind = ObjectWaveGuideEventKind::Started;
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    container::String effect;
    LogicFixedVec3 position{};
    math::q32_32 rotationRadians{};
    math::q32_32 targetRotationRadians{};
    // Nonzero only for the persistent WaveSpray attachment set. FrontSpray
    // starts one attached particle; Finished/InvalidPath stops the group.
    uint64_t attachmentGroup = 0;
    uint64_t terrainSourceRecordIndex = UINT64_MAX;
    math::q32_32 ySize{};
    math::q32_32 bendMagnitude{};
    math::q32_32 damageRadius{};
    math::q32_32 toppleForce{};
    math::q32_32 preferredHeight{};
    uint32_t authoredOrder = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
};

// Authoritative bridge-impact payload extracted before presentation consumes
// the matching wave particle event.  Keeping this as a distinct value type
// prevents renderer/audio publication from owning terrain or lifecycle work.
struct ObjectWaveGuideBridgeImpact final {
    ObjectId source = INVALID_OBJECT_ID;
    ObjectId target = INVALID_OBJECT_ID;
    LogicFixedVec3 position{};
    math::q32_32 targetRotationRadians{};
    uint64_t terrainSourceRecordIndex = UINT64_MAX;
    container::String bridgeParticle;
    math::q32_32 bridgeParticleRotationRadians{};
    uint32_t authoredOrder = 0;
    uint64_t emissionSequence = 0;
    uint64_t confirmedTick = 0;
};

struct ObjectWaveGuideRuntime final {
    container::Vector<LogicFixedVec3> path;
    LogicFixedVec3 fixedPosition{};
    math::q32_32 movementSpeedUnitsPerSecond{};
    math::q32_32 yawRadians{};
    uint64_t activatedTick = 0;
    uint64_t delayTicks = 0;
    uint64_t nextSplashTick = 0;
    uint64_t lastUpdateTick = UINT64_MAX;
    uint32_t segmentIndex = 0;
    uint32_t sourceSequence = 1;
    bool defaultDisableInstalled = false;
    bool activationObserved = false;
    bool initialized = false;
    bool loopingAudioActive = false;
};

struct ObjectWaveGuideComponent final {
    container::SharedPtr<const game::ObjectWaveGuidePlan> plan;
    container::Vector<ObjectWaveGuideRuntime> instances;
};

class ObjectWaveGuideSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const GameContentSnapshot& content,
                          const ObjectSimulationRules& rules,
                          uint64_t confirmedTick) const;

    void update(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        game::terrain::TerrainLogic& terrain,
        SimulationRandom& random, const ObjectSimulationRules& rules,
        uint64_t confirmedTick, uint64_t& nextEmissionSequence,
        container::Vector<ObjectDamageRequest>& outDamage,
        container::Vector<ObjectWaveGuideBridgeImpact>& outBridgeImpacts,
        container::Vector<ObjectWaveGuideEvent>& outEvents,
        container::Vector<ObjectFireAudioCommand>& outAudio) const;

    void onObjectReclaim(
        ecs::registry& registry, const ObjectLifecycle& lifecycle,
        ObjectId object, uint64_t confirmedTick,
        uint64_t& nextEmissionSequence,
        container::Vector<ObjectWaveGuideEvent>& outEvents,
        container::Vector<ObjectFireAudioCommand>& outAudio) const;
};
} // namespace engine
