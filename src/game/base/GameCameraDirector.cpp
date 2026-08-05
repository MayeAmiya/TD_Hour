#include "core/container/container_types.h"
#include "game/base/GameCameraDirector.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine {
namespace {

constexpr float kMinimumScriptCameraDistance = 2.0f;
constexpr float kMaximumScriptZoom = 64.0f;
constexpr float kMaximumScriptFxPitch = 8.0f;
constexpr float kLegacyWaypointMinimumDistance = 10.0f;

bool finite(float value) noexcept {
    return std::isfinite(value);
}

bool finiteVector(const math::vec3& value) noexcept {
    return finite(value.x()) && finite(value.y()) && finite(value.z());
}

bool usableDirection(const math::vec3& value) noexcept {
    const float lengthSq = value.length_sq();
    return finiteVector(value) && finite(lengthSq) &&
           lengthSq > math::EPSILON * math::EPSILON;
}

} // namespace

void GameCameraDirector::configureScriptGeometry(
    GameCameraScriptGeometry geometry) noexcept {
    geometry.cameraHeight = std::max(
        finiteOr(geometry.cameraHeight, 232.0f), 1.0f);
    geometry.maximumHeightAboveGround = std::max(
        finiteOr(geometry.maximumHeightAboveGround, 310.0f), 1.0f);
    geometry.initialGroundLevel = finiteOr(geometry.initialGroundLevel, 0.0f);
    geometry.defaultPitchRadians = std::clamp(
        finiteOr(geometry.defaultPitchRadians, math::deg_to_rad(37.5f)),
        math::deg_to_rad(1.0f), math::deg_to_rad(89.0f));
    m_scriptGeometry = geometry;
    m_scriptConfiguredMaximumHeight = geometry.maximumHeightAboveGround;
}

void GameCameraDirector::reset(GameCameraState camera) noexcept {
    advanceCameraCut();
    m_camera = camera.sanitized();
    m_camera.cameraCutRevision = m_cameraCutRevision;
    m_visualSpeedMultiplier = 1;
    m_active = {};
    clearScriptTracks();
    m_status = GameCameraTransitionStatus::Idle;
    m_scriptFollowBlend = 0.0f;
    synchronizeScriptRigFromCamera(true);
}

void GameCameraDirector::setVisualSpeedMultiplier(int32_t multiplier) noexcept {
    // W3DView's setter is a raw assignment. In particular, do not coerce a
    // zero/negative legacy value to one here: the fixed-step pacing consumer
    // makes that policy decision, while camera tracks may still interpolate
    // the exact authored signed endpoint.
    m_visualSpeedMultiplier = multiplier;
}

bool GameCameraDirector::acceptsManualInput() const noexcept {
    // ScriptAction camera tracks retain the original View behavior: ordinary
    // RTS camera input takes control and stops the current authored motion.
    // A full-pose request remains an explicit modern API and keeps its own
    // input policy for tools/cinematics which deliberately lock the camera.
    if (hasScriptTracksActive()) return true;
    return !isTransitionActive() ||
           m_active.inputPolicy == GameCameraInputPolicy::CancelOnManualInput;
}

void GameCameraDirector::startTransition(GameCameraTransitionRequest request) noexcept {
    GameCameraState destination = request.destination.sanitized();
    const float duration = finiteOr(request.durationSeconds, 0.0f);

    clearScriptTracks();
    m_scriptRigValid = false;
    m_camera = m_camera.sanitized();
    destination.cameraCutRevision = m_cameraCutRevision;
    if (duration <= math::EPSILON) {
        advanceCameraCut();
        m_camera = destination;
        m_camera.cameraCutRevision = m_cameraCutRevision;
        m_active = {};
        m_status = GameCameraTransitionStatus::Finished;
        return;
    }

    m_active.start = m_camera;
    m_active.destination = destination;
    m_active.durationSeconds = duration;
    m_active.elapsedSeconds = 0.0f;
    m_active.easing = request.easing;
    m_active.inputPolicy = request.inputPolicy;
    m_status = GameCameraTransitionStatus::Running;
}

bool GameCameraDirector::cancelTransition() noexcept {
    const bool wasTransitioning = isTransitionActive();
    m_active = {};
    clearScriptTracks();
    if (wasTransitioning) {
        m_status = GameCameraTransitionStatus::Cancelled;
        // A cancelled scripted pose is still a usable basis for the next
        // authored action. Derive a fresh rig lazily from this exact pose.
        m_scriptRigValid = false;
    }
    return wasTransitioning;
}

void GameCameraDirector::setCamera(GameCameraState camera) noexcept {
    const bool wasTransitioning = isTransitionActive();
    const uint64_t cutRevision = m_cameraCutRevision;
    m_camera = camera.sanitized();
    m_camera.cameraCutRevision = cutRevision;
    m_active = {};
    clearScriptTracks();
    m_status = wasTransitioning ? GameCameraTransitionStatus::Cancelled
                                : GameCameraTransitionStatus::Idle;
    m_scriptFollowBlend = 0.0f;
    // Session startup/map framing calls this path, so it is the natural
    // modern equivalent of W3DView's default camera configuration used by
    // RESET_CAMERA later in a mission.
    synchronizeScriptRigFromCamera(true);
}

void GameCameraDirector::settlePresentationCamera(
    GameCameraState camera) noexcept {
    if (camera.cameraCutRevision != 0u)
        m_cameraCutRevision = camera.cameraCutRevision;
    setCamera(camera);
}

void GameCameraDirector::update(float fixedDeltaSeconds) noexcept {
    m_camera = m_camera.sanitized();
    if (!finite(fixedDeltaSeconds) || fixedDeltaSeconds <= 0.0f) return;

    if (m_active.durationSeconds > 0.0f &&
        m_status == GameCameraTransitionStatus::Running) {
        // Large deltas can occur in deterministic tools which deliberately
        // step over an entire camera action. Clamp at the endpoint rather
        // than letting a malformed value produce a camera overshoot.
        m_active.elapsedSeconds = std::min(m_active.durationSeconds,
                                           m_active.elapsedSeconds + fixedDeltaSeconds);
        const float normalized = m_active.durationSeconds > math::EPSILON
            ? m_active.elapsedSeconds / m_active.durationSeconds : 1.0f;
        if (normalized >= 1.0f) {
            m_camera = m_active.destination;
            m_active = {};
            m_status = GameCameraTransitionStatus::Finished;
            m_scriptRigValid = false;
            return;
        }
        m_camera = interpolate(m_active, easedProgress(m_active.easing, normalized));
        return;
    }

    if (hasScriptTracksActive()) {
        updateScriptTracks(fixedDeltaSeconds);
    }
}

bool GameCameraDirector::applyManualInput(const GameCameraInput& input,
                                          float fixedDeltaSeconds) noexcept {
    if (!input.hasManualInput()) return false;

    if (hasScriptTracksActive()) {
        clearScriptTracks();
        m_status = GameCameraTransitionStatus::Cancelled;
        // The current composed pose is intentional; feed it through the
        // manipulator below and rebuild the rig from the final manual pose.
        m_scriptRigValid = false;
    }

    if (m_active.durationSeconds > 0.0f &&
        m_status == GameCameraTransitionStatus::Running) {
        switch (m_active.inputPolicy) {
        case GameCameraInputPolicy::CancelOnManualInput:
            m_active = {};
            m_status = GameCameraTransitionStatus::Cancelled;
            m_scriptRigValid = false;
            break;
        case GameCameraInputPolicy::IgnoreManualInput:
        case GameCameraInputPolicy::LockedUntilFinished:
            return false;
        }
    }

    GameCameraManipulator::apply(m_camera, input, fixedDeltaSeconds);
    synchronizeScriptRigFromCamera(false);
    return true;
}

void GameCameraDirector::scriptMoveTo(math::vec3 pivot, GameCameraScriptTiming timing,
                                      bool orientAlongMotion) noexcept {
    if (!finiteVector(pivot)) return;
    beginScriptCameraOperation();
    m_scriptPathTrack = {};
    // W3D's setupWaypointPath explicitly removes Scripted_Rotate. A move is
    // therefore a new movement family, not a pivot track layered under an
    // old rotation.
    m_scriptYawTrack = {};
    m_scriptHoldTrack = {};
    m_scriptMotionKind = ScriptMotionKind::None;
    const math::vec3 startPivot = m_scriptRig.pivot;
    startScriptTrack(m_scriptPivotTrack, startPivot, pivot, timing);
    seedVisualSpeedTrack(m_scriptPivotTrack);
    if (!m_scriptPivotTrack.active) m_scriptRig.pivot = pivot;

    if (orientAlongMotion) {
        const math::vec3 delta = pivot - startPivot;
        if (delta.x() * delta.x() + delta.y() * delta.y() > math::EPSILON * math::EPSILON) {
            const float targetYaw = yawToward(startPivot, pivot);
            const float endYaw = m_scriptRig.yawRadians +
                normalizedAngle(targetYaw - m_scriptRig.yawRadians);
            startScriptTrack(m_scriptYawTrack, m_scriptRig.yawRadians, endYaw, timing);
            if (!m_scriptYawTrack.active) m_scriptRig.yawRadians = endYaw;
        }
    }

    if (m_scriptPivotTrack.active || m_scriptYawTrack.active) {
        m_scriptMotionKind = ScriptMotionKind::Move;
    }
    applyScriptRigToCamera();
    m_status = hasScriptTracksActive() ? GameCameraTransitionStatus::Running
                                       : GameCameraTransitionStatus::Finished;
}

void GameCameraDirector::scriptMoveAlongPath(container::Span<const math::vec3> pivots,
                                             GameCameraScriptTiming timing,
                                             bool orientAlongMotion) noexcept {
    beginScriptCameraOperation();
    m_scriptPivotTrack = {};
    m_scriptYawTrack = {};
    m_scriptHoldTrack = {};
    m_scriptMotionKind = ScriptMotionKind::None;

    ScriptPathTrack path;
    path.pivots.reserve(pivots.size() + 1);
    path.pivots.push_back(m_scriptRig.pivot);
    container::Vector<math::vec3> finitePivots;
    finitePivots.reserve(pivots.size());
    for (const math::vec3 pivot : pivots) {
        if (finiteVector(pivot)) finitePivots.push_back(pivot);
    }
    for (size_t index = 0; index < finitePivots.size(); ++index) {
        const math::vec3 pivot = finitePivots[index];
        const math::vec3 delta = pivot - path.pivots.back();
        const float xyDistance = std::sqrt(
            delta.x() * delta.x() + delta.y() * delta.y());
        if (xyDistance < kLegacyWaypointMinimumDistance) {
            if (index + 1u < finitePivots.size()) continue;
            // Retail keeps the authored final point by replacing the prior
            // near point, rather than ending the path short of its target.
            path.pivots.back() = pivot;
            continue;
        }
        path.pivots.push_back(pivot);
    }
    if (path.pivots.size() == 1) {
        m_scriptRig.pivot = path.pivots.back();
        m_scriptPathTrack = {};
        applyScriptRigToCamera();
        m_status = GameCameraTransitionStatus::Finished;
        return;
    }

    path.cumulativeDistances.resize(path.pivots.size(), 0.0f);
    for (size_t index = 1; index < path.pivots.size(); ++index) {
        const math::vec3 delta = path.pivots[index] - path.pivots[index - 1];
        const float segmentLength = std::sqrt(
            delta.x() * delta.x() + delta.y() * delta.y());
        path.cumulativeDistances[index] = path.cumulativeDistances[index - 1] +
            (finite(segmentLength) ? std::max(segmentLength, 0.0f) : 0.0f);
    }
    path.totalDistance = path.cumulativeDistances.back();
    if (path.totalDistance > math::EPSILON) {
        const float startZ = m_scriptRig.pivot.z();
        const float finalGroundZ = path.pivots.back().z();
        for (size_t index = 0; index < path.pivots.size(); ++index) {
            const float factor = std::clamp(
                path.cumulativeDistances[index] / path.totalDistance,
                0.0f, 1.0f);
            path.pivots[index][2] = startZ + (finalGroundZ - startZ) * factor;
        }
    }
    path.timing = {
        .durationSeconds = std::max(finiteOr(timing.durationSeconds, 0.0f), 0.0f),
        .easeInSeconds = std::max(finiteOr(timing.easeInSeconds, 0.0f), 0.0f),
        .easeOutSeconds = std::max(finiteOr(timing.easeOutSeconds, 0.0f), 0.0f),
    };
    path.orientAlongMotion = orientAlongMotion;
    seedVisualSpeedTrack(path);
    path.active = path.timing.durationSeconds > math::EPSILON &&
                  path.totalDistance > math::EPSILON;
    if (!path.active) {
        m_scriptRig.pivot = path.pivots.back();
        if (orientAlongMotion) {
            const math::vec3& prior = path.pivots[path.pivots.size() - 2];
            m_scriptRig.yawRadians = yawToward(prior, path.pivots.back());
        }
        m_scriptPathTrack = {};
        applyScriptRigToCamera();
        m_status = GameCameraTransitionStatus::Finished;
        return;
    }

    m_scriptPathTrack = std::move(path);
    m_scriptMotionKind = ScriptMotionKind::Move;
    applyScriptRigToCamera();
    m_status = GameCameraTransitionStatus::Running;
}

bool GameCameraDirector::scriptSetCurrentMoveLookToward(math::vec3 target) noexcept {
    if (!finiteVector(target)) return false;
    if (m_scriptMotionKind != ScriptMotionKind::Move) return false;
    if (m_scriptPathTrack.active) {
        m_scriptPathTrack.lookToward = target;
        // RefCode writes every path angle for CAMERA_MOD_LOOK_TOWARD, so a
        // later full modifier deliberately supersedes a prior final-only
        // override rather than composing an unexpected third behavior.
        m_scriptPathTrack.finalLookToward.reset();
        return true;
    }
    if (!m_scriptPivotTrack.active) return false;

    const float remaining = std::max(m_scriptPivotTrack.timing.durationSeconds -
                                     m_scriptPivotTrack.elapsedSeconds, 0.0f);
    if (remaining <= math::EPSILON) return false;
    const float destinationYaw = yawToward(m_scriptPivotTrack.destination, target);
    const float endYaw = m_scriptRig.yawRadians +
        normalizedAngle(destinationYaw - m_scriptRig.yawRadians);
    GameCameraScriptTiming timing = m_scriptPivotTrack.timing;
    timing.durationSeconds = remaining;
    timing.easeInSeconds = std::min(timing.easeInSeconds, remaining);
    timing.easeOutSeconds = std::min(timing.easeOutSeconds, remaining);
    startScriptTrack(m_scriptYawTrack, m_scriptRig.yawRadians, endYaw, timing);
    return true;
}

bool GameCameraDirector::scriptSetCurrentMoveRollingAverage(
    int32_t framesToAverage) noexcept {
    // W3DView::cameraModRollingAverage clamps the authored INT instead of
    // rejecting it.  It writes the waypoint-movement state only, so a
    // rotate/zoom/pitch action has no observable response and must not gain
    // a deferred modifier that leaks into a later movement.
    const int32_t clampedFrames = std::max(framesToAverage, int32_t{1});
    if (m_scriptMotionKind != ScriptMotionKind::Move) return false;
    if (m_scriptPathTrack.active) {
        m_scriptPathTrack.rollingAverageFrames = clampedFrames;
        return true;
    }
    if (m_scriptPivotTrack.active) {
        m_scriptPivotTrack.rollingAverageFrames = clampedFrames;
        return true;
    }
    return false;
}

std::optional<GameCameraScriptTiming> GameCameraDirector::activeScriptMotionTiming() const noexcept {
    const auto remainingScalar = [](const ScriptScalarTrack& track) noexcept {
        GameCameraScriptTiming timing = track.timing;
        timing.durationSeconds = std::max(track.timing.durationSeconds - track.elapsedSeconds, 0.0f);
        timing.easeInSeconds = std::min(std::max(track.timing.easeInSeconds, 0.0f),
                                        timing.durationSeconds);
        timing.easeOutSeconds = std::min(std::max(track.timing.easeOutSeconds, 0.0f),
                                         timing.durationSeconds);
        return timing;
    };
    const auto remainingPivot = [](const ScriptPivotTrack& track) noexcept {
        GameCameraScriptTiming timing = track.timing;
        timing.durationSeconds = std::max(track.timing.durationSeconds - track.elapsedSeconds, 0.0f);
        timing.easeInSeconds = std::min(std::max(track.timing.easeInSeconds, 0.0f),
                                        timing.durationSeconds);
        timing.easeOutSeconds = std::min(std::max(track.timing.easeOutSeconds, 0.0f),
                                         timing.durationSeconds);
        return timing;
    };
    const auto remainingPath = [](const ScriptPathTrack& track) noexcept {
        GameCameraScriptTiming timing = track.timing;
        timing.durationSeconds = std::max(track.timing.durationSeconds - track.elapsedSeconds, 0.0f);
        timing.easeInSeconds = std::min(std::max(track.timing.easeInSeconds, 0.0f),
                                        timing.durationSeconds);
        timing.easeOutSeconds = std::min(std::max(track.timing.easeOutSeconds, 0.0f),
                                         timing.durationSeconds);
        return timing;
    };

    if (m_scriptMotionKind == ScriptMotionKind::Move) {
        if (m_scriptPathTrack.active) return remainingPath(m_scriptPathTrack);
        if (m_scriptPivotTrack.active) return remainingPivot(m_scriptPivotTrack);
        return std::nullopt;
    }
    if (m_scriptMotionKind != ScriptMotionKind::Rotate) return std::nullopt;

    // A rotate-toward-object track owns its scripted state through its hold
    // interval. W3D's cameraModFinal{Zoom,Pitch} uses that full remaining
    // rotate duration, not just the yaw interpolation portion.
    if (!m_scriptYawTrack.active && !m_scriptHoldTrack.active) return std::nullopt;
    GameCameraScriptTiming timing = m_scriptYawTrack.active
        ? remainingScalar(m_scriptYawTrack)
        : remainingScalar(m_scriptHoldTrack);
    if (m_scriptHoldTrack.active) {
        const GameCameraScriptTiming hold = remainingScalar(m_scriptHoldTrack);
        if (hold.durationSeconds > timing.durationSeconds) {
            timing = hold;
        }
    }
    return timing.durationSeconds > math::EPSILON ? std::optional{timing} : std::nullopt;
}

bool GameCameraDirector::scriptSetFinalZoom(float normalizedZoom, float easeInFraction,
                                            float easeOutFraction) noexcept {
    if (!finite(normalizedZoom) || !finite(easeInFraction) || !finite(easeOutFraction)) {
        return false;
    }
    const std::optional<GameCameraScriptTiming> motionTiming = activeScriptMotionTiming();
    if (!motionTiming) return false;

    GameCameraScriptTiming timing = *motionTiming;
    timing.easeInSeconds = timing.durationSeconds * std::clamp(easeInFraction, 0.0f, 1.0f);
    timing.easeOutSeconds = timing.durationSeconds * std::clamp(easeOutFraction, 0.0f, 1.0f);
    const float destination = std::clamp(
        normalizedZoom * maximumScriptZoomAt(scriptZoomEndpoint()),
        0.01f, kMaximumScriptZoom);
    startScriptTrack(m_scriptZoomTrack, m_scriptRig.normalizedZoom,
                     destination, timing);
    seedVisualSpeedTrack(m_scriptZoomTrack);
    if (!m_scriptZoomTrack.active) m_scriptRig.normalizedZoom = m_scriptZoomTrack.destination;
    applyScriptRigToCamera();
    return true;
}

bool GameCameraDirector::scriptSetFinalPitch(float fxPitch, float easeInFraction,
                                             float easeOutFraction) noexcept {
    if (!finite(fxPitch) || !finite(easeInFraction) || !finite(easeOutFraction)) {
        return false;
    }
    const std::optional<GameCameraScriptTiming> motionTiming = activeScriptMotionTiming();
    if (!motionTiming) return false;

    GameCameraScriptTiming timing = *motionTiming;
    timing.easeInSeconds = timing.durationSeconds * std::clamp(easeInFraction, 0.0f, 1.0f);
    timing.easeOutSeconds = timing.durationSeconds * std::clamp(easeOutFraction, 0.0f, 1.0f);
    startScriptTrack(m_scriptPitchTrack, m_scriptRig.fxPitch,
                     std::clamp(fxPitch, 0.0f, kMaximumScriptFxPitch), timing);
    seedVisualSpeedTrack(m_scriptPitchTrack);
    if (!m_scriptPitchTrack.active) m_scriptRig.fxPitch = m_scriptPitchTrack.destination;
    applyScriptRigToCamera();
    return true;
}

bool GameCameraDirector::scriptSetCurrentMoveFinalLookToward(math::vec3 target) noexcept {
    if (!finiteVector(target) || m_scriptMotionKind != ScriptMotionKind::Move) return false;
    if (m_scriptPathTrack.active) {
        m_scriptPathTrack.finalLookToward = target;
        return true;
    }
    if (!m_scriptPivotTrack.active) return false;

    const std::optional<GameCameraScriptTiming> timing = activeScriptMotionTiming();
    if (!timing) return false;
    const float destinationYaw = yawToward(m_scriptPivotTrack.destination, target);
    const float endYaw = m_scriptRig.yawRadians +
        normalizedAngle(destinationYaw - m_scriptRig.yawRadians);
    startScriptTrack(m_scriptYawTrack, m_scriptRig.yawRadians, endYaw, *timing);
    return true;
}

bool GameCameraDirector::scriptSetCurrentMoveFinalPivot(math::vec3 target) noexcept {
    // ScriptActions::doModCameraMoveToSelection averages selected Drawable
    // positions, then calls W3DView::cameraModFinalMoveTo. That W3D method
    // changes XY only: its path already owns a terrain-derived Z profile,
    // precomputed segment lengths and camera angles. Preserve that narrow
    // contract instead of turning selection into an ordinary camera cut.
    if (!finiteVector(target) || m_scriptMotionKind != ScriptMotionKind::Move) return false;

    const auto retargetXY = [&target](math::vec3 current) noexcept {
        return math::vec3{target.x(), target.y(), current.z()};
    };

    if (m_scriptPathTrack.active && m_scriptPathTrack.pivots.size() >= 2) {
        const math::vec3 endpoint = m_scriptPathTrack.pivots.back();
        const float offsetX = target.x() - endpoint.x();
        const float offsetY = target.y() - endpoint.y();
        if (!finite(offsetX) || !finite(offsetY)) return false;

        // W3D stores [current,current,waypoint0,...] and shifts entries
        // starting at index two. The modern path removes that duplicate
        // current entry, so the equivalent range is every authored waypoint
        // (index one onward). Do not recompute cumulative distances or yaw:
        // the original intentionally keeps its already-authored timing and
        // angular samples after the endpoint is moved.
        for (size_t index = 1; index < m_scriptPathTrack.pivots.size(); ++index) {
            const math::vec3& pivot = m_scriptPathTrack.pivots[index];
            m_scriptPathTrack.pivots[index] = {
                pivot.x() + offsetX,
                pivot.y() + offsetY,
                pivot.z(),
            };
        }
        return true;
    }

    if (!m_scriptPivotTrack.active) return false;
    // scriptMoveTo is the modern compact representation of W3D's simple
    // waypoint path. Its destination is the same final path entry above;
    // retain its Z and existing yaw track exactly as cameraModFinalMoveTo.
    m_scriptPivotTrack.destination = retargetXY(m_scriptPivotTrack.destination);
    return true;
}

bool GameCameraDirector::scriptFreezeCurrentMoveAngle() noexcept {
    if (m_scriptMotionKind == ScriptMotionKind::None) return false;

    if (m_scriptMotionKind == ScriptMotionKind::Move) {
        if (!m_scriptPathTrack.active && !m_scriptPivotTrack.active) return false;
        if (m_scriptPathTrack.active) {
            // W3D freezes every precomputed path angle at its current start
            // angle. Preserve the position track while removing both kinds
            // of authored orientation override.
            m_scriptPathTrack.orientAlongMotion = false;
            m_scriptPathTrack.lookToward.reset();
            m_scriptPathTrack.finalLookToward.reset();
        }
        if (m_scriptYawTrack.active) {
            m_scriptYawTrack.start = m_scriptRig.yawRadians;
            m_scriptYawTrack.destination = m_scriptRig.yawRadians;
        }
        return true;
    }

    // Keep the rotate/hold state alive for the remaining original duration;
    // only its angular endpoints collapse to the current angle. This is the
    // important distinction from cancelling a rotate entirely: subsequent
    // CAMERA_MOVEMENT_FINISHED must remain false until the authored track
    // would have completed.
    if (!m_scriptYawTrack.active && !m_scriptHoldTrack.active) return false;
    if (m_scriptYawTrack.active) {
        m_scriptYawTrack.start = m_scriptRig.yawRadians;
        m_scriptYawTrack.destination = m_scriptRig.yawRadians;
    }
    return true;
}

bool GameCameraDirector::scriptSetFinalVisualSpeedMultiplier(int32_t multiplier) noexcept {
    // Retain W3DView::cameraModFinalTimeMultiplier's independent property
    // writes and update order. Zoom/Pitch may coexist with a rotate or move;
    // the latter owns the final fallback only when no rotate is active.
    bool appliedToTrack = false;
    if (m_scriptZoomTrack.active) {
        m_scriptZoomTrack.endVisualSpeedMultiplier = multiplier;
        m_scriptZoomTrack.drivesVisualSpeedMultiplier = true;
        appliedToTrack = true;
    }
    if (m_scriptPitchTrack.active) {
        m_scriptPitchTrack.endVisualSpeedMultiplier = multiplier;
        m_scriptPitchTrack.drivesVisualSpeedMultiplier = true;
        appliedToTrack = true;
    }
    if (m_scriptMotionKind == ScriptMotionKind::Rotate) {
        // A rotate-toward-object may be in its legacy hold interval after
        // yaw itself completed. RefCode still writes m_rcInfo's endpoint but
        // no longer has a per-frame yaw sample to apply, so absorbing the
        // modifier (rather than falling through to an immediate write) is
        // the compatible observable behavior.
        m_scriptYawTrack.endVisualSpeedMultiplier = multiplier;
        m_scriptYawTrack.drivesVisualSpeedMultiplier = true;
        appliedToTrack = true;
    } else if (m_scriptMotionKind == ScriptMotionKind::Move) {
        if (m_scriptPathTrack.active) {
            ScriptPathTrack& path = m_scriptPathTrack;
            path.endVisualSpeedMultiplier = multiplier;
            if (path.visualSpeedMultipliers.size() == path.pivots.size() &&
                path.cumulativeDistances.size() == path.pivots.size() &&
                path.totalDistance > math::EPSILON) {
                // RefCode mutates each waypoint's existing integer value by
                // its cumulative path-distance weight, then interpolates the
                // adjacent integer values while moving. Preserve that shape
                // so repeated modifiers do not collapse to a single global
                // start/end lerp.
                for (size_t index = 1; index < path.visualSpeedMultipliers.size(); ++index) {
                    const float factor = std::clamp(
                        path.cumulativeDistances[index] / path.totalDistance, 0.0f, 1.0f);
                    path.visualSpeedMultipliers[index] = interpolateVisualSpeedMultiplier(
                        path.visualSpeedMultipliers[index], multiplier, factor);
                }
            }
            appliedToTrack = true;
        } else if (m_scriptPivotTrack.active) {
            m_scriptPivotTrack.endVisualSpeedMultiplier = multiplier;
            appliedToTrack = true;
        }
    }
    if (!appliedToTrack) {
        setVisualSpeedMultiplier(multiplier);
    }
    return appliedToTrack;
}

void GameCameraDirector::scriptZoomTo(float normalizedZoom,
                                      GameCameraScriptTiming timing) noexcept {
    if (!finite(normalizedZoom)) return;
    beginScriptCameraOperation();
    const float destination = std::clamp(normalizedZoom, 0.01f, kMaximumScriptZoom);
    startScriptTrack(m_scriptZoomTrack, m_scriptRig.normalizedZoom, destination, timing);
    seedVisualSpeedTrack(m_scriptZoomTrack);
    if (!m_scriptZoomTrack.active) m_scriptRig.normalizedZoom = destination;
    applyScriptRigToCamera();
    m_status = hasScriptTracksActive() ? GameCameraTransitionStatus::Running
                                       : GameCameraTransitionStatus::Finished;
}

void GameCameraDirector::scriptPitchTo(float fxPitch, GameCameraScriptTiming timing) noexcept {
    if (!finite(fxPitch)) return;
    beginScriptCameraOperation();
    const float destination = std::clamp(fxPitch, 0.0f, kMaximumScriptFxPitch);
    startScriptTrack(m_scriptPitchTrack, m_scriptRig.fxPitch, destination, timing);
    seedVisualSpeedTrack(m_scriptPitchTrack);
    if (!m_scriptPitchTrack.active) m_scriptRig.fxPitch = destination;
    applyScriptRigToCamera();
    m_status = hasScriptTracksActive() ? GameCameraTransitionStatus::Running
                                       : GameCameraTransitionStatus::Finished;
}

void GameCameraDirector::scriptRotateBy(float rotations,
                                        GameCameraScriptTiming timing) noexcept {
    if (!finite(rotations)) return;
    beginScriptCameraOperation();
    // W3D's rotateCamera removes Scripted_MoveOnWaypointPath. Keep this
    // explicit even when an earlier path left a concurrent zoom/pitch track.
    m_scriptPivotTrack = {};
    m_scriptPathTrack = {};
    m_scriptHoldTrack = {};
    m_scriptMotionKind = ScriptMotionKind::None;
    const float destination = m_scriptRig.yawRadians + rotations * (2.0f * math::PI);
    startScriptTrack(m_scriptYawTrack, m_scriptRig.yawRadians, destination, timing);
    seedVisualSpeedTrack(m_scriptYawTrack);
    if (!m_scriptYawTrack.active) m_scriptRig.yawRadians = destination;
    if (m_scriptYawTrack.active) m_scriptMotionKind = ScriptMotionKind::Rotate;
    applyScriptRigToCamera();
    m_status = hasScriptTracksActive() ? GameCameraTransitionStatus::Running
                                       : GameCameraTransitionStatus::Finished;
}

void GameCameraDirector::scriptRotateToward(math::vec3 target,
                                            GameCameraScriptTiming timing,
                                            bool reverseRotation) noexcept {
    if (!finiteVector(target)) return;
    beginScriptCameraOperation();
    const math::vec3 delta = target - m_scriptRig.pivot;
    if (delta.x() * delta.x() + delta.y() * delta.y() <= math::EPSILON * math::EPSILON) {
        return;
    }

    m_scriptPivotTrack = {};
    m_scriptPathTrack = {};
    m_scriptHoldTrack = {};
    m_scriptMotionKind = ScriptMotionKind::None;

    const float targetYaw = yawToward(m_scriptRig.pivot, target);
    float deltaYaw = normalizedAngle(targetYaw - m_scriptRig.yawRadians);
    if (reverseRotation) {
        // The original reverse flag explicitly asks for the long arc.  Keep
        // an exact 180-degree target deterministic by choosing positive.
        if (deltaYaw <= 0.0f) deltaYaw += 2.0f * math::PI;
        else deltaYaw -= 2.0f * math::PI;
    }
    startScriptTrack(m_scriptYawTrack, m_scriptRig.yawRadians,
                     m_scriptRig.yawRadians + deltaYaw, timing);
    seedVisualSpeedTrack(m_scriptYawTrack);
    if (!m_scriptYawTrack.active) m_scriptRig.yawRadians += deltaYaw;
    if (m_scriptYawTrack.active) m_scriptMotionKind = ScriptMotionKind::Rotate;
    applyScriptRigToCamera();
    m_status = hasScriptTracksActive() ? GameCameraTransitionStatus::Running
                                       : GameCameraTransitionStatus::Finished;
}

void GameCameraDirector::scriptHold(float durationSeconds) noexcept {
    if (!finite(durationSeconds) || durationSeconds <= math::EPSILON) return;
    beginScriptCameraOperation();
    startScriptTrack(m_scriptHoldTrack, 0.0f, 1.0f,
                     {.durationSeconds = durationSeconds});
    m_status = GameCameraTransitionStatus::Running;
}

void GameCameraDirector::scriptSetup(math::vec3 pivot, float normalizedZoom, float fxPitch,
                                     math::vec3 lookAt,
                                     GameCameraScriptTiming confirmationTiming) noexcept {
    if (!finiteVector(pivot) || !finiteVector(lookAt) || !finite(normalizedZoom) ||
        !finite(fxPitch)) {
        return;
    }
    beginScriptCameraOperation();
    advanceCameraCut();
    clearScriptTracks();
    m_scriptRig.pivot = pivot;
    m_scriptRig.normalizedZoom = std::clamp(
        normalizedZoom * maximumScriptZoomAt(pivot),
        0.01f, kMaximumScriptZoom);
    m_scriptRig.fxPitch = std::clamp(fxPitch, 0.0f, kMaximumScriptFxPitch);
    const math::vec3 horizontal = lookAt - pivot;
    if (horizontal.x() * horizontal.x() + horizontal.y() * horizontal.y() >
        math::EPSILON * math::EPSILON) {
        m_scriptRig.yawRadians = yawToward(pivot, lookAt);
    }
    // ScriptActions::doSetupCamera calls W3DView::moveCameraTo(..., 0), and
    // W3DView clamps that request to one frame before retaining
    // Scripted_MoveOnWaypointPath. Keep the pose immediate, but represent the
    // confirmation state as a zero-distance Move track so later actions in
    // the same source-order pass still observe movement in progress.
    startScriptTrack(m_scriptPivotTrack, pivot, pivot, confirmationTiming);
    seedVisualSpeedTrack(m_scriptPivotTrack);
    if (m_scriptPivotTrack.active) {
        m_scriptMotionKind = ScriptMotionKind::Move;
    }
    applyScriptRigToCamera();
    m_status = hasScriptTracksActive() ? GameCameraTransitionStatus::Running
                                       : GameCameraTransitionStatus::Finished;
}

void GameCameraDirector::scriptReset(math::vec3 pivot, GameCameraScriptTiming timing) noexcept {
    if (!finiteVector(pivot)) return;
    beginScriptCameraOperation();
    m_scriptPathTrack = {};
    m_scriptHoldTrack = {};
    m_scriptMotionKind = ScriptMotionKind::None;
    startScriptTrack(m_scriptPivotTrack, m_scriptRig.pivot, pivot, timing);
    seedVisualSpeedTrack(m_scriptPivotTrack);
    const float resetYaw = m_scriptRig.yawRadians +
        normalizedAngle(m_scriptDefaults.yawRadians - m_scriptRig.yawRadians);
    startScriptTrack(m_scriptYawTrack, m_scriptRig.yawRadians, resetYaw, timing);
    const float resetZoom = maximumScriptZoomAt(pivot);
    startScriptTrack(m_scriptZoomTrack, m_scriptRig.normalizedZoom,
                     resetZoom, timing);
    seedVisualSpeedTrack(m_scriptZoomTrack);
    startScriptTrack(m_scriptPitchTrack, m_scriptRig.fxPitch,
                     m_scriptDefaults.fxPitch, timing);
    seedVisualSpeedTrack(m_scriptPitchTrack);
    startScriptTrack(m_scriptBaselineTrack, m_scriptRig.baselineDistance,
                     m_scriptDefaults.baselineDistance, timing);
    startScriptTrack(m_scriptElevationTrack, m_scriptRig.orbitalElevationRadians,
                     m_scriptDefaults.orbitalElevationRadians, timing);
    // RefCode resetCamera separately calls pitchCamera(1.0f), while
    // setDefaultView stores W3D constraint/default-pitch state. A modern
    // value-only rig has no W3D constraint solver, so baseline/elevation are
    // the explicit observable map-framing approximation here, not a claim of
    // bit-identical legacy camera math.
    if (!m_scriptPivotTrack.active) m_scriptRig.pivot = pivot;
    if (!m_scriptYawTrack.active) m_scriptRig.yawRadians = resetYaw;
    if (!m_scriptZoomTrack.active) m_scriptRig.normalizedZoom = resetZoom;
    if (!m_scriptPitchTrack.active) m_scriptRig.fxPitch = 1.0f;
    if (!m_scriptBaselineTrack.active) m_scriptRig.baselineDistance = m_scriptDefaults.baselineDistance;
    if (!m_scriptElevationTrack.active) {
        m_scriptRig.orbitalElevationRadians = m_scriptDefaults.orbitalElevationRadians;
    }
    if (m_scriptPivotTrack.active || m_scriptYawTrack.active) {
        m_scriptMotionKind = ScriptMotionKind::Move;
    }
    applyScriptRigToCamera();
    m_status = hasScriptTracksActive() ? GameCameraTransitionStatus::Running
                                       : GameCameraTransitionStatus::Finished;
}

void GameCameraDirector::scriptSetDefaults(float pitchRadians, float ignoredAngleRadians,
                                           float maxHeightScale) noexcept {
    if (!finite(pitchRadians) || !finite(ignoredAngleRadians) || !finite(maxHeightScale)) {
        return;
    }
    ensureScriptRig();
    if (!m_scriptMapDefaultsValid) {
        m_scriptMapDefaults = m_scriptDefaults;
        m_scriptMapDefaultsValid = true;
    }

    // RefCode W3DView::setDefaultView intentionally comments out its angle
    // assignment. Keep the parameter in the typed command but do not let an
    // ignored retail field rotate a modern map.
    static_cast<void>(ignoredAngleRadians);

    m_scriptDefaults = m_scriptMapDefaults;
    // Original authored scripts conventionally use zero as the map default
    // pitch. The retail compatibility path offsets a positive value from that
    // default; retain this stable relative mapping and clamp it to a usable
    // modern orbit rather than permitting a malformed map to invert a rig.
    m_scriptDefaults.orbitalElevationRadians = std::clamp(
        m_scriptMapDefaults.orbitalElevationRadians - pitchRadians,
        math::deg_to_rad(1.0f), math::deg_to_rad(89.0f));
    // W3D sets max height from the global map value rather than multiplying a
    // preceding script setting. Likewise derive from the captured map frame,
    // so repeated CAMERA_SET_DEFAULT actions are idempotent by value.
    const float safeScale = std::max(maxHeightScale, 0.0f);
    m_scriptGeometry.defaultPitchRadians = std::clamp(
        pitchRadians, math::deg_to_rad(1.0f), math::deg_to_rad(89.0f));
    m_scriptGeometry.maximumHeightAboveGround = std::max(
        1.0f, m_scriptConfiguredMaximumHeight * safeScale);
    m_scriptDefaults.baselineDistance = std::max(
        m_scriptMapDefaults.baselineDistance * safeScale,
        kMinimumScriptCameraDistance);
    m_scriptDefaults.normalizedZoom = 1.0f;
    m_scriptDefaults.fxPitch = 1.0f;
    m_scriptDefaultsValid = true;
}

void GameCameraDirector::scriptFollowPivot(math::vec3 pivot, bool snap,
                                           float fixedDeltaSeconds) noexcept {
    if (!finiteVector(pivot)) return;
    ensureScriptRig();
    // W3DView's camera lock takes ownership of path motion but deliberately
    // leaves zoom/pitch/rotate state alone. Follow itself is not represented
    // as a Scripted_Move bit, so it must not make the completion predicate
    // false in this modern director either.
    m_scriptPivotTrack = {};
    m_scriptPathTrack = {};
    if (m_scriptMotionKind == ScriptMotionKind::Move) {
        m_scriptMotionKind = ScriptMotionKind::None;
    }
    if (snap) {
        advanceCameraCut();
        m_scriptRig.pivot = pivot;
        m_scriptFollowBlend = 0.0f;
    } else {
        const float delta = std::max(finiteOr(fixedDeltaSeconds, 0.0f), 0.0f);
        // RefCode raises followFactor by 0.05 per logic update until it
        // reaches 1.0. At the default 30Hz that is 1.5/sec; preserve the
        // trajectory in a fixed-tick rather than render-frame form.
        m_scriptFollowBlend = std::clamp(m_scriptFollowBlend + delta * 1.5f, 0.05f, 1.0f);
        m_scriptRig.pivot = math::vec3::lerp(m_scriptRig.pivot, pivot, m_scriptFollowBlend);
    }
    applyScriptRigToCamera();
}

void GameCameraDirector::scriptTetherPivot(math::vec3 pivot, bool snap, float play,
                                           float partitionCellSize) noexcept {
    if (!finiteVector(pivot)) return;
    ensureScriptRig();
    // Both CameraLock variants cancel a currently authored path, but tether
    // itself must remain outside CAMERA_MOVEMENT_FINISHED just like Follow.
    m_scriptPivotTrack = {};
    m_scriptPathTrack = {};
    if (m_scriptMotionKind == ScriptMotionKind::Move) {
        m_scriptMotionKind = ScriptMotionKind::None;
    }
    if (snap) {
        advanceCameraCut();
        m_scriptRig.pivot = pivot;
        applyScriptRigToCamera();
        return;
    }

    // W3DView's LOCK_TETHER compares horizontal distance with one partition
    // cell, then moves half-way toward a remote target or by `0.01 * play`
    // once inside that tolerance. Keep the original Z lock and orientation
    // preservation while making the missing GlobalData cell size explicit at
    // the modern camera boundary. GameSession seals GlobalData's partition
    // size at action time; the default keeps standalone camera tools usable.
    const float cellSize = std::isfinite(partitionCellSize) &&
            partitionCellSize > math::EPSILON
        ? partitionCellSize : 100.0f;
    const float toleranceSquared = cellSize * cellSize;
    math::vec3 next = m_scriptRig.pivot;
    const float dx = pivot.x() - next.x();
    const float dy = pivot.y() - next.y();
    const float distanceSquared = dx * dx + dy * dy;
    if (std::isfinite(distanceSquared) && distanceSquared >= toleranceSquared) {
        const float ratio = 1.0f - toleranceSquared / std::max(distanceSquared, toleranceSquared);
        const float factor = std::clamp(ratio * 0.5f, 0.0f, 0.5f);
        next = {next.x() + dx * factor, next.y() + dy * factor, pivot.z()};
    } else {
        // The reference accepts the authored scalar without an INI clamp;
        // retain negative and large finite values rather than silently
        // changing a mod's tether response.
        const float rawFactor = finiteOr(play, 0.0f) * 0.01f;
        const float factor = finite(rawFactor) ? rawFactor : 0.0f;
        next = {next.x() + dx * factor, next.y() + dy * factor, pivot.z()};
    }
    m_scriptRig.pivot = next;
    applyScriptRigToCamera();
}

void GameCameraDirector::scriptStopFollowing() noexcept {
    m_scriptFollowBlend = 0.0f;
}

bool GameCameraDirector::hasScriptTracksActive() const noexcept {
    return m_scriptPivotTrack.active || m_scriptPathTrack.active || m_scriptYawTrack.active ||
           m_scriptZoomTrack.active || m_scriptPitchTrack.active ||
           m_scriptBaselineTrack.active || m_scriptElevationTrack.active ||
           m_scriptHoldTrack.active;
}

float GameCameraDirector::easedProgress(GameCameraEasing easing, float progress) noexcept {
    const float t = std::clamp(finiteOr(progress, 0.0f), 0.0f, 1.0f);
    switch (easing) {
    case GameCameraEasing::Linear:
        return t;
    case GameCameraEasing::SmoothStep:
        return t * t * (3.0f - 2.0f * t);
    case GameCameraEasing::SmootherStep:
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    case GameCameraEasing::LegacyParabolic:
        return legacyParabolicProgress(t, {});
    }
    return t;
}

GameCameraState GameCameraDirector::interpolate(const ActiveTransition& transition,
                                                 float progress) noexcept {
    const float t = std::clamp(finiteOr(progress, 0.0f), 0.0f, 1.0f);
    if (t >= 1.0f) return transition.destination;

    GameCameraState result = transition.start;
    result.position = math::vec3::lerp(transition.start.position, transition.destination.position, t);
    result.target = math::vec3::lerp(transition.start.target, transition.destination.target, t);
    result.up = math::vec3::lerp(transition.start.up, transition.destination.up, t);
    result.verticalFovRadians = math::lerp(transition.start.verticalFovRadians,
                                           transition.destination.verticalFovRadians, t);
    result.horizontalFovRadians = math::lerp(
        transition.start.horizontalFovRadians,
        transition.destination.horizontalFovRadians, t);
    result.tacticalViewportHeightScale = math::lerp(
        transition.start.tacticalViewportHeightScale,
        transition.destination.tacticalViewportHeightScale, t);
    result.nearClip = math::lerp(transition.start.nearClip, transition.destination.nearClip, t);
    result.farClip = math::lerp(transition.start.farClip, transition.destination.farClip, t);
    result.visibilityDistance = math::lerp(transition.start.visibilityDistance,
                                           transition.destination.visibilityDistance, t);
    result.fogColor = math::vec3::lerp(transition.start.fogColor,
                                       transition.destination.fogColor, t);
    result.fogStartDistance = math::lerp(transition.start.fogStartDistance,
                                         transition.destination.fogStartDistance, t);
    result.fogEndDistance = math::lerp(transition.start.fogEndDistance,
                                       transition.destination.fogEndDistance, t);
    // Fog enable is discrete. Preserve the starting environment through the
    // move and make the destination authoritative on the exact final tick.
    result.fogEnabled = transition.start.fogEnabled;

    // Position/target interpolation can be degenerate for an authored 180°
    // cut. Retain a finite start view direction for the intermediate frame;
    // the destination is still copied exactly at t=1 above.
    if (!usableDirection(result.target - result.position)) {
        const math::vec3 startForward = transition.start.target - transition.start.position;
        const math::vec3 destinationForward =
            transition.destination.target - transition.destination.position;
        const math::vec3 fallback = usableDirection(startForward) ? startForward : destinationForward;
        if (usableDirection(fallback)) {
            result.target = result.position + fallback.normalized() * fallback.length();
        }
    }
    return result.sanitized();
}

float GameCameraDirector::legacyParabolicProgress(float progress,
                                                   GameCameraScriptTiming timing) noexcept {
    const float t = std::clamp(finiteOr(progress, 0.0f), 0.0f, 1.0f);
    const float duration = std::max(finiteOr(timing.durationSeconds, 0.0f), math::EPSILON);
    float easeIn = std::clamp(finiteOr(timing.easeInSeconds, 0.0f) / duration, 0.0f, 1.0f);
    const float easeOut = std::clamp(finiteOr(timing.easeOutSeconds, 0.0f) / duration,
                                     0.0f, 1.0f);
    const float out = 1.0f - easeOut;
    // RefCode clamps overlapping authored ease ranges by shrinking ease-in
    // to the outgoing boundary before it evaluates ParabolicEase.
    if (easeIn > out) easeIn = out;
    const float v0 = 1.0f + out - easeIn;
    if (t < easeIn) {
        return easeIn > math::EPSILON ? (t * t) / (v0 * easeIn) : 0.0f;
    }
    if (t <= out) {
        return (easeIn + 2.0f * (t - easeIn)) / v0;
    }
    const float tail = 1.0f - out;
    if (tail <= math::EPSILON) return 1.0f;
    return std::clamp((easeIn + 2.0f * (out - easeIn) +
                       (2.0f * (t - out) + out * out - t * t) / tail) / v0,
                      0.0f, 1.0f);
}

float GameCameraDirector::finiteOr(float value, float fallback) noexcept {
    return finite(value) ? value : fallback;
}

float GameCameraDirector::normalizedAngle(float radians) noexcept {
    if (!finite(radians)) return 0.0f;
    return std::remainder(radians, 2.0f * math::PI);
}

float GameCameraDirector::yawToward(math::vec3 pivot, math::vec3 target) noexcept {
    const float x = target.x() - pivot.x();
    const float y = target.y() - pivot.y();
    if (!finite(x) || !finite(y) || x * x + y * y <= math::EPSILON * math::EPSILON) {
        return 0.0f;
    }
    // m_scriptRig yaw is the eye radial, while an authored direction is the
    // camera's forward vector. Place the eye behind the forward direction.
    return normalizedAngle(std::atan2(y, x) + math::PI);
}

float GameCameraDirector::maximumScriptZoomAt(math::vec3 pivot) const noexcept {
    const float cameraOffset = std::max(
        m_scriptGeometry.initialGroundLevel + m_scriptGeometry.cameraHeight,
        1.0f);
    const float maximumHeight = std::max(
        pivot.z() + m_scriptGeometry.maximumHeightAboveGround, 1.0f);
    return std::clamp(maximumHeight / cameraOffset, 0.01f,
                      kMaximumScriptZoom);
}

math::vec3 GameCameraDirector::scriptZoomEndpoint() const noexcept {
    if (m_scriptPathTrack.active && !m_scriptPathTrack.pivots.empty())
        return m_scriptPathTrack.pivots.back();
    if (m_scriptPivotTrack.active)
        return m_scriptPivotTrack.destination;
    return m_scriptRig.pivot;
}

int32_t GameCameraDirector::interpolateVisualSpeedMultiplier(
    int32_t start, int32_t end, float factor) noexcept {
    const float clamped = std::clamp(finiteOr(factor, 0.0f), 0.0f, 1.0f);
    const double value = static_cast<double>(start) +
        (static_cast<double>(end) - static_cast<double>(start)) *
            static_cast<double>(clamped);
    if (!std::isfinite(value)) return clamped >= 0.5f ? end : start;

    // RefCode uses REAL_TO_INT_FLOOR(0.5 + value), rather than banker or
    // truncation rounding. Keep the same negative-value behavior and clamp
    // only at the modern int32 storage boundary.
    const double rounded = std::floor(value + 0.5);
    const double minimum = static_cast<double>(std::numeric_limits<int32_t>::min());
    const double maximum = static_cast<double>(std::numeric_limits<int32_t>::max());
    if (rounded <= minimum) return std::numeric_limits<int32_t>::min();
    if (rounded >= maximum) return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(rounded);
}

void GameCameraDirector::startScriptTrack(ScriptScalarTrack& track, float start,
                                          float destination, GameCameraScriptTiming timing) noexcept {
    const float duration = finiteOr(timing.durationSeconds, 0.0f);
    track.start = finiteOr(start, 0.0f);
    track.destination = finiteOr(destination, track.start);
    track.timing = {
        .durationSeconds = std::max(duration, 0.0f),
        .easeInSeconds = std::max(finiteOr(timing.easeInSeconds, 0.0f), 0.0f),
        .easeOutSeconds = std::max(finiteOr(timing.easeOutSeconds, 0.0f), 0.0f),
    };
    track.elapsedSeconds = 0.0f;
    track.startVisualSpeedMultiplier = 1;
    track.endVisualSpeedMultiplier = 1;
    track.drivesVisualSpeedMultiplier = false;
    track.active = track.timing.durationSeconds > math::EPSILON;
}

void GameCameraDirector::startScriptTrack(ScriptPivotTrack& track, math::vec3 start,
                                          math::vec3 destination,
                                          GameCameraScriptTiming timing) noexcept {
    const float duration = finiteOr(timing.durationSeconds, 0.0f);
    track.start = finiteVector(start) ? start : math::vec3{};
    track.destination = finiteVector(destination) ? destination : track.start;
    track.timing = {
        .durationSeconds = std::max(duration, 0.0f),
        .easeInSeconds = std::max(finiteOr(timing.easeInSeconds, 0.0f), 0.0f),
        .easeOutSeconds = std::max(finiteOr(timing.easeOutSeconds, 0.0f), 0.0f),
    };
    track.elapsedSeconds = 0.0f;
    // W3DView::setupWaypointPath resets this for every newly authored
    // MoveTo; a preceding modifier must never become a camera-wide default.
    track.rollingAverageFrames = 1;
    track.startVisualSpeedMultiplier = 1;
    track.endVisualSpeedMultiplier = 1;
    track.active = track.timing.durationSeconds > math::EPSILON;
}

void GameCameraDirector::seedVisualSpeedTrack(ScriptScalarTrack& track) noexcept {
    track.startVisualSpeedMultiplier = m_visualSpeedMultiplier;
    track.endVisualSpeedMultiplier = m_visualSpeedMultiplier;
    track.drivesVisualSpeedMultiplier = true;
}

void GameCameraDirector::seedVisualSpeedTrack(ScriptPivotTrack& track) noexcept {
    track.startVisualSpeedMultiplier = m_visualSpeedMultiplier;
    track.endVisualSpeedMultiplier = m_visualSpeedMultiplier;
}

void GameCameraDirector::seedVisualSpeedTrack(ScriptPathTrack& track) noexcept {
    track.startVisualSpeedMultiplier = m_visualSpeedMultiplier;
    track.endVisualSpeedMultiplier = m_visualSpeedMultiplier;
    track.visualSpeedMultipliers.assign(track.pivots.size(), m_visualSpeedMultiplier);
}

void GameCameraDirector::sampleVisualSpeedTrack(const ScriptScalarTrack& track,
                                                 float fixedDeltaSeconds) noexcept {
    if (!track.active || !track.drivesVisualSpeedMultiplier) return;
    const float duration = std::max(track.timing.durationSeconds, math::EPSILON);
    const float elapsed = std::min(duration, track.elapsedSeconds + fixedDeltaSeconds);
    const float factor = legacyParabolicProgress(elapsed / duration, track.timing);
    m_visualSpeedMultiplier = interpolateVisualSpeedMultiplier(
        track.startVisualSpeedMultiplier, track.endVisualSpeedMultiplier, factor);
}

void GameCameraDirector::sampleVisualSpeedTrack(const ScriptPivotTrack& track,
                                                 float fixedDeltaSeconds) noexcept {
    if (!track.active) return;
    const float duration = std::max(track.timing.durationSeconds, math::EPSILON);
    const float elapsed = std::min(duration, track.elapsedSeconds + fixedDeltaSeconds);
    const float factor = legacyParabolicProgress(elapsed / duration, track.timing);
    m_visualSpeedMultiplier = interpolateVisualSpeedMultiplier(
        track.startVisualSpeedMultiplier, track.endVisualSpeedMultiplier, factor);
}

void GameCameraDirector::sampleVisualSpeedTrack(const ScriptPathTrack& track,
                                                 float fixedDeltaSeconds) noexcept {
    if (!track.active) return;
    const float duration = std::max(track.timing.durationSeconds, math::EPSILON);
    const float elapsed = std::min(duration, track.elapsedSeconds + fixedDeltaSeconds);
    const float normalized = elapsed / duration;
    const float pathProgress = legacyParabolicProgress(normalized, track.timing);
    const float distance = track.totalDistance * pathProgress;
    if (track.visualSpeedMultipliers.size() == track.pivots.size() &&
        track.cumulativeDistances.size() == track.pivots.size() &&
        track.pivots.size() >= 2) {
        const auto end = track.cumulativeDistances.end();
        const auto found = std::upper_bound(track.cumulativeDistances.begin() + 1, end, distance);
        const size_t destinationIndex = found == end
            ? track.pivots.size() - 1
            : static_cast<size_t>(found - track.cumulativeDistances.begin());
        const size_t startIndex = destinationIndex - 1;
        const float segmentLength = track.cumulativeDistances[destinationIndex] -
            track.cumulativeDistances[startIndex];
        const float local = segmentLength > math::EPSILON
            ? std::clamp((distance - track.cumulativeDistances[startIndex]) / segmentLength,
                         0.0f, 1.0f)
            : 1.0f;
        m_visualSpeedMultiplier = interpolateVisualSpeedMultiplier(
            track.visualSpeedMultipliers[startIndex],
            track.visualSpeedMultipliers[destinationIndex], local);
        return;
    }
    m_visualSpeedMultiplier = interpolateVisualSpeedMultiplier(
        track.startVisualSpeedMultiplier, track.endVisualSpeedMultiplier, pathProgress);
}

bool GameCameraDirector::advanceScriptTrack(ScriptScalarTrack& track,
                                             float fixedDeltaSeconds, float& result) noexcept {
    if (!track.active) return false;
    track.elapsedSeconds = std::min(track.timing.durationSeconds,
                                    track.elapsedSeconds + fixedDeltaSeconds);
    const float t = track.timing.durationSeconds > math::EPSILON
        ? track.elapsedSeconds / track.timing.durationSeconds : 1.0f;
    if (t >= 1.0f) {
        result = track.destination;
        track.active = false;
        return true;
    }
    result = math::lerp(track.start, track.destination,
                        legacyParabolicProgress(t, track.timing));
    return true;
}

bool GameCameraDirector::advanceScriptTrack(ScriptPivotTrack& track,
                                             float fixedDeltaSeconds,
                                             math::vec3& result) noexcept {
    if (!track.active) return false;
    track.elapsedSeconds = std::min(track.timing.durationSeconds,
                                    track.elapsedSeconds + fixedDeltaSeconds);
    const float t = track.timing.durationSeconds > math::EPSILON
        ? track.elapsedSeconds / track.timing.durationSeconds : 1.0f;
    if (t >= 1.0f) {
        result = track.destination;
        track.active = false;
        return true;
    }
    result = math::vec3::lerp(track.start, track.destination,
                               legacyParabolicProgress(t, track.timing));
    return true;
}

void GameCameraDirector::clearScriptTracks() noexcept {
    m_scriptPivotTrack = {};
    m_scriptPathTrack = {};
    m_scriptYawTrack = {};
    m_scriptZoomTrack = {};
    m_scriptPitchTrack = {};
    m_scriptBaselineTrack = {};
    m_scriptElevationTrack = {};
    m_scriptHoldTrack = {};
    m_scriptMotionKind = ScriptMotionKind::None;
}

void GameCameraDirector::ensureScriptRig() noexcept {
    if (!m_scriptRigValid) synchronizeScriptRigFromCamera(false);
    if (!m_scriptDefaultsValid) {
        m_scriptDefaults = m_scriptRig;
        m_scriptDefaultsValid = true;
    }
}

void GameCameraDirector::synchronizeScriptRigFromCamera(bool captureDefaults) noexcept {
    m_camera = m_camera.sanitized();
    const math::vec3 radial = m_camera.position - m_camera.target;
    const float radialLength = radial.length();

    ScriptRigState rig;
    rig.pivot = m_camera.target;
    if (usableDirection(radial) && finite(radialLength)) {
        rig.baselineDistance = std::max(radialLength, kMinimumScriptCameraDistance);
        rig.yawRadians = std::atan2(radial.y(), radial.x());
        rig.orbitalElevationRadians = std::atan2(radial.z(),
            std::sqrt(radial.x() * radial.x() + radial.y() * radial.y()));
    } else {
        rig.baselineDistance = kMinimumScriptCameraDistance;
        rig.yawRadians = -math::PI * 0.5f;
        rig.orbitalElevationRadians = math::deg_to_rad(45.0f);
    }
    rig.normalizedZoom = 1.0f;
    rig.fxPitch = 1.0f;
    m_scriptRig = rig;
    m_scriptRigValid = true;
    if (captureDefaults) {
        m_scriptDefaults = rig;
        m_scriptDefaultsValid = true;
        m_scriptMapDefaults = rig;
        m_scriptMapDefaultsValid = true;
    }
}

void GameCameraDirector::applyScriptRigToCamera() noexcept {
    ensureScriptRig();
    ScriptRigState& rig = m_scriptRig;
    if (!finiteVector(rig.pivot)) rig.pivot = {};
    rig.baselineDistance = std::max(finiteOr(rig.baselineDistance,
                                             kMinimumScriptCameraDistance),
                                    kMinimumScriptCameraDistance);
    rig.normalizedZoom = std::clamp(finiteOr(rig.normalizedZoom, 1.0f),
                                    0.01f, kMaximumScriptZoom);
    rig.fxPitch = std::clamp(finiteOr(rig.fxPitch, 1.0f), 0.0f,
                             kMaximumScriptFxPitch);
    rig.yawRadians = finiteOr(rig.yawRadians, 0.0f);
    rig.orbitalElevationRadians = std::clamp(
        finiteOr(rig.orbitalElevationRadians, math::deg_to_rad(45.0f)),
        math::deg_to_rad(1.0f), math::deg_to_rad(89.0f));

    // Retail ZH computes the eye's absolute Z from the map's initial ground
    // level, not from the current pivot. This is why waypoint Z must remain a
    // separate durable value and why a hill must not multiply authored zoom.
    const float eyeZ =
        (m_scriptGeometry.initialGroundLevel + m_scriptGeometry.cameraHeight) *
        rig.normalizedZoom;
    const float verticalDistance = std::max(
        eyeZ - rig.pivot.z(), kMinimumScriptCameraDistance);
    const float horizontalDistance = verticalDistance /
        std::tan(rig.orbitalElevationRadians);
    const math::vec3 radial{
        horizontalDistance * std::cos(rig.yawRadians),
        horizontalDistance * std::sin(rig.yawRadians),
        verticalDistance,
    };

    GameCameraState result = m_camera;
    result.position = rig.pivot + radial;
    result.target = rig.pivot;
    // W3D's FX pitch leaves the eye/pivot plane intact and adjusts the
    // vertical look-at relation. This maintains that authored behavior while
    // still publishing a conventional position/target camera to D3D12.
    if (rig.fxPitch <= 1.0f) {
        result.target[2] = result.position.z() -
            (result.position.z() - rig.pivot.z()) * rig.fxPitch;
    } else {
        result.position[0] = rig.pivot.x() +
            (result.position.x() - rig.pivot.x()) / rig.fxPitch;
        result.position[1] = rig.pivot.y() +
            (result.position.y() - rig.pivot.y()) / rig.fxPitch;
    }
    result.up = {0.0f, 0.0f, 1.0f};
    m_camera = result.sanitized();
}

void GameCameraDirector::updateScriptTracks(float fixedDeltaSeconds) noexcept {
    ensureScriptRig();
    // W3DView advances Zoom, Pitch, then Rotate (or its waypoint movement)
    // in this exact order. A same-tick CAMERA_MOD_SET_FINAL_SPEED_MULTIPLIER
    // may have armed more than one of those independent tracks, so sample
    // their visual-clock values before mutating/retiring the tracks below.
    sampleVisualSpeedTrack(m_scriptZoomTrack, fixedDeltaSeconds);
    sampleVisualSpeedTrack(m_scriptPitchTrack, fixedDeltaSeconds);
    if (m_scriptMotionKind == ScriptMotionKind::Rotate) {
        sampleVisualSpeedTrack(m_scriptYawTrack, fixedDeltaSeconds);
    } else if (m_scriptMotionKind == ScriptMotionKind::Move) {
        if (m_scriptPathTrack.active) {
            sampleVisualSpeedTrack(m_scriptPathTrack, fixedDeltaSeconds);
        } else {
            sampleVisualSpeedTrack(m_scriptPivotTrack, fixedDeltaSeconds);
        }
    }
    math::vec3 pivot = m_scriptRig.pivot;
    float yaw = m_scriptRig.yawRadians;
    float zoom = m_scriptRig.normalizedZoom;
    float pitch = m_scriptRig.fxPitch;
    float baseline = m_scriptRig.baselineDistance;
    float elevation = m_scriptRig.orbitalElevationRadians;
    float hold = 0.0f;
    // `moveCameraTo` is implemented by RefCode through the same waypoint
    // state as a linked path.  Capture its eased progress before advancing
    // the yaw track so rolling average can approach the exact endpoint on
    // the final segment without delaying CAMERA_MOVEMENT_FINISHED.
    const bool smoothPivotYaw = m_scriptMotionKind == ScriptMotionKind::Move &&
        m_scriptPivotTrack.active;
    const int32_t pivotRollingAverageFrames = m_scriptPivotTrack.rollingAverageFrames;
    const float pivotProgress = smoothPivotYaw &&
            m_scriptPivotTrack.timing.durationSeconds > math::EPSILON
        ? legacyParabolicProgress(
              std::clamp((m_scriptPivotTrack.elapsedSeconds + fixedDeltaSeconds) /
                             m_scriptPivotTrack.timing.durationSeconds,
                         0.0f, 1.0f),
              m_scriptPivotTrack.timing)
        : 1.0f;
    if (m_scriptPathTrack.active) {
        updateScriptPathTrack(fixedDeltaSeconds);
        pivot = m_scriptRig.pivot;
        yaw = m_scriptRig.yawRadians;
    } else {
        static_cast<void>(advanceScriptTrack(m_scriptPivotTrack, fixedDeltaSeconds, pivot));
    }
    const float priorYaw = yaw;
    const bool advancedYaw = advanceScriptTrack(m_scriptYawTrack, fixedDeltaSeconds, yaw);
    if (smoothPivotYaw && advancedYaw) {
        const float baseFactor = 1.0f /
            static_cast<float>(std::max(pivotRollingAverageFrames, int32_t{1}));
        // In W3DView the simple MoveTo is a one-segment waypoint path, so
        // its final-segment factor rises from 1/N to one over the move.
        const float averageFactor = baseFactor + (1.0f - baseFactor) * pivotProgress;
        yaw = priorYaw + normalizedAngle(yaw - priorYaw) * averageFactor;
    }
    static_cast<void>(advanceScriptTrack(m_scriptZoomTrack, fixedDeltaSeconds, zoom));
    static_cast<void>(advanceScriptTrack(m_scriptPitchTrack, fixedDeltaSeconds, pitch));
    static_cast<void>(advanceScriptTrack(m_scriptBaselineTrack, fixedDeltaSeconds, baseline));
    static_cast<void>(advanceScriptTrack(m_scriptElevationTrack, fixedDeltaSeconds, elevation));
    static_cast<void>(advanceScriptTrack(m_scriptHoldTrack, fixedDeltaSeconds, hold));
    m_scriptRig.pivot = pivot;
    m_scriptRig.yawRadians = yaw;
    m_scriptRig.normalizedZoom = zoom;
    m_scriptRig.fxPitch = pitch;
    m_scriptRig.baselineDistance = baseline;
    m_scriptRig.orbitalElevationRadians = elevation;
    applyScriptRigToCamera();
    if (m_scriptMotionKind == ScriptMotionKind::Move && !m_scriptPathTrack.active &&
        !m_scriptPivotTrack.active) {
        m_scriptMotionKind = ScriptMotionKind::None;
    } else if (m_scriptMotionKind == ScriptMotionKind::Rotate && !m_scriptYawTrack.active &&
               !m_scriptHoldTrack.active) {
        m_scriptMotionKind = ScriptMotionKind::None;
    }
    if (!hasScriptTracksActive()) m_status = GameCameraTransitionStatus::Finished;
}

void GameCameraDirector::updateScriptPathTrack(float fixedDeltaSeconds) noexcept {
    ScriptPathTrack& path = m_scriptPathTrack;
    if (!path.active || path.pivots.size() < 2 ||
        path.cumulativeDistances.size() != path.pivots.size() ||
        path.totalDistance <= math::EPSILON) {
        path = {};
        return;
    }

    path.elapsedSeconds = std::min(path.timing.durationSeconds,
                                   path.elapsedSeconds + fixedDeltaSeconds);
    const float normalized = path.timing.durationSeconds > math::EPSILON
        ? path.elapsedSeconds / path.timing.durationSeconds : 1.0f;
    const float distance = path.totalDistance *
        legacyParabolicProgress(normalized, path.timing);
    const auto end = path.cumulativeDistances.end();
    const auto found = std::upper_bound(path.cumulativeDistances.begin() + 1, end, distance);
    const size_t destinationIndex = found == end
        ? path.pivots.size() - 1
        : static_cast<size_t>(found - path.cumulativeDistances.begin());
    const size_t startIndex = destinationIndex - 1;
    const float segmentStart = path.cumulativeDistances[startIndex];
    const float segmentEnd = path.cumulativeDistances[destinationIndex];
    const float segmentLength = segmentEnd - segmentStart;
    const float local = segmentLength > math::EPSILON
        ? std::clamp((distance - segmentStart) / segmentLength, 0.0f, 1.0f)
        : 1.0f;
    m_scriptRig.pivot = math::vec3::lerp(path.pivots[startIndex],
                                          path.pivots[destinationIndex], local);
    const float previousYaw = m_scriptRig.yawRadians;
    float desiredYaw = previousYaw;
    bool hasDesiredYaw = false;
    if (path.lookToward) {
        const math::vec3 delta = *path.lookToward - m_scriptRig.pivot;
        if (delta.x() * delta.x() + delta.y() * delta.y() > math::EPSILON * math::EPSILON) {
            desiredYaw = yawToward(m_scriptRig.pivot, *path.lookToward);
            hasDesiredYaw = true;
        }
    } else if (path.orientAlongMotion) {
        const math::vec3 delta = path.pivots[destinationIndex] - path.pivots[startIndex];
        if (delta.x() * delta.x() + delta.y() * delta.y() > math::EPSILON * math::EPSILON) {
            desiredYaw = yawToward(path.pivots[startIndex],
                                   path.pivots[destinationIndex]);
            hasDesiredYaw = true;
        }
    }
    if (path.finalLookToward) {
        const math::vec3 delta = *path.finalLookToward - m_scriptRig.pivot;
        if (delta.x() * delta.x() + delta.y() * delta.y() > math::EPSILON * math::EPSILON) {
            // W3DView::cameraModFinalLookToward rewrites only its final
            // waypoint angles (two where the path is long enough), blending
            // the first one halfway and the endpoint all the way. Our path
            // is distance-parameterized instead of node-spline based, so
            // apply the equivalent shortest-arc blend over its final two
            // segments while retaining the full-path/orient result earlier.
            const size_t segmentCount = path.pivots.size() - 1;
            const size_t firstFinalSegment = segmentCount > 2 ? segmentCount - 2 : 0;
            if (startIndex >= firstFinalSegment) {
                const float segmentProgress = static_cast<float>(startIndex - firstFinalSegment) + local;
                const float finalProgress = std::clamp(
                    segmentProgress / static_cast<float>(segmentCount - firstFinalSegment),
                    0.0f, 1.0f);
                const float targetYaw = yawToward(m_scriptRig.pivot, *path.finalLookToward);
                desiredYaw += normalizedAngle(targetYaw - desiredYaw) * finalProgress;
                hasDesiredYaw = true;
            }
        }
    }
    if (hasDesiredYaw) {
        const float baseFactor = 1.0f /
            static_cast<float>(std::max(path.rollingAverageFrames, int32_t{1}));
        // RefCode blends only the angle (`View::setAngle`), not the path
        // position. Its final path segment progressively raises the factor
        // to one, preventing a visible lag at the authored endpoint.
        const float averageFactor = destinationIndex == path.pivots.size() - 1
            ? baseFactor + (1.0f - baseFactor) * local
            : baseFactor;
        m_scriptRig.yawRadians = previousYaw +
            normalizedAngle(desiredYaw - previousYaw) * averageFactor;
    }
    if (normalized >= 1.0f) {
        m_scriptRig.pivot = path.pivots.back();
        path.active = false;
    }
}

void GameCameraDirector::beginScriptCameraOperation() noexcept {
    // Full-pose transitions are a separate modern API. A legacy script action
    // takes over from one exactly as an ordinary W3D scripted camera command
    // did, but it preserves unrelated legacy property tracks below.
    m_active = {};
    ensureScriptRig();
}

void GameCameraDirector::advanceCameraCut() noexcept {
    if (m_cameraCutRevision == std::numeric_limits<uint64_t>::max()) {
        m_cameraCutRevision = 1;
    } else {
        ++m_cameraCutRevision;
    }
    m_camera.cameraCutRevision = m_cameraCutRevision;
}

} // namespace engine
