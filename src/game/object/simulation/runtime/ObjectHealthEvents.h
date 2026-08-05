#pragma once

#include "core/container/container_types.h"
#include "game/base/DamageTypes.h"
#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "core/ecs/ObjectId.h"

#include <cstdint>

namespace engine {

enum class ObjectHealthEventKind : uint8_t {
    Damaged,
    Healed,
    DamageStateChanged,
    SecondLifeStarted,
    SubdualDamaged,
    SubdualRecovered,
    SubdualStateChanged,
    StatusApplied,
    SpecialDamageApplied,
    Died,
    Ignored,
};

enum class ObjectHealthScoreKind : uint8_t {
    None,
    Unit,
    Building,
};

struct ObjectHealthEvent final {
    ObjectHealthEventKind kind = ObjectHealthEventKind::Ignored;
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId source = INVALID_OBJECT_ID;
    game::DamageType damageType = game::DamageType::EXPLOSION;
    game::DamageType damageFxType = game::DamageType::EXPLOSION;
    game::DamageType bodyLastDamageType = game::DamageType::EXPLOSION;
    // Non-empty only when this committed hit passed the victim-owned
    // DamageFX throttle and selected a frozen Minor/Major FXList. It is a
    // value event; simulation never invokes renderer or audio services.
    container::String damageFxListName;
    // Frozen one-shot AudioEvent names selected by the confirmed Body
    // transaction. Session publication converts them to typed GameAudioEvent
    // values; the simulation never calls an audio service directly.
    container::String damageStateAudioEventName;
    container::String voiceFearAudioEventName;
    game::DeathType deathType = game::DeathType::NORMAL;
    ObjectHealthComponent::Scalar requestedAmount{};
    ObjectHealthComponent::Scalar appliedAmountFixed{};
    ObjectHealthComponent::Scalar actualDamageDealtFixed{};
    ObjectHealthComponent::Scalar previousHealth{};
    ObjectHealthComponent::Scalar currentHealth{};
    ObjectHealthComponent::Scalar previousSubdualDamage{};
    ObjectHealthComponent::Scalar currentSubdualDamage{};
    bool wasSubdued = false;
    bool isSubdued = false;
    ObjectBodyDamageState previousState = ObjectBodyDamageState::Pristine;
    ObjectBodyDamageState currentState = ObjectBodyDamageState::Pristine;
    uint64_t confirmedTick = 0;
    // Internal Body ownership stamp. Re-entrant onDie children append to the
    // same lossless journal, so a parent resolver must not consume a nested
    // transaction's event suffix a second time. Zero means not yet claimed by
    // its producing Body transaction.
    uint64_t bodyTransactionOrdinal = 0;
    PlayerId sourcePlayer = INVALID_PLAYER_ID;
    PlayerId victimPlayer = INVALID_PLAYER_ID;
    container::String victimTemplateName;
    ObjectHealthScoreKind scoreKind = ObjectHealthScoreKind::None;
    // ActiveBody is the only generic Body that calls damager->scoreTheKill.
    // InactiveBody's UNRESISTABLE onDie path must not record either victim
    // loss or killer destruction through that API.
    bool scoreTheKillPath = false;
    bool sourceObjectPresent = false;
    bool victimPlayableSide = false;
    bool victimIgnoredInGui = false;
    bool victimUnderConstruction = false;
    bool sourceIsEnemy = false;
    // Exact Object::onDie advisor classification frozen before authored Die
    // callbacks can mutate KindOf/ownership. These are independent of SCORE:
    // ZH uses STRUCTURE+MP_COUNT_FOR_VICTORY for BuildingLost and any
    // INFANTRY/VEHICLE for UnitLost/fake-radar feedback.
    bool victimEvaBuilding = false;
    bool victimEvaUnit = false;
    // Frozen authoritative facts for simulation consumers such as guard
    // retaliation. Presentation consumers project these values to float only
    // at the FX/audio boundary.
    LogicFixedVec3 victimPositionFixed{};
    math::q32_32 victimBoundingCircleRadiusFixed{};
    math::q32_32 victimBoundingSphereRadiusFixed{};
    LogicFixedVec3 sourcePositionFixed{};
    math::q32_32 sourceBoundingSphereRadiusFixed{};
    bool sourceAirborne = false;
    bool victimDrone = false;
    bool healthDecreased = false;
};

} // namespace engine
