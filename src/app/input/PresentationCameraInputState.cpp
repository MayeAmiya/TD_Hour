#include "app/input/PresentationCameraInputState.h"

#include "app/runtime/GameUiProjection.h"
#include "engine/input/GameCameraController.h"
#include "engine/system/RendererSubsystem.h"
#include "game/base/GameTacticalCamera.h"

#include <algorithm>
#include <cmath>

namespace app::input {
namespace {

engine::GameCameraScriptGeometry scriptGeometryFor(
    const runtime::GameUiProjection& projection) noexcept {
    const auto& settings = projection.cameraInputSettings;
    const float ground = projection.hasCameraState &&
            std::isfinite(projection.cameraState.target.z())
        ? projection.cameraState.target.z() : 0.0f;
    return {
        .cameraHeight = settings.initialHeight,
        .maximumHeightAboveGround = settings.maximumHeight,
        .initialGroundLevel = std::min(120.0f, ground),
        .defaultPitchRadians = math::deg_to_rad(settings.pitchDegrees),
    };
}

engine::GameCameraScriptTiming cameraTiming(
    const engine::script::ScriptCameraPresentationCommand& command) noexcept {
    return {
        .durationSeconds = command.durationSeconds,
        .easeInSeconds = command.easeInSeconds,
        .easeOutSeconds = command.easeOutSeconds,
    };
}

engine::script::ScriptCameraPresentationCompletion completionFor(
    uint64_t epoch, uint64_t revision,
    const engine::GameCameraState& camera,
    int32_t visualSpeedMultiplier) {
    return {
        .presentationEpoch = epoch,
        .movementRevision = revision,
        .position = camera.position,
        .target = camera.target,
        .up = camera.up,
        .verticalFovRadians = camera.verticalFovRadians,
        .horizontalFovRadians = camera.horizontalFovRadians,
        .tacticalViewportHeightScale = camera.tacticalViewportHeightScale,
        .nearClip = camera.nearClip,
        .farClip = camera.farClip,
        .visibilityDistance = camera.visibilityDistance,
        .fogEnabled = camera.fogEnabled,
        .fogColor = camera.fogColor,
        .fogStartDistance = camera.fogStartDistance,
        .fogEndDistance = camera.fogEndDistance,
        .cameraCutRevision = camera.cameraCutRevision,
        .visualSpeedMultiplier = visualSpeedMultiplier,
    };
}

} // namespace

void PresentationCameraInputState::setProjection(
    const runtime::GameUiProjection& projection) {
    const uint64_t priorRevision = m_sessionRevision;
    const bool sameSession = m_sessionRevision == projection.sessionRevision;
    if (!projection.hasSession || !projection.hasCameraState) {
        if (m_overrideActive && priorRevision != 0u)
            m_renderer.clearPresentationCamera(priorRevision);
        m_localCamera.reset();
        m_localDirty = false;
        m_overrideActive = false;
        m_scriptOverrideActive = false;
        m_scriptRigContinuous = false;
    } else if (!sameSession || !m_localCamera) {
        if (m_overrideActive && priorRevision != 0u)
            m_renderer.clearPresentationCamera(priorRevision);
        m_localCamera = projection.cameraState;
        m_localDirty = false;
        m_overrideActive = false;
        m_scriptOverrideActive = false;
        m_scriptDirector.configureScriptGeometry(scriptGeometryFor(projection));
        m_scriptDirector.reset(projection.cameraState);
        m_scriptDirector.setVisualSpeedMultiplier(
            projection.cameraVisualSpeedMultiplier);
        m_scriptRigContinuous = true;
        m_presentationEpoch = projection.presentationEpoch;
        m_consumedScriptCameraSequence = 0;
        m_latestScriptMovementRevision = 0;
        m_acknowledgedScriptMovementRevision = 0;
    } else if (m_scriptOverrideActive) {
        // Keep the presentation-owned rig after acknowledgement. The camera
        // target contains FXPitch's visual look-at and is not W3D's durable
        // m_pos pivot; rebuilding the rig from that target corrupts the Z
        // origin of the next authored movement. Once logic acknowledges the
        // movement, release only the renderer override while retaining that
        // internal rig for the next authored command.
        if (!projection.cameraScriptMovementActive &&
            m_acknowledgedScriptMovementRevision >=
                m_latestScriptMovementRevision) {
            if (m_overrideActive && m_localCamera)
                m_renderer.releasePresentationCamera(
                    *m_localCamera, projection.sessionRevision);
            m_localCamera = projection.cameraState;
            m_localDirty = false;
            m_overrideActive = false;
            m_scriptOverrideActive = false;
        }
    } else if (m_localDirty && projection.cameraManualInputAllowed &&
               !projection.cameraScriptMovementActive &&
               projection.cameraState.cameraCutRevision ==
                   m_projectionBase.cameraCutRevision) {
        // Presentation retains the smooth local endpoint.
    } else {
        if (m_overrideActive)
            m_renderer.clearPresentationCamera(projection.sessionRevision);
        m_localCamera = projection.cameraState;
        m_localDirty = false;
        m_overrideActive = false;
        if (projection.cameraState.cameraCutRevision !=
            m_projectionBase.cameraCutRevision) {
            m_scriptRigContinuous = false;
        }
    }
    if (projection.presentationEpoch != m_presentationEpoch) {
        m_presentationEpoch = projection.presentationEpoch;
        m_consumedScriptCameraSequence = 0;
        m_latestScriptMovementRevision = 0;
        m_acknowledgedScriptMovementRevision = 0;
        m_scriptDirector.configureScriptGeometry(scriptGeometryFor(projection));
        m_scriptDirector.reset(projection.cameraState);
        m_scriptDirector.setVisualSpeedMultiplier(
            projection.cameraVisualSpeedMultiplier);
        m_scriptOverrideActive = false;
        m_scriptRigContinuous = true;
    }
    if (projection.scriptCameraCommandsTrimmedThroughSequence >
        m_consumedScriptCameraSequence) {
        // A latest-value UI mailbox may legitimately reconnect after the
        // bounded session tail expired.  Continue from the actually displayed
        // local pose and fast-forward the command cursor; replaying an
        // incomplete path from the logic camera would visibly jump backwards.
        m_consumedScriptCameraSequence =
            projection.scriptCameraCommandsTrimmedThroughSequence;
        m_latestScriptMovementRevision = std::max(
            m_latestScriptMovementRevision,
            projection.scriptCameraMovementRevision);
        m_scriptDirector.configureScriptGeometry(scriptGeometryFor(projection));
        m_scriptDirector.reset(
            m_localCamera.value_or(projection.cameraState));
        m_scriptDirector.setVisualSpeedMultiplier(
            projection.cameraVisualSpeedMultiplier);
        m_scriptOverrideActive = true;
        m_scriptRigContinuous = true;
    }
    bool releaseScriptAuthority = false;
    for (const engine::script::ScriptCameraPresentationCommand& command :
         projection.scriptCameraCommands) {
        if (command.stamp.presentationEpoch != m_presentationEpoch ||
            command.stamp.sequence == 0 ||
            command.stamp.sequence <= m_consumedScriptCameraSequence) {
            continue;
        }
        if (!m_scriptOverrideActive) {
            if (!m_scriptRigContinuous || m_localDirty) {
                m_scriptDirector.configureScriptGeometry(scriptGeometryFor(projection));
                m_scriptDirector.reset(
                    m_localCamera.value_or(projection.cameraState));
            }
            m_scriptDirector.setVisualSpeedMultiplier(
                projection.cameraVisualSpeedMultiplier);
        }
        using Operation =
            engine::script::ScriptCameraPresentationOperation;
        const engine::GameCameraScriptTiming timing = cameraTiming(command);
        if (command.operation != Operation::CancelMovement)
            releaseScriptAuthority = false;
        switch (command.operation) {
        case Operation::SetPose: {
            engine::GameCameraState camera = m_scriptDirector.camera();
            camera.position = command.position;
            camera.target = command.target;
            m_scriptDirector.setCamera(camera);
            break;
        }
        case Operation::MoveTo:
            m_scriptDirector.scriptMoveTo(
                command.position, timing, command.orientAlongMotion);
            break;
        case Operation::MoveAlongPath:
            m_scriptDirector.scriptMoveAlongPath(
                command.path, timing, command.orientAlongMotion);
            break;
        case Operation::Setup:
            m_scriptDirector.scriptSetup(
                command.position, command.value, command.secondaryValue,
                command.target, timing);
            break;
        case Operation::Zoom:
            m_scriptDirector.scriptZoomTo(command.value, timing);
            break;
        case Operation::Pitch:
            m_scriptDirector.scriptPitchTo(command.value, timing);
            break;
        case Operation::Rotate:
            m_scriptDirector.scriptRotateBy(command.value, timing);
            break;
        case Operation::LookToward:
            m_scriptDirector.scriptRotateToward(
                command.position, timing, command.reverseRotation);
            if (command.tertiaryValue > 0.0f)
                m_scriptDirector.scriptHold(command.tertiaryValue);
            break;
        case Operation::ModifyLookToward:
            static_cast<void>(m_scriptDirector.
                scriptSetCurrentMoveLookToward(command.position));
            break;
        case Operation::ModifyFinalZoom:
            static_cast<void>(m_scriptDirector.scriptSetFinalZoom(
                command.value, command.secondaryValue,
                command.tertiaryValue));
            break;
        case Operation::ModifyFinalPitch:
            static_cast<void>(m_scriptDirector.scriptSetFinalPitch(
                command.value, command.secondaryValue,
                command.tertiaryValue));
            break;
        case Operation::ModifyFinalLookToward:
            static_cast<void>(m_scriptDirector.
                scriptSetCurrentMoveFinalLookToward(command.position));
            break;
        case Operation::ModifyFinalPivot:
            static_cast<void>(m_scriptDirector.
                scriptSetCurrentMoveFinalPivot(command.position));
            break;
        case Operation::FreezeAngle:
            static_cast<void>(
                m_scriptDirector.scriptFreezeCurrentMoveAngle());
            break;
        case Operation::ModifyFinalSpeedMultiplier:
            static_cast<void>(m_scriptDirector.
                scriptSetFinalVisualSpeedMultiplier(command.integerValue));
            break;
        case Operation::ModifyRollingAverage:
            static_cast<void>(m_scriptDirector.
                scriptSetCurrentMoveRollingAverage(command.integerValue));
            break;
        case Operation::Reset:
            m_scriptDirector.scriptReset(command.position, timing);
            break;
        case Operation::SetDefault:
            m_scriptDirector.scriptSetDefaults(
                command.value, command.secondaryValue,
                command.tertiaryValue);
            break;
        case Operation::CancelMovement:
            static_cast<void>(m_scriptDirector.cancelTransition());
            // Follow/Tether are owned by the logic-side object lock and feed
            // their changing pose through projection. Do not leave a stale
            // presentation override above that source.
            releaseScriptAuthority = true;
            m_scriptRigContinuous = false;
            break;
        }
        m_consumedScriptCameraSequence = command.stamp.sequence;
        m_latestScriptMovementRevision = std::max(
            m_latestScriptMovementRevision, command.movementRevision);
        m_scriptOverrideActive = true;
        m_scriptRigContinuous = true;
        m_localCamera = m_scriptDirector.camera();
    }
    if (releaseScriptAuthority) {
        if (m_overrideActive)
            m_renderer.clearPresentationCamera(projection.sessionRevision);
        m_localCamera = projection.cameraState;
        m_localDirty = false;
        m_overrideActive = false;
        m_scriptOverrideActive = false;
        m_scriptRigContinuous = false;
    }
    if (projection.hasCameraState) m_projectionBase = projection.cameraState;
    m_sessionRevision = projection.sessionRevision;
}

void PresentationCameraInputState::reset() noexcept {
    if (m_overrideActive && m_sessionRevision != 0u)
        m_renderer.clearPresentationCamera(m_sessionRevision);
    m_pendingRestore.reset();
    m_localCamera.reset();
    m_localDirty = false;
    m_overrideActive = false;
    m_scriptOverrideActive = false;
    m_scriptRigContinuous = false;
    m_sessionRevision = 0;
    m_presentationEpoch = 0;
    m_consumedScriptCameraSequence = 0;
    m_latestScriptMovementRevision = 0;
    m_acknowledgedScriptMovementRevision = 0;
}

void PresentationCameraInputState::saveView(
    size_t oneBasedSlot, const engine::GameCameraState& fallback) {
    if (oneBasedSlot == 0 || oneBasedSlot > m_savedViews.size()) return;
    m_savedViews[oneBasedSlot - 1u] = m_localCamera
        ? *m_localCamera : fallback;
}

bool PresentationCameraInputState::requestRestore(size_t oneBasedSlot) {
    if (oneBasedSlot == 0 || oneBasedSlot > m_savedViews.size() ||
        !m_savedViews[oneBasedSlot - 1u]) {
        return false;
    }
    m_pendingRestore = *m_savedViews[oneBasedSlot - 1u];
    m_scriptRigContinuous = false;
    return true;
}

void PresentationCameraInputState::applyPendingRestore(
    engine::GameCameraInput& input) {
    if (!m_pendingRestore) return;
    input.absoluteState = *m_pendingRestore;
    input.hasAbsoluteState = true;
    input.manualIntent = true;
    m_pendingRestore.reset();
}

std::optional<engine::script::ScriptCameraPresentationCompletion>
PresentationCameraInputState::applyImmediate(
    const engine::GameCameraInput& input,
    const runtime::GameUiProjection& projection,
    float presentationDeltaSeconds) {
    if (!m_localCamera || !projection.hasCameraState) {
        return std::nullopt;
    }
    const float deltaSeconds =
        std::isfinite(presentationDeltaSeconds) &&
                presentationDeltaSeconds > 0.0f
        ? std::min(presentationDeltaSeconds, 0.100f)
        : 0.0f;
    if (m_scriptOverrideActive) {
        if (input.hasManualInput() &&
            projection.cameraManualInputAllowed) {
            static_cast<void>(
                m_scriptDirector.applyManualInput(input, deltaSeconds));
        }
        m_scriptDirector.update(deltaSeconds);
        *m_localCamera = m_scriptDirector.camera();
        m_renderer.submitPresentationCamera(
            *m_localCamera, projection.sessionRevision);
        m_overrideActive = true;
        if (m_latestScriptMovementRevision != 0 &&
            m_acknowledgedScriptMovementRevision <
                m_latestScriptMovementRevision &&
            !m_scriptDirector.isTransitionActive()) {
            m_acknowledgedScriptMovementRevision =
                m_latestScriptMovementRevision;
            return completionFor(
                m_presentationEpoch,
                m_acknowledgedScriptMovementRevision,
                *m_localCamera,
                m_scriptDirector.visualSpeedMultiplier());
        }
        return std::nullopt;
    }
    if (!input.hasManualInput() ||
        !projection.cameraManualInputAllowed) {
        return std::nullopt;
    }
    engine::GameCameraManipulator::apply(
        *m_localCamera, input, deltaSeconds);
    if (projection.hasCameraPlayableExtent) {
        static_cast<void>(engine::GameTacticalCamera::
            constrainToPlayableExtent(
                *m_localCamera,
                projection.cameraPlayableMinimum,
                projection.cameraPlayableMaximum,
                projection.cameraInputSettings,
                input.tacticalViewportAspectRatio));
    }
    m_renderer.submitPresentationCamera(
        *m_localCamera, projection.sessionRevision);
    m_overrideActive = true;
    m_localDirty = true;
    m_scriptRigContinuous = false;
    return std::nullopt;
}

} // namespace app::input
