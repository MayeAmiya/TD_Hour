#include "core/container/container_types.h"
#include "core/container/hash_containers.h"
#include "GameRenderExtraction.h"
#include "GameRenderExtractionChannelAnimation.h"
#include "GameRenderExtractionChannelPostProcess.h"
#include "GameRenderExtractionChannelPolicy.h"
#include "GameRenderExtractionChannelState.h"
#include "GameRenderExtractionEntityEffects.h"
#include "GameRenderExtractionGarrisonPresentation.h"
#include "GameRenderExtractionJetLockonPresentation.h"
#include "GameRenderExtractionEntitySource.h"
#include "GameRenderExtractionObjectUi.h"

#include "game/object/component/ObjectComponentsCombatWeaponProjectileArmor.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/object/contracts/ObjectDeathReaction.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingModelRecipe.h"
#include "game/object/definition/ThingObjectRecipe.h"
#include "game/object/definition/W3dPristineBoneCatalog.h"
#include "game/player/FactionTemplate.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/query/GameSessionRulesetQueryPort.h"
#include "presentation/render/SupportDrawPresentation.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include "GameRenderExtractionDetail.h"

namespace engine {
using namespace render_extraction_detail;
namespace {

struct RenderCullingBounds final {
    render::RenderVector centerOffset{};
    float radius = 0.0f;
};

[[nodiscard]] RenderCullingBounds conservativeRenderCullingBounds(
    const ObjectGeometryComponent* geometry,
    float fallbackRootRadius) noexcept {
    const float fallback = std::isfinite(fallbackRootRadius)
        ? std::max(0.0f, fallbackRootRadius) : 0.0f;
    if (!geometry) return {.radius = fallback};

    const auto finiteNonnegative = [](float value) noexcept {
        return std::isfinite(value) ? std::max(0.0f, value) : 0.0f;
    };
    const float major = finiteNonnegative(
        geometry->majorRadiusFixed.to_float());
    const float minor = finiteNonnegative(
        geometry->minorRadiusFixed.to_float());
    const float height = finiteNonnegative(
        geometry->heightFixed.to_float());
    const float authoredSphere = finiteNonnegative(
        geometry->boundingSphereRadiusFixed.to_float());

    RenderCullingBounds result;
    switch (geometry->shape) {
    case ObjectGeometryShape::Sphere:
        result.radius = std::max({major, authoredSphere, fallback});
        break;
    case ObjectGeometryShape::Cylinder: {
        const float halfHeight = height * 0.5f;
        result.centerOffset = {0.0f, 0.0f, halfHeight};
        const float strictRadius = std::hypot(major, halfHeight);
        result.radius = std::max({
            strictRadius, authoredSphere,
            fallback + halfHeight});
        break;
    }
    case ObjectGeometryShape::Box: {
        const float halfHeight = height * 0.5f;
        result.centerOffset = {0.0f, 0.0f, halfHeight};
        const float horizontalRadius = std::hypot(major, minor);
        const float strictRadius = std::hypot(
            horizontalRadius, halfHeight);
        result.radius = std::max({
            strictRadius, authoredSphere,
            fallback + halfHeight});
        break;
    }
    }
    if (!std::isfinite(result.radius)) {
        result.centerOffset = {};
        result.radius = fallback;
    }
    return result;
}


} // namespace

void GameRenderExtraction::extractEntitiesAndUi(
    const GameRenderEntityExtractionSource& source,
    render::WorldRenderSnapshot& snapshot,
    uint64_t simulationFrame,
    container::Span<const ObjectId> localSelection,
    ObjectId localHover,
    bool includeVisualAssetDependencies,
    container::Span<const ObjectId> objectFilter,
    bool filterObjects,
    container::Vector<render::TacticalRadarEventRenderSnapshot>*
        rawGameplayRadarCandidates) {
    const GameSessionWorldState& world = source.world;
    const GameSessionContentStartState& contentState = source.content;
    const GameSessionScriptPresentationState& presentation =
        source.presentation;
    const GameSessionObjectEventState& objectEvents = source.objectEvents;
    const ecs::registry& registry = world.m_registry;
    const ObjectLifecycle& lifecycle = world.m_objects;
    const PlayerList& players = contentState.m_players;
    const GameSessionRulesetQueryPort ruleset{
        contentState.m_ruleset.get()};
    const RenderFeatureQualitySettings featureQuality =
        snapshot.renderFeatureQuality
        ? snapshot.renderFeatureQuality->requested
        : RenderFeatureQualitySettings{};
    const RenderBodyParticleGameData& bodyParticleSettings =
        presentation.m_renderGameDataSettings.visual.bodyParticles;
    const PlayerState* radarObserver = players.localPlayer();
    const uint64_t radarEpoch = presentation.m_scriptPresentationEpoch;
    const GameContentSnapshot& content =
        contentState.m_contentSnapshot;
    const WeaponPresentationSource weaponPresentationSource{
        .content = content,
        .cacheOwner = source.cacheOwner,
        .presentationEpoch = radarEpoch,
    };
    const auto objects = ecs::view<const ObjectIdentityComponent, const TransformComponent,
                                   const RenderModelComponent>(registry);
    thread_local container::Vector<ecs::entity> extractionEntities;
    extractionEntities.clear();
    if (filterObjects) {
        extractionEntities.reserve(objectFilter.size());
        for (const ObjectId object : objectFilter) {
            const std::optional<ecs::entity> entity =
                lifecycle.entityFromId(object);
            if (entity) extractionEntities.push_back(*entity);
        }
    } else {
        extractionEntities.reserve(objects.size_hint());
        for (const ecs::entity entity : objects)
            extractionEntities.push_back(entity);
    }
    const script::ScriptInfantryLightingPresentationState& infantryLighting =
        presentation.m_scriptInfantryLightingPresentation;
    const script::ScriptObjectPresentationState& objectPresentation =
        presentation.m_scriptObjectPresentation;
    const float infantryDirectionalLightScale =
        infantryLighting.overrideScale && std::isfinite(*infantryLighting.overrideScale) &&
                *infantryLighting.overrideScale > 0.0f
            ? *infantryLighting.overrideScale
            : 1.0f;
    const size_t extractionCapacity = filterObjects
        ? objectFilter.size() : objects.size_hint();
    snapshot.entities.reserve(extractionCapacity);
    snapshot.animationEndpointAdmissions.reserve(extractionCapacity);
    snapshot.objectUi.objects.reserve(extractionCapacity);
    snapshot.projectiles.reserve(extractionCapacity / 4u + 1u);
    container::HashSet<container::String> dependencyKeys;
    if (includeVisualAssetDependencies) {
        dependencyKeys.reserve(objects.size_hint() * 2u);
    }
    container::Vector<render::TacticalRadarEventRenderSnapshot>
        gameplayRadarCandidates;
    gameplayRadarCandidates.reserve(objects.size_hint() / 8u + 1u);
    if (radarObserver && radarObserver->isSimulationParticipant() &&
        objectEvents.m_upgradeRadarEpoch ==
            radarEpoch) {
        const uint64_t radarLifetime =
            static_cast<uint64_t>(snapshot.objectUi.logicFramesPerSecond) *
            4u;
        const uint64_t radarFade = std::max<uint64_t>(
            1u, static_cast<uint64_t>(
                snapshot.objectUi.logicFramesPerSecond) / 2u);
        for (const auto& event : objectEvents.m_upgradeRadarHistory) {
            if (event.player != radarObserver->id ||
                simulationFrame < event.confirmedTick ||
                simulationFrame - event.confirmedTick > radarLifetime) {
                continue;
            }
            const uint64_t dieTick = event.confirmedTick >
                    std::numeric_limits<uint64_t>::max() - radarLifetime
                ? std::numeric_limits<uint64_t>::max()
                : event.confirmedTick + radarLifetime;
            gameplayRadarCandidates.push_back({
                .eventIdentity = radarEventIdentity(
                    event.producer.value ^
                        (static_cast<uint64_t>(event.sourceSequence) << 32u),
                    event.confirmedTick, 2),
                .sourceObjectId = event.producer.value,
                .worldPosition = event.position,
                .eventType = 2,
                .createTick = event.confirmedTick,
                .fadeTick = dieTick > radarFade
                    ? dieTick - radarFade : event.confirmedTick,
                .dieTick = dieTick,
            });
        }
    }
    for (const ecs::entity entity : extractionEntities) {
        const ObjectIdentityComponent* identityValue =
            ecs::try_get<ObjectIdentityComponent>(registry, entity);
        const TransformComponent* transformValue =
            ecs::try_get<TransformComponent>(registry, entity);
        const RenderModelComponent* visualValue =
            ecs::try_get<RenderModelComponent>(registry, entity);
        if (!identityValue || !transformValue || !visualValue) continue;
        const auto& identity = *identityValue;
        const auto& transform = *transformValue;
        const auto& visual = *visualValue;
        if (identity.id == INVALID_OBJECT_ID) continue;

        const OwnerComponent* owner =
            ecs::try_get<OwnerComponent>(registry, entity);
        const PlayerState* localObserver = players.localPlayer();
        const bool hasSimulationObserver = localObserver &&
            localObserver->isSimulationParticipant();
        const bool hasLocalObserver = localObserver &&
            !localObserver->isNeutral();
        const bool localObserverIsSpectator = localObserver &&
            (localObserver->participation ==
                 PlayerParticipationKind::Observer ||
             localObserver->controller == PlayerControllerKind::Observer);
        const PlayerId observerId = hasSimulationObserver
            ? localObserver->id : INVALID_PLAYER_ID;
        const bool alliedToObserver = hasSimulationObserver && owner &&
            players.relationship(observerId, owner->player) ==
                PlayerRelationship::Allies;
        const render::RenderVector objectPosition{transform.x, transform.y, transform.z};
        const ObjectGeometryComponent* objectGeometry =
            ecs::try_get<ObjectGeometryComponent>(registry, entity);
        const ObjectKindOfComponent* kinds =
            ecs::try_get<ObjectKindOfComponent>(registry, entity);
        const ThingTemplateComponent* templateComponent =
            ecs::try_get<ThingTemplateComponent>(registry, entity);
        const ObjectWeaponComponent* weapons =
            ecs::try_get<ObjectWeaponComponent>(registry, entity);
        const ObjectHealthComponent* health =
            ecs::try_get<ObjectHealthComponent>(registry, entity);
        const ObjectStatusComponent* status =
            ecs::try_get<ObjectStatusComponent>(registry, entity);
        const game::ThingTemplate* templateData =
            templateComponent && templateComponent->archetype
                ? &templateComponent->archetype->templateData : nullptr;
        // Endpoint admission is a live Draw-channel fact, not a visible-mesh
        // fact. Publish it before contained/beacon/LOD/Model=None filters so a
        // hidden channel cannot replay an old transient state when it later
        // becomes drawable again.
        const size_t admissionChannelCount = templateData &&
                !templateData->drawVisualChannels.empty()
            ? templateData->drawVisualChannels.size() : 1u;
        for (size_t channelIndex = 0;
             channelIndex < admissionChannelCount; ++channelIndex) {
            const uint64_t generation = channelIndex < visual.channels.size()
                ? visual.channels[channelIndex].animationStateGeneration
                : channelIndex == 0 ? visual.animationStateGeneration : 0u;
            if (generation == 0 || channelIndex > UINT32_MAX) continue;
            snapshot.animationEndpointAdmissions.push_back({
                .objectId = identity.id.value,
                .channelIndex = static_cast<uint32_t>(channelIndex),
                .generation = generation,
            });
        }
        const bool hasBeaconClient = templateComponent &&
            templateComponent->archetype &&
            templateComponent->archetype->techBuildingPlan &&
            !templateComponent->archetype->techBuildingPlan->beacons.empty();
        // Hiding the beacon model is an observer-relative presentation rule.
        // A headless extraction has no observer whose alliance can be tested;
        // it may suppress local radar/smoke consumers, but must not classify
        // the object as an enemy beacon and erase its Draw channels.
        const bool beaconHiddenFromLocalObserver = hasBeaconClient &&
            hasLocalObserver &&
            !render::beaconVisibleToObserver(
                hasLocalObserver,
                alliedToObserver || localObserverIsSpectator,
                visual.hidden);
        const RenderCullingBounds objectCullingBounds =
            conservativeRenderCullingBounds(
                objectGeometry, visual.boundingRadius);
        const float visibilityRadius = objectCullingBounds.radius;
        const render::RenderVector visibilityCenter =
            objectPosition + objectCullingBounds.centerOffset;
        const bool outsidePlayableBounds =
            !snapshot.localVisibility.isInsidePlayableBounds(
                visibilityCenter);
        const render::LocalVisibilityRenderCellState localVisibilityState =
            outsidePlayableBounds
                ? render::LocalVisibilityRenderCellState::Shrouded
                : snapshot.localVisibility.enabled && !alliedToObserver
                    ? snapshot.localVisibility.worldStateSphere(
                          visibilityCenter, visibilityRadius)
                    : render::LocalVisibilityRenderCellState::Visible;
        const bool hiddenByLocalVisibility = localVisibilityState !=
            render::LocalVisibilityRenderCellState::Visible;
        const PlayerState* dependencyOwner = owner
            ? players.get(owner->player) : nullptr;
        const bool constructionLifecycleActive = status && status->hasAny(
            game::objectStatusBit(
                game::ObjectStatusFlag::UnderConstruction) |
            game::objectStatusBit(game::ObjectStatusFlag::Sold));
        const bool mayPublishDependencies = templateData && dependencyOwner &&
            dependencyOwner->isPlayableSide() &&
            (alliedToObserver || !hiddenByLocalVisibility);
        if (includeVisualAssetDependencies && mayPublishDependencies) {
            appendThingVisualAssetDependencies(
                snapshot, dependencyKeys, *templateData,
                visual.modelConditionFlags);
            appendWeaponProjectileVisualAssetDependencies(
                snapshot, dependencyKeys, content, weapons,
                visual.modelConditionFlags);
            if (templateComponent && templateComponent->archetype) {
                appendSpecialPowerVisualAssetDependencies(
                    snapshot, dependencyKeys, content,
                    *templateComponent->archetype,
                    visual.modelConditionFlags);
            }
        } else if (constructionLifecycleActive &&
                   mayPublishDependencies) {
            // Runtime world snapshots normally omit the broad startup asset
            // dependency set. A newly placed/sold structure is the exception:
            // its fence, scaffold, crane, main body and reverse teardown form
            // one short-lived Draw state machine and must be requested as a
            // unit before the next condition edge arrives.
            appendThingVisualAssetDependencies(
                snapshot, dependencyKeys, *templateData,
                visual.modelConditionFlags);
        }

        const ObjectProjectileComponent* projectile =
            ecs::try_get<ObjectProjectileComponent>(registry, entity);
        appendProjectilePresentation(
            {
                .registry = registry,
                .objects = lifecycle,
                .content = content,
                .featureQuality = featureQuality,
                .localVisibility = snapshot.localVisibility,
                .logicFramesPerSecond =
                    contentState.m_startInfo.gameSpeedFPS,
            },
            entity, identity, transform, objectGeometry, projectile,
            hiddenByLocalVisibility, alliedToObserver, snapshot);

        if (hiddenByLocalVisibility) {
            ++snapshot.localVisibility.hiddenEntityCount;
        }
        const ObjectContainedByComponent* containedBy =
            ecs::try_get<ObjectContainedByComponent>(registry, entity);
        if (containedBy) {
            const bool hasVisibleContainerAttachment =
                !containedBy->enclosing && templateData &&
                std::any_of(
                    templateData->drawVisualChannels.begin(),
                    templateData->drawVisualChannels.end(),
                    [](const game::ModelDrawVisualChannel& channel) {
                        return !channel.attachToBoneInContainer.empty();
                    });
            // RefCode only clears W3DDependencyModelDraw for a non-enclosing
            // containment edge. Ordinary enclosed passengers remain off-map
            // even if malformed content happens to author an attachment bone.
            if (!hasVisibleContainerAttachment) continue;
        }
        uint64_t selectionFlashIdentity = 0;
        if (const auto flash = presentation.m_objectSelectionFlashes.find(
                identity.id);
            flash != presentation.m_objectSelectionFlashes.end() &&
            simulationFrame >= flash->second.startTick &&
            simulationFrame < flash->second.expireTick) {
            selectionFlashIdentity = flash->second.identity;
        }
        // Enclosed transport/tunnel/garrison passengers are off-world. Their
        // model was already suppressed below, but publishing ObjectUi before
        // that gate still created floating health bars, selection rings and
        // support auras at the carrier transform. RefCode has no client
        // Drawable for an enclosed passenger, so it cannot contribute UI.
        if (!containedBy || !containedBy->enclosing) {
            appendObjectUiPresentation(
                {
                    .registry = registry,
                    .lifecycle = lifecycle,
                    .players = players,
                    .ruleset = contentState.m_ruleset.get(),
                    .content = content,
                    .localSelection = localSelection,
                    .localHover = localHover,
                    .simulationFrame = simulationFrame,
                    .logicFramesPerSecond =
                        snapshot.objectUi.logicFramesPerSecond,
                },
                {
                    .entity = entity,
                    .identity = identity,
                    .transform = transform,
                    .visual = visual,
                    .owner = owner,
                    .geometry = objectGeometry,
                    .kinds = kinds,
                    .templateData = templateData,
                    .containmentPlan = templateComponent &&
                            templateComponent->archetype
                        ? templateComponent->archetype->containmentPlan.get()
                        : nullptr,
                    .weapons = weapons,
                    .health = health,
                    .worldPosition = objectPosition,
                    .visibilityRadius = visibilityRadius,
                    .localVisibilityState = localVisibilityState,
                    .selectionFlashIdentity = selectionFlashIdentity,
                    .observerId = observerId,
                    .beaconHiddenFromLocalObserver =
                        beaconHiddenFromLocalObserver,
                    .hasSimulationObserver = hasSimulationObserver,
                },
                gameplayRadarCandidates, snapshot);
        }
        // RefCode BeaconClientUpdate::hideBeacon is observer-local: it hides
        // the drawable and disables its shadow without mutating authoritative
        // object state. Keep UI extraction above, then suppress all model,
        // shadow and attached Draw channels for this local presentation.
        if (beaconHiddenFromLocalObserver) continue;
        const ObjectRenderChannelPolicy channelPolicy =
            resolveObjectRenderChannelPolicy(
                registry, entity, templateData, visual);
        const game::ModelConditionMask& presentationModelConditions =
            channelPolicy.presentationConditions;
        const uint64_t supplyCurrent = channelPolicy.supplyCurrent;
        const uint64_t supplyMaximum = channelPolicy.supplyMaximum;
        const size_t renderChannelCount = channelPolicy.channelCount;
        const bool objectReceivesDynamicLights =
            channelPolicy.receivesDynamicLights;
        const auto siblingAttachmentOffset = [
                &content, templateComponent, templateData,
                &presentationModelConditions](
                size_t selectedChannel,
                container::StringView boneName)
                -> std::optional<render::RenderVector> {
            const game::W3dPristineBoneCatalog* catalog =
                content.pristineBoneCatalog();
            if (boneName.empty() || !catalog || !catalog->isLoaded() ||
                !templateComponent || !templateComponent->archetype ||
                !templateData) {
                return std::nullopt;
            }
            size_t flattenedRuleOffset = 0;
            for (size_t channelIndex = 0;
                 channelIndex < templateData->drawVisualChannels.size();
                 ++channelIndex) {
                const game::ModelDrawVisualChannel& channel =
                    templateData->drawVisualChannels[channelIndex];
                const size_t ruleIndex =
                    game::selectModelConditionVisualRuleIndex(
                        channel, presentationModelConditions);
                if (channelIndex != selectedChannel &&
                    ruleIndex < channel.conditionVisuals.size()) {
                    if (const auto bone = catalog->find(
                            templateComponent->archetype->name,
                            flattenedRuleOffset + ruleIndex, boneName)) {
                        // The authoritative catalog returns the legacy
                        // Drawable-scale-adjusted pristine pose. Snapshot
                        // transforms carry that scale separately, so publish
                        // model-space here and let renderer composition apply
                        // the final channel scale exactly once.
                        const math::q32_32 assetScale =
                            templateData->assetScale;
                        if (assetScale == math::q32_32{}) {
                            return render::RenderVector{};
                        }
                        return render::RenderVector{
                            (bone->translation.x / assetScale).to_float(),
                            (bone->translation.y / assetScale).to_float(),
                            (bone->translation.z / assetScale).to_float(),
                        };
                    }
                }
                flattenedRuleOffset += channel.conditionVisuals.size();
            }
            return std::nullopt;
        };
        // RefCode does not retain a bib beneath every live structure. Bibs are
        // transient client feedback published by build-placement legality
        // checks (normal for the current place icon, red for invalid/nearby
        // structures). Do not infer that client state from authoritative ECS
        // objects or from ProductionUpdate exit points. The B08 renderer value
        // path remains ready; the placement controller must publish the exact
        // FactoryExitWidth/FactoryExtraBibWidth footprint into terrainBibs.
        size_t channelVisualRuleOffset = 0;
        size_t channelTransitionRuleOffset = 0;
        for (size_t channelIndex = 0;
             channelIndex < renderChannelCount; ++channelIndex) {
        const game::ModelDrawVisualChannel* channelRecipe =
            templateData && channelIndex < templateData->drawVisualChannels.size()
                ? &templateData->drawVisualChannels[channelIndex] : nullptr;
        // Preserve the old fallback-channel identity: channels that have no
        // authored recipe used offset zero rather than inheriting the sum of
        // the last authored channel.
        const size_t currentChannelVisualRuleOffset =
            channelRecipe ? channelVisualRuleOffset : 0;
        const size_t currentChannelTransitionRuleOffset =
            channelRecipe ? channelTransitionRuleOffset : 0;
        if (channelRecipe) {
            channelVisualRuleOffset += channelRecipe->conditionVisuals.size();
            channelTransitionRuleOffset += channelRecipe->transitions.size();
        }
        if (channelRecipe && !allowsDrawModuleAtStaticLod(
                featureQuality.staticLod,
                static_cast<uint8_t>(channelRecipe->minimumLod),
                featureQuality.useDrawModuleLod)) {
            continue;
        }
        if (channelRecipe && equalsInsensitive(
                channelRecipe->sourceModuleClass, "W3DDebrisDraw")) {
            const DebrisDrawPresentationComponent* debris =
                ecs::try_get<DebrisDrawPresentationComponent>(
                    registry, entity);
            if (debris && !allowsDrawModuleAtStaticLod(
                    featureQuality.staticLod,
                    debris->minimumLod,
                    featureQuality.useDrawModuleLod)) {
                continue;
            }
        }
        const container::StringView archetypeName =
            templateComponent && templateComponent->archetype
            ? container::StringView{templateComponent->archetype->name}
            : container::StringView{};
        const size_t normalPoseRuleCount = templateData
            ? templateData->modelConditionVisuals.size() : 0;
        const RenderChannelRuntimeProjection channelRuntime =
            projectRenderChannelRuntime(
                visual, channelRecipe, templateData,
                presentationModelConditions, channelIndex);
        const auto* visualRules = channelRuntime.visualRules;
        const auto* transitionRules = channelRuntime.transitionRules;
        const game::ModelConditionMask& channelPresentedConditions =
            *channelRuntime.presentedConditions;
        const container::String& channelAnimationState =
            *channelRuntime.animationState;
        const game::ModelConditionMask&
            channelWaitingSourceConditionSnapshot =
                *channelRuntime.waitingSourceConditions;
        const game::ModelConditionMask&
            channelAnimationStartSourceConditionSnapshot =
                *channelRuntime.animationStartSourceConditions;
        const float channelAnimationTimeSeconds =
            channelRuntime.animationTimeSeconds;
        const float channelAnimationRate = channelRuntime.animationRate;
        const game::ModelAnimationMode channelAnimationMode =
            channelRuntime.animationMode;
        const uint32_t channelAnimationManualFrame =
            channelRuntime.animationManualFrame;
        const bool channelAnimationPaused = channelRuntime.animationPaused;
        const uint64_t channelAnimationStateEnterTick =
            channelRuntime.animationStateEnterTick;
        const uint32_t channelResolvedVisualRuleIndex =
            channelRuntime.resolvedVisualRuleIndex;
        const uint32_t channelActiveTransitionRuleIndex =
            channelRuntime.activeTransitionRuleIndex;
        const uint32_t channelWaitingSourceVisualRuleIndex =
            channelRuntime.waitingSourceVisualRuleIndex;
        const uint64_t channelAnimationStateGeneration =
            channelRuntime.animationStateGeneration;
        const uint8_t channelAnimationCompletionMask =
            channelRuntime.animationCompletionMask;
        const uint64_t channelAnimationResourcePendingGeneration =
            channelRuntime.animationResourcePendingGeneration;
        const uint8_t channelAnimationResourcePendingPhase =
            channelRuntime.animationResourcePendingPhase;
        const uint32_t channelAnimationCandidateOverrideIndex =
            channelRuntime.animationCandidateOverrideIndex;
        const uint64_t channelAnimationCandidateOverrideGeneration =
            channelRuntime.animationCandidateOverrideGeneration;
        const VisualAnimationStartKind channelAnimationStartKind =
            channelRuntime.animationStartKind;
        const float channelAnimationRandomStartFraction =
            channelRuntime.animationRandomStartFraction;
        const uint32_t channelAnimationStartSourceVisualRuleIndex =
            channelRuntime.animationStartSourceVisualRuleIndex;
        const float channelAnimationStartSourceTimeSeconds =
            channelRuntime.animationStartSourceTimeSeconds;
        const uint64_t channelAnimationStartSourceGeneration =
            channelRuntime.animationStartSourceGeneration;
        render::RenderEntitySnapshot output;
        output.id = renderInstanceId(identity.id.value, channelIndex);
        output.objectId = identity.id.value;
        output.channelIndex = static_cast<uint32_t>(channelIndex);
        output.boundingRadius = objectCullingBounds.radius;
        output.cullingCenterOffset = objectCullingBounds.centerOffset;
        if (const ObjectLifecycleComponent* lifecycle =
                ecs::try_get<ObjectLifecycleComponent>(
                    registry, entity);
            lifecycle && lifecycle->createdAtTick == simulationFrame) {
            output.interpolationDisabled = true;
        }
        if (channelRecipe &&
            !channelRecipe->attachToBoneInAnotherModule.empty())
        {
            // The bone lives in a sibling Draw channel of this same object, so
            // no container relationship is involved. Publish the immutable
            // condition-state pristine offset instead of a live bone pose.
            output.attachToBoneInAnotherModule =
                channelRecipe->attachToBoneInAnotherModule;
            output.attachToBoneInAnotherModuleOffset =
                siblingAttachmentOffset(
                    channelIndex,
                    channelRecipe->attachToBoneInAnotherModule);
        }
        if (channelRecipe && !channelRecipe->attachToBoneInContainer.empty())
        {
            if (containedBy && !containedBy->enclosing &&
                containedBy->container)
            {
                output.containerObjectId = containedBy->container.value;
                output.attachToBoneInContainer = channelRecipe->attachToBoneInContainer;
            }
        }
        output.localVisibilityState = localVisibilityState;
        output.hiddenByLocalVisibility = hiddenByLocalVisibility;
        output.visual.receivesLocalVisibility =
            snapshot.localVisibility.enabled && !alliedToObserver;
        if (outsidePlayableBounds) {
            // Border clipping has no two-second grace and no ghost memory.
            // A hard-hidden policy fails closed before packet/particle
            // preparation, including for friendly and allied staging objects.
            output.localVisibilityMemoryPolicy =
                render::RenderLocalVisibilityMemoryPolicy::HardHidden;
            output.localVisibilityPersistenceTicks = 0;
        } else if (snapshot.localVisibility.enabled && !alliedToObserver) {
            const bool staticGhostEligible =
                (hasObjectKind(kinds, game::ObjectKindOf::Immobile) ||
                 hasObjectKind(kinds, game::ObjectKindOf::Structure)) &&
                !hasObjectKind(kinds, game::ObjectKindOf::Mine);
            output.localVisibilityMemoryPolicy = staticGhostEligible
                ? render::RenderLocalVisibilityMemoryPolicy::StaticGhost
                : render::RenderLocalVisibilityMemoryPolicy::Timed;
            const uint32_t framesPerSecond = static_cast<uint32_t>(
                std::max(1, contentState.m_startInfo.gameSpeedFPS));
            const uint32_t persistenceSeconds =
                health && health->effectivelyDead ? 5u : 2u;
            output.localVisibilityPersistenceTicks =
                framesPerSecond >
                    std::numeric_limits<uint32_t>::max() /
                        persistenceSeconds
                ? std::numeric_limits<uint32_t>::max()
                : framesPerSecond * persistenceSeconds;
        }
        output.visual.animationManualFrame = channelAnimationManualFrame;
        output.visual.animationCompletionMask =
            channelAnimationCompletionMask;
        output.visual.animationResourcePendingGeneration =
            channelAnimationResourcePendingGeneration;
        output.visual.animationResourcePendingPhase =
            channelAnimationResourcePendingPhase;
        for (size_t slot = 0;
             slot < output.weaponLaunchBoneSequenceOrdinals.size(); ++slot) {
            output.weaponLaunchBoneSequenceOrdinals[slot] =
                visual.lastWeaponFireSequences[slot];
        }
        if (channelRecipe) {
            const size_t logicVisualIndex =
                game::selectModelConditionVisualRuleIndex(
                    *channelRecipe, presentationModelConditions);
            if (logicVisualIndex < channelRecipe->conditionVisuals.size()) {
                output.weaponLaunchBones = typedWeaponLaunchBones(
                    channelRecipe->conditionVisuals[
                        logicVisualIndex].weaponBones);
            }
        } else if (templateData) {
            output.weaponLaunchBones = extractWeaponLaunchBones(
                *templateData, presentationModelConditions);
        }
        if (templateData) {
            static_assert(game::thingShadowBit(game::ThingShadowFlag::Decal) ==
                          render::renderShadowBit(render::RenderShadowFlag::Decal));
            static_assert(game::thingShadowBit(game::ThingShadowFlag::Volume) ==
                          render::renderShadowBit(render::RenderShadowFlag::Volume));
            static_assert(game::thingShadowBit(game::ThingShadowFlag::Projection) ==
                          render::renderShadowBit(render::RenderShadowFlag::Projection));
            static_assert(game::thingShadowBit(game::ThingShadowFlag::DynamicProjection) ==
                          render::renderShadowBit(render::RenderShadowFlag::DynamicProjection));
            static_assert(game::thingShadowBit(game::ThingShadowFlag::DirectionalProjection) ==
                          render::renderShadowBit(render::RenderShadowFlag::DirectionalProjection));
            static_assert(game::thingShadowBit(game::ThingShadowFlag::AlphaDecal) ==
                          render::renderShadowBit(render::RenderShadowFlag::AlphaDecal));
            static_assert(game::thingShadowBit(game::ThingShadowFlag::AdditiveDecal) ==
                          render::renderShadowBit(render::RenderShadowFlag::AdditiveDecal));
            output.shadow = {
                .typeMask = templateData->shadow.typeMask,
                .textureName = templateData->shadow.texture,
                .sizeX = templateData->shadow.sizeX,
                .sizeY = templateData->shadow.sizeY,
                .offsetX = templateData->shadow.offsetX,
                .offsetY = templateData->shadow.offsetY,
            };
            if (ecs::try_get<ObjectShadowSuppressionComponent>(
                    registry, entity)) {
                output.shadow.typeMask = 0;
            }
        }
        output.modelAsset = channelRecipe
            ? channelRecipe->defaultModel : visual.modelAsset;
        output.visual.receivesDynamicLights =
            objectReceivesDynamicLights;
        if (owner) {
            // RefCode always supplies an owning-player colour to W3DModelDraw.
            // Lobby players use MultiplayerColor; campaign/map-only players
            // use PlayerTemplate.PreferredColor.
            output.visual.hasScriptIndicatorColor = true;
            output.visual.scriptIndicatorColor = {1.0f, 1.0f, 1.0f};
            const PlayerState* owningPlayer =
                players.get(owner->player);
            if (owningPlayer) {
                const std::optional<PlayerRgbColor> owningColor =
                    ruleset.presentationColor(*owningPlayer);
                if (owningColor) {
                    output.visual.scriptIndicatorColor = {
                        static_cast<float>(owningColor->red) / 255.0f,
                        static_cast<float>(owningColor->green) / 255.0f,
                        static_cast<float>(owningColor->blue) / 255.0f,
                    };
                }
            }
        }
        if (hasObjectKind(kinds, game::ObjectKindOf::Infantry)) {
            output.directionalLightScale = infantryDirectionalLightScale;
        }
        const ChannelAnimationResult animation =
            applyRenderChannelAnimation(
                {
                    .registry = registry,
                    .weaponPresentation = weaponPresentationSource,
                    .objectPresentation = objectPresentation,
                    .featureQuality = featureQuality,
                    .simulationFrame = simulationFrame,
                    .logicFramesPerSecond =
                        contentState.m_startInfo.gameSpeedFPS,
                },
                {
                    .entity = entity,
                    .channelIndex = channelIndex,
                    .identity = identity,
                    .visual = visual,
                    .recipe = channelRecipe,
                    .templateData = templateData,
                    .weapons = weapons,
                    .runtime = channelRuntime,
                    .presentationConditions =
                        presentationModelConditions,
                    .archetypeName = archetypeName,
                    .normalPoseRuleCount = normalPoseRuleCount,
                    .visualRuleOffset =
                        currentChannelVisualRuleOffset,
                    .transitionRuleOffset =
                        currentChannelTransitionRuleOffset,
                    .supplyCurrent = supplyCurrent,
                    .supplyMaximum = supplyMaximum,
                },
                output);
        const float animationRuleRateFactor =
            animation.ruleRateFactor;
        const float authoredAssetScale =
            animation.authoredAssetScale;
        // A Draw module with Model=None remains a live presentation channel
        // so later condition changes can activate it, but it emits no mesh in
        // this sealed frame.
        if (!animation.drawable) continue;
        if (const ObjectSubObjectVisibilityOverrideComponent* upgradedVisibility =
                ecs::try_get<ObjectSubObjectVisibilityOverrideComponent>(
                    registry, entity)) {
            const auto appendUpgradeOverrides = [upgradedVisibility](
                    container::Vector<render::RenderSubObjectVisibility>& destination) {
                destination.reserve(destination.size() +
                                    upgradedVisibility->entries.size());
                for (const ObjectSubObjectVisibilityOverride& entry :
                     upgradedVisibility->entries) {
                    if (!entry.active || entry.name.empty()) continue;
                    destination.push_back({
                        .name = entry.name,
                        .visible = entry.visible,
                    });
                }
            };
            // Upgrade overrides are deliberately appended last. The shared
            // renderer applies last-match-wins, so ConditionState, animation
            // visibility channels and weapon flashes cannot undo them.
            appendUpgradeOverrides(output.visual.subObjectVisibility);
            if (output.animationCompletionTarget) {
                appendUpgradeOverrides(
                    output.animationCompletionTarget->subObjectVisibility);
            }
            if (output.animationFinalTarget) {
                appendUpgradeOverrides(
                    output.animationFinalTarget->subObjectVisibility);
            }
        }
        appendFinalizedRenderChannel(
            {
                .registry = registry,
                .localSelection = localSelection,
                .objectPresentation = objectPresentation,
                .trackMarks = presentation.m_trackMarksPresentationSettings,
                .bodyParticles = bodyParticleSettings,
                .simulationFrame = simulationFrame,
                .logicFramesPerSecond =
                    contentState.m_startInfo.gameSpeedFPS,
            },
            {
                .entity = entity,
                .channelIndex = channelIndex,
                .identity = identity,
                .transform = transform,
                .visual = visual,
                .projectile = projectile,
                .kinds = kinds,
                .health = health,
                .status = status,
                .geometry = objectGeometry,
                .templateData = templateData,
                .presentedConditions = channelPresentedConditions,
                .animationState = channelAnimationState,
                .animationMode = channelAnimationMode,
                .animationTimeSeconds = channelAnimationTimeSeconds,
                .animationRate = channelAnimationRate,
                .animationRuleRateFactor = animationRuleRateFactor,
                .authoredAssetScale = authoredAssetScale,
                .animationStateEnterTick =
                    channelAnimationStateEnterTick,
                .animationPaused = channelAnimationPaused,
                .hasSimulationObserver = hasSimulationObserver,
                .alliedToObserver = alliedToObserver,
                .adjustHeightByConstructionPercent =
                    animation.adjustHeightByConstructionPercent,
            },
            std::move(output), snapshot);
        }
        appendGarrisonGunPresentation(
            registry, content, entity, identity.id,
            localVisibilityState, hiddenByLocalVisibility,
            simulationFrame,
            static_cast<uint32_t>(std::max(
                1, contentState.m_startInfo.gameSpeedFPS)),
            snapshot);
        appendJetLockonPresentation(
            registry, content, entity,
            localVisibilityState, hiddenByLocalVisibility,
            simulationFrame,
            static_cast<uint32_t>(std::max(
                1, contentState.m_startInfo.gameSpeedFPS)),
            snapshot);
    }
    if (rawGameplayRadarCandidates) {
        rawGameplayRadarCandidates->insert(
            rawGameplayRadarCandidates->end(),
            std::make_move_iterator(gameplayRadarCandidates.begin()),
            std::make_move_iterator(gameplayRadarCandidates.end()));
    } else {
        updateGameplayRadarHistoryAndAppend(
            source.gameplayRadarHistory,
            source.gameplayRadarEpoch,
            radarEpoch,
            std::move(gameplayRadarCandidates),
            snapshot.tacticalRadar.events.mutableValues(),
            simulationFrame, snapshot.objectUi.logicFramesPerSecond);
    }
    if (snapshot.tacticalRadar.events.size() >
        script::ScriptMapPresentationState::kMaximumRadarEvents) {
        auto& events = snapshot.tacticalRadar.events.mutableValues();
        events.erase(
            events.begin(),
            events.end() -
                script::ScriptMapPresentationState::kMaximumRadarEvents);
    }
}

} // namespace engine
