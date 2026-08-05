#include "game/session/presentation/GameSessionPresentationPort.h"

#include "game/session/state/GameSessionDomainState.h"
#include "game/base/GameTacticalCamera.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "presentation/render/RenderWorldDescriptorContracts.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace engine {

GameSessionPresentationSnapshot GameSessionPresentationPort::snapshot()
    const {
    GameSessionPresentationSnapshot result;
    result.scriptEpoch = m_presentation.m_scriptPresentationEpoch;
    result.audioEpoch = m_presentation.m_audioJournal.presentationEpoch();
    result.fxEpoch = m_presentation.m_fxInvocations.presentationEpoch();
    result.renderSettings = m_presentation.m_renderGameDataSettingsSnapshot;
    if (!result.renderSettings) {
        result.renderSettings = std::make_shared<const RenderGameDataSettings>(
            m_presentation.m_renderGameDataSettings);
    }
    result.featureQuality = m_presentation.m_renderFeatureQualitySnapshot;
    result.camera = m_presentation.m_cameraDirector.camera();
    if (m_content.m_terrain.isLoaded()) {
        const game::terrain::TerrainExtent extent =
            m_content.m_terrain.map().playableExtent();
        result.cameraPlayableMinimum = extent.minimum;
        result.cameraPlayableMaximum = extent.maximum;
        result.hasCameraPlayableExtent = true;
    }
    result.cameraManualInputAllowed =
        m_presentation.m_cameraDirector.acceptsManualInput();
    result.cameraScriptMovementActive =
        m_presentation.m_scriptCameraCompletedRevision <
        m_presentation.m_scriptCameraMovementRevision;
    result.scriptCameraCommands =
        m_presentation.m_scriptCameraPresentationJournal;
    result.scriptCameraCommandsTrimmedThroughSequence =
        m_presentation.m_scriptCameraPresentationJournalTrimmedThroughSequence;
    result.scriptCameraMovementRevision =
        m_presentation.m_scriptCameraMovementRevision;
    result.placement = m_presentation.m_localPlacementPresentation.snapshot();
    result.localPlacementActive =
        m_presentation.m_localPlacementPresentation.active();
    result.gameplayInputEnabled =
        m_presentation.m_scriptUiPresentation.gameplayInputEnabled();
    if (m_presentation.m_scriptClientOptions.hasFrameRateLimitOverride()) {
        result.frameRateLimit =
            m_presentation.m_scriptClientOptions.effectiveFrameRateLimit();
    }
    const GameStartInfo& start = m_content.m_startInfo;
    result.gameSpeedFramesPerSecond = start.gameSpeedFPS;
    result.visualSpeedMultiplier =
        m_presentation.m_cameraDirector.visualSpeedMultiplier();
    result.networkEnabled = start.network.enabled;
    result.localFastForwardActive = !result.networkEnabled &&
        m_presentation.m_cameraDirector.visualSpeedMultiplier() > 1;
    return result;
}

void GameSessionPresentationPort::frameCameraOnTerrain() noexcept {
    if (!m_content.m_terrain.isLoaded()) return;

    math::vec3 pivot{50.0f, 50.0f, 0.0f};
    const bool usesMatchStart = m_content.m_startInfo.network.enabled ||
        m_content.m_startInfo.mode == GameMode::Skirmish ||
        m_content.m_startInfo.mode == GameMode::Replay;
    if (usesMatchStart) {
        if (const PlayerState* local = m_content.m_players.localPlayer();
            local && local->startPosition >= 0) {
            for (const game::terrain::MultiplayerStartPosition& start :
                 m_content.m_terrain.multiplayerStartPositions()) {
                if (start.index == local->startPosition) {
                    pivot = start.position;
                    break;
                }
            }
        }
    } else if (const game::terrain::WaypointRecord* initial =
                   m_content.m_terrain.waypointByName("InitialCameraPosition")) {
        pivot = initial->position;
    }

    const RenderCameraGameData& settings =
        m_presentation.m_renderGameDataSettings.visual.camera;
    const float tacticalHeight = static_cast<float>(
        m_presentation.m_initialRenderDisplaySnapshot.effective.height);
    const float tacticalAspect = tacticalHeight > math::EPSILON
        ? static_cast<float>(
              m_presentation.m_initialRenderDisplaySnapshot.effective.width) /
              tacticalHeight
        : 5.0f / 3.0f;
    const GameCameraState initial = GameTacticalCamera::makeInitial(
        pivot, m_content.m_terrain.map(), settings, tacticalAspect);
    m_presentation.m_cameraDirector.configureScriptGeometry({
        .cameraHeight = settings.initialHeight,
        .maximumHeightAboveGround = settings.maximumHeight,
        .initialGroundLevel = std::min(120.0f, initial.target.z()),
        .defaultPitchRadians = math::deg_to_rad(settings.pitchDegrees),
    });
    m_presentation.m_cameraDirector.setCamera(initial);
}

void GameSessionPresentationPort::updateCameraSystems(float deltaSeconds) {
    if (!m_content.m_active) return;
    GameSessionScriptCameraState& scriptCamera = m_presentation.m_scriptCamera;
    GameCameraDirector& cameraDirector = m_presentation.m_cameraDirector;
    const auto stopCameraLock = [&]() noexcept {
        scriptCamera.clearLock();
        cameraDirector.scriptStopFollowing();
    };
    if (m_presentation.m_pendingCameraInput.hasManualInput()) {
        stopCameraLock();
    } else if (scriptCamera.object()) {
        const std::optional<ecs::entity> followed =
            entityFromId(scriptCamera.object());
        const TransformComponent* transform = followed
            ? ecs::try_get<TransformComponent>(m_world.m_registry, *followed)
            : nullptr;
        if (!transform) {
            stopCameraLock();
        } else {
            const math::vec3 pivot{transform->x, transform->y, transform->z};
            if (scriptCamera.isTether()) {
                cameraDirector.scriptTetherPivot(
                    pivot, scriptCamera.snapPending(),
                    scriptCamera.tetherPlay(),
                    scriptCamera.tetherPartitionCellSize());
            } else {
                cameraDirector.scriptFollowPivot(
                    pivot, scriptCamera.snapPending(), deltaSeconds);
            }
            scriptCamera.consumeSnap();
        }
    }
    const bool manualCameraChanged = cameraDirector.applyManualInput(
        m_presentation.m_pendingCameraInput, deltaSeconds);
    if (manualCameraChanged && m_content.m_terrain.isLoaded()) {
        GameCameraState constrained = cameraDirector.camera();
        GameTacticalCamera::constrainManualCamera(
            constrained, m_presentation.m_pendingCameraInput, deltaSeconds,
            m_content.m_terrain.map(),
            m_presentation.m_renderGameDataSettings.visual.camera);
        cameraDirector.setCamera(constrained);
    }
    m_presentation.m_pendingCameraInput.clear();
    // Scripted transitions are advanced by the presentation thread.  This
    // logic-side endpoint still owns manual input and object follow/tether,
    // but must not make authored camera duration depend on confirmed ticks.
}

void GameSessionPresentationPort::queueCameraInput(
    const GameCameraInput& input) noexcept {
    if (!m_content.m_active) return;
    m_presentation.m_pendingCameraInput.accumulate(input);
}

bool GameSessionPresentationPort::acknowledgeScriptCameraCompletion(
    const script::ScriptCameraPresentationCompletion& completion) {
    if (!m_content.m_active || completion.presentationEpoch == 0 ||
        completion.presentationEpoch !=
            m_presentation.m_scriptPresentationEpoch ||
        completion.movementRevision == 0 ||
        completion.movementRevision >
            m_presentation.m_scriptCameraMovementRevision ||
        completion.movementRevision <=
            m_presentation.m_scriptCameraCompletedRevision) {
        return false;
    }
    m_presentation.m_scriptCameraCompletedRevision =
        completion.movementRevision;
    if (completion.movementRevision ==
        m_presentation.m_scriptCameraMovementRevision) {
        GameCameraState settled{
            .position = completion.position,
            .target = completion.target,
            .up = completion.up,
            .verticalFovRadians = completion.verticalFovRadians,
            .horizontalFovRadians = completion.horizontalFovRadians,
            .tacticalViewportHeightScale =
                completion.tacticalViewportHeightScale,
            .nearClip = completion.nearClip,
            .farClip = completion.farClip,
            .visibilityDistance = completion.visibilityDistance,
            .fogEnabled = completion.fogEnabled,
            .fogColor = completion.fogColor,
            .fogStartDistance = completion.fogStartDistance,
            .fogEndDistance = completion.fogEndDistance,
            .cameraCutRevision = completion.cameraCutRevision,
        };
        m_presentation.m_cameraDirector.settlePresentationCamera(settled);
        m_presentation.m_cameraDirector.setVisualSpeedMultiplier(
            completion.visualSpeedMultiplier);
        m_presentation.m_scriptCamera.disarmTimeFreeze();
    }
    return true;
}

bool GameSessionPresentationPort::recordAnimationCompletion(
    const render::RenderAnimationCompletionFeedback& completion) {
    if (!m_content.m_active || completion.presentationEpoch == 0 ||
        completion.presentationEpoch != m_presentation.m_scriptPresentationEpoch ||
        completion.objectId == 0 || completion.objectId > UINT32_MAX ||
        completion.generation == 0 ||
        completion.simulationFrame > m_presentation.m_confirmedTick ||
        !std::isfinite(completion.completedDurationSeconds) ||
        completion.completedDurationSeconds < 0.0f) {
        return false;
    }
    const uint8_t phase = static_cast<uint8_t>(completion.phase);
    if (phase > static_cast<uint8_t>(
            render::RenderAnimationCompletionPhase::ActiveState)) {
        return false;
    }
    const uint8_t kind = static_cast<uint8_t>(completion.kind);
    if (kind >= static_cast<uint8_t>(render::RenderAnimationFeedbackKind::Count)) {
        return false;
    }
    if (completion.kind == render::RenderAnimationFeedbackKind::EndpointPublished) {
        const ObjectId object{static_cast<uint32_t>(completion.objectId)};
        const uint64_t key =
            (static_cast<uint64_t>(object.value) << 32u) |
            completion.channelIndex;
        const GameSessionScriptPresentationState::PendingVisualAnimationAdmission pending{
            .object = object,
            .channelIndex = completion.channelIndex,
            .generation = completion.generation,
            .simulationFrame = completion.simulationFrame,
        };
        auto [entry, inserted] =
            m_presentation.m_pendingVisualAnimationAdmissions.try_emplace(
                key, pending);
        if (!inserted &&
            (pending.generation > entry->second.generation ||
             (pending.generation == entry->second.generation &&
              pending.simulationFrame > entry->second.simulationFrame))) {
            entry->second = pending;
        }
        return true;
    }
    const GameSessionScriptPresentationState::PendingVisualAnimationCompletion pending{
        .object = ObjectId{static_cast<uint32_t>(completion.objectId)},
        .channelIndex = completion.channelIndex,
        .generation = completion.generation,
        .phase = phase,
        .kind = kind,
        .simulationFrame = completion.simulationFrame,
        .completedDurationSeconds = completion.completedDurationSeconds,
    };
    const auto duplicate = std::find_if(
        m_presentation.m_pendingVisualAnimationCompletions.begin(),
        m_presentation.m_pendingVisualAnimationCompletions.end(),
        [&pending](const auto& current) {
            return current.object == pending.object &&
                current.channelIndex == pending.channelIndex &&
                current.generation == pending.generation &&
                current.phase == pending.phase;
        });
    if (duplicate != m_presentation.m_pendingVisualAnimationCompletions.end()) {
        if (pending.simulationFrame > duplicate->simulationFrame ||
            (pending.simulationFrame == duplicate->simulationFrame &&
             pending.kind > duplicate->kind)) {
            *duplicate = pending;
        }
        return true;
    }
    m_presentation.m_pendingVisualAnimationCompletions.push_back(pending);
    return true;
}

bool GameSessionPresentationPort::dismissPopup(
    uint64_t presentationEpoch,
    uint64_t presentationSequence) noexcept {
    if (!m_content.m_active ||
        presentationEpoch != m_presentation.m_scriptPresentationEpoch) {
        return false;
    }
    return m_presentation.m_scriptUiPresentation.dismissPopup(
        presentationEpoch, presentationSequence);
}

bool GameSessionPresentationPort::emitFxInvocation(
    game::FxInvocationEvent event) {
    return m_publication.emitFxInvocationEvent(std::move(event));
}

std::optional<ecs::entity> GameSessionPresentationPort::entityFromId(
    ObjectId object) const noexcept {
    return m_world.m_objects.entityFromId(object);
}

} // namespace engine
