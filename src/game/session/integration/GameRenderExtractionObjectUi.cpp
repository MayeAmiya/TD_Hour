#include "GameRenderExtractionObjectUi.h"

#include "GameRenderExtractionDetail.h"
#include "game/content/runtime/GameContentSnapshot.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/contracts/ObjectLifecycle.h"
#include "game/object/plan/containment/ObjectContainmentPlanTypes.h"
#include "game/object/simulation/combat/ObjectStickyBomb.h"
#include "game/object/simulation/combat/ObjectWeaponBonusUpdate.h"
#include "game/object/simulation/containment/ObjectSpawnSlave.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/status/ObjectDisabled.h"
#include "game/object/simulation/status/ObjectExperience.h"
#include "game/player/FactionTemplate.h"
#include "game/player/PlayerRegistry.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace engine::render_extraction_detail {
namespace {

[[nodiscard]] render::RenderVector aggregateHealthBoxOffset(
    const ecs::registry& registry,
    const ObjectLifecycle& lifecycle,
    ecs::entity entity,
    const render::RenderVector& objectPosition) {
    const ObjectSpawnSlaveComponent* spawn =
        ecs::try_get<ObjectSpawnSlaveComponent>(registry, entity);
    if (!spawn || !spawn->plan) return {};

    render::RenderVector accumulated{};
    size_t count = 0;
    const size_t ruleCount = std::min(
        spawn->plan->spawns.size(), spawn->spawns.size());
    for (size_t ruleIndex = 0; ruleIndex < ruleCount; ++ruleIndex) {
        if (!spawn->plan->spawns[ruleIndex].aggregateHealth) continue;
        for (const ObjectId child : spawn->spawns[ruleIndex].children) {
            const std::optional<ecs::entity> childEntity =
                lifecycle.entityFromId(child);
            const TransformComponent* childTransform = childEntity
                ? ecs::try_get<TransformComponent>(registry, *childEntity)
                : nullptr;
            if (!childTransform) continue;
            accumulated += render::RenderVector{
                childTransform->x,
                childTransform->y,
                childTransform->z,
            };
            ++count;
        }
    }
    if (count == 0u) return {};
    return accumulated / static_cast<float>(count) - objectPosition;
}

[[nodiscard]] std::optional<uint16_t> authoredContainerPipCapacity(
    const ObjectContainmentPlan* plan) noexcept {
    if (!plan) return std::nullopt;
    for (const ObjectContainmentRule& rule : plan->rules) {
        if (rule.kind == ObjectContainmentKind::Overlord ||
            rule.kind == ObjectContainmentKind::RiderChange ||
            !rule.shouldDrawPips) {
            return uint16_t{0};
        }
        if (rule.containMax == 0) continue;
        return static_cast<uint16_t>(std::min<uint32_t>(
            rule.containMax, std::numeric_limits<uint16_t>::max()));
    }
    return std::nullopt;
}

} // namespace

void appendObjectUiPresentation(
    const ObjectUiExtractionSource& source,
    const ObjectUiEntityInput& input,
    container::Vector<render::TacticalRadarEventRenderSnapshot>&
        radarCandidates,
    render::WorldRenderSnapshot& snapshot) {
    if (input.visual.hidden) return;

    render::ObjectUiRenderSnapshot ui;
    ui.objectId = input.identity.id.value;
    ui.worldPosition = input.worldPosition;
    const float heightAbovePosition = input.geometry
        ? (input.geometry->shape == ObjectGeometryShape::Sphere
               ? std::max(
                     0.0f, input.geometry->majorRadiusFixed.to_float())
               : std::max(0.0f, input.geometry->heightFixed.to_float()))
        : input.visibilityRadius;
    const render::RenderVector healthBoxOffset = aggregateHealthBoxOffset(
        source.registry, source.lifecycle, input.entity,
        input.worldPosition);
    ui.captionAnchor = input.worldPosition + render::RenderVector{
        0.0f, 0.0f,
        input.geometry && input.geometry->shape != ObjectGeometryShape::Sphere
            ? std::max(0.0f, input.geometry->heightFixed.to_float()) * 0.5f
            : 0.0f};
    ui.healthAnchor = input.worldPosition + healthBoxOffset +
        render::RenderVector{
            0.0f, 0.0f,
            std::max(0.0f, heightAbovePosition) + 10.0f +
                (hasObjectKind(input.kinds, game::ObjectKindOf::MobNexus)
                     ? 20.0f : 0.0f)};
    ui.worldRadius = std::max(1.0f, input.geometry
        ? std::max(input.geometry->majorRadiusFixed.to_float(),
                   input.geometry->minorRadiusFixed.to_float())
        : input.visibilityRadius);
    ui.selectionYawRadians = input.transform.rotation;
    if (input.geometry) {
        switch (input.geometry->shape) {
        case ObjectGeometryShape::Sphere:
            ui.selectionBounds =
                render::ObjectUiSelectionBoundsKind::Sphere;
            break;
        case ObjectGeometryShape::Cylinder:
            ui.selectionBounds =
                render::ObjectUiSelectionBoundsKind::Cylinder;
            break;
        case ObjectGeometryShape::Box:
            ui.selectionBounds = render::ObjectUiSelectionBoundsKind::Box;
            break;
        }
        ui.selectionMajorRadius = std::max(
            0.01f, input.geometry->majorRadiusFixed.to_float());
        ui.selectionMinorRadius = std::max(
            0.01f, input.geometry->minorRadiusFixed.to_float());
        ui.selectionHeight = std::max(
            0.01f, input.geometry->heightFixed.to_float());
    } else {
        ui.selectionMajorRadius = std::max(0.01f, input.visibilityRadius);
        ui.selectionMinorRadius = ui.selectionMajorRadius;
        ui.selectionHeight = ui.selectionMajorRadius * 2.0f;
    }
    const float majorRadius = input.geometry
        ? std::max(0.0f, input.geometry->majorRadiusFixed.to_float())
        : std::max(0.0f, input.visibilityRadius);
    const float minorRadius = input.geometry
        ? std::max(0.0f, input.geometry->minorRadiusFixed.to_float())
        : std::max(0.0f, input.visibilityRadius);
    ui.healthBoxWorldWidth = 2.0f * std::clamp(
        majorRadius + minorRadius, 20.0f, 150.0f);
    ui.healthRatio = input.health &&
            input.health->maximumFixed > math::q32_32{}
        ? std::clamp(
              (input.health->currentFixed /
               input.health->maximumFixed).to_float(),
              0.0f, 1.0f)
        : 0.0f;
    ui.effectivelyDead = input.health && input.health->effectivelyDead;
    ui.damageState = input.health
        ? static_cast<uint8_t>(input.health->damageState) : 0u;
    ui.selected = std::binary_search(
        source.localSelection.begin(), source.localSelection.end(),
        input.identity.id);
    ui.selectionFlashIdentity = input.selectionFlashIdentity;
    ui.hovered = source.localHover == input.identity.id;
    ui.visibility = input.localVisibilityState;
    ui.ignoredInGui = hasObjectKind(
        input.kinds, game::ObjectKindOf::IgnoredInGui);
    if (input.localVisibilityState ==
            render::LocalVisibilityRenderCellState::Visible &&
        !input.beaconHiddenFromLocalObserver) {
        if (const ObjectDrawableCaptionComponent* caption =
                ecs::try_get<ObjectDrawableCaptionComponent>(
                    source.registry, input.entity)) {
            ui.caption = caption->text;
        }
    }
    if (input.templateData) {
        ui.radarStructure = input.templateData->radarPriority ==
            game::ObjectRadarPriority::Structure;
        ui.radarUnit = input.templateData->radarPriority ==
                game::ObjectRadarPriority::Unit ||
            input.templateData->radarPriority ==
                game::ObjectRadarPriority::LocalUnitOnly;
        ui.radarLocalOnly = input.templateData->radarPriority ==
            game::ObjectRadarPriority::LocalUnitOnly;
    }

    const ObjectStatusComponent* status =
        ecs::try_get<ObjectStatusComponent>(source.registry, input.entity);
    const bool alwaysSelectable = hasObjectKind(
        input.kinds, game::ObjectKindOf::AlwaysSelectable);
    const bool authoredSelectable = hasObjectKind(
        input.kinds, game::ObjectKindOf::Selectable);
    const bool temporarilyUnselectable = status && status->hasAny(
        game::objectStatusBit(game::ObjectStatusFlag::Unselectable) |
        game::objectStatusBit(game::ObjectStatusFlag::Masked));
    ui.selectable = (authoredSelectable || alwaysSelectable) &&
        !temporarilyUnselectable &&
        (!ui.effectivelyDead || alwaysSelectable);
    ui.shrubberyTarget = hasObjectKind(
        input.kinds, game::ObjectKindOf::Shrubbery);
    ui.mineTarget = hasObjectKind(input.kinds, game::ObjectKindOf::Mine);
    // W3DModelDraw assigns PICK_TYPE_FORCEATTACKABLE only to templates
    // authored KINDOF_FORCEATTACKABLE, and CommandTranslator adds that pick
    // type solely in force-attack mode. Without the kind test every visible
    // object answered the force-attack pick, which let Ctrl+click target
    // props RefCode never exposes. KINDOF_CLICK_THROUGH sets collision type
    // zero, so such an object is not pickable at all.
    ui.forceAttackable = !ui.effectivelyDead && !ui.ignoredInGui &&
        !hasObjectKind(input.kinds, game::ObjectKindOf::ClickThrough) &&
        hasObjectKind(input.kinds, game::ObjectKindOf::ForceAttackable);
    if (status) {
        ui.underConstruction = status->hasAny(game::objectStatusBit(
            game::ObjectStatusFlag::UnderConstruction));
        ui.sold = status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Sold));
        ui.carBomb = status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::IsCarBomb));
        ui.stealthed = status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Stealthed));
        ui.detected = status->hasAny(
            game::objectStatusBit(game::ObjectStatusFlag::Detected));
    }
    if (ui.underConstruction) {
        ui.constructionPercent = 0.0f;
        if (const ObjectConstructionSiteComponent* site =
                ecs::try_get<ObjectConstructionSiteComponent>(
                    source.registry, input.entity)) {
            const uint32_t required = std::max(1u, site->requiredFrames);
            ui.constructionPercent = std::clamp(
                static_cast<float>(site->completedFrames) * 100.0f /
                    static_cast<float>(required),
                0.0f, 100.0f);
        }
    }
    const ObjectDisabledMask disabledMask = objectDisabledMask(
        source.registry, input.entity, source.simulationFrame);
    ui.disabled = (disabledMask & ~objectDisabledBit(
        ObjectDisabledReason::Held)) != 0;
    const ObjectDisabledMask disabledIconMask =
        objectDisabledBit(ObjectDisabledReason::Hacked) |
        objectDisabledBit(ObjectDisabledReason::Emp) |
        objectDisabledBit(ObjectDisabledReason::Paralyzed) |
        objectDisabledBit(ObjectDisabledReason::Underpowered) |
        objectDisabledBit(ObjectDisabledReason::ScriptUnderpowered) |
        objectDisabledBit(ObjectDisabledReason::Subdued);
    ui.disabledIcon = (disabledMask & disabledIconMask) != 0;
    ui.structure = hasObjectKind(
        input.kinds, game::ObjectKindOf::Structure);
    ui.vehicle = hasObjectKind(input.kinds, game::ObjectKindOf::Vehicle);
    ui.noHealIcon = hasObjectKind(
        input.kinds, game::ObjectKindOf::NoHealIcon);
    if (input.health && input.health->hasLastDamageInfo &&
        input.health->lastDamageType == game::DamageType::HEALING &&
        input.health->currentFixed < input.health->maximumFixed &&
        source.simulationFrame >
            static_cast<uint64_t>(source.logicFramesPerSecond) * 3u &&
        source.simulationFrame >= input.health->lastDamageTick &&
        source.simulationFrame - input.health->lastDamageTick <=
            static_cast<uint64_t>(source.logicFramesPerSecond) * 3u) {
        ui.recentlyHealing = true;
        const uint64_t duration =
            static_cast<uint64_t>(source.logicFramesPerSecond) * 3u;
        ui.recentlyHealingUntilTick = input.health->lastDamageTick >
                std::numeric_limits<uint64_t>::max() - duration
            ? std::numeric_limits<uint64_t>::max()
            : input.health->lastDamageTick + duration;
    }
    if (const ObjectWeaponBonusComponent* bonus =
            ecs::try_get<ObjectWeaponBonusComponent>(
                source.registry, input.entity)) {
        ui.enthusiastic = (bonus->conditions & game::weaponBonusConditionBit(
            game::WeaponBonusCondition::Enthusiastic)) != 0;
        ui.subliminal = (bonus->conditions & game::weaponBonusConditionBit(
            game::WeaponBonusCondition::Subliminal)) != 0;
    }
    if (const ObjectStickyBombComponent* sticky =
            ecs::try_get<ObjectStickyBombComponent>(
                source.registry, input.entity);
        sticky && !sticky->instances.empty()) {
        const ObjectStickyBombRuntime& runtime = sticky->instances.front();
        ui.stickyBombAttached = runtime.attached && !runtime.detonated &&
            runtime.target &&
            source.lifecycle.entityFromId(runtime.target).has_value();
        ui.stickyBombTimed =
            ui.stickyBombAttached && runtime.dieTick.has_value();
        ui.stickyBombDieTick = runtime.dieTick.value_or(0);
    }
    if (const ObjectVeterancyComponent* veterancy =
            ecs::try_get<ObjectVeterancyComponent>(
                source.registry, input.entity)) {
        ui.veterancyLevel = static_cast<uint8_t>(veterancy->level);
    }
    if (const ObjectExperienceComponent* experience =
            ecs::try_get<ObjectExperienceComponent>(
                source.registry, input.entity);
        experience && input.templateData && ui.veterancyLevel < 3u) {
        const size_t level = std::min<size_t>(
            ui.veterancyLevel,
            input.templateData->experienceRequired.size() - 2u);
        const int64_t low = input.templateData->experienceRequired[level];
        const int64_t high = input.templateData->experienceRequired[level + 1u];
        if (high > low) {
            ui.experienceRatio = std::clamp(
                static_cast<float>(experience->currentPoints - low) /
                    static_cast<float>(high - low),
                0.0f, 1.0f);
        }
    }
    if (input.weapons && input.weapons->activeWeaponSetIndex &&
        *input.weapons->activeWeaponSetIndex < input.weapons->sets.size()) {
        const ObjectWeaponSetRuntime& activeSet =
            input.weapons->sets[*input.weapons->activeWeaponSetIndex];
        for (const ObjectWeaponSlotRuntime& slot : activeSet.slots) {
            const game::WeaponTemplate* definition =
                source.content.findWeapon(slot.content);
            if (!definition || !definition->showsAmmoPips ||
                definition->clipSize <= 0) {
                continue;
            }
            ui.ammoTotal = static_cast<uint16_t>(std::min<int32_t>(
                definition->clipSize,
                std::numeric_limits<uint16_t>::max()));
            ui.ammoFull = static_cast<uint16_t>(std::min<uint32_t>(
                slot.ammoInClip, ui.ammoTotal));
            break;
        }
    }
    if (const ObjectContainmentComponent* containment =
            ecs::try_get<ObjectContainmentComponent>(
                source.registry, input.entity)) {
        ui.containerFull = static_cast<uint16_t>(std::min<size_t>(
            containment->objects.size(),
            std::numeric_limits<uint16_t>::max()));
        ui.containerTotal = authoredContainerPipCapacity(input.containmentPlan)
            .value_or(0u);
        if (ui.containerTotal != 0)
            ui.containerTotal = std::max(ui.containerTotal, ui.containerFull);
        for (const ObjectContainedObjectRecord& child : containment->objects) {
            const std::optional<ecs::entity> childEntity =
                source.lifecycle.entityFromId(child.object);
            const ObjectKindOfComponent* childKinds = childEntity
                ? ecs::try_get<ObjectKindOfComponent>(
                      source.registry, *childEntity)
                : nullptr;
            if (hasObjectKind(childKinds, game::ObjectKindOf::Infantry) &&
                ui.containerInfantry < ui.containerFull) {
                ++ui.containerInfantry;
            }
        }
    }

    const PlayerState* ownerPlayer = input.owner
        ? source.players.get(input.owner->player) : nullptr;
    if (ownerPlayer && source.ruleset) {
        const PlayerRgbColor color = resolvePlayerPresentationColor(
            *ownerPlayer, *source.ruleset);
        ui.indicatorColor = 0xff000000u |
            (static_cast<uint32_t>(color.red) << 16u) |
            (static_cast<uint32_t>(color.green) << 8u) |
            static_cast<uint32_t>(color.blue);
    }
    if (!input.hasSimulationObserver) {
        ui.relationship = render::ObjectUiRelationship::Observer;
    } else if (input.owner && input.owner->player == input.observerId) {
        ui.relationship = render::ObjectUiRelationship::Owned;
    } else if (input.owner) {
        const PlayerRelationship relationship = source.players.relationship(
            input.observerId, input.owner->player);
        ui.relationship = relationship == PlayerRelationship::Allies
            ? render::ObjectUiRelationship::Allied
            : relationship == PlayerRelationship::Enemies
                ? render::ObjectUiRelationship::Enemy
                : render::ObjectUiRelationship::Neutral;
    } else {
        ui.relationship = render::ObjectUiRelationship::Neutral;
    }

    const uint64_t radarLifetime =
        static_cast<uint64_t>(source.logicFramesPerSecond) * 4u;
    const uint64_t radarFade = std::max<uint64_t>(
        1u, source.logicFramesPerSecond / 2u);
    if (ui.relationship == render::ObjectUiRelationship::Owned &&
        input.health && input.health->hasLastDamageInfo &&
        input.health->lastDamageType != game::DamageType::HEALING &&
        source.simulationFrame >= input.health->lastDamageTick &&
        source.simulationFrame - input.health->lastDamageTick <=
            static_cast<uint64_t>(source.logicFramesPerSecond) * 10u) {
        const uint64_t dieTick =
            input.health->lastDamageTick + radarLifetime;
        radarCandidates.push_back({
            .eventIdentity = radarEventIdentity(
                input.identity.id.value, input.health->lastDamageTick, 3),
            .sourceObjectId = input.identity.id.value,
            .worldPosition = input.worldPosition,
            .eventType = 3,
            .createTick = input.health->lastDamageTick,
            .fadeTick = dieTick > radarFade ? dieTick - radarFade : 0,
            .dieTick = dieTick,
        });
    }
    const game::ObjectStatusMask construction =
        game::objectStatusBit(game::ObjectStatusFlag::UnderConstruction);
    if (ui.relationship == render::ObjectUiRelationship::Owned && status &&
        (status->lastSetMask & construction) != 0 &&
        source.simulationFrame >= status->lastChangedTick) {
        const uint64_t dieTick = status->lastChangedTick + radarLifetime;
        if (source.simulationFrame <= dieTick) {
            radarCandidates.push_back({
                .eventIdentity = radarEventIdentity(
                    input.identity.id.value, status->lastChangedTick, 1),
                .sourceObjectId = input.identity.id.value,
                .worldPosition = input.worldPosition,
                .eventType = 1,
                .createTick = status->lastChangedTick,
                .fadeTick = dieTick > radarFade ? dieTick - radarFade : 0,
                .dieTick = dieTick,
            });
        }
    }
    snapshot.objectUi.objects.push_back(std::move(ui));
}

} // namespace engine::render_extraction_detail
