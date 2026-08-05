#include "engine/renderer/world/overlay/SelectionFlashPresentation.h"

#include <algorithm>
#include <cmath>

namespace engine::render {
namespace {

constexpr float kFadeRateEpsilon = 0.001f;
constexpr float kSelectionDecayFrames = 4.0f;

[[nodiscard]] float length(const RenderVector& value) noexcept {
    return std::sqrt(value.x() * value.x() + value.y() * value.y() +
                     value.z() * value.z());
}

[[nodiscard]] RenderVector unpackRgb(uint32_t argb) noexcept {
    constexpr float inverseByte = 1.0f / 255.0f;
    return {
        static_cast<float>((argb >> 16u) & 0xffu) * inverseByte,
        static_cast<float>((argb >> 8u) & 0xffu) * inverseByte,
        static_cast<float>(argb & 0xffu) * inverseByte,
    };
}

} // namespace

RenderVector legacySelectionFlashPeak(
    uint32_t indicatorColor,
    const RenderObjectFeedbackGameData& settings) noexcept {
    const RenderVector source = settings.selectionFlashHouseColor
        ? unpackRgb(indicatorColor)
        : RenderVector{1.0f, 1.0f, 1.0f};
    const float factor = std::isfinite(
            settings.selectionFlashSaturationFactor)
        ? std::max(0.0f, settings.selectionFlashSaturationFactor)
        : 0.5f;
    const float halfFactor = factor * 0.5f;
    return source * factor -
        RenderVector{halfFactor, halfFactor, halfFactor};
}

void SelectionFlashPresentation::reset(
    uint64_t presentationEpoch) noexcept {
    m_objects.clear();
    m_presentationEpoch = presentationEpoch;
    m_lastSimulationFrame = 0;
    m_initialized = true;
    m_lastFrameInitialized = false;
}

void SelectionFlashPresentation::play(
    ObjectState& state, const RenderVector& peak) noexcept {
    state.peak = peak;
    // attackFrames=0 is MAX(1, 0) in RefCode, so one update reaches peak.
    state.attackRate = state.peak - state.current;
    state.decayRate = state.peak * (-1.0f / kSelectionDecayFrames);
    state.envelope = EnvelopeState::Attack;
    state.effective = true;
    if (length(state.current - state.peak) <= kFadeRateEpsilon)
        state.envelope = EnvelopeState::Sustain;
}

void SelectionFlashPresentation::update(ObjectState& state) noexcept {
    switch (state.envelope) {
    case EnvelopeState::Rest:
        state.current = {};
        state.effective = false;
        break;
    case EnvelopeState::Decay:
        // RefCode uses strict greater-than.  Equality performs the final
        // subtraction to exact zero, with isEffective clearing next update.
        if (length(state.decayRate) > length(state.current) ||
            length(state.current) <= kFadeRateEpsilon) {
            state.envelope = EnvelopeState::Rest;
            state.effective = false;
        } else {
            state.current += state.decayRate;
            state.effective = true;
        }
        break;
    case EnvelopeState::Attack: {
        const RenderVector delta = state.current - state.peak;
        if (length(state.attackRate) > length(delta) ||
            length(delta) <= kFadeRateEpsilon) {
            // Selection flashes have sustainAtPeak=0, so this update changes
            // only state.  The first 1/4 decay is applied on the next update.
            state.envelope = EnvelopeState::Decay;
        } else {
            state.current += state.attackRate;
            state.effective = true;
        }
        break;
    }
    case EnvelopeState::Sustain:
        // play() enters Sustain only when a retrigger already sits at peak.
        // With sustainAtPeak=0, release() changes state without changing tint.
        state.envelope = EnvelopeState::Decay;
        break;
    }
}

void SelectionFlashPresentation::consume(
    const ObjectUiRenderState& incoming,
    uint64_t simulationFrame,
    const RenderObjectFeedbackGameData& settings) {
    if (m_initialized && incoming.presentationEpoch != 0 &&
        m_presentationEpoch != 0 &&
        incoming.presentationEpoch < m_presentationEpoch) {
        return;
    }
    if (!m_initialized ||
        incoming.presentationEpoch != m_presentationEpoch ||
        (m_lastFrameInitialized &&
         simulationFrame < m_lastSimulationFrame)) {
        reset(incoming.presentationEpoch);
    }

    const uint64_t elapsed = m_lastFrameInitialized &&
            simulationFrame > m_lastSimulationFrame
        ? simulationFrame - m_lastSimulationFrame
        : 0u;
    const uint64_t preCurrentSteps = elapsed > 0 ? elapsed - 1u : 0u;
    container::HashSet<RenderEntityId> seen;
    seen.reserve(incoming.objects.size());
    for (const ObjectUiRenderSnapshot& object : incoming.objects) {
        if (object.objectId == 0 || !seen.insert(object.objectId).second)
            continue;
        ObjectState& state = m_objects[object.objectId];
        for (uint64_t step = 0; step < preCurrentSteps; ++step) {
            update(state);
            // The envelope has a finite six-update tail.  A replay seek may
            // jump billions of confirmed frames; Rest is an exact fixed point
            // and therefore needs no per-frame catch-up work.
            if (state.envelope == EnvelopeState::Rest && !state.effective)
                break;
        }

        const bool selectionStarted = object.selected && !state.selected;
        const bool externalFlashStarted =
            object.selectionFlashIdentity != 0 &&
            object.selectionFlashIdentity != state.externalFlashIdentity;
        if (selectionStarted || externalFlashStarted)
            play(state, legacySelectionFlashPeak(
                object.indicatorColor, settings));

        // A newly observed edge is sampled after the same update that the old
        // Drawable received before drawing.  Repeated presentation of one
        // confirmed frame remains idempotent.
        if (elapsed > 0 || selectionStarted || externalFlashStarted)
            update(state);
        state.externalFlashIdentity = object.selectionFlashIdentity;
        state.selected = object.selected;
    }

    std::erase_if(m_objects, [&seen](const auto& entry) {
        return !seen.contains(entry.first);
    });
    m_lastSimulationFrame = simulationFrame;
    m_lastFrameInitialized = true;
}

RenderVector SelectionFlashPresentation::tintFor(
    RenderEntityId objectId) const noexcept {
    const auto found = m_objects.find(objectId);
    return found != m_objects.end() && found->second.effective
        ? found->second.current : RenderVector{};
}

} // namespace engine::render
