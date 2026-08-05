#pragma once

#include "core/ecs/ObjectId.h"

#include <cstddef>
#include <cstdint>

namespace engine::gameplay {

// Session-owned gameplay vocabulary. Payloads live in typed pools; the hot
// execution stack carries only this kind, an envelope and a pool index.
enum class GameplayTransactionKind : uint8_t {
    Weapon,
    WeaponImpact,
    Damage,
    ObjectCreationList,
    Crate,
    Replacement,
    UpgradeFx,
    StructureFx,
    TransitionOcl,
    InstantDeath,
    SlowDeath,
    SlowDeathRubble,
    TopplePathfind,
    ToppleStump,
    PhysicsCrash,
    AIMovementObstructionBatch,
    DestroyObject,
    MineSpawn,
    ParticleUplinkRemnant,
    WaveBridgeImpact,
    CheckpointNavigation,
    TensileNavigation,
    DynamicGeometry,
    Transport,
    DeathWalk,
    DeleteWalk,
    BodyResume,
    OwnershipChange,
    Defection,
    PilotVehicleTakeover,
    RailedTransportDockAttach,
    RailroadDisembark,
    RailroadCarriageSpawn,
    SpawnSlave,
    SpecialPowerSpawn,
    BridgeState,
    ConstructionCompletion,
    BridgeRepairScaffoldBatch,
    RebuildTargetRemap,
    RebuildHoleExpose,
    RebuildWorkerSpawn,
    RebuildCompletion,
    ContainmentEvent,
    VehicleNeutralization,
    CratePickupBatch,
    CountermeasureFlareSpawn,
    ProductionSpawn,
    ProductionUpgrade,
    SpecialAbilityEffect,
    SpecialPowerCompletion,
    Count,
};

struct GameplayEnvelope final {
    uint64_t confirmedTick = 0;
    uint64_t submissionOrdinal = 0;
    ObjectId producer = INVALID_OBJECT_ID;
    uint32_t authoredOrder = 0;
    uint32_t localOrdinal = 0;
};

[[nodiscard]] constexpr bool gameplayEnvelopeLess(
    const GameplayEnvelope& left,
    const GameplayEnvelope& right) noexcept {
    if (left.confirmedTick != right.confirmedTick)
        return left.confirmedTick < right.confirmedTick;
    if (left.submissionOrdinal != right.submissionOrdinal)
        return left.submissionOrdinal < right.submissionOrdinal;
    if (left.producer != right.producer)
        return left.producer < right.producer;
    if (left.authoredOrder != right.authoredOrder)
        return left.authoredOrder < right.authoredOrder;
    return left.localOrdinal < right.localOrdinal;
}

struct GameplayTransactionToken final {
    GameplayEnvelope envelope;
    GameplayTransactionKind kind = GameplayTransactionKind::Damage;
    uint32_t payloadIndex = 0;
};

static_assert(sizeof(GameplayTransactionToken) <= 40);

} // namespace engine::gameplay
