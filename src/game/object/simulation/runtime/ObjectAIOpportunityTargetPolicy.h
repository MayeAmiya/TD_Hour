#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

#include "core/container/container_types.h"
#include "game/object/ai/states/combat/AIOpportunityAttackMoveStateData.h"

namespace engine
{

// Detached, ObjectId-stable input for the production opportunity-target
// query. GameSession samples ECS into these values; the selector never keeps
// an entity, registry pointer, spatial-record pointer, or mutable service.
struct ObjectAIOpportunityTargetCandidate final
{
    ObjectId target = INVALID_OBJECT_ID;
    ai::AIFixedPosition position{};
    bool attackable = false;
    // KINDOF_UNATTACKABLE on the candidate. RefCode's acquisition filters run
    // through WeaponSet::getAbleToAttackSpecificObject, which rejects the
    // victim outright, so such objects can never be auto-acquired.
    bool unattackable = false;
    bool effectivelyDead = false;
    bool containedPassenger = false;
    bool hiddenStealth = false;
    bool sameOwner = false;
    bool allied = false;
    bool enemy = false;
    bool crate = false;
    bool rejectedByAcquirePolicy = false;
    bool rejectedByTargetability = false;
    // Session-resolved RefCode PartitionFilterInsignificantBuildings result.
    // It is enabled only for ordinary idle auto-acquisition; explicit,
    // guard, squad and script target queries do not inherit that filter.
    bool ignoredInsignificantBuilding = false;
    // RefCode treats priority zero as an explicit "never acquire" rule.
    // GameSession replaces the fallback with the session-frozen AIData value.
    int32_t attackPriority = 1;
    int64_t attackPriorityDistanceModifierRaw = int64_t{100} << 32;
    int64_t maximumAcquisitionDistanceRaw =
        std::numeric_limits<int64_t>::max();
};

struct ObjectAIOpportunityTargetSelection final
{
    ObjectId target = INVALID_OBJECT_ID;
    uint64_t distanceSquared = std::numeric_limits<uint64_t>::max();
    int64_t effectiveAttackPriority = std::numeric_limits<int64_t>::min();
    int32_t rawAttackPriority = std::numeric_limits<int32_t>::min();
};

namespace object_ai_opportunity_detail
{

[[nodiscard]] constexpr uint64_t unsignedDistance(
    int64_t left, int64_t right) noexcept
{
    return left >= right
        ? static_cast<uint64_t>(left) - static_cast<uint64_t>(right)
        : static_cast<uint64_t>(right) - static_cast<uint64_t>(left);
}

[[nodiscard]] constexpr uint64_t saturatingSquare(uint64_t value) noexcept
{
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
    return value != 0 && value > maximum / value ? maximum : value * value;
}

[[nodiscard]] constexpr uint64_t saturatingAdd(
    uint64_t left, uint64_t right) noexcept
{
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
}

[[nodiscard]] constexpr uint64_t distanceSquared(
    const ai::AIFixedPosition& left,
    const ai::AIFixedPosition& right) noexcept
{
    return saturatingAdd(
        saturatingSquare(unsignedDistance(left.xRaw, right.xRaw)),
        saturatingSquare(unsignedDistance(left.yRaw, right.yRaw)));
}

[[nodiscard]] constexpr uint64_t integerSquareRoot(uint64_t value) noexcept
{
    // Restoring binary square root avoids platform libm differences in the
    // confirmed target ranker. The result is floor(sqrt(value)).
    uint64_t result = 0;
    uint64_t bit = uint64_t{1} << 62;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

} // namespace object_ai_opportunity_detail

inline void considerObjectAIOpportunityTarget(
    ai::AIOpportunityAttackMoveQueryCommandKind query,
    const ai::AIFixedPosition& subjectPosition,
    const ObjectAIOpportunityTargetCandidate& candidate,
    ObjectAIOpportunityTargetSelection& selection) noexcept
{
    const bool requestedKind =
        query == ai::AIOpportunityAttackMoveQueryCommandKind::FindCrate
            ? candidate.crate
            : query ==
                  ai::AIOpportunityAttackMoveQueryCommandKind::FindMoodTarget
                ? !candidate.crate
                : false;
    // A KINDOF_UNATTACKABLE candidate is never a weapon target, but it may
    // still be a legal crate pickup: RefCode's crate path never consults
    // WeaponSet::getAbleToAttackSpecificObject.
    const bool usable =
        query == ai::AIOpportunityAttackMoveQueryCommandKind::FindCrate
            ? candidate.crate
            : candidate.attackable && !candidate.unattackable &&
                candidate.enemy;
    if (!candidate.target || !requestedKind || !usable ||
        candidate.attackPriority == 0 ||
        candidate.rejectedByAcquirePolicy ||
        candidate.rejectedByTargetability ||
        candidate.effectivelyDead || candidate.containedPassenger ||
        candidate.hiddenStealth || candidate.sameOwner || candidate.allied)
    {
        return;
    }
    if (query ==
            ai::AIOpportunityAttackMoveQueryCommandKind::FindMoodTarget &&
        candidate.ignoredInsignificantBuilding) {
        return;
    }
    const uint64_t distance =
        object_ai_opportunity_detail::distanceSquared(
            subjectPosition, candidate.position);
    const uint64_t distanceRaw =
        object_ai_opportunity_detail::integerSquareRoot(distance);
    if (candidate.maximumAcquisitionDistanceRaw >= 0 &&
        distanceRaw > static_cast<uint64_t>(
            candidate.maximumAcquisitionDistanceRaw)) {
        return;
    }
    const uint64_t modifierRaw = candidate.attackPriorityDistanceModifierRaw > 0
        ? static_cast<uint64_t>(
              candidate.attackPriorityDistanceModifierRaw)
        : 0;
    const int64_t penalty = modifierRaw != 0
        ? static_cast<int64_t>(distanceRaw / modifierRaw)
        : 0;
    const int64_t effectivePriority = std::max<int64_t>(
        1, static_cast<int64_t>(candidate.attackPriority) - penalty);
    if (!selection.target ||
        effectivePriority > selection.effectiveAttackPriority ||
        (effectivePriority == selection.effectiveAttackPriority &&
         candidate.attackPriority > selection.rawAttackPriority) ||
        (effectivePriority == selection.effectiveAttackPriority &&
         candidate.attackPriority == selection.rawAttackPriority &&
         distance < selection.distanceSquared) ||
        (effectivePriority == selection.effectiveAttackPriority &&
         candidate.attackPriority == selection.rawAttackPriority &&
         distance == selection.distanceSquared &&
         candidate.target < selection.target))
    {
        selection.target = candidate.target;
        selection.distanceSquared = distance;
        selection.effectiveAttackPriority = effectivePriority;
        selection.rawAttackPriority = candidate.attackPriority;
    }
}

// Select nearest eligible target, breaking equal-distance ties by ObjectId.
// Candidate input order therefore cannot leak ECS/storage iteration order
// into confirmed AI decisions.
[[nodiscard]] inline ObjectId selectObjectAIOpportunityTarget(
    ai::AIOpportunityAttackMoveQueryCommandKind query,
    const ai::AIFixedPosition& subjectPosition,
    container::Span<const ObjectAIOpportunityTargetCandidate> candidates)
    noexcept
{
    ObjectAIOpportunityTargetSelection selection;
    for (const ObjectAIOpportunityTargetCandidate& candidate : candidates)
    {
        considerObjectAIOpportunityTarget(
            query, subjectPosition, candidate, selection);
    }
    return selection.target;
}

} // namespace engine
