#include "GameGroundDecalExtraction.h"
#include "GameRenderExtractionDetail.h"

#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/simulation/combat/ObjectNeutronMissileSlowDeath.h"
#include "game/object/simulation/structure/ObjectAirfield.h"
#include "game/object/simulation/world/ObjectDynamicShroud.h"
#include "game/object/simulation/world/ObjectRadiusDecal.h"
#include "game/object/simulation/world/ObjectTerrainDecal.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace engine {
using namespace render_extraction_detail;

render::GroundDecalPresentationBatch
render_extraction_detail::extractGroundDecalPresentation(
    GroundDecalExtractionSource source) {
    const PlayerState* localPlayer = source.players.localPlayer();
    render::GroundDecalPresentationBatch batch{
        .presentationEpoch = source.presentationEpoch,
        .confirmedFrame = source.confirmedFrame,
        .observerPlayer = localPlayer
            ? localPlayer->id.value : uint8_t{0xff},
        .drawIconUiEnabled = source.drawIconUiEnabled,
        .hasCompleteOwnerSet = true,
    };
    uint64_t sequence = 1;
    const auto resolvedColor = [&source](
        PlayerId owner, const container::Array<uint8_t, 4>& authored,
        bool usesPlayerColor) noexcept {
        if (!usesPlayerColor) {
            return math::vec4{
                static_cast<float>(authored[0]) / 255.0f,
                static_cast<float>(authored[1]) / 255.0f,
                static_cast<float>(authored[2]) / 255.0f,
                static_cast<float>(authored[3]) / 255.0f,
            };
        }
        const PlayerState* player = source.players.get(owner);
        const std::optional<PlayerRgbColor> color = player
            ? source.ruleset.presentationColor(*player)
            : std::nullopt;
        return color
            ? math::vec4{
                  static_cast<float>(color->red) / 255.0f,
                  static_cast<float>(color->green) / 255.0f,
                  static_cast<float>(color->blue) / 255.0f, 1.0f}
            : math::vec4{1.0f, 1.0f, 1.0f, 1.0f};
    };
    const auto position = [](const LogicFixedVec3& value) noexcept {
        return render::RenderVector{
            value.x.to_float(), value.y.to_float(), value.z.to_float()};
    };

    for (const ObjectRadiusDecalEvent& event : source.radiusEvents) {
        const auto presentationKind = [&event]() noexcept {
            switch (event.kind) {
            case ObjectRadiusDecalEventKind::Begin:
                return render::GroundDecalPresentationEventKind::Begin;
            case ObjectRadiusDecalEventKind::Update:
                return render::GroundDecalPresentationEventKind::Update;
            case ObjectRadiusDecalEventKind::End:
                return render::GroundDecalPresentationEventKind::End;
            }
            return render::GroundDecalPresentationEventKind::End;
        }();
        const auto presentationSource = [&event]() noexcept {
            switch (event.source) {
            case ObjectRadiusDecalEventSource::NeutronMissileUpdate:
                return render::GroundDecalPresentationSource::NeutronDelivery;
            case ObjectRadiusDecalEventSource::SpectreAttackArea:
                return render::GroundDecalPresentationSource::SpectreAttackArea;
            case ObjectRadiusDecalEventSource::SpectreTargetingReticle:
                return render::GroundDecalPresentationSource::SpectreTargetingReticle;
            case ObjectRadiusDecalEventSource::RadiusDecalUpdate:
                return render::GroundDecalPresentationSource::ObjectRadius;
            }
            return render::GroundDecalPresentationSource::ObjectRadius;
        }();
        batch.events.push_back({
            .kind = presentationKind,
            .key = {
                .source = presentationSource,
                .object = event.object,
                .authoredOrder = event.authoredOrder,
            },
            .confirmedFrame = event.confirmedTick,
            .streamSequence = sequence++,
            .ownerPlayer = event.owner.value,
            .textureName = event.texture,
            .position = position(event.position),
            .radius = event.radius.to_float(),
            .shadowTypeMask = event.shadowTypeMask,
            .minimumOpacity = event.minimumOpacity.to_float(),
            .maximumOpacity = event.maximumOpacity.to_float(),
            .opacityThrobFrames = event.opacityThrobTicks,
            .color = resolvedColor(
                event.owner, event.color, event.usesPlayerColor),
            .onlyVisibleToOwningPlayer =
                event.onlyVisibleToOwningPlayer,
        });
    }
    for (const ObjectDynamicShroudDecalEvent& event :
         source.dynamicShroudEvents) {
        render::GroundDecalPresentationEventKind kind =
            render::GroundDecalPresentationEventKind::Update;
        if (event.kind == ObjectDynamicShroudDecalEventKind::Begin) {
            kind = render::GroundDecalPresentationEventKind::Begin;
        } else if (event.kind == ObjectDynamicShroudDecalEventKind::End) {
            kind = render::GroundDecalPresentationEventKind::End;
        }
        batch.events.push_back({
            .kind = kind,
            .key = {
                .source =
                    render::GroundDecalPresentationSource::DynamicShroudGrid,
                .object = event.object,
                .authoredOrder = event.authoredOrder,
            },
            .confirmedFrame = event.confirmedTick,
            .streamSequence = sequence++,
            .ownerPlayer = event.owner.value,
            .textureName = event.recipe.texture,
            .position = position(event.position),
            .radius = event.initialDecalRadius.to_float(),
            .shadowTypeMask = event.recipe.shadowTypeMask,
            .minimumOpacity = event.recipe.minimumOpacity.to_float(),
            .maximumOpacity = event.recipe.maximumOpacity.to_float(),
            .opacityThrobFrames = event.opacityThrobTicks,
            .color = resolvedColor(
                event.owner, event.recipe.color,
                event.recipe.usesPlayerColor),
            .onlyVisibleToOwningPlayer =
                event.recipe.onlyVisibleToOwningPlayer,
            .decalCount = event.decalCount,
            .gridSnapSize = event.gridSnapSize,
            .initialDecalRadius = event.initialDecalRadius.to_float(),
            .nativeClearingRange = event.nativeClearingRange.to_float(),
            .currentClearingRange = event.currentClearingRange.to_float(),
            .totalFrames = event.totalTicks,
            .stateCountdown = event.stateCountdown,
        });
    }

    // A complete desired-state projection accompanies the ordered edges.
    // It heals same-epoch replay seeks and external object destruction without
    // letting presentation inspect ECS after this function returns.
    {
        const auto view = ecs::view<
            const ObjectIdentityComponent, const OwnerComponent,
            const ObjectRadiusDecalComponent>(source.registry);
        for (const ecs::entity entity : view) {
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(entity);
            const OwnerComponent& owner =
                view.template get<const OwnerComponent>(entity);
            const ObjectRadiusDecalComponent& component =
                view.template get<const ObjectRadiusDecalComponent>(entity);
            if (!identity.id || !component.plan) continue;
            const size_t count = std::min(
                component.plan->rules.size(), component.instances.size());
            for (size_t index = 0; index < count; ++index) {
                const ObjectRadiusDecalRuntime& runtime =
                    component.instances[index];
                if (!runtime.active) continue;
                const render::GroundDecalPresentationKey key{
                    .source =
                        render::GroundDecalPresentationSource::ObjectRadius,
                    .object = identity.id,
                    .authoredOrder =
                        component.plan->rules[index].authoredOrder,
                };
                batch.synchronizedOwners.push_back(key);
                batch.events.push_back({
                    .kind = render::GroundDecalPresentationEventKind::Begin,
                    .key = key,
                    .confirmedFrame = batch.confirmedFrame,
                    .streamSequence = sequence++,
                    .ownerPlayer = owner.player.value,
                    .textureName = runtime.texture,
                    .position = position(runtime.position),
                    .radius = runtime.radius.to_float(),
                    .shadowTypeMask = runtime.shadowTypeMask,
                    .minimumOpacity = runtime.minimumOpacity.to_float(),
                    .maximumOpacity = runtime.maximumOpacity.to_float(),
                    .opacityThrobFrames = runtime.opacityThrobTicks,
                    .color = resolvedColor(
                        owner.player, runtime.color,
                        runtime.usesPlayerColor),
                    .onlyVisibleToOwningPlayer =
                        runtime.onlyVisibleToOwningPlayer,
                });
            }
        }
    }
    {
        const auto view = ecs::view<
            const ObjectIdentityComponent, const OwnerComponent,
            const ObjectAirfieldComponent>(source.registry);
        const uint64_t logicFps = std::max<uint32_t>(
            1u, source.logicFramesPerSecond);
        for (const ecs::entity entity : view) {
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(entity);
            const OwnerComponent& owner =
                view.template get<const OwnerComponent>(entity);
            const ObjectAirfieldComponent& component =
                view.template get<const ObjectAirfieldComponent>(entity);
            if (!identity.id || !component.plan) continue;
            const size_t count = std::min(
                component.plan->spectreGunships.size(),
                component.spectreGunships.size());
            for (size_t index = 0; index < count; ++index) {
                const ObjectSpectreGunshipRuntime& runtime =
                    component.spectreGunships[index];
                if (!runtime.targetingDecalsActive) continue;
                const game::ObjectSpectreGunshipRule& rule =
                    component.plan->spectreGunships[index];
                const auto appendSpectre = [&] (
                    render::GroundDecalPresentationSource source,
                    const game::ObjectSpectreRadiusDecalRule& decal,
                    const LogicFixedVec3& worldPosition, float radius) {
                    if (decal.texture.empty() || !(radius > 0.0f) ||
                        decal.shadowTypeMask == 0) return;
                    const render::GroundDecalPresentationKey key{
                        .source = source,
                        .object = identity.id,
                        .authoredOrder = rule.authoredOrder,
                    };
                    batch.synchronizedOwners.push_back(key);
                    batch.events.push_back({
                        .kind = render::GroundDecalPresentationEventKind::Begin,
                        .key = key,
                        .confirmedFrame = batch.confirmedFrame,
                        .streamSequence = sequence++,
                        .ownerPlayer = owner.player.value,
                        .textureName = decal.texture,
                        .position = position(worldPosition),
                        .radius = radius,
                        .shadowTypeMask = decal.shadowTypeMask,
                        .minimumOpacity = std::clamp(
                            decal.minimumOpacity.to_float(), 0.0f, 1.0f),
                        .maximumOpacity = std::clamp(
                            decal.maximumOpacity.to_float(), 0.0f, 1.0f),
                        .opacityThrobFrames = std::max<uint64_t>(
                            1u, (static_cast<uint64_t>(
                                    decal.opacityThrobMilliseconds) *
                                logicFps + 999u) / 1000u),
                        .color = resolvedColor(
                            owner.player, decal.color,
                            decal.usesPlayerColor),
                        .onlyVisibleToOwningPlayer =
                            decal.onlyVisibleToOwningPlayer,
                    });
                };
                appendSpectre(
                    render::GroundDecalPresentationSource::SpectreAttackArea,
                    rule.attackAreaDecal, runtime.initialTargetPosition,
                    rule.attackAreaRadiusFixed.to_float());
                appendSpectre(
                    render::GroundDecalPresentationSource::
                        SpectreTargetingReticle,
                    rule.targetingReticleDecal,
                    runtime.overrideTargetDestination,
                    rule.targetingReticleRadiusFixed.to_float());
            }
        }
    }
    {
        const auto view = ecs::view<
            const ObjectIdentityComponent, const OwnerComponent,
            const ObjectDynamicShroudComponent>(
                source.registry);
        for (const ecs::entity entity : view) {
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(entity);
            const OwnerComponent& owner =
                view.template get<const OwnerComponent>(entity);
            const ObjectDynamicShroudComponent& component =
                view.template get<const ObjectDynamicShroudComponent>(entity);
            if (!identity.id || !component.plan) continue;
            const size_t count = std::min(
                component.plan->rules.size(), component.instances.size());
            for (size_t index = 0; index < count; ++index) {
                const game::ObjectDynamicShroudRule& rule =
                    component.plan->rules[index];
                const ObjectDynamicShroudRuntime& runtime =
                    component.instances[index];
                if (!runtime.decalBeginEmitted || runtime.decalEndEmitted ||
                    !runtime.decalPresentationSampleValid ||
                    rule.gridDecal.texture.empty()) continue;
                const render::GroundDecalPresentationKey key{
                    .source = render::GroundDecalPresentationSource::
                        DynamicShroudGrid,
                    .object = identity.id,
                    .authoredOrder = rule.authoredOrder,
                };
                batch.synchronizedOwners.push_back(key);
                batch.events.push_back({
                    .kind = render::GroundDecalPresentationEventKind::Begin,
                    .key = key,
                    .confirmedFrame = batch.confirmedFrame,
                    .streamSequence = sequence++,
                    .ownerPlayer = owner.player.value,
                    .textureName = rule.gridDecal.texture,
                    .position = position(runtime.decalPresentedPosition),
                    .radius = 100.0f,
                    .shadowTypeMask = rule.gridDecal.shadowTypeMask,
                    .minimumOpacity =
                        rule.gridDecal.minimumOpacity.to_float(),
                    .maximumOpacity =
                        rule.gridDecal.maximumOpacity.to_float(),
                    .opacityThrobFrames = runtime.opacityThrobTicks,
                    .color = resolvedColor(
                        owner.player, rule.gridDecal.color,
                        rule.gridDecal.usesPlayerColor),
                    .onlyVisibleToOwningPlayer =
                        rule.gridDecal.onlyVisibleToOwningPlayer,
                    .decalCount = 30,
                    .gridSnapSize = 23,
                    .initialDecalRadius = 100.0f,
                    .nativeClearingRange =
                        runtime.nativeClearingRange.to_float(),
                    .currentClearingRange =
                        runtime.decalPresentedClearingRange.to_float(),
                    .totalFrames = runtime.totalTicks,
                    .stateCountdown =
                        runtime.decalPresentedStateCountdown,
                });
            }
        }
    }
    {
        const auto view = ecs::view<
            const ObjectIdentityComponent, const OwnerComponent,
            const ObjectNeutronMissileProjectileComponent>(
                source.registry);
        for (const ecs::entity entity : view) {
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(entity);
            const OwnerComponent& owner =
                view.template get<const OwnerComponent>(entity);
            const ObjectNeutronMissileProjectileComponent& neutron =
                view.template get<
                    const ObjectNeutronMissileProjectileComponent>(entity);
            if (!identity.id || !neutron.deliveryDecalActive) continue;
            const render::GroundDecalPresentationKey key{
                .source =
                    render::GroundDecalPresentationSource::NeutronDelivery,
                .object = identity.id,
                .authoredOrder = neutron.deliveryDecalAuthoredOrder,
            };
            batch.synchronizedOwners.push_back(key);
            batch.events.push_back({
                .kind = render::GroundDecalPresentationEventKind::Begin,
                .key = key,
                .confirmedFrame = batch.confirmedFrame,
                .streamSequence = sequence++,
                .ownerPlayer = owner.player.value,
                .textureName = neutron.deliveryDecalTexture,
                .position = position(neutron.target),
                .radius = neutron.deliveryDecalRadius.to_float(),
                .shadowTypeMask = neutron.deliveryDecalShadowTypeMask,
                .minimumOpacity =
                    neutron.deliveryDecalMinimumOpacity.to_float(),
                .maximumOpacity =
                    neutron.deliveryDecalMaximumOpacity.to_float(),
                .opacityThrobFrames =
                    neutron.deliveryDecalOpacityThrobFrames,
                .color = resolvedColor(
                    owner.player, neutron.deliveryDecalColor,
                    neutron.deliveryDecalUsesPlayerColor),
                .onlyVisibleToOwningPlayer =
                    neutron.deliveryDecalOnlyVisibleToOwningPlayer,
            });
        }
    }
    {
        // RefCode Drawable owns one terrain-decal slot. Project all stock
        // producers into that same stable owner so Nationalism/Fanaticism,
        // Horde exit, ChemSuit, destruction, replay seeks and epoch changes
        // replace/remove one another instead of stacking projectors.
        constexpr uint32_t kDrawableTerrainDecalSlot = 0;
        const PlayerState* terrainDecalObserver =
            source.players.localPlayer();
        const bool terrainDecalObserverIsSpectator = terrainDecalObserver &&
            (terrainDecalObserver->participation ==
                 PlayerParticipationKind::Observer ||
             terrainDecalObserver->controller ==
                 PlayerControllerKind::Observer);
        const auto view = ecs::view<
            const ObjectIdentityComponent, const OwnerComponent,
            const TransformComponent, const ThingTemplateComponent>(
                source.registry);
        for (const ecs::entity entity : view) {
            const ObjectIdentityComponent& identity =
                view.template get<const ObjectIdentityComponent>(entity);
            const OwnerComponent& owner =
                view.template get<const OwnerComponent>(entity);
            const TransformComponent& transform =
                view.template get<const TransformComponent>(entity);
            const ThingTemplateComponent& type =
                view.template get<const ThingTemplateComponent>(entity);
            if (!identity.id || !type.archetype) continue;
            const ObjectHealthComponent* health =
                ecs::try_get<ObjectHealthComponent>(source.registry, entity);
            const ObjectTerrainDecalComponent* decalState =
                ecs::try_get<ObjectTerrainDecalComponent>(
                    source.registry, entity);
            const ObjectTerrainDecalKind decalKind = decalState
                ? decalState->kind : ObjectTerrainDecalKind::None;
            if (health && health->effectivelyDead &&
                decalKind == ObjectTerrainDecalKind::None) continue;

            const ObjectKindOfComponent* kinds =
                ecs::try_get<ObjectKindOfComponent>(source.registry, entity);
            const RenderModelComponent* visual =
                ecs::try_get<RenderModelComponent>(source.registry, entity);
            // Drawable::setHidden forwards the same enable/disable edge to
            // its terrain decal.  Keep script-hidden objects from leaving a
            // detached projector behind while the model itself is absent.
            if (visual && visual->hidden) continue;
            const bool observerParticipates = terrainDecalObserver &&
                terrainDecalObserver->isSimulationParticipant();
            const bool alliedToObserver = observerParticipates &&
                source.players.relationship(
                    terrainDecalObserver->id, owner.player) ==
                    PlayerRelationship::Allies;
            const ObjectStatusComponent* status =
                ecs::try_get<ObjectStatusComponent>(
                    source.registry, entity);
            // An undetected enemy Drawable is hidden altogether, and
            // W3DModelDraw::setHidden toggles the terrain decal with the model.
            // Detected stealth only disables the ordinary shadow via
            // setShadowsEnabled; it deliberately leaves terrain decals alone.
            if (observerParticipates && !alliedToObserver && status &&
                status->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::Stealthed)) &&
                !status->hasAny(game::objectStatusBit(
                    game::ObjectStatusFlag::Detected))) {
                continue;
            }
            const bool fakeStructure =
                hasObjectKind(kinds, game::ObjectKindOf::FsFake);
            const bool hordeDecal =
                decalKind == ObjectTerrainDecalKind::HordeInfantry ||
                decalKind == ObjectTerrainDecalKind::HordeNationalismInfantry ||
                decalKind == ObjectTerrainDecalKind::HordeVehicle ||
                decalKind == ObjectTerrainDecalKind::HordeNationalismVehicle ||
                decalKind == ObjectTerrainDecalKind::HordeFanaticism;
            const bool vehicleHorde =
                decalKind == ObjectTerrainDecalKind::HordeVehicle ||
                decalKind == ObjectTerrainDecalKind::HordeNationalismVehicle ||
                (decalKind == ObjectTerrainDecalKind::HordeFanaticism &&
                 !hasObjectKind(kinds, game::ObjectKindOf::Infantry));
            const bool crate = decalKind == ObjectTerrainDecalKind::Crate;

            container::StringView texture;
            switch (decalKind) {
            case ObjectTerrainDecalKind::Demoralized: texture = "DM_RING"; break;
            case ObjectTerrainDecalKind::HordeInfantry: texture = "EXHorde"; break;
            case ObjectTerrainDecalKind::HordeNationalismInfantry:
                texture = "EXHorde_UP"; break;
            case ObjectTerrainDecalKind::HordeVehicle: texture = "EXHordeB"; break;
            case ObjectTerrainDecalKind::HordeNationalismVehicle:
                texture = "EXHordeB_UP"; break;
            case ObjectTerrainDecalKind::HordeFanaticism:
                texture = "EXHordeC_UP"; break;
            case ObjectTerrainDecalKind::Crate: texture = "EXJunkCrate"; break;
            case ObjectTerrainDecalKind::ChemSuit: texture = "EXChemSuit"; break;
            case ObjectTerrainDecalKind::None: break;
            }
            if (texture.empty() && fakeStructure) {
                bool visibleToObserver = terrainDecalObserverIsSpectator;
                if (terrainDecalObserver &&
                    terrainDecalObserver->isSimulationParticipant()) {
                    const PlayerRelationship relationship = owner.player
                        ? source.players.relationship(
                              terrainDecalObserver->id, owner.player)
                        : PlayerRelationship::Neutral;
                    visibleToObserver =
                        relationship == PlayerRelationship::Allies ||
                        relationship == PlayerRelationship::Neutral;
                }
                if (visibleToObserver) {
                    texture = type.archetype->templateData.shadow.texture;
                }
            }
            if (texture.empty()) continue;

            const game::ThingShadowTemplate& footprint =
                type.archetype->templateData.shadow;
            float sizeX = footprint.sizeX;
            float sizeY = footprint.sizeY;
            const ObjectGeometryComponent* geometry =
                ecs::try_get<ObjectGeometryComponent>(
                    source.registry, entity);
            if (vehicleHorde) {
                if (geometry) {
                    sizeX = sizeY = 3.5f *
                        geometry->majorRadiusFixed.to_float();
                }
            } else if (crate) {
                if (geometry) {
                    sizeX = sizeY = 2.5f *
                        geometry->majorRadiusFixed.to_float();
                }
            }
            // addDecal(RenderObjClass*, ...) resolves an authored zero axis
            // from the render object's object-space bounding box. Geometry is
            // the detached gameplay approximation; boundingRadius is the
            // final fallback for render-only fixtures without GeometryInfo.
            const float fallbackSizeX = geometry
                ? 2.0f * geometry->majorRadiusFixed.to_float()
                : visual ? 2.0f * visual->boundingRadius : 0.0f;
            const float fallbackSizeY = geometry
                ? 2.0f * geometry->minorRadiusFixed.to_float()
                : visual ? 2.0f * visual->boundingRadius : 0.0f;
            if (sizeX == 0.0f) sizeX = fallbackSizeX;
            if (sizeY == 0.0f) sizeY = fallbackSizeY;
            if (!(sizeX > 0.0f) || !(sizeY > 0.0f) ||
                !std::isfinite(sizeX) || !std::isfinite(sizeY)) {
                continue;
            }

            const render::GroundDecalPresentationKey key{
                .source = render::GroundDecalPresentationSource::ObjectTerrain,
                .object = identity.id,
                .authoredOrder = kDrawableTerrainDecalSlot,
            };
            batch.synchronizedOwners.push_back(key);
            batch.events.push_back({
                .kind = render::GroundDecalPresentationEventKind::Begin,
                .key = key,
                .confirmedFrame = batch.confirmedFrame,
                .streamSequence = sequence++,
                .ownerPlayer = owner.player.value,
                .textureName = container::String(texture),
                .position = {transform.x, transform.y, transform.z},
                .sizeX = sizeX,
                .sizeY = sizeY,
                .offsetX = footprint.offsetX,
                .offsetY = footprint.offsetY,
                .yawRadians = transform.rotation,
                .shadowTypeMask = 0x20u,
                .minimumOpacity = 1.0f,
                .maximumOpacity = 1.0f,
                .opacityThrobFrames = 1,
                .authoritativeOpacity = decalState
                    ? std::clamp(decalState->opacity.to_float(), 0.0f, 1.0f)
                    : 1.0f,
                .hasAuthoritativeOpacity = decalState != nullptr,
                .color = {1.0f, 1.0f, 1.0f, 1.0f},
                .onlyVisibleToOwningPlayer = false,
                .requiresDrawIconUi = hordeDecal,
            });
        }
    }
    return batch;
}


} // namespace engine
