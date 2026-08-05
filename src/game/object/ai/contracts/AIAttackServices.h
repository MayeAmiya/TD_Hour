#pragma once

#include <cstddef>
#include <cstdint>

#include "core/container/container_types.h"
#include "game/object/ai/contracts/AIStateCommands.h"
#include "game/object/ai/runtime/AIStateTypes.h"

namespace engine::ai
{

enum class AIAttackPhase : uint8_t
{
    Inactive,
    Aim,
    Fire,
    Chase,
    Approach,
};

struct AIAttackPolicy final
{
    bool attacksObject = false;
    bool forceAttack = false;
    bool follow = false;
    bool valid = false;
};

struct AIAttackSoAColumns final
{
    container::Span<uint64_t> requestTick;
    container::Span<uint32_t> requestSequence;
    container::Span<AIAttackPhase> phase;
    container::Span<uint32_t> phaseRevision;
    container::Span<uint64_t> weaponRevision;
    container::Span<uint64_t> sourceOrderRevision;
    container::Span<uint32_t> pathGeneration;
    container::Span<uint64_t> pathHandle;
    container::Span<ObjectId> trackedTarget;
    container::Span<int64_t> targetXRaw;
    container::Span<int64_t> targetYRaw;
    container::Span<int64_t> targetZRaw;
    container::Span<int64_t> arrivalRadiusRaw;
    container::Span<int64_t> minimumArrivalRadiusRaw;
    container::Span<uint8_t> pathRequestIssued;
    container::Span<uint8_t> movementActive;
    container::Span<uint8_t> aimingActive;
    container::Span<uint8_t> firingActive;
    container::Span<uint8_t> fireCommandIssued;
    container::Span<uint8_t> contactWeapon;
};

[[nodiscard]] constexpr AIAttackPolicy attackPolicyFor(AIStateId state) noexcept
{
    switch (state)
    {
    case AIStateId::AttackPosition:
        return {.attacksObject = false, .forceAttack = false, .follow = false, .valid = true};
    case AIStateId::AttackObject:
        return {.attacksObject = true, .forceAttack = false, .follow = false, .valid = true};
    case AIStateId::ForceAttackObject:
        return {.attacksObject = true, .forceAttack = true, .follow = false, .valid = true};
    case AIStateId::AttackAndFollowObject:
        return {.attacksObject = true, .forceAttack = false, .follow = true, .valid = true};
    default:
        return {};
    }
}

// Combat feedback is correlated to both the activation and the selected
// weapon. phaseRevision additionally prevents a late Aim answer from being
// consumed after the same weapon has already advanced through Fire or Chase.
struct AIAttackCorrelation final
{
    ObjectId subject = INVALID_OBJECT_ID;
    AIStateRequestId stateRequest{};
    AIStateId state = AIStateId::Invalid;
    AIAttackPhase phase = AIAttackPhase::Inactive;
    uint64_t weaponRevision = 0;
    uint32_t phaseRevision = 0;
    AIAsyncOrderIdentity orderIdentity;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return subject && stateRequest.isValid() && attackPolicyFor(state).valid && phase != AIAttackPhase::Inactive &&
               weaponRevision != 0 && phaseRevision != 0;
    }
    constexpr bool operator==(const AIAttackCorrelation&) const noexcept = default;
};

enum class AIAttackFeedbackKind : uint8_t
{
    None,
    Snapshot,
    FireCompleted,
    Unsupported,
};

// Snapshot fields are resolved by the combat authority for one confirmed
// tick. The kernel never dereferences Object or Weapon and never guesses a
// weapon revision from an object identity.
struct AIAttackFeedback final
{
    AIAttackCorrelation correlation{};
    AIAttackFeedbackKind kind = AIAttackFeedbackKind::None;
    ObjectId target = INVALID_OBJECT_ID;
    AIFixedPosition targetPosition{};
    uint64_t confirmedTick = 0;
    bool targetValid = false;
    bool targetEffectivelyDead = false;
    // Combat-owned AttackMove child reached the MaxShotsToFire cap. This is
    // distinct from targetEffectivelyDead so position attacks can terminate
    // successfully without pretending their ground target was destroyed.
    bool shotLimitReached = false;
    bool targetMobile = false;
    bool hasWeapon = false;
    bool attackAllowed = false;
    bool canPossiblyAttack = false;
    bool inRange = false;
    bool viewBlocked = false;
    bool wantToSquishTarget = false;
    bool aimReady = false;
    bool aimTemporarilyPrevented = false;
    bool weaponPreAttack = false;
    bool weaponReady = false;
    bool weaponSlotAllowed = false;
    bool contactWeapon = false;
    bool canPursue = false;
    bool chaseAllowed = false;
    int64_t attackArrivalRadiusRaw = 0;
    int64_t attackMinimumArrivalRadiusRaw = 0;
};

template <typename Value, size_t CapacityValue = 4>
struct AIAttackValueBuffer final
{
    static_assert(CapacityValue > 0);

    container::Array<Value, CapacityValue> values{};
    size_t count = 0;
    bool overflowed = false;

    [[nodiscard]] constexpr bool hasCapacity(size_t additional = 1) const noexcept
    {
        return count <= values.size() && additional <= values.size() - count;
    }

    [[nodiscard]] bool push(const Value& value) noexcept
    {
        if (!hasCapacity())
        {
            overflowed = true;
            return false;
        }
        values[count++] = value;
        return true;
    }

    void clear() noexcept
    {
        count = 0;
        overflowed = false;
    }
};

using AIAttackFeedbackBuffer = AIAttackValueBuffer<AIAttackFeedback>;

enum class AIAttackCommandKind : uint8_t
{
    BeginAim,
    EndAim,
    BeginFire,
    Fire,
    EndFire,
};

struct AIAttackCommand final
{
    AIAttackCorrelation correlation{};
    AIAttackCommandKind kind = AIAttackCommandKind::BeginAim;
    ObjectId target = INVALID_OBJECT_ID;
    AIFixedPosition targetPosition{};
    bool attacksObject = false;
    bool forceAttack = false;
    uint64_t confirmedTick = 0;
};

using AIAttackCommandBuffer = AIAttackValueBuffer<AIAttackCommand, 8>;

// Runtime-owned terminal handoff. Combat feedback drives the Attack state to
// a terminal transition; this value preserves the exact state activation and
// order identity until GameSession can atomically validate and remove only
// the matching queue head.
struct AIAttackOrderCompletion final
{
    AIAttackCorrelation correlation{};
    AIStateOutcome outcome = AIStateOutcome::Continue;
    uint64_t confirmedTick = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return correlation.isValid() &&
               correlation.orderIdentity.isValid();
    }
};

// Borrowed for one executor call. Both sides retain ownership of their
// bounded buffers; state storage retains only scalar correlation fields.
struct AIAttackServicePorts final
{
    AIAttackCommandBuffer* commands = nullptr;
    const AIAttackFeedbackBuffer* feedback = nullptr;
};

} // namespace engine::ai
