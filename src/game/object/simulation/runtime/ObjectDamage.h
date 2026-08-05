#pragma once

#include "game/base/DamageTypes.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

#include <cstdint>
#include <optional>

namespace engine {

enum class ObjectBodyDamageState : uint8_t;

// A same-tick ordering barrier for rare compound transactions. Ordinary
// requests remain in the original source/sequence/ObjectId order. A
// DumbProjectile detonation, however, must finish its warhead impact before
// its own DetonateCallsKill Body hit is allowed to run; otherwise INVALID_ID
// would sort the self-kill ahead of the launcher-credited impact.
enum class ObjectDamageResolutionPhase : uint8_t {
    Standard,
    PostDetonationSelfKill,
    // Physics landing first applies its authored FALLING transaction; an
    // optional KillWhenRestingOnGround request is deliberately later so it
    // cannot hide the impact/DeathType from the Body pipeline.
    PostPhysicsRestKill,
};

// A value-only health transaction.  Scripts, weapons and future hazards all
// submit this same payload; ObjectSimulation remains the only code allowed to
// mutate Body health or request structural destruction.
struct ObjectDamageRequest final {
    ObjectId target = INVALID_OBJECT_ID;
    ObjectId source = INVALID_OBJECT_ID;
    // A producer-local monotonic ordinal (ScriptEffect ordinal, weapon shot
    // sequence, etc.). It participates in the canonical same-tick ordering;
    // zero remains valid for legacy/direct callers.
    uint32_t sourceSequence = 0;
    // Optional owner-thread admission ordinal. Producers that emit several
    // transaction kinds from one natural callback reserve this at emission
    // time so a later adapter never compares unrelated private counters.
    uint64_t submissionOrdinal = 0;
    // Nonzero groups coupled requests that originate from one atomic impact.
    // A DumbProjectile warhead and its DetonateCallsKill Body request share
    // the projectile ObjectId, so they resolve consecutively even when other
    // projectiles detonate during the same confirmed frame.
    ObjectId causalGroup = INVALID_OBJECT_ID;
    // Authored map/INI/config values are quantized before they enter this
    // transaction. Damage remains Q32.32 throughout the authoritative
    // runtime, command, snapshot and future network/replay boundaries.
    math::q32_32 amount{};
    game::DamageType damageType = game::DamageType::EXPLOSION;
    // DAMAGE_STATUS carries one sanitized modern ObjectStatus bit. Keeping
    // it on the immutable transaction mirrors DamageInfoInput without making
    // Body retain a WeaponTemplate pointer.
    uint64_t damageStatusMask = 0;
    // Body/armor uses damageType. Presentation may deliberately retain a
    // different legacy DamageFX classification (PoisonedBehavior repeats its
    // hit as UNRESISTABLE to avoid reinfection, but still asks for POISON FX).
    std::optional<game::DamageType> damageFxOverride;
    game::DeathType deathType = game::DeathType::NORMAL;
    // Weapon shockwave metadata travels with the same immutable damage
    // transaction because RefCode applies Body first, then physics to that
    // exact victim. Zero amount/radius means no physics side effect.
    math::q32_32 shockWaveAmount{};
    math::q32_32 shockWaveRadius{};
    math::q32_32 shockWaveTaperOff{};
    math::q32_32 shockWaveVectorX{};
    math::q32_32 shockWaveVectorY{};
    math::q32_32 shockWaveVectorZ{};
    ObjectDamageResolutionPhase resolutionPhase = ObjectDamageResolutionPhase::Standard;
    bool forceKill = false;
    // RefCode's first crusher overlap submits a zero-amount CRUSH hit solely
    // to run Body/DamageFX feedback before the real crush transition. Normal
    // zero requests remain ignored; only this explicit value transaction may
    // publish a Damaged event with unchanged health.
    bool emitZeroDamageFeedback = false;
    uint64_t confirmedTick = 0;
};

// Adapter record used while producers still submit through
// ObjectSimulation::queueDamage. The ordinal already belongs to the common
// owner-thread admission clock; GameSession preserves that order while
// assigning the stored GameplayEnvelope token its own lifetime ordinal.
struct ObjectDamageTransactionIngress final {
    ObjectDamageRequest request;
    uint64_t submissionOrdinal = 0;
};

// Low-level ActiveBody::setDamageState value projection. Containment and any
// future producer may request a Body state by stable ObjectId, but only
// ObjectSimulation's Body authority is allowed to mutate Health. Applying
// this record deliberately does not manufacture DamageInfo, damage-module
// callbacks, retaliation, score, damage FX/audio or a death transaction.
struct ObjectBodyStateProjection final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId source = INVALID_OBJECT_ID;
    ObjectBodyDamageState state{};
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

// Low-level ActiveBody::setInitialHealth/internalChangeHealth projection.
// It changes the authoritative Body value and derived state without creating
// a damage/healing event. Aggregate-health controllers and map/OCL importers
// use this contract instead of writing ObjectHealthComponent directly.
struct ObjectBodyHealthProjection final {
    ObjectId object = INVALID_OBJECT_ID;
    ObjectId source = INVALID_OBJECT_ID;
    math::q32_32 desiredHealth{};
    uint32_t authoredOrder = 0;
    uint64_t confirmedTick = 0;
};

} // namespace engine
