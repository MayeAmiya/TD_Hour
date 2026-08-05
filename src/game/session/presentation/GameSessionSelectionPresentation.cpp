#include "game/session/presentation/GameSessionPresentationPort.h"

#include "game/audio/GameAudioEvents.h"
#include "game/base/GameCameraDirector.h"
#include "core/container/string_utils.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/script/contracts/ScriptPresentationLimits.h"
#include "game/script/presentation/ScriptCinematicPresentationControls.h"
#include "game/selection/LocalSelectionState.h"
#include "game/session/state/GameSessionDomainState.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace engine {
namespace {

[[nodiscard]] bool finitePosition(const math::vec3& position) noexcept {
    return std::isfinite(position.x()) && std::isfinite(position.y()) &&
        std::isfinite(position.z());
}

[[nodiscard]] bool validAudioName(container::StringView name) noexcept {
    return name.size() <= script::kMaximumScriptPresentationNameLength &&
        name.find('\0') == container::StringView::npos &&
        !container::asciiEqualIgnoreCase(name, "NoSound");
}

[[nodiscard]] bool hasLiveVisual(
    const ObjectLifecycle& objects,
    const ecs::registry& registry,
    ObjectId object) noexcept {
    const std::optional<ecs::entity> entity = objects.entityFromId(object);
    if (!entity) return false;
    const RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(registry, *entity);
    return visual && !visual->modelAsset.empty();
}

[[nodiscard]] std::optional<math::vec3> selectedCentroid(
    const ObjectLifecycle& objects,
    const ecs::registry& registry,
    const selection::LocalSelectionState& selection) noexcept {
    double sumX = 0.0;
    double sumY = 0.0;
    double sumZ = 0.0;
    size_t count = 0;
    for (const ObjectId object : selection.selected()) {
        const std::optional<ecs::entity> entity = objects.entityFromId(object);
        if (!entity) continue;
        const TransformComponent* transform =
            ecs::try_get<TransformComponent>(registry, *entity);
        if (!transform || !std::isfinite(transform->x) ||
            !std::isfinite(transform->y) ||
            !std::isfinite(transform->z)) {
            continue;
        }
        sumX += static_cast<double>(transform->x);
        sumY += static_cast<double>(transform->y);
        sumZ += static_cast<double>(transform->z);
        if (!std::isfinite(sumX) || !std::isfinite(sumY) ||
            !std::isfinite(sumZ)) {
            return std::nullopt;
        }
        ++count;
    }
    if (count == 0) return std::nullopt;

    const double inverseCount = 1.0 / static_cast<double>(count);
    const double centerX = sumX * inverseCount;
    const double centerY = sumY * inverseCount;
    const double centerZ = sumZ * inverseCount;
    constexpr double maximumFloat =
        static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(centerX) || !std::isfinite(centerY) ||
        !std::isfinite(centerZ) || centerX < -maximumFloat ||
        centerX > maximumFloat || centerY < -maximumFloat ||
        centerY > maximumFloat || centerZ < -maximumFloat ||
        centerZ > maximumFloat) {
        return std::nullopt;
    }
    return math::vec3{
        static_cast<float>(centerX), static_cast<float>(centerY),
        static_cast<float>(centerZ)};
}

} // namespace

bool GameSessionPresentationPort::emitLocalUnitVoice(
    container::StringView eventName, ObjectId object) {
    if (eventName.empty() || !object || !validAudioName(eventName))
        return false;
    // A unit with no visual has either not finished spawning or is contained,
    // and RefCode's CommandXlat walks drawables for exactly this reason: an
    // object the player cannot see must not answer out loud.
    if (!hasLiveVisual(m_world.m_objects, m_world.m_registry, object))
        return false;
    return m_publication.emitAudioEvent({
        .eventName = container::String{eventName},
        .emitter = object,
        .owner = object,
    });
}

bool GameSessionPresentationPort::consumeForceObjectSelection(
    container::Span<
        const script::ScriptForceObjectSelectionPresentation> requests,
    selection::LocalSelectionState& selection,
    uint64_t confirmedTick) {
    bool changed = false;
    const uint64_t epoch = m_presentation.m_scriptPresentationEpoch;
    for (const auto& request : requests) {
        if (request.stamp.presentationEpoch != epoch ||
            request.stamp.sequence == 0 ||
            request.stamp.confirmedTick > confirmedTick ||
            !request.object || !validAudioName(request.audioEventName) ||
            !hasLiveVisual(m_world.m_objects, m_world.m_registry,
                           request.object)) {
            continue;
        }
        const container::Array<ObjectId, 1> selected = {request.object};
        changed = selection.replace(selected) || changed;
        if (!request.audioEventName.empty()) {
            changed = m_publication.emitAudioEvent({
                .eventName = request.audioEventName,
                .emitter = request.object,
                .owner = request.object,
            }) || changed;
        }
        if (request.centerInView && request.position &&
            finitePosition(*request.position)) {
            const int framesPerSecond =
                std::max(1, m_content.m_startInfo.gameSpeedFPS);
            ++m_presentation.m_scriptCameraMovementRevision;
            if (m_presentation.m_scriptCameraMovementRevision == 0)
                ++m_presentation.m_scriptCameraMovementRevision;
            script::ScriptCameraPresentationCommand cameraCommand{
                .stamp = request.stamp,
                .operation = script::ScriptCameraPresentationOperation::MoveTo,
                .movementRevision =
                    m_presentation.m_scriptCameraMovementRevision,
                .position = *request.position,
                .durationSeconds =
                    1.0f / static_cast<float>(framesPerSecond),
                .orientAlongMotion = false,
            };
            auto& journal =
                m_presentation.m_scriptCameraPresentationJournal;
            journal.insert(std::upper_bound(
                journal.begin(), journal.end(), request.stamp.sequence,
                [](uint64_t sequence, const auto& existing) {
                    return sequence < existing.stamp.sequence;
                }), std::move(cameraCommand));
            static_cast<void>(script::trimScriptPresentationJournal(
                journal, script::kMaximumScriptCameraPresentationCommands,
                m_presentation
                    .m_scriptCameraPresentationJournalTrimmedThroughSequence));
            changed = true;
        }
    }
    return changed;
}

bool GameSessionPresentationPort::consumeMoveCameraToSelection(
    container::Span<
        const script::ScriptMoveCameraToSelectionPresentation> requests,
    const selection::LocalSelectionState& selection,
    uint64_t confirmedTick) {
    bool changed = false;
    const uint64_t epoch = m_presentation.m_scriptPresentationEpoch;
    for (const auto& request : requests) {
        if (request.stamp.presentationEpoch != epoch ||
            request.stamp.sequence == 0 ||
            request.stamp.confirmedTick > confirmedTick) {
            continue;
        }
        const std::optional<math::vec3> centroid =
            selectedCentroid(
                m_world.m_objects, m_world.m_registry, selection);
        if (!centroid) continue;
        script::ScriptCameraPresentationCommand cameraCommand{
            .stamp = request.stamp,
            .operation = script::ScriptCameraPresentationOperation::
                ModifyFinalPivot,
            .movementRevision =
                m_presentation.m_scriptCameraMovementRevision,
            .position = *centroid,
        };
        auto& journal = m_presentation.m_scriptCameraPresentationJournal;
        journal.insert(std::upper_bound(
            journal.begin(), journal.end(), request.stamp.sequence,
            [](uint64_t sequence, const auto& existing) {
                return sequence < existing.stamp.sequence;
            }), std::move(cameraCommand));
        static_cast<void>(script::trimScriptPresentationJournal(
            journal, script::kMaximumScriptCameraPresentationCommands,
            m_presentation
                .m_scriptCameraPresentationJournalTrimmedThroughSequence));
        changed = true;
    }
    return changed;
}

} // namespace engine
