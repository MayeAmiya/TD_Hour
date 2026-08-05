#pragma once

#include "core/ecs/ObjectId.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/session/object/GameSessionObjectContracts.h"
#include "game/session/transaction/GameSessionTransactionPorts.h"

#include <cstddef>
#include <cstdint>

namespace engine {

class GameSessionContentStartState;
class GameSessionWorldState;
class GameSessionScriptPresentationState;
class GameSessionObjectEventState;

// Authoritative object create/destroy path shared by script AuthorityPort,
// weapons, production, map import and containment. Post-create payload spawn
// and Die/DeleteWalk barriers still run through the Session sink so nested
// membership cascades stay singular.
class GameSessionObjectLifecycleTransactions final {
public:
    GameSessionObjectLifecycleTransactions(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionLifecycleTransactionPort barrier,
        GameSessionObjectEventState* objectEvents = nullptr) noexcept;

    [[nodiscard]] GameSessionObjectSpawnResult spawnObject(
        ObjectSpawnRequest request);
    [[nodiscard]] bool requestDestroyObject(
        ObjectId id, ObjectDestroyReason reason, uint64_t confirmedTick);
    [[nodiscard]] bool destroyObject(ObjectId id);
    [[nodiscard]] bool completeConstruction(
        ObjectId id, uint64_t confirmedTick);
    // A newly committed construction footprint may legally cover movable
    // non-enemies. Give those actors a new-goal evacuation order before Physics
    // sees the new structure, instead of relying on penetration separation to
    // eject them from the footprint.
    [[nodiscard]] size_t evacuateConstructionFootprint(
        ObjectId structure, ObjectId builder, uint64_t confirmedTick);
    [[nodiscard]] size_t flushPending();
    void resolveQueuedObjectDamage() {
        m_barrier.resolveQueuedObjectDamage();
    }
    [[nodiscard]] GameSessionLifecycleTransactionPort barrier() const noexcept {
        return m_barrier;
    }

private:
    void refreshDerivedAggregates(uint64_t confirmedTick);
    void spawnInitialContainmentPayloads(
        ObjectId host, uint32_t initialPathfindLayer,
        uint64_t confirmedTick);

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionLifecycleTransactionPort m_barrier;
    GameSessionObjectEventState* m_objectEvents = nullptr;
};

} // namespace engine
