#include "game/object/simulation/status/ObjectBodyRuntime.h"

#include "game/object/definition/ModelConditionState.h"
#include "game/object/runtime/ObjectStatus.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace engine
{
namespace
{

using HealthScalar = ObjectHealthComponent::Scalar;

const HealthScalar kHealthZero{};

[[nodiscard]] HealthScalar dividePositiveSaturated(
    HealthScalar numerator, HealthScalar denominator) noexcept
{
    if (numerator <= kHealthZero || denominator <= kHealthZero)
        return kHealthZero;
    const uint64_t a = static_cast<uint64_t>(numerator.raw());
    const uint64_t b = static_cast<uint64_t>(denominator.raw());
    constexpr uint64_t kMaximum =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
#if defined(_MSC_VER) && defined(_M_X64)
    const uint64_t shiftedHigh = a >> 32;
    const uint64_t shiftedLow = a << 32;
    uint64_t thresholdHigh = 0;
    const uint64_t thresholdLow = _umul128(kMaximum, b, &thresholdHigh);
    if (shiftedHigh > thresholdHigh ||
        (shiftedHigh == thresholdHigh && shiftedLow > thresholdLow))
    {
        return HealthScalar::from_raw(std::numeric_limits<int64_t>::max());
    }
    uint64_t remainder = 0;
    return HealthScalar::from_raw(static_cast<int64_t>(
        _udiv128(shiftedHigh, shiftedLow, b, &remainder)));
#else
    const unsigned __int128 shifted =
        static_cast<unsigned __int128>(a) << 32;
    const unsigned __int128 threshold =
        static_cast<unsigned __int128>(kMaximum) * b;
    if (shifted > threshold)
        return HealthScalar::from_raw(std::numeric_limits<int64_t>::max());
    return HealthScalar::from_raw(static_cast<int64_t>(shifted / b));
#endif
}

[[nodiscard]] HealthScalar multiplyPositiveSaturated(
    HealthScalar left, HealthScalar right) noexcept
{
    if (left <= kHealthZero || right <= kHealthZero)
        return kHealthZero;
    const uint64_t a = static_cast<uint64_t>(left.raw());
    const uint64_t b = static_cast<uint64_t>(right.raw());
#if defined(_MSC_VER) && defined(_M_X64)
    uint64_t high = 0;
    const uint64_t low = _umul128(a, b, &high);
    constexpr uint64_t kThresholdHigh =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) >> 32;
    constexpr uint64_t kThresholdLow =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) << 32;
    if (high > kThresholdHigh ||
        (high == kThresholdHigh && low > kThresholdLow))
    {
        return HealthScalar::from_raw(std::numeric_limits<int64_t>::max());
    }
    return HealthScalar::from_raw(static_cast<int64_t>(
        (high << 32) | (low >> 32)));
#else
    const unsigned __int128 product =
        static_cast<unsigned __int128>(a) * b;
    const unsigned __int128 threshold =
        static_cast<unsigned __int128>(
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) << 32;
    if (product > threshold)
        return HealthScalar::from_raw(std::numeric_limits<int64_t>::max());
    return HealthScalar::from_raw(static_cast<int64_t>(product >> 32));
#endif
}

void addConditions(RenderModelComponent& visual,
                   const game::ModelConditionMask& mask) noexcept
{
    for (size_t index = 0; index < visual.modelConditionFlags.words.size(); ++index)
        visual.modelConditionFlags.words[index] |= mask.words[index];
}

} // namespace

ObjectBodyDamageState objectBodyDamageStateFor(
    HealthScalar health,
    HealthScalar maximum,
    const ObjectSimulationRules& rules) noexcept
{
    if (health <= kHealthZero || maximum <= kHealthZero)
        return ObjectBodyDamageState::Rubble;
    const HealthScalar ratio = health / maximum;
    if (ratio > rules.unitDamagedThresholdFixed)
        return ObjectBodyDamageState::Pristine;
    if (ratio > rules.unitReallyDamagedThresholdFixed)
        return ObjectBodyDamageState::Damaged;
    return ObjectBodyDamageState::ReallyDamaged;
}

void projectObjectBodyDamageVisual(ObjectBodyDamageState state,
                                   RenderModelComponent& visual) noexcept
{
    static const game::ModelConditionMask damaged = game::modelConditionMaskOf(game::ModelConditionFlag::Damaged);
    static const game::ModelConditionMask reallyDamaged =
        game::modelConditionMaskOf(game::ModelConditionFlag::ReallyDamaged);
    static const game::ModelConditionMask rubble = game::modelConditionMaskOf(game::ModelConditionFlag::Rubble);
    static const game::ModelConditionMask postCollapse =
        game::modelConditionMaskOf(game::ModelConditionFlag::PostCollapse);
    game::ModelConditionMask bodyMask = damaged;
    for (size_t index = 0; index < bodyMask.words.size(); ++index)
        bodyMask.words[index] |= reallyDamaged.words[index] | rubble.words[index];
    visual.modelConditionFlags.clear(bodyMask);
    switch (state)
    {
    case ObjectBodyDamageState::Damaged:
        addConditions(visual, damaged);
        break;
    case ObjectBodyDamageState::ReallyDamaged:
        addConditions(visual, reallyDamaged);
        break;
    case ObjectBodyDamageState::Rubble:
        if (visual.modelConditionFlags.intersectionCount(postCollapse) == 0)
            addConditions(visual, rubble);
        break;
    case ObjectBodyDamageState::Pristine:
        break;
    }
}

ObjectBodyDamageState objectBodyDamagePresentationState(
    const ecs::registry& registry, ecs::entity entity,
    ObjectBodyDamageState logicalState) noexcept
{
    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(registry, entity);
    if (status && status->hasAny(game::objectStatusBit(
                      game::ObjectStatusFlag::UnderConstruction)))
    {
        return ObjectBodyDamageState::Pristine;
    }
    return logicalState;
}

void projectObjectBodyDamageVisual(
    const ecs::registry& registry, ecs::entity entity,
    ObjectBodyDamageState logicalState,
    RenderModelComponent& visual) noexcept
{
    projectObjectBodyDamageVisual(
        objectBodyDamagePresentationState(registry, entity, logicalState),
        visual);
}

bool scaleObjectMaximumHealthPreserveRatio(
    ObjectHealthComponent& health,
    math::q32_32 numerator,
    math::q32_32 denominator,
    const ObjectSimulationRules& rules,
    RenderModelComponent* visual) noexcept
{
    if (numerator <= kHealthZero || denominator <= kHealthZero ||
        health.maximumFixed <= kHealthZero)
    {
        return false;
    }

    const HealthScalar previousMaximum = health.maximumFixed;
    const HealthScalar previousCurrent = health.currentFixed;
    // RefCode computes `mult = newBonus / oldBonus` first and then applies it
    // to max health. Use saturating wide arithmetic at both fixed-point
    // boundaries so a valid-but-extreme mod cannot wrap positive health into
    // a negative Body or trip the platform's 128/64 divide overflow.
    const HealthScalar multiplier =
        dividePositiveSaturated(numerator, denominator);
    const HealthScalar nextMaximum =
        multiplyPositiveSaturated(previousMaximum, multiplier);
    if (nextMaximum <= kHealthZero)
        return false;
    // ActiveBody::setMaxHealth(PRESERVE_RATIO) computes the old health ratio
    // first, then applies it to the new maximum.  Preserve that operation
    // order in fixed point: it both matches the source rounding boundary and
    // avoids forming the much larger nextMaximum * previousCurrent product.
    const HealthScalar previousRatio =
        dividePositiveSaturated(previousCurrent, previousMaximum);
    const HealthScalar nextCurrent = HealthScalar::min(
        nextMaximum,
        HealthScalar::max(kHealthZero,
                          multiplyPositiveSaturated(nextMaximum,
                                                    previousRatio)));

    health.previousFixed = previousCurrent;
    health.maximumFixed = nextMaximum;
    health.initialFixed = nextMaximum;
    health.currentFixed = nextCurrent;
    health.damageState = objectBodyDamageStateFor(nextCurrent, nextMaximum, rules);
    if (visual)
        projectObjectBodyDamageVisual(health.damageState, *visual);
    return true;
}

} // namespace engine
