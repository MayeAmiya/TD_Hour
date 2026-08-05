#pragma once

#include "core/container/container_types.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/definition/ObjectKindOf.h"

#include <cstdint>
#include <limits>
#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"
#include "math/fixed/q32_32.h"

namespace game
{

struct ThingTemplate;

// Immutable, value-only projection of one AutoHealBehavior declaration in a
// fully resolved object recipe. Self, allied-radius and controlling-player
// target selection are all consumed by the production ECS system.
struct ObjectAutoHealParameters final
{
    uint32_t authoredOrder = 0;
    math::q32_32 healingAmount{};
    uint32_t healingDelayMilliseconds = std::numeric_limits<uint32_t>::max();
    uint32_t startHealingDelayMilliseconds = 0;
    math::q32_32 radius{};
    bool startsActive = false;
    bool singleBurst = false;
    bool affectsWholePlayer = false;
    bool skipSelfForHealing = false;
    bool requiresAllTriggers = false;

    // UpgradeMux data remains authored as names at this recipe boundary.  The
    // session combines player and object-local inventories before evaluation,
    // without retaining a Player or legacy UpgradeMask pointer.
    container::Vector<container::String> triggeredBy;
    container::Vector<container::String> conflictsWith;
    container::Vector<container::String> removesUpgrades;
    engine::UpgradeMask triggeredByMask;
    engine::UpgradeMask conflictsWithMask;
    engine::UpgradeMask removesUpgradesMask;
    container::String upgradeFx;
    bool upgradeMasksCompiled = false;

    // Empty KindOf means the legacy default all-bits mask; ForbiddenKindOf
    // remains an any-match veto.
    ObjectKindOfMask kindOfMask{};
    ObjectKindOfMask forbiddenKindOfMask{};
    container::String radiusParticleSystemName;
    container::String unitHealPulseParticleSystemName;

};

struct ObjectAutoHealPlan final
{
    container::Vector<ObjectAutoHealParameters> rules;
};

// Compiles the final inherited object recipe once at content-load time.
// Null means that the recipe has no AutoHealBehavior modules.
[[nodiscard]] container::SharedPtr<const ObjectAutoHealPlan>
compileObjectAutoHealPlan(
    const ThingTemplate& templateData,
    const engine::UpgradeCatalog* upgradeCatalog = nullptr);

// UpgradeMux's pure predicate, exposed for focused content/system probes.
// Player technology is UpgradeMask; object inventory remains name vectors.
// Trigger names resolve through the sealed UpgradeCatalog.
[[nodiscard]] bool objectAutoHealUpgradeMatches(
    const ObjectAutoHealParameters& parameters,
    const engine::UpgradeMask& completedUpgrades,
    const engine::UpgradeCatalog* catalog) noexcept;
[[nodiscard]] bool objectAutoHealUpgradeMatches(
    const ObjectAutoHealParameters& parameters,
    const engine::UpgradeMask& playerCompletedUpgrades,
    const engine::UpgradeMask& objectCompletedUpgrades,
    const engine::UpgradeCatalog* catalog) noexcept;

} // namespace game

