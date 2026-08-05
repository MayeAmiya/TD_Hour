#pragma once

#include "game/object/simulation/combat/ObjectProjectileSystem.h"

namespace engine::session_projectile {

[[nodiscard]] constexpr bool invokesDetonationFx(
    ObjectProjectileEventKind kind) noexcept {
    switch (kind) {
    case ObjectProjectileEventKind::Collided:
    case ObjectProjectileEventKind::ReachedDestination:
    case ObjectProjectileEventKind::Expired:
    case ObjectProjectileEventKind::PathInvalid:
        return true;
    case ObjectProjectileEventKind::Spawned:
    case ObjectProjectileEventKind::GarrisonCleared:
    case ObjectProjectileEventKind::Effect:
    case ObjectProjectileEventKind::GroundDecalBegin:
    case ObjectProjectileEventKind::GroundDecalEnd:
    case ObjectProjectileEventKind::UnsupportedTemplate:
        return false;
    }
    return false;
}

} // namespace engine::session_projectile
