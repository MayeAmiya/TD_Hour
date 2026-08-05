#include "engine/renderer/world/resource/WorldStartupSceneTicket.h"

#include "engine/renderer/runtime/RendererStats.h"
#include "engine/renderer/world/model/W3dAnimationCache.h"
#include "engine/renderer/world/pipeline/WorldRenderPipeline.h"

#include <algorithm>
#include <limits>
#include <optional>

namespace engine::render {

void WorldStartupSceneTicket::begin(
    uint64_t epoch, uint64_t session, uint64_t loading) {
    if (m_presentationEpoch != epoch || m_sessionRevision != session ||
        m_loadingRevision != loading) {
        m_presentationEpoch = epoch;
        m_sessionRevision = session;
        m_loadingRevision = loading;
    }
    m_models.clear();
    m_animations.clear();
}

void WorldStartupSceneTicket::reset() {
    m_presentationEpoch = 0;
    m_sessionRevision = 0;
    m_loadingRevision = 0;
    m_models.clear();
    m_animations.clear();
}

void WorldStartupSceneTicket::addModel(W3dModelHandle handle) {
    if (handle && std::find(m_models.begin(), m_models.end(), handle) ==
                      m_models.end()) {
        m_models.push_back(handle);
    }
}

void WorldStartupSceneTicket::addAnimation(container::StringView animation) {
    if (animation.empty()) return;
    if (std::find(m_animations.begin(), m_animations.end(), animation) ==
        m_animations.end()) {
        m_animations.emplace_back(animation);
    }
}

bool WorldStartupSceneTicket::matches(
    const PreparedWorldFrame& frame) const noexcept {
    return m_loadingRevision != 0u &&
        m_presentationEpoch == frame.presentationEpoch &&
        m_sessionRevision == frame.sessionRevision &&
        m_loadingRevision == frame.loadingRevision;
}

StartupSceneTicketRenderStats WorldStartupSceneTicket::projectStats(
    const PreparedWorldFrame& frame,
    const W3dAssetCache& assets,
    const W3dAnimationCache& animations,
    bool terrainReady,
    bool bibRequired,
    bool bibsReady,
    bool requiredFailed) const {
    StartupSceneTicketRenderStats result;
    if (!matches(frame)) return result;

    const auto boundedU32 = [](size_t value) noexcept {
        return static_cast<uint32_t>(std::min<size_t>(
            value, std::numeric_limits<uint32_t>::max()));
    };
    result.presentationEpoch = frame.presentationEpoch;
    result.sessionRevision = frame.sessionRevision;
    result.loadingRevision = frame.loadingRevision;
    result.requiredTotal = 1u + static_cast<uint32_t>(bibRequired);
    result.requiredReady = static_cast<uint32_t>(terrainReady) +
        static_cast<uint32_t>(bibRequired && bibsReady);
    result.requiredFailed = requiredFailed ? result.requiredTotal : 0u;
    result.requiredPending = result.requiredTotal -
        std::min(result.requiredTotal,
                 result.requiredReady + result.requiredFailed);
    result.optionalTotal = boundedU32(
        m_models.size() + m_animations.size());

    for (const W3dModelHandle handle : m_models) {
        const std::optional<W3dAssetState> state = assets.state(handle);
        if (!state || *state == W3dAssetState::Failed) {
            ++result.optionalDegraded;
            continue;
        }
        if (*state != W3dAssetState::GpuReady) {
            ++result.optionalPending;
            continue;
        }
        bool degraded = false;
        if (const auto dependencies = assets.dependencies(handle)) {
            degraded = std::any_of(
                dependencies->nodes.begin(), dependencies->nodes.end(),
                [](const W3dDependencyNode& node) {
                    return node.state == W3dDependencyState::Missing ||
                        node.state == W3dDependencyState::Fallback;
                });
        }
        if (degraded) ++result.optionalDegraded;
        else ++result.optionalReady;
    }
    for (const container::String& animation : m_animations) {
        const W3dAnimationDependency dependency =
            animations.dependency(animation);
        if (dependency.ready) ++result.optionalReady;
        else if (!dependency.diagnostic.empty())
            ++result.optionalDegraded;
        else
            ++result.optionalPending;
    }

    if (result.requiredFailed != 0u) {
        result.state = StartupSceneTicketState::Failed;
    } else if (result.requiredPending != 0u ||
               result.optionalPending != 0u) {
        // Every model/animation referenced by the sealed startup snapshot is
        // part of the first visible scene. "Optional" means a missing asset
        // may degrade instead of aborting the session; it does not mean a
        // still-loading asset may appear progressively after Loading retires.
        result.state = StartupSceneTicketState::Pending;
    } else if (result.optionalDegraded != 0u) {
        result.state = StartupSceneTicketState::Degraded;
    } else {
        result.state = StartupSceneTicketState::Ready;
    }
    return result;
}

} // namespace engine::render
