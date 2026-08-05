#include "engine/renderer/world/terrain/GroundDecalPresentation.h"
#include "engine/renderer/world/terrain/D3D12TerrainVisual.h"
#include "engine/renderer/world/terrain/TerrainTileMeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace engine::render {
namespace {

[[nodiscard]] bool finite(const GroundDecalPresentationEvent& event) noexcept {
    const bool legacyRadius = std::isfinite(event.radius) &&
        event.radius > 0.0f;
    const bool rectangularFootprint =
        event.key.source == GroundDecalPresentationSource::ObjectTerrain &&
        ((event.sizeX > 0.0f && event.sizeY > 0.0f) || legacyRadius);
    return event.key.object && !event.textureName.empty() &&
        std::isfinite(event.position.x()) &&
        std::isfinite(event.position.y()) &&
        std::isfinite(event.position.z()) && std::isfinite(event.radius) &&
        std::isfinite(event.sizeX) && std::isfinite(event.sizeY) &&
        std::isfinite(event.offsetX) && std::isfinite(event.offsetY) &&
        std::isfinite(event.yawRadians) &&
        (rectangularFootprint || legacyRadius) &&
        std::isfinite(event.minimumOpacity) &&
        std::isfinite(event.maximumOpacity) &&
        std::isfinite(event.fadeInRatePerFrame) &&
        std::isfinite(event.fadeOutRatePerFrame) &&
        std::isfinite(event.authoritativeOpacity) &&
        event.fadeInRatePerFrame >= 0.0f &&
        event.fadeOutRatePerFrame >= 0.0f &&
        event.authoritativeOpacity >= 0.0f &&
        event.authoritativeOpacity <= 1.0f &&
        std::isfinite(event.color.x()) && std::isfinite(event.color.y()) &&
        std::isfinite(event.color.z()) && std::isfinite(event.color.w());
}

[[nodiscard]] GroundProjectorBlendMode blendMode(uint32_t mask) noexcept {
    return (mask & 0x40u) != 0
        ? GroundProjectorBlendMode::Additive
        : GroundProjectorBlendMode::Alpha;
}

[[nodiscard]] float throbOpacity(const GroundDecalPresentationEvent& event,
                                 uint64_t frame) noexcept {
    const float minimum = std::clamp(event.minimumOpacity, 0.0f, 1.0f);
    const float maximum = std::clamp(
        std::max(event.minimumOpacity, event.maximumOpacity), 0.0f, 1.0f);
    const uint64_t period = std::max<uint64_t>(1, event.opacityThrobFrames);
    const float theta = 2.0f * std::numbers::pi_v<float> *
        static_cast<float>(frame % period) / static_cast<float>(period);
    const float percent = 0.5f * (std::sin(theta) + 1.0f);
    return minimum + percent * (maximum - minimum);
}

[[nodiscard]] float legacyGridSnap(float value, uint32_t snap) noexcept {
    if (snap == 0 || !std::isfinite(value)) return value;
    const int64_t integral = static_cast<int64_t>(value);
    return value - static_cast<float>(
        integral % static_cast<int64_t>(snap));
}

void appendEventProjectors(
    container::Vector<GroundProjectorInstance>& output,
    const GroundDecalPresentationEvent& event,
    RenderVector position, float radius, float opacity,
    const TerrainRenderSnapshot* terrain,
    const RenderCameraSnapshot* camera,
    float viewportAspectRatio,
    const TerrainPrimaryCellTopologyResolver* topology) {
    if (opacity <= 0.0f || (event.shadowTypeMask & 0x7fu) == 0) return;
    const float visibilityRadius = event.key.source ==
            GroundDecalPresentationSource::ObjectTerrain
        ? 0.5f * std::hypot(
              event.sizeX > 0.0f ? event.sizeX : radius * 2.0f,
              event.sizeY > 0.0f ? event.sizeY : radius * 2.0f)
        : radius * std::sqrt(2.0f);
    if (camera &&
        !D3D12TerrainVisual::chunkSphereVisible(
            *camera, viewportAspectRatio, position,
            std::max(visibilityRadius, 0.0f))) {
        return;
    }
    const math::vec4 color{
        event.color.x(), event.color.y(), event.color.z(),
        std::clamp(event.color.w() * opacity, 0.0f, 1.0f)};
    if (event.key.source == GroundDecalPresentationSource::ObjectTerrain) {
        const float legacyDiameter = radius * 2.0f;
        GroundProjectorRenderer::appendTexturedRectDecals(
            output,
            position,
            event.sizeX > 0.0f ? event.sizeX : legacyDiameter,
            event.sizeY > 0.0f ? event.sizeY : legacyDiameter,
            event.offsetX, event.offsetY, event.yawRadians,
            event.textureName, color, blendMode(event.shadowTypeMask),
            terrain, 0, topology);
    } else {
        GroundProjectorRenderer::appendTexturedDecals(
            output,
            position, radius, 0.0f, event.textureName, color,
            blendMode(event.shadowTypeMask), terrain, 0, topology);
    }
}

} // namespace

bool GroundDecalPresentation::submit(
    const GroundDecalPresentationBatch& batch) {
    if (batch.presentationEpoch == 0) return false;
    if (m_presentationEpoch != 0 &&
        batch.presentationEpoch < m_presentationEpoch) {
        m_stats.staleRejectedEvents += std::max<size_t>(
            1u, batch.events.size());
        return false;
    }
    if (batch.presentationEpoch != m_presentationEpoch ||
        batch.confirmedFrame < m_confirmedFrame) {
        reset(batch.presentationEpoch);
        ++m_stats.epochResets;
    }
    m_presentationEpoch = batch.presentationEpoch;
    m_confirmedFrame = batch.confirmedFrame;
    m_observerPlayer = batch.observerPlayer;
    m_drawIconUiEnabled = batch.drawIconUiEnabled;

    // Advance legacy Drawable decal fades once per confirmed frame, not once
    // per render submission. This remains deterministic across pause/replay
    // and does not make the renderer infer a gameplay clock from wall time.
    container::Vector<GroundDecalPresentationKey> completedFades;
    for (auto& [key, record] : m_active) {
        const uint64_t elapsed = batch.confirmedFrame > record.lastFadeFrame
            ? batch.confirmedFrame - record.lastFadeFrame : 0;
        if (elapsed != 0 && !record.value.hasAuthoritativeOpacity) {
            const float rate = record.fadingOut
                ? record.value.fadeOutRatePerFrame
                : record.value.fadeInRatePerFrame;
            if (rate > 0.0f) {
                const float delta = rate * static_cast<float>(elapsed);
                record.fadeOpacity = std::clamp(
                    record.fadeOpacity + (record.fadingOut ? -delta : delta),
                    0.0f, 1.0f);
            }
            record.lastFadeFrame = batch.confirmedFrame;
        }
        if (record.fadingOut && record.fadeOpacity <= 0.0f) {
            completedFades.push_back(key);
        }
    }
    for (const GroundDecalPresentationKey& key : completedFades) {
        m_active.erase(key);
    }

    for (const GroundDecalPresentationEvent& event : batch.events) {
        auto found = m_active.find(event.key);
        if (found != m_active.end() &&
            (event.confirmedFrame < found->second.lastConfirmedFrame ||
             (event.confirmedFrame == found->second.lastConfirmedFrame &&
              event.streamSequence <= found->second.lastStreamSequence))) {
            ++m_stats.staleRejectedEvents;
            continue;
        }
        if (event.kind == GroundDecalPresentationEventKind::End) {
            if (found != m_active.end()) m_active.erase(found);
            ++m_stats.appliedEvents;
            continue;
        }
        if (!finite(event)) continue;
        if (event.kind == GroundDecalPresentationEventKind::Update) {
            if (found == m_active.end()) {
                ++m_stats.orphanUpdateEvents;
                continue;
            }
            found->second.value.position = event.position;
            if (event.key.source ==
                GroundDecalPresentationSource::ObjectTerrain) {
                // Drawable terrain decals are mutable presentation state:
                // Horde can replace its texture after Nationalism/Fanaticism,
                // upgrades can change the active kind, and the legacy fade
                // clock changes opacity without ending the owner.  Update is
                // therefore a complete detached value for this source, not
                // merely a transform sample.
                found->second.value = event;
                found->second.fadingOut = false;
                found->second.lastFadeFrame = batch.confirmedFrame;
                if (event.hasAuthoritativeOpacity) {
                    found->second.fadeOpacity = event.authoritativeOpacity;
                }
            }
            found->second.value.currentClearingRange =
                event.currentClearingRange;
            found->second.value.nativeClearingRange =
                event.nativeClearingRange;
            found->second.value.totalFrames = event.totalFrames;
            found->second.value.stateCountdown = event.stateCountdown;
            found->second.lastConfirmedFrame = event.confirmedFrame;
            found->second.lastStreamSequence = event.streamSequence;
        } else {
            if (found == m_active.end() &&
                m_active.size() >= ground_decals::performance_limits::
                    kHardMaximumPersistentOwners) {
                ++m_stats.budgetRejectedOwners;
                continue;
            }
            if (found != m_active.end() &&
                event.key.source ==
                    GroundDecalPresentationSource::ObjectTerrain) {
                // Complete-owner extraction republishes Begin as the current
                // desired value every frame. Replace material/pose without
                // restarting the Drawable fade clock.
                found->second.value = event;
                found->second.lastConfirmedFrame = event.confirmedFrame;
                found->second.lastStreamSequence = event.streamSequence;
                found->second.lastFadeFrame = batch.confirmedFrame;
                found->second.fadingOut = false;
                if (event.hasAuthoritativeOpacity) {
                    found->second.fadeOpacity = event.authoritativeOpacity;
                }
            } else {
                m_active[event.key] = ActiveRecord{
                    .value = event,
                    .lastConfirmedFrame = event.confirmedFrame,
                    .lastStreamSequence = event.streamSequence,
                    .lastFadeFrame = batch.confirmedFrame,
                    .fadeOpacity = event.hasAuthoritativeOpacity
                        ? event.authoritativeOpacity
                        : (event.fadeInRatePerFrame > 0.0f ? 0.0f : 1.0f),
                };
            }
        }
        ++m_stats.appliedEvents;
    }
    if (batch.hasCompleteOwnerSet) {
        container::HashSet<GroundDecalPresentationKey,
                           GroundDecalPresentationKeyHash> synchronized;
        synchronized.reserve(batch.synchronizedOwners.size());
        for (const GroundDecalPresentationKey& key :
             batch.synchronizedOwners) {
            synchronized.insert(key);
        }
        container::Vector<GroundDecalPresentationKey> staleOwners;
        for (const auto& [key, record] : m_active) {
            if (!synchronized.contains(key)) staleOwners.push_back(key);
        }
        for (const GroundDecalPresentationKey& key : staleOwners) {
            auto found = m_active.find(key);
            if (found != m_active.end() &&
                key.source == GroundDecalPresentationSource::ObjectTerrain &&
                found->second.value.fadeOutRatePerFrame > 0.0f &&
                found->second.fadeOpacity > 0.0f) {
                found->second.fadingOut = true;
                found->second.lastFadeFrame = batch.confirmedFrame;
            } else {
                m_active.erase(key);
            }
        }
    }
    m_stats.activeOwners = static_cast<uint32_t>(m_active.size());
    m_stats.highWaterOwners = std::max(
        m_stats.highWaterOwners, m_stats.activeOwners);
    return true;
}

container::Vector<GroundProjectorInstance>
GroundDecalPresentation::buildProjectors(
    const TerrainRenderSnapshot* terrain,
    const RenderCameraSnapshot* camera,
    float viewportAspectRatio) noexcept {
    container::Vector<GroundProjectorInstance> output;
    buildProjectorsInto(output, terrain, camera, viewportAspectRatio);
    return output;
}

void GroundDecalPresentation::buildProjectorsInto(
    container::Vector<GroundProjectorInstance>& output,
    const TerrainRenderSnapshot* terrain,
    const RenderCameraSnapshot* camera,
    float viewportAspectRatio) noexcept {
    output.clear();
    appendProjectors(output, terrain, camera, viewportAspectRatio);
}

void GroundDecalPresentation::appendProjectors(
    container::Vector<GroundProjectorInstance>& output,
    const TerrainRenderSnapshot* terrain,
    const RenderCameraSnapshot* camera,
    float viewportAspectRatio) noexcept {
    const size_t initialSize = output.size();
    m_stats.emittedProjectors = 0;
    if (!terrain || !terrain->isValid()) return;
    const TerrainPrimaryCellTopologyResolver topology =
        prepareTerrainPrimaryCellTopologyResolver(*terrain);
    for (const auto& [key, record] : m_active) {
        static_cast<void>(key);
        const GroundDecalPresentationEvent& event = record.value;
        if (event.onlyVisibleToOwningPlayer &&
            m_observerPlayer != event.ownerPlayer) {
            continue;
        }
        if (!m_drawIconUiEnabled && event.requiresDrawIconUi) continue;
        if (event.key.source ==
            GroundDecalPresentationSource::DynamicShroudGrid) {
            if (event.decalCount == 0 || event.nativeClearingRange <= 0.0f) {
                continue;
            }
            const float elapsed = static_cast<float>(
                event.totalFrames >= event.stateCountdown
                    ? event.totalFrames - event.stateCountdown : 0);
            const float ringRadius =
                event.currentClearingRange + elapsed * 2.0f;
            const float opacity = std::clamp(
                1.0f - event.currentClearingRange /
                    event.nativeClearingRange,
                0.0f, 1.0f);
            const float angleStep = 2.0f * std::numbers::pi_v<float> /
                static_cast<float>(event.decalCount);
            for (uint32_t index = 0; index < event.decalCount; ++index) {
                const float angle = angleStep * static_cast<float>(index);
                const RenderVector position{
                    legacyGridSnap(event.position.x() +
                                       std::sin(angle) * ringRadius,
                                   event.gridSnapSize),
                    legacyGridSnap(event.position.y() +
                                       std::cos(angle) * ringRadius,
                                   event.gridSnapSize),
                    event.position.z(),
                };
                appendEventProjectors(output, event, position,
                                      event.initialDecalRadius, opacity,
                                      terrain, camera,
                                      viewportAspectRatio, &topology);
            }
        } else {
            appendEventProjectors(
                output, event, event.position, event.radius,
                throbOpacity(event, m_confirmedFrame) * record.fadeOpacity,
                terrain, camera, viewportAspectRatio, &topology);
        }
    }
    m_stats.emittedProjectors = static_cast<uint32_t>(std::min<size_t>(
        output.size() - initialSize, UINT32_MAX));
}

void GroundDecalPresentation::reset(uint64_t presentationEpoch) noexcept {
    m_active.clear();
    m_presentationEpoch = presentationEpoch;
    m_confirmedFrame = 0;
    m_observerPlayer = 0xff;
    m_drawIconUiEnabled = true;
    m_stats.activeOwners = 0;
    m_stats.emittedProjectors = 0;
}

} // namespace engine::render
