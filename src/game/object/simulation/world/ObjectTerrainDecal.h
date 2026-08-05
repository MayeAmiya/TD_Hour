#pragma once

#include "core/ecs/registry.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace engine {

enum class ObjectTerrainDecalKind : uint8_t {
    None,
    Demoralized,
    HordeInfantry,
    HordeNationalismInfantry,
    HordeVehicle,
    HordeNationalismVehicle,
    HordeFanaticism,
    Crate,
    ChemSuit,
};

// Confirmed counterpart of Drawable's single terrain-decal slot.  Keeping the
// current opacity/target/rate here is required for save/replay seek and
// presentation-epoch rebuild; renderer-local interpolation alone cannot
// recover a fade already in progress.
struct ObjectTerrainDecalComponent final {
    ObjectTerrainDecalKind kind = ObjectTerrainDecalKind::None;
    math::q32_32 opacity{};
    math::q32_32 fadeTarget{};
    math::q32_32 fadeRatePerFrame{};
    uint64_t lastUpdateTick = 0;
    uint64_t revision = 0;
};

void advanceObjectTerrainDecal(ObjectTerrainDecalComponent& state,
                               uint64_t confirmedTick) noexcept;

void setObjectTerrainDecalKind(ecs::registry& registry, ecs::entity entity,
                               ObjectTerrainDecalKind kind,
                               uint64_t confirmedTick,
                               bool transparentWhenPreviouslyEmpty) noexcept;

void setObjectTerrainDecalFade(ecs::registry& registry, ecs::entity entity,
                               math::q32_32 targetOpacity,
                               math::q32_32 ratePerFrame,
                               uint64_t confirmedTick) noexcept;

void updateObjectTerrainDecals(ecs::registry& registry,
                               uint64_t confirmedTick) noexcept;

} // namespace engine
