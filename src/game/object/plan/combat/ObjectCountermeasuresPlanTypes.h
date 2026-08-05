#pragma once

#include "core/container/container_types.h"
#include "game/object/plan/combat/ObjectFireWeaponBehaviorPlanTypes.h"
#include "math/fixed/q32_32.h"

#include <cstdint>

namespace game {

struct ThingTemplate;

// Frozen CountermeasuresBehavior occurrence. Durations stay in authored
// milliseconds until one session materializes them against its confirmed
// logic rate; all scalar gameplay math is Q32.32 below this content boundary.
struct ObjectCountermeasuresRule final {
    uint32_t authoredOrder = 0;
    container::String flareTemplate;
    container::String flareBoneBaseName; // legacy compatibility metadata
    uint32_t volleySize = 0;
    uint32_t numberOfVolleys = 0;
    math::q32_32 volleyArcRadians{};
    math::q32_32 volleyVelocityFactor{int32_t{1}};
    uint32_t delayBetweenVolleysMilliseconds = 0;
    uint32_t reloadMilliseconds = 0;
    math::q32_32 evasionRate{};
    uint32_t missileDecoyMilliseconds = 0;
    uint32_t reactionLaunchLatencyMilliseconds = 0;
    bool mustReloadAtAirfield = false; // parsed; source runtime never reads it
    ObjectUpgradeMuxRecipe upgradeMux;
};

struct ObjectCountermeasuresPlan final {
    container::Vector<ObjectCountermeasuresRule> rules;
    container::Vector<container::String> diagnostics;
};

[[nodiscard]] container::SharedPtr<const ObjectCountermeasuresPlan>
compileObjectCountermeasuresPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog = nullptr);

} // namespace game
