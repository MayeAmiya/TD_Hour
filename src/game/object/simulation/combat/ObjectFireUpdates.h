#pragma once

#include "core/container/container_types.h"

#include "core/ecs/registry.h"
#include "game/object/definition/CombatProfile.h"
#include "game/object/creation/ObjectCreationListRuntime.h"
#include "game/object/simulation/combat/ObjectFireWeaponBehavior.h"
#include "game/object/spatial/ObjectSpatialIndex.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include "game/object/plan/combat/ObjectFireUpdatesPlanTypes.h"
namespace engine {

class GameContentSnapshot;
class ObjectLifecycle;
class PlayerRegistry;
class SimulationRandom;
struct ObjectDamageRequest;
struct ObjectHealthEvent;

enum class ObjectFlammabilityState : uint8_t {
    Normal,
    Aflame,
    Burned,
};

struct ObjectFlammableRuntime final {
    math::q32_32 remainingFlameDamage{};
    ObjectId flameSource = INVALID_OBJECT_ID;
    uint64_t aflameEndTick = 0;
    uint64_t burnedTick = 0;
    uint64_t nextAflameDamageTick = 0;
    uint64_t lastFlameDamageTick = 0;
    ObjectFlammabilityState state = ObjectFlammabilityState::Normal;
    bool hasReceivedFlameDamage = false;
};

struct ObjectFlammableComponent final {
    container::SharedPtr<const game::ObjectFlammablePlan> plan;
    container::Vector<ObjectFlammableRuntime> instances;
};

struct ObjectFireSpreadRuntime final {
    game::ObjectCreationListContentId embersContent;
    uint64_t nextSpreadTick = 0;
    bool armed = false;
};

struct ObjectFireSpreadComponent final {
    container::SharedPtr<const game::ObjectFireSpreadPlan> plan;
    container::Vector<ObjectFireSpreadRuntime> instances;
};

struct ObjectFireOclAfterCooldownRuntime final {
    game::ObjectCreationListContentId content;
    uint64_t startTick = 0;
    uint32_t consecutiveShots = 0;
    uint32_t lastObservedShotSequence = 0;
    bool valid = false;
    bool upgradeActivated = false;
};

struct ObjectFireOclAfterCooldownComponent final {
    container::SharedPtr<const game::ObjectFireOclAfterCooldownPlan> plan;
    container::Vector<ObjectFireOclAfterCooldownRuntime> instances;
};

enum class ObjectFireAudioCommandKind : uint8_t {
    StartLoop,
    StopLoop,
};

struct ObjectFireAudioCommand final {
    ObjectFireAudioCommandKind kind = ObjectFireAudioCommandKind::StartLoop;
    ObjectId object = INVALID_OBJECT_ID;
    container::String eventName;
    // Nonzero addresses a simulation-owned presentation emitter rather than
    // the source Object root. ParticleUplink uses its stable orbital beam
    // identity so the annihilation loop follows the moving ground target.
    uint64_t emitterKeyOverride = 0;
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

class ObjectFireUpdateSystem final {
public:
    void initializeObject(ecs::registry& registry, ecs::entity entity,
                          const GameContentSnapshot& content) const;

    void onHealthEvent(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const ObjectHealthEvent& event, SimulationRandom* random,
        uint32_t logicFramesPerSecond,
        container::Vector<ObjectFireAudioCommand>& outAudio) const;

    void updateFlammable(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        uint32_t logicFramesPerSecond, uint64_t confirmedTick,
        container::Vector<ObjectDamageRequest>& outDamage,
        container::Vector<ObjectFireAudioCommand>& outAudio) const;

    void updateSpread(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const GameContentSnapshot& content, SimulationRandom& random,
        uint32_t logicFramesPerSecond, uint64_t confirmedTick,
        uint64_t& nextEmissionSequence,
        container::Vector<ObjectCreationListInvocation>& outInvocations,
        container::Vector<ObjectFireAudioCommand>& outAudio);

    void updateOclAfterCooldown(
        ecs::registry& registry, ObjectLifecycle& lifecycle,
        const PlayerRegistry& players, const GameContentSnapshot& content,
        uint32_t logicFramesPerSecond, uint64_t confirmedTick,
        uint64_t& nextEmissionSequence,
        container::Vector<ObjectCreationListInvocation>& outInvocations) const;

private:
    ObjectSpatialIndex m_currentPositionIndex;
};

} // namespace engine
