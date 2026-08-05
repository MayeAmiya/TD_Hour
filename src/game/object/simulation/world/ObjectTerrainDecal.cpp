#include "game/object/simulation/world/ObjectTerrainDecal.h"

#include <algorithm>
#include <limits>

namespace engine {
namespace {

using Fixed = math::q32_32;

[[nodiscard]] Fixed unit(Fixed value) noexcept {
    return Fixed::min(Fixed{int32_t{1}}, Fixed::max(Fixed{}, value));
}

} // namespace

void advanceObjectTerrainDecal(ObjectTerrainDecalComponent& state,
                               uint64_t confirmedTick) noexcept {
    if (confirmedTick <= state.lastUpdateTick) return;
    const uint64_t elapsed = confirmedTick - state.lastUpdateTick;
    state.lastUpdateTick = confirmedTick;
    if (state.fadeRatePerFrame <= Fixed{} ||
        state.opacity == state.fadeTarget) {
        return;
    }
    const Fixed amount = state.fadeRatePerFrame * Fixed{static_cast<int32_t>(
        std::min<uint64_t>(elapsed,
            static_cast<uint64_t>(std::numeric_limits<int32_t>::max())))};
    const Fixed before = state.opacity;
    if (state.opacity < state.fadeTarget) {
        state.opacity = std::min(state.fadeTarget, state.opacity + amount);
    } else {
        state.opacity = std::max(state.fadeTarget, state.opacity - amount);
    }
    if (state.opacity != before) ++state.revision;
    if (state.opacity <= Fixed{} && state.fadeTarget <= Fixed{} &&
        state.kind != ObjectTerrainDecalKind::None) {
        state.kind = ObjectTerrainDecalKind::None;
        ++state.revision;
    }
}

void setObjectTerrainDecalKind(ecs::registry& registry, ecs::entity entity,
                               ObjectTerrainDecalKind kind,
                               uint64_t confirmedTick,
                               bool transparentWhenPreviouslyEmpty) noexcept {
    ObjectTerrainDecalComponent* state =
        ecs::try_get<ObjectTerrainDecalComponent>(registry, entity);
    if (!state) {
        ObjectTerrainDecalComponent value;
        value.kind = kind;
        value.opacity = transparentWhenPreviouslyEmpty ? Fixed{} : Fixed{1};
        value.fadeTarget = value.opacity;
        value.lastUpdateTick = confirmedTick;
        value.revision = 1;
        ecs::emplace<ObjectTerrainDecalComponent>(registry, entity, value);
        return;
    }
    advanceObjectTerrainDecal(*state, confirmedTick);
    if (state->kind == kind) return;
    const bool wasEmpty = state->kind == ObjectTerrainDecalKind::None;
    state->kind = kind;
    if (wasEmpty) {
        state->opacity = transparentWhenPreviouslyEmpty ? Fixed{} : Fixed{1};
        state->fadeTarget = state->opacity;
        state->fadeRatePerFrame = Fixed{};
    }
    ++state->revision;
}

void setObjectTerrainDecalFade(ecs::registry& registry, ecs::entity entity,
                               Fixed targetOpacity, Fixed ratePerFrame,
                               uint64_t confirmedTick) noexcept {
    ObjectTerrainDecalComponent* state =
        ecs::try_get<ObjectTerrainDecalComponent>(registry, entity);
    if (!state || state->kind == ObjectTerrainDecalKind::None) return;
    advanceObjectTerrainDecal(*state, confirmedTick);
    const Fixed target = unit(targetOpacity);
    const Fixed rate = unit(Fixed::abs(ratePerFrame));
    if (state->fadeTarget == target && state->fadeRatePerFrame == rate) return;
    state->fadeTarget = target;
    state->fadeRatePerFrame = rate;
    ++state->revision;
}

void updateObjectTerrainDecals(ecs::registry& registry,
                               uint64_t confirmedTick) noexcept {
    const auto view = ecs::view<ObjectTerrainDecalComponent>(registry);
    for (const ecs::entity entity : view) {
        advanceObjectTerrainDecal(
            view.template get<ObjectTerrainDecalComponent>(entity),
            confirmedTick);
    }
}

} // namespace engine
