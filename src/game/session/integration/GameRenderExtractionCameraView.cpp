#include "core/container/container_types.h"
#include "GameRenderExtraction.h"
#include "GameRenderExtractionDetail.h"

#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/session/state/GameSessionDomainState.h"
#include "presentation/render/SupportDrawPresentation.h"
#include "core/config/GlobalData.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace engine {
using namespace render_extraction_detail;
namespace {

[[nodiscard]] render::ScreenFadeBlendMode toRenderScreenFadeBlendMode(
    script::ScriptScreenFadeBlendMode mode) noexcept {
    switch (mode) {
    case script::ScriptScreenFadeBlendMode::Add:
        return render::ScreenFadeBlendMode::Add;
    case script::ScriptScreenFadeBlendMode::Subtract:
        return render::ScreenFadeBlendMode::Subtract;
    case script::ScriptScreenFadeBlendMode::Saturate:
        return render::ScreenFadeBlendMode::Saturate;
    case script::ScriptScreenFadeBlendMode::Multiply:
        return render::ScreenFadeBlendMode::Multiply;
    case script::ScriptScreenFadeBlendMode::Count:
        break;
    }
    return render::ScreenFadeBlendMode::None;
}

[[nodiscard]] uint8_t quantizeLegacyScreenFadeIntensity(float intensity) noexcept {
    // W3DStatusCircle writes `Int clr = 255 * intensity` into an ARGB
    // diffuse value. Valid authored maps use 0..1, where truncation (rather
    // than rounding) is observable. Keep raw values in GameSession for
    // diagnostics/compatibility, but clamp only at the safe UNORM GPU edge.
    if (!std::isfinite(intensity)) return 0;
    const float clamped = std::clamp(intensity, 0.0f, 1.0f);
    return static_cast<uint8_t>(static_cast<uint32_t>(clamped * 255.0f));
}

[[nodiscard]] render::ScreenShakeRenderIntensity toRenderScreenShakeIntensity(
    script::ScriptScreenShakeIntensity intensity) noexcept {
    using Source = script::ScriptScreenShakeIntensity;
    using Target = render::ScreenShakeRenderIntensity;
    switch (intensity) {
    case Source::Subtle: return Target::Subtle;
    case Source::Normal: return Target::Normal;
    case Source::Strong: return Target::Strong;
    case Source::Severe: return Target::Severe;
    case Source::CineExtreme: return Target::CineExtreme;
    case Source::CineInsane: return Target::CineInsane;
    case Source::Count: break;
    }
    return Target::Count;
}

[[nodiscard]] render::MotionBlurRenderMode toRenderMotionBlurMode(
    script::ScriptMotionBlurMode mode) noexcept {
    using Source = script::ScriptMotionBlurMode;
    using Target = render::MotionBlurRenderMode;
    switch (mode) {
    case Source::ZoomIn: return Target::ZoomIn;
    case Source::ZoomOut: return Target::ZoomOut;
    case Source::ZoomJump: return Target::ZoomJump;
    case Source::Follow: return Target::Follow;
    case Source::EndFollow: return Target::EndFollow;
    case Source::Count: break;
    }
    return Target::Count;
}

[[nodiscard]] render::LocalVisibilityRenderSnapshot extractLocalVisibility(
    const void* sessionIdentity,
    container::SharedPtr<const game::terrain::MapVisibilitySnapshot> source,
    const PlayerList& players,
    const PlayerState* observer,
    uint64_t presentationEpoch) {
    render::LocalVisibilityRenderSnapshot output;
    // Observer/replay policy remains explicit: without a local simulation
    // participant the renderer sees the full map until spectator rules are
    // implemented. Normal matches also preserve current presentation until
    // script/dynamic visibility actually activates the authority.
    if (!source || !source->renderingActive || !observer ||
        !observer->isSimulationParticipant() || source->revision == 0 ||
        source->width <= 0 || source->height <= 0) {
        return output;
    }
    const size_t width = static_cast<size_t>(source->width);
    const size_t height = static_cast<size_t>(source->height);
    if (width > std::numeric_limits<size_t>::max() / height) return output;
    const size_t cellCount = width * height;

    struct VisibilityProjectionCache final {
        const void* owner = nullptr;
        const game::terrain::MapVisibilitySnapshot* source = nullptr;
        uint64_t presentationEpoch = 0;
        uint64_t sourceRevision = 0;
        uint64_t policyRevision = 0;
        uint8_t observerPlayer = UINT8_MAX;
        render::LocalVisibilityRenderSnapshot snapshot;
    };
    thread_local VisibilityProjectionCache cache;
    const uint64_t policyRevision = observer->revisions.diplomacy;
    if (cache.owner == sessionIdentity && cache.source == source.get() &&
        cache.presentationEpoch == presentationEpoch &&
        cache.sourceRevision == source->revision &&
        cache.policyRevision == policyRevision &&
        cache.observerPlayer == observer->id.value) {
        return cache.snapshot;
    }

    output.presentationEpoch = presentationEpoch;
    output.revision = source->revision;
    output.policyRevision = policyRevision;
    output.terrainLayoutRevision = source->terrainLayoutRevision;
    output.observerPlayer = observer->id.value;
    output.width = source->width;
    output.height = source->height;
    output.borderSize = source->borderSize;
    output.originX = source->originX;
    output.originY = source->originY;
    output.cellWorldSize = source->cellWorldSize;
    output.dirtyRegion = {
        source->dirtyRegion.minX,
        source->dirtyRegion.minY,
        source->dirtyRegion.maxX,
        source->dirtyRegion.maxY,
    };
    auto cells = std::make_shared<container::Vector<uint8_t>>(
        cellCount,
        static_cast<uint8_t>(
            render::LocalVisibilityRenderCellState::Shrouded));
    auto visualLevels = std::make_shared<container::Vector<uint8_t>>(
        cellCount, uint8_t{0});

    const game::terrain::MapVisibilityPlayerSnapshot* observerVisibility =
        source->player(observer->id);
    const bool observerGridValid = observerVisibility && observerVisibility->cells &&
        observerVisibility->cells->size() == cellCount;
    // A partially initialized visibility authority must not erase the world.
    // The renderer already treats an invalid/disabled snapshot as fully
    // visible, which is the safe presentation fallback while the observer
    // grid is being created or rebuilt.  Keeping the grid fail-open here also
    // prevents a transient missing player plane from hiding every enemy
    // building and unit for the remainder of the frame.
    if (!observerGridValid) return output;
    if (observerGridValid) {
        for (size_t index = 0; index < cellCount; ++index) {
            (*cells)[index] =
                static_cast<uint8_t>((*observerVisibility->cells)[index]);
            (*visualLevels)[index] = observerVisibility->visualLevels &&
                    observerVisibility->visualLevels->size() == cellCount
                ? (*observerVisibility->visualLevels)[index]
                : ((*cells)[index] == static_cast<uint8_t>(
                        render::LocalVisibilityRenderCellState::Visible)
                    ? uint8_t{255}
                    : ((*cells)[index] == static_cast<uint8_t>(
                            render::LocalVisibilityRenderCellState::Explored)
                        ? uint8_t{127} : uint8_t{0}));
        }
    }
    for (const PlayerId player : players.activePlayerIds()) {
        if (player == observer->id) continue;
        if (players.relationship(observer->id, player) !=
            PlayerRelationship::Allies) {
            continue;
        }
        const game::terrain::MapVisibilityPlayerSnapshot* visibility =
            source->player(player);
        if (!visibility || !visibility->cells ||
            visibility->cells->size() != cellCount) {
            continue;
        }
        for (size_t index = 0; index < cellCount; ++index) {
            (*cells)[index] = std::max<uint8_t>(
                (*cells)[index],
                static_cast<uint8_t>((*visibility->cells)[index]));
            const uint8_t alliedLevel = visibility->visualLevels &&
                    visibility->visualLevels->size() == cellCount
                ? (*visibility->visualLevels)[index]
                : ((*visibility->cells)[index] ==
                        game::terrain::MapVisibilityCellState::Clear
                    ? uint8_t{255}
                    : ((*visibility->cells)[index] ==
                            game::terrain::MapVisibilityCellState::Fogged
                        ? uint8_t{127} : uint8_t{0}));
            (*visualLevels)[index] = std::max(
                (*visualLevels)[index], alliedLevel);
        }
    }
    output.sharedCells = std::move(cells);
    output.sharedVisualLevels = std::move(visualLevels);
    output.enabled = true;
    cache = {
        .owner = sessionIdentity,
        .source = source.get(),
        .presentationEpoch = presentationEpoch,
        .sourceRevision = source->revision,
        .policyRevision = policyRevision,
        .observerPlayer = observer->id.value,
        .snapshot = output,
    };
    return output;
}

} // namespace

render::RenderCameraSnapshot GameRenderExtraction::extractCamera(
    const GameCameraState& camera) noexcept {
    const GameCameraState state = camera.sanitized();
    render::RenderCameraSnapshot snapshot;
    snapshot.position = state.position;
    snapshot.visibilityDistance = state.visibilityDistance;
    snapshot.target = state.target;
    snapshot.up = state.up;
    snapshot.verticalFovRadians = state.verticalFovRadians;
    snapshot.horizontalFovRadians = state.horizontalFovRadians;
    snapshot.tacticalViewportHeightScale =
        state.tacticalViewportHeightScale;
    snapshot.nearClip = state.nearClip;
    snapshot.farClip = state.farClip;
    snapshot.fogEnabled = state.fogEnabled;
    snapshot.fogColor = state.fogColor;
    snapshot.fogStartDistance = state.fogStartDistance;
    snapshot.fogEndDistance = state.fogEndDistance;
    snapshot.cameraCutRevision = state.cameraCutRevision;
    return snapshot;
}

void GameRenderExtraction::extractViewAndVisibility(
    GameSessionScriptPresentationState& presentation,
    const GameSessionContentStartState& content,
    const GameSessionWorldState& world,
    const void* sessionIdentity,
    render::WorldRenderSnapshot& snapshot,
    render::RenderCameraSnapshot camera,
    uint64_t simulationFrame) {
    snapshot.simulationFrame = simulationFrame;
    snapshot.presentationEpoch = presentation.m_scriptPresentationEpoch;
    snapshot.renderGameDataSettings =
        presentation.m_renderGameDataSettingsSnapshot;
    snapshot.renderFeatureQuality =
        presentation.m_renderFeatureQualitySnapshot;
    const RenderFeatureQualitySettings featureQuality =
        snapshot.renderFeatureQuality
        ? snapshot.renderFeatureQuality->requested
        : RenderFeatureQualitySettings{};
    snapshot.camera = camera;
    const script::ScriptScreenFadePresentationState& fade =
        presentation.m_scriptScreenFadePresentation;
    // Publish the session epoch even after a curve finishes. This gives the
    // render consumer an explicit stale-frame fence rather than making an
    // inactive fade indistinguishable from an unrelated/debug snapshot.
    snapshot.screenFade.presentationEpoch = presentation.m_scriptPresentationEpoch;
    snapshot.screenFade.presentationSequence = fade.stamp.sequence;
    if (fade.active) {
        const render::ScreenFadeBlendMode blendMode =
            toRenderScreenFadeBlendMode(fade.blendMode);
        if (blendMode != render::ScreenFadeBlendMode::None) {
            snapshot.screenFade = {
                .presentationEpoch = presentation.m_scriptPresentationEpoch,
                .presentationSequence = fade.stamp.sequence,
                .blendMode = blendMode,
                .intensity = quantizeLegacyScreenFadeIntensity(fade.currentIntensity),
                .active = true,
            };
        }
    }
    const script::ScriptBlackAndWhitePresentationState& blackAndWhite =
        presentation.m_scriptBlackAndWhitePresentation;
    snapshot.blackAndWhite = {
        .presentationEpoch = presentation.m_scriptPresentationEpoch,
        .presentationSequence = blackAndWhite.stamp.sequence,
        .commandsTrimmedThroughSequence =
            presentation.m_scriptBlackAndWhiteJournalTrimmedThroughSequence,
        .enabled = blackAndWhite.enabled,
        .transitionFrames = blackAndWhite.transitionFrames,
    };
    // BW actions are renderer commands rather than a pure final value: a
    // Begin followed by End in one confirmed frame must arrive in source
    // order so the renderer can install BW before it decides whether End can
    // fade it out. Preserve the bounded session journal just like shake.
    const container::Vector<script::ScriptBlackAndWhitePresentationState>& blackAndWhiteJournal =
        presentation.m_scriptBlackAndWhiteJournal;
    snapshot.blackAndWhite.commands.reserve(blackAndWhiteJournal.size());
    for (const script::ScriptBlackAndWhitePresentationState& source : blackAndWhiteJournal) {
        snapshot.blackAndWhite.commands.push_back({
            .presentationEpoch = source.stamp.presentationEpoch,
            .presentationSequence = source.stamp.sequence,
            .confirmedTick = source.stamp.confirmedTick,
            .sourceScriptId = source.stamp.sourceScriptId,
            .ordinal = source.stamp.ordinal,
            .enabled = source.enabled,
            .transitionFrames = source.transitionFrames,
        });
    }
    const script::ScriptMotionBlurPresentationState& motionBlur =
        presentation.m_scriptMotionBlurPresentation;
    snapshot.motionBlur = {
        .presentationEpoch = presentation.m_scriptPresentationEpoch,
        .presentationSequence = motionBlur.stamp.sequence,
        .commandsTrimmedThroughSequence =
            presentation.m_scriptMotionBlurJournalTrimmedThroughSequence,
        .mode = toRenderMotionBlurMode(motionBlur.mode),
        .saturate = motionBlur.saturate,
        .hasJumpTarget = motionBlur.jumpTarget.has_value(),
        .jumpTarget = motionBlur.jumpTarget.value_or(math::vec3{}),
        .followAmount = motionBlur.followAmount,
    };
    // BW and motion blur compete for W3D's one tactical-view filter slot.
    // Keep this source-ordered tail rather than reducing a same-tick Begin,
    // Jump or EndFollow to a final value before renderer consumption.
    const container::Vector<script::ScriptMotionBlurPresentationState>& motionBlurJournal =
        presentation.m_scriptMotionBlurJournal;
    snapshot.motionBlur.commands.reserve(motionBlurJournal.size());
    for (const script::ScriptMotionBlurPresentationState& source : motionBlurJournal) {
        const render::MotionBlurRenderMode mode = toRenderMotionBlurMode(source.mode);
        if (mode == render::MotionBlurRenderMode::Count) continue;
        snapshot.motionBlur.commands.push_back({
            .presentationEpoch = source.stamp.presentationEpoch,
            .presentationSequence = source.stamp.sequence,
            .confirmedTick = source.stamp.confirmedTick,
            .sourceScriptId = source.stamp.sourceScriptId,
            .ordinal = source.stamp.ordinal,
            .mode = mode,
            .saturate = source.saturate,
            .hasJumpTarget = source.jumpTarget.has_value(),
            .jumpTarget = source.jumpTarget.value_or(math::vec3{}),
            .followAmount = source.followAmount,
        });
    }
    const script::ScriptCameraSlavePresentationState& cameraSlave =
        presentation.m_scriptCameraSlavePresentation;
    snapshot.cameraSlave = {
        .presentationEpoch = presentation.m_scriptPresentationEpoch,
        .presentationSequence = cameraSlave.stamp.sequence,
        .enabled = cameraSlave.enabled,
        .objectId = cameraSlave.object.value,
        .boneName = cameraSlave.boneName,
    };
    const script::ScriptSkyboxPresentationState& skybox =
        presentation.m_scriptSkyboxPresentation;
    snapshot.skybox = {
        .presentationEpoch = presentation.m_scriptPresentationEpoch,
        .presentationSequence = skybox.stamp.sequence,
        .enabled = skybox.enabled,
        .textureNames = presentation.m_scriptSkyboxPresentationTextures.textureNames,
    };
    if (config::TheWritableGlobalData) {
        const float positionZ = config::TheWritableGlobalData->skyBoxPositionZ();
        const float scale = config::TheWritableGlobalData->skyBoxScale();
        snapshot.skybox.positionZ = std::isfinite(positionZ) ? positionZ : 0.0f;
        snapshot.skybox.scale = std::isfinite(scale) && std::abs(scale) > math::EPSILON
            ? scale : 4.5f;
    }
    const script::ScriptTreeSwayPresentationState& treeSway =
        presentation.m_scriptTreeSwayPresentation;
    snapshot.treeSway = {
        .presentationEpoch = presentation.m_scriptPresentationEpoch,
        .presentationSequence = treeSway.stamp.sequence,
        .confirmedTick = treeSway.stamp.confirmedTick,
        .enabled = treeSway.enabled && featureQuality.useTreeSway,
        .directionRadians = treeSway.directionRadians,
        .intensityRadians = treeSway.intensityRadians,
        .leanRadians = treeSway.leanRadians,
        .periodFrames = treeSway.periodFrames,
        .randomness = treeSway.randomness,
    };
    const script::ScriptWeatherPresentationState& weather =
        presentation.m_scriptWeatherPresentation;
    snapshot.weather = {
        .presentationEpoch = presentation.m_scriptPresentationEpoch,
        .presentationSequence = weather.stamp.sequence,
        .visible = weather.visible,
        .snowEnabled = weather.snow.enabled,
        .usePointSprites = weather.snow.usePointSprites,
        .snowTexture = weather.snow.texture,
        .frequencyScaleX = weather.snow.frequencyScaleX,
        .frequencyScaleY = weather.snow.frequencyScaleY,
        .amplitude = weather.snow.amplitude,
        .pointSize = weather.snow.pointSize,
        .maximumPointSize = weather.snow.maximumPointSize,
        .minimumPointSize = weather.snow.minimumPointSize,
        .quadSize = weather.snow.quadSize,
        .boxDimensions = weather.snow.boxDimensions,
        .boxDensity = weather.snow.boxDensity,
        .velocity = weather.snow.velocity,
    };
    snapshot.screenShake.presentationEpoch = presentation.m_scriptPresentationEpoch;
    snapshot.screenShake.impulsesTrimmedThroughSequence =
        presentation.m_scriptScreenShakeJournalTrimmedThroughSequence;
    snapshot.screenShake.localizedImpulsesTrimmedThroughSequence =
        presentation.m_scriptLocalizedCameraShakeJournalTrimmedThroughSequence;
    snapshot.screenShake.logicFramesPerSecond = static_cast<uint32_t>(
        std::max(1, content.m_startInfo.gameSpeedFPS));
    const container::Vector<script::ScriptScreenShakeImpulse>& shakeJournal =
        presentation.m_scriptScreenShakeJournal;
    snapshot.screenShake.impulses.reserve(shakeJournal.size());
    for (const script::ScriptScreenShakeImpulse& source : shakeJournal) {
        const render::ScreenShakeRenderIntensity intensity =
            toRenderScreenShakeIntensity(source.intensity);
        if (intensity == render::ScreenShakeRenderIntensity::Count) continue;
        snapshot.screenShake.impulses.push_back({
            .presentationEpoch = source.stamp.presentationEpoch,
            .sequence = source.stamp.sequence,
            .confirmedTick = source.stamp.confirmedTick,
            .sourceScriptId = source.stamp.sourceScriptId,
            .ordinal = source.stamp.ordinal,
            .intensity = intensity,
        });
    }
    const container::Vector<script::ScriptLocalizedCameraShakeImpulse>& localizedShakeJournal =
        presentation.m_scriptLocalizedCameraShakeJournal;
    snapshot.screenShake.localizedImpulses.reserve(localizedShakeJournal.size());
    for (const script::ScriptLocalizedCameraShakeImpulse& source : localizedShakeJournal) {
        snapshot.screenShake.localizedImpulses.push_back({
            .presentationEpoch = source.stamp.presentationEpoch,
            .sequence = source.stamp.sequence,
            .confirmedTick = source.stamp.confirmedTick,
            .sourceScriptId = source.stamp.sourceScriptId,
            .ordinal = source.stamp.ordinal,
            .position = source.position,
            .amplitude = source.amplitude,
            .radius = source.radius,
            .durationTicks = source.durationTicks,
        });
    }
    const script::ScriptClientOptionsState& clientOptions =
        presentation.m_scriptClientOptions;
    snapshot.clientOptions = {
        .presentationEpoch = presentation.m_scriptPresentationEpoch,
        .presentationSequence = clientOptions.lastMutation().sequence,
        .occlusionEnabled = clientOptions.occlusionEnabled(),
        .drawIconUiEnabled = clientOptions.drawIconUiEnabled(),
    };
    snapshot.objectIcons.presentationEpoch = presentation.m_scriptPresentationEpoch;
    snapshot.objectIcons.presentationSequence = presentation.m_scriptPresentationSequence;
    snapshot.objectUi.presentationEpoch = presentation.m_scriptPresentationEpoch;
    snapshot.objectUi.presentationSequence = presentation.m_scriptPresentationSequence;
    snapshot.objectUi.showObjectHealth = !snapshot.renderGameDataSettings ||
        snapshot.renderGameDataSettings->visual.objectFeedback.showObjectHealth;
    if (snapshot.renderGameDataSettings) {
        const RenderObjectFeedbackGameData& feedback =
            snapshot.renderGameDataSettings->visual.objectFeedback;
        snapshot.objectUi.captionStyle = {
            .fontName = feedback.drawableCaptionFont,
            .pointSize = feedback.drawableCaptionPointSize,
            .color = feedback.drawableCaptionColor,
            .bold = feedback.drawableCaptionBold,
        };
    }
    snapshot.objectUi.logicFramesPerSecond = static_cast<uint32_t>(
        std::max(1, content.m_startInfo.gameSpeedFPS));
    const script::ScriptMapPresentationState& radarPresentation =
        presentation.m_scriptMapPresentation;
    const PlayerState* radarObserver = content.m_players.localPlayer();
    const bool radarSpectator = !radarObserver ||
        !radarObserver->isSimulationParticipant();
    const bool radarCapability = radarObserver && radarObserver->radar.hasRadar(
        radarObserver->energy, simulationFrame);
    snapshot.tacticalRadar = {
        .presentationEpoch = presentation.m_scriptPresentationEpoch,
        .presentationSequence = radarPresentation.lastMutation().sequence,
        .visible = radarPresentation.radarForced() ||
            (radarPresentation.radarVisible() &&
             (radarCapability || radarSpectator)),
        .forced = radarPresentation.radarForced(),
        .spectator = radarSpectator,
    };
    snapshot.tacticalRadar.events.reserve(
        radarPresentation.radarEvents().size());
    for (const script::ScriptRadarEventPresentation& event :
         radarPresentation.radarEvents()) {
        snapshot.tacticalRadar.events.push_back({
            .eventIdentity = radarEventIdentity(
                event.stamp.sequence ^
                    (static_cast<uint64_t>(event.stamp.sourceScriptId) << 32u),
                event.stamp.confirmedTick, event.eventType),
            .worldPosition = event.position,
            .eventType = event.eventType,
            .createTick = event.stamp.confirmedTick,
            .fadeTick = event.fadeTick,
            .dieTick = event.dieTick,
        });
    }
    const uint64_t objectLossRadarLifetime = static_cast<uint64_t>(
        std::max(1, content.m_startInfo.gameSpeedFPS)) * 4u;
    for (const ObjectLossRadarPresentationEvent& event :
         presentation.m_objectLossRadarEvents) {
        if (simulationFrame < event.confirmedTick ||
            simulationFrame - event.confirmedTick >
                objectLossRadarLifetime) {
            continue;
        }
        const uint64_t dieTick = event.confirmedTick >
                std::numeric_limits<uint64_t>::max() -
                    objectLossRadarLifetime
            ? std::numeric_limits<uint64_t>::max()
            : event.confirmedTick + objectLossRadarLifetime;
        snapshot.tacticalRadar.events.push_back({
            .eventIdentity = radarEventIdentity(
                event.object.value, event.confirmedTick, 10),
            .sourceObjectId = event.object.value,
            .worldPosition = {
                event.position.x.to_float(),
                event.position.y.to_float(),
                event.position.z.to_float()},
            .eventType = 10,
            .createTick = event.confirmedTick,
            .fadeTick = dieTick,
            .dieTick = dieTick,
        });
    }
    const uint64_t radarEpoch = presentation.m_scriptPresentationEpoch;
    if (presentation.m_renderBeaconRadarEpoch != radarEpoch) {
        presentation.m_renderBeaconRadarHistory.clear();
        presentation.m_renderBeaconRadarEpoch = radarEpoch;
    }
    const uint64_t framesPerSecond = static_cast<uint64_t>(
        std::max(1, content.m_startInfo.gameSpeedFPS));
    auto& beaconHistory = presentation.m_renderBeaconRadarHistory;
    auto beacon = beaconHistory.begin();
    while (beacon != beaconHistory.end()) {
        const uint64_t duration = std::max<uint64_t>(
            1u, (static_cast<uint64_t>(
                    beacon->radarPulseDurationMilliseconds) *
                framesPerSecond + 999u) / 1000u);
        const uint64_t dieTick = beacon->confirmedTick >
                std::numeric_limits<uint64_t>::max() - duration
            ? std::numeric_limits<uint64_t>::max()
            : beacon->confirmedTick + duration;
        if (simulationFrame > dieTick) {
            beacon = beaconHistory.erase(beacon);
            continue;
        }
        const PlayerState* localBeaconObserver =
            content.m_players.localPlayer();
        const bool hasLocalBeaconObserver = localBeaconObserver &&
            !localBeaconObserver->isNeutral();
        const bool localBeaconObserverIsSpectator = localBeaconObserver &&
            (localBeaconObserver->participation ==
                 PlayerParticipationKind::Observer ||
             localBeaconObserver->controller == PlayerControllerKind::Observer);
        const std::optional<ecs::entity> beaconEntity =
            world.m_objects.entityFromId(beacon->object);
        const OwnerComponent* beaconOwner = beaconEntity
            ? ecs::try_get<OwnerComponent>(
                  world.m_registry, *beaconEntity)
            : nullptr;
        const RenderModelComponent* beaconVisual = beaconEntity
            ? ecs::try_get<RenderModelComponent>(
                  world.m_registry, *beaconEntity)
            : nullptr;
        const bool beaconAllied = localBeaconObserver &&
            localBeaconObserver->isSimulationParticipant() && beaconOwner &&
            content.m_players.relationship(
                localBeaconObserver->id, beaconOwner->player) ==
                PlayerRelationship::Allies;
        if (!render::beaconVisibleToObserver(
                hasLocalBeaconObserver,
                beaconAllied || localBeaconObserverIsSpectator,
                !beaconEntity || !beaconVisual || beaconVisual->hidden)) {
            ++beacon;
            continue;
        }
        const uint64_t fadeDuration = std::max<uint64_t>(1u, duration / 4u);
        snapshot.tacticalRadar.events.push_back({
            .eventIdentity = radarEventIdentity(
                beacon->object.value ^
                    (static_cast<uint64_t>(beacon->authoredOrder) << 32u),
                beacon->confirmedTick, 5),
            .sourceObjectId = beacon->object.value,
            .worldPosition = {
                beacon->position.x.to_float(),
                beacon->position.y.to_float(),
                beacon->position.z.to_float()},
            .eventType = 5,
            .createTick = beacon->confirmedTick,
            .fadeTick = dieTick > fadeDuration
                ? dieTick - fadeDuration : beacon->confirmedTick,
            .dieTick = dieTick,
        });
        ++beacon;
    }
    if (beaconHistory.size() > 64u) {
        beaconHistory.erase(
            beaconHistory.begin(), beaconHistory.end() - 64);
    }
    const script::ScriptViewCompatibilityState& viewCompatibility =
        presentation.m_scriptViewCompatibility;
    snapshot.viewCompatibility = {
        .presentationEpoch = presentation.m_scriptPresentationEpoch,
        .presentationSequence = viewCompatibility.lastMutation().sequence,
        .terrainOversizeTiles = viewCompatibility.terrainOversizeTiles(),
        .guardBandX = viewCompatibility.guardBandX(),
        .guardBandY = viewCompatibility.guardBandY(),
    };
    snapshot.localVisibility = extractLocalVisibility(
        sessionIdentity,
        world.m_mapVisibility.snapshot(),
        content.m_players,
        content.m_players.localPlayer(),
        presentation.m_scriptPresentationEpoch);
    if (content.m_terrain.isLoaded()) {
        // RefCode's display border level controls only the unused terrain's
        // colour. Objects beyond the active partition remain fully shrouded
        // even when DISABLE_BORDER_SHROUD makes that terrain clear.
        const game::terrain::TerrainExtent extent =
            content.m_terrain.map().playableExtent();
        snapshot.localVisibility.playableMinimum = extent.minimum;
        snapshot.localVisibility.playableMaximum = extent.maximum;
        snapshot.localVisibility.playableBoundsEnabled = true;
        snapshot.localVisibility.borderShroudEnabled =
            presentation.m_scriptMapPresentation.borderShroudEnabled();
    }
    snapshot.worldFeedback.presentationEpoch =
        presentation.m_scriptPresentationEpoch;
    snapshot.worldFeedback.presentationSequence =
        presentation.m_scriptPresentationSequence;
    const auto feedbackVisible = [&snapshot](const math::vec3& position) {
        if (!snapshot.localVisibility.isInsidePlayableBounds(position)) {
            return false;
        }
        return !snapshot.localVisibility.enabled ||
            snapshot.localVisibility.worldState(position) ==
                render::LocalVisibilityRenderCellState::Visible;
    };
    snapshot.worldFeedback.animations.reserve(
        presentation.m_objectWorldAnimations.size());
    for (const ObjectWorldAnimationPresentationEvent& event :
         presentation.m_objectWorldAnimations) {
        if (simulationFrame < event.startTick ||
            simulationFrame >= event.expireTick ||
            !feedbackVisible(event.worldAnchor)) {
            continue;
        }
        snapshot.worldFeedback.animations.push_back({
            .objectId = event.object.value,
            .worldAnchor = event.worldAnchor,
            .animationName = event.animationName,
            .presentationEpoch = presentation.m_scriptPresentationEpoch,
            .presentationSequence = event.identity,
            .startTick = event.startTick,
            .lastVisibleTick = event.expireTick - 1u,
            .logicFramesPerSecond = event.logicFramesPerSecond,
            .zRisePerSecond = event.zRisePerSecond,
            .fadeOnExpire = true,
            .permanent = false,
        });
    }
    snapshot.worldFeedback.floatingTexts.reserve(
        presentation.m_objectFloatingTexts.size());
    for (const ObjectFloatingTextPresentationEvent& event :
         presentation.m_objectFloatingTexts) {
        if (simulationFrame < event.startTick ||
            simulationFrame >= event.expireTick ||
            !feedbackVisible(event.worldAnchor)) {
            continue;
        }
        snapshot.worldFeedback.floatingTexts.push_back({
            .identity = event.identity,
            .worldAnchor = event.worldAnchor,
            .amount = event.amount,
            .color = event.color,
            .startTick = event.startTick,
            .timeoutTick = event.timeoutTick,
            .expireTick = event.expireTick,
            .logicFramesPerSecond = event.logicFramesPerSecond,
            .moveUpPerSecond = event.moveUpPerSecond,
            .vanishPerSecond = event.vanishPerSecond,
        });
    }
}

} // namespace engine
