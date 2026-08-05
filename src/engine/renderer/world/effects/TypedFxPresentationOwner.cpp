#include "engine/renderer/world/effects/TypedFxPresentationOwner.h"

#include "core/math/wwmath/base/wwmath_core.h"
#include "debug/debug.h"
#include "engine/renderer/world/model/W3dAssetCache.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

namespace engine::render {

TypedFxPresentationOwner::TypedFxPresentationOwner(
    d3d12::D3D12Device& device,
    container::SharedPtr<WorldTextureCache> textures)
    : m_worldRenderer(device, std::move(textures)) {}

void TypedFxPresentationOwner::reset(uint64_t sessionEpoch) {
    m_worldRenderer.reset();
    m_dynamicPointLights.reset(sessionEpoch);
    m_terrainScorches.clear();
    m_viewShakes.clear();
    m_pendingSounds.clear();
    m_sessionEpoch = sessionEpoch;
    m_lastShakeSimulationFrame = 0;
    m_shakeDirection = {};
    m_shakeOffset = {};
    m_shakeIntensity = 0.0f;
    m_rejectedCommands = 0;
}

void TypedFxPresentationOwner::consume(
    fx::FxPresentationCommandBatch commands,
    W3dAssetCache& assets,
    size_t maximumOwnerCommands) {
    if (commands.sessionEpoch == 0) {
        reset();
        return;
    }
    if (m_sessionEpoch != 0 && commands.sessionEpoch < m_sessionEpoch) {
        m_rejectedCommands += commands.size();
        return;
    }
    if (commands.sessionEpoch != m_sessionEpoch) {
        reset(commands.sessionEpoch);
    }

    m_dynamicPointLights.submit(commands.sessionEpoch, commands.lightPulses);
    for (const fx::FxRayCommand& ray : commands.rays) {
        if (!ray.templateResolved ||
            ray.descriptor.kind != fx::LegacyBeamTemplateKind::ModelRay ||
            ray.descriptor.modelAsset.empty()) {
            continue;
        }
        W3dAssetRequest request;
        request.source = ray.descriptor.modelAsset;
        request.queueGpuUpload = true;
        request.gpuUploadPriority = W3dGpuUploadPriority::Visible;
        static_cast<void>(assets.requestAsync(request));
    }
    m_worldRenderer.submit(commands);

    // Bound each queue independently.  These three used to share one running
    // total, but m_terrainScorches is persistent — consumers only re-read its
    // span each frame, and nothing drains it short of reset() — while sounds and
    // shakes are drained every frame.  Since scorches are appended first they
    // always won the budget race, so once a long session accumulated
    // maximumOwnerCommands scorches, every subsequent FX sound and camera shake
    // was silently and permanently dropped.
    const auto appendBounded = [](auto& destination, auto& source,
                                  size_t maximum) -> size_t {
        const size_t available = destination.size() < maximum
            ? maximum - destination.size() : 0;
        const size_t count = std::min(available, source.size());
        destination.insert(destination.end(),
            std::make_move_iterator(source.begin()),
            std::make_move_iterator(source.begin() + count));
        return source.size() - count;
    };
    const size_t rejected =
        appendBounded(m_terrainScorches, commands.terrainScorches,
                      maximumOwnerCommands) +
        appendBounded(m_viewShakes, commands.viewShakes,
                      maximumOwnerCommands) +
        appendBounded(m_pendingSounds, commands.sounds,
                      maximumOwnerCommands);
    if (rejected == 0) return;

    m_rejectedCommands += rejected;
    if (m_rejectedCommands == rejected ||
        m_rejectedCommands % 1024u < rejected) {
        TD_LOG_WARN(
            "[TypedFxWorld] owner queues rejected {} commands (total={})",
            rejected, m_rejectedCommands);
    }
}

container::Vector<fx::FxSoundCommand>
TypedFxPresentationOwner::takeSounds() {
    container::Vector<fx::FxSoundCommand> output =
        std::move(m_pendingSounds);
    m_pendingSounds.clear();
    return output;
}

RenderCameraSnapshot TypedFxPresentationOwner::applyViewShake(
    const RenderCameraSnapshot& camera,
    uint64_t simulationFrame) {
    if (m_sessionEpoch == 0) return camera;
    if (m_lastShakeSimulationFrame != 0 &&
        simulationFrame < m_lastShakeSimulationFrame) {
        m_lastShakeSimulationFrame = 0;
        m_shakeDirection = {};
        m_shakeOffset = {};
        m_shakeIntensity = 0.0f;
    }

    constexpr container::Array<float, 6> strengths{
        0.25f, 0.45f, 0.75f, 1.05f, 1.55f, 2.20f};
    for (auto iterator = m_viewShakes.begin();
         iterator != m_viewShakes.end();) {
        if (iterator->identity.confirmedFrame > simulationFrame) {
            ++iterator;
            continue;
        }
        const size_t intensityIndex = static_cast<size_t>(iterator->type);
        if (intensityIndex < strengths.size()) {
            uint64_t mixed = iterator->identity.eventId ^
                iterator->identity.variationSeed ^
                (iterator->identity.confirmedFrame *
                 0x9e3779b97f4a7c15ull);
            mixed += 0x9e3779b97f4a7c15ull;
            mixed = (mixed ^ (mixed >> 30u)) *
                0xbf58476d1ce4e5b9ull;
            mixed = (mixed ^ (mixed >> 27u)) *
                0x94d049bb133111ebull;
            mixed ^= mixed >> 31u;
            const float angle = static_cast<float>(mixed & 0xffffu) *
                (math::PI * 2.0f / 65536.0f);
            m_shakeDirection = {std::cos(angle), std::sin(angle)};
            m_shakeIntensity = std::min(
                3.0f, m_shakeIntensity + strengths[intensityIndex]);
        }
        iterator = m_viewShakes.erase(iterator);
    }

    uint64_t steps = 1;
    if (m_lastShakeSimulationFrame != 0 &&
        simulationFrame > m_lastShakeSimulationFrame) {
        steps = std::min<uint64_t>(
            simulationFrame - m_lastShakeSimulationFrame, 64);
    }
    if (m_shakeIntensity > 0.001f) {
        m_shakeOffset = m_shakeDirection * m_shakeIntensity;
        m_shakeIntensity *= std::pow(0.75f, static_cast<float>(steps));
        m_shakeDirection = -m_shakeDirection;
    } else {
        m_shakeIntensity = 0.0f;
        m_shakeOffset = {};
    }
    m_lastShakeSimulationFrame = simulationFrame;

    RenderCameraSnapshot output = camera;
    const math::vec3 offset{
        m_shakeOffset.x(), m_shakeOffset.y(), 0.0f};
    output.position += offset;
    output.target += offset;
    return output;
}

uint64_t TypedFxPresentationOwner::retainedQueueCapacityBytes() const
    noexcept {
    uint64_t total = 0;
    const auto add = [&total]<typename Value>(
                         const container::Vector<Value>& values) noexcept {
        const uint64_t capacity = static_cast<uint64_t>(values.capacity());
        constexpr uint64_t elementBytes = sizeof(Value);
        const uint64_t bytes = capacity >
                std::numeric_limits<uint64_t>::max() / elementBytes
            ? std::numeric_limits<uint64_t>::max()
            : capacity * elementBytes;
        total = bytes > std::numeric_limits<uint64_t>::max() - total
            ? std::numeric_limits<uint64_t>::max()
            : total + bytes;
    };
    add(m_terrainScorches);
    add(m_viewShakes);
    add(m_pendingSounds);
    return total;
}

} // namespace engine::render
