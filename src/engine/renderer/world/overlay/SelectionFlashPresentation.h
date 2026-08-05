#pragma once

#include "presentation/render/RenderOverlaySnapshot.h"
#include "presentation/render/RenderGameDataSettings.h"

#include "core/container/hash_containers.h"

#include <cstddef>
#include <cstdint>

namespace engine::render {

// Drawable::saturateRGB is deliberately a signed light-environment transform,
// not an HSV saturation operation.  Preserve its exact per-channel formula so
// low house-colour channels may darken while high channels brighten.
[[nodiscard]] RenderVector legacySelectionFlashPeak(
    uint32_t indicatorColor,
    const RenderObjectFeedbackGameData& settings) noexcept;

// Renderer-owned equivalent of Drawable's lazily allocated selection
// TintEnvelope.  The immutable snapshot supplies selection and indicator
// colour only; edge history, attack/decay state and replay reset remain local
// presentation data shared by every Draw channel of one objectId.
class SelectionFlashPresentation final {
public:
    void reset(uint64_t presentationEpoch = 0) noexcept;
    void consume(const ObjectUiRenderState& incoming,
                 uint64_t simulationFrame,
                 const RenderObjectFeedbackGameData& settings);

    [[nodiscard]] RenderVector tintFor(
        RenderEntityId objectId) const noexcept;
    [[nodiscard]] size_t trackedObjectCount() const noexcept {
        return m_objects.size();
    }
    [[nodiscard]] uint64_t presentationEpoch() const noexcept {
        return m_presentationEpoch;
    }

private:
    enum class EnvelopeState : uint8_t {
        Rest,
        Attack,
        Sustain,
        Decay,
    };

    struct ObjectState final {
        RenderVector attackRate{};
        RenderVector decayRate{};
        RenderVector peak{};
        RenderVector current{};
        EnvelopeState envelope = EnvelopeState::Rest;
        uint64_t externalFlashIdentity = 0;
        bool selected = false;
        bool effective = false;
    };

    static void play(ObjectState& state, const RenderVector& peak) noexcept;
    static void update(ObjectState& state) noexcept;

    container::HashMap<RenderEntityId, ObjectState> m_objects;
    uint64_t m_presentationEpoch = 0;
    uint64_t m_lastSimulationFrame = 0;
    bool m_initialized = false;
    bool m_lastFrameInitialized = false;
};

} // namespace engine::render
