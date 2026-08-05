#include "GameSessionScriptPortDetail.h"

#include "debug/debug.h"
#include "game/base/GameBalanceConstants.h"
#include "game/base/DamageTypes.h"
#include "game/base/GameCameraDirector.h"
#include "game/base/GameSettings.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/audio/GameAudioEvents.h"
#include "game/command/CommandButtonStore.h"
#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/UpgradeCatalog.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/simulation/containment/ObjectContainment.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/economy/ObjectEconomy.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/economy/ObjectProduction.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/terrain/MapVisibilityAuthority.h"
#include "core/math/wwmath/base/wwmath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace engine::script::detail {

[[nodiscard]] bool objectVisibleToPlayer(
    const ecs::registry& registry,
    const game::terrain::MapVisibilityAuthority& mapVisibility,
    ecs::entity entity, PlayerId observer,
    uint64_t confirmedTick, bool enforceObjectConcealment) noexcept {
    if (enforceObjectConcealment && isObjectDisabledBy(
            registry, entity, ObjectDisabledReason::Held,
            confirmedTick)) {
        return false;
    }
    if (enforceObjectConcealment) {
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, entity);
        if (status &&
            status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::Stealthed)) &&
            !status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::Detected)) &&
            !status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::Disguised))) {
            return false;
        }
    }

    const ThingTemplateComponent* type =
        ecs::try_get<ThingTemplateComponent>(registry, entity);
    if (type && type->archetype && kindOfContains(
            type->archetype, game::ObjectKindOf::AlwaysVisible)) {
        return true;
    }
    // RefCode returns CLEAR when an Object has no PartitionData, notably for
    // contained soldiers. NAMED/TEAM already rejected Held above; the player
    // discovery predicate intentionally reaches this compatibility path.
    if (ecs::try_get<ObjectContainedByComponent>(registry, entity)) {
        return true;
    }
    const ObjectMapStatusComponent* mapStatus =
        ecs::try_get<ObjectMapStatusComponent>(registry, entity);
    if (mapStatus && mapStatus->offMap) return false;

    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, entity);
    const auto visibility = mapVisibility.snapshot();
    if (!transform || !visibility) return false;
    const ObjectGeometryComponent* geometry =
        ecs::try_get<ObjectGeometryComponent>(registry, entity);
    const LogicFixedVec3 position = readAuthoritativeObjectPosition(
        registry, entity, *transform);
    const math::q32_32 radius = geometry
        ? math::q32_32::max(
              math::q32_32{}, geometry->boundingCircleRadiusFixed)
        : math::q32_32{};
    return visibility->footprintHasClearCellRaw(
        observer, position.x.raw(), position.y.raw(), radius.raw());
}

[[nodiscard]] bool kindOfContains(
    const game::ObjectArchetype* archetype,
    game::ObjectKindOf wanted) noexcept {
    return archetype && game::objectHasKind(archetype->kindOfMask, wanted);
}


[[nodiscard]] bool templateHasAiUpdate(const ThingTemplateComponent* templateComponent) noexcept {
    return templateComponent && templateComponent->archetype &&
           templateComponent->archetype->hasAiUpdate;
}

[[nodiscard]] bool templateIsInert(const ThingTemplateComponent* templateComponent) noexcept {
    return templateComponent && templateComponent->archetype &&
           kindOfContains(templateComponent->archetype,
                          game::ObjectKindOf::Inert);
}

// Script condition SURFACES_ALLOWED predates the full locomotor bit mask:
// bit 0 means ground and bit 1 means air. RefCode expands its air bit from
// 0x02 to LOCOMOTORSURFACE_AIR (0x08) before intersecting a LocomotorSet.
[[nodiscard]] game::LocomotorSurfaceMask teamAreaAllowedSurfaces(uint8_t allowed) noexcept {
    game::LocomotorSurfaceMask result = 0;
    if ((allowed & 0x01u) != 0) {
        result |= game::locomotorSurfaceBit(game::LocomotorSurface::Ground);
    }
    if ((allowed & 0x02u) != 0) {
        result |= game::locomotorSurfaceBit(game::LocomotorSurface::Air);
    }
    return result;
}

// RefCode asks an AIUpdateInterface for its LocomotorSet's union of valid
// surfaces; objects without an AI update are treated as ground objects. The
// modern session already owns that immutable recipe and its frozen locomotor
// templates, even where Stage-0 movement has not yet materialized an air or
// hover simulation component.
[[nodiscard]] game::LocomotorSurfaceMask teamAreaObjectSurfaces(
    const GameContentSnapshot& content,
    const ThingTemplateComponent* templateComponent,
    const ObjectLocomotionComponent* locomotion) {
    const game::LocomotorSurfaceMask ground =
        game::locomotorSurfaceBit(game::LocomotorSurface::Ground);
    if (!templateHasAiUpdate(templateComponent)) return ground;

    game::LocomotorSurfaceMask result = 0;
    const game::ThingTemplate& objectTemplate = templateComponent->archetype->templateData;
    for (const container::String& locomotorName : objectTemplate.locomotors) {
        const game::FrozenLocomotorTemplate* locomotor =
            content.findLocomotor(locomotorName);
        if (locomotor && locomotor->supportsRuntimeLocomotion())
            result |= locomotor->surfaces;
    }

    // A live Stage-0 locomotion projection is authoritative evidence of a
    // valid current locomotor. This fallback only protects a partially frozen
    // development fixture; it does not turn an AI object with no locomotor
    // data into an invented ground unit.
    if (result == 0 && locomotion) result = locomotion->surfaces;
    return result;
}

[[nodiscard]] std::optional<ScriptWorldObjectSnapshot> scriptObjectSnapshot(
    const ecs::registry& registry, const ObjectLifecycle& objects,
    ObjectId object) {
    if (!object) return std::nullopt;
    const std::optional<ecs::entity> entity = objects.entityFromId(object);
    if (!entity) return std::nullopt;
    const TransformComponent* transform =
        ecs::try_get<TransformComponent>(registry, *entity);
    if (!transform) return std::nullopt;
    const LogicFixedVec3 fixedPosition = readAuthoritativeObjectPosition(
        registry, *entity, *transform);
    const OwnerComponent* owner =
        ecs::try_get<OwnerComponent>(registry, *entity);
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(registry, *entity);
    std::optional<ScriptWorldObjectHealthSnapshot> healthSnapshot;
    if (health && health->acceptsDamage) {
        healthSnapshot = {
            .current = health->currentFixed,
            .initial = health->initialFixed,
        };
    }
    return ScriptWorldObjectSnapshot{
        .id = object,
        .positionFixed = {
            .x = fixedPosition.x,
            .y = fixedPosition.y,
            .z = fixedPosition.z,
        },
        .position = {
            fixedPosition.x.to_float(),
            fixedPosition.y.to_float(),
            fixedPosition.z.to_float(),
        },
        .owner = owner ? owner->player : INVALID_PLAYER_ID,
        .health = healthSnapshot,
        .alive = true,
    };
}

} // namespace engine::script::detail
