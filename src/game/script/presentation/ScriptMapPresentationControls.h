#pragma once

#include "core/container/container_types.h"
#include "core/math/fixed/q32_32.h"

#include "game/script/presentation/ScriptCinematicPresentationControls.h"
#include "game/script/contracts/ScriptPresentationValueTypes.h"
#include "core/math/wwmath/base/wwmath.h"

#include <cstddef>
#include <cstdint>
#include <limits>
namespace engine::script {

// Radar policy is presentation state, separate from the future per-player
// shroud/visibility authority. `Forced` corresponds to RefCode's
// RADAR_FORCE_ENABLE and ignores normal radar-disable requests until a
// RADAR_REVERT_TO_NORMAL action restores ordinary control.
enum class ScriptRadarMode : uint8_t {
    Normal,
    Disabled,
    Forced,
};


struct ScriptRadarEventPresentation final {
    math::vec3 position{};
    int32_t eventType = 0;
    ScriptPresentationControlStamp stamp{};
    // RefCode's Radar::internalCreateEvent keeps an event active through its
    // die frame and begins its visual fade half a second before that. The UI
    // consumer owns the fade drawing, while this value-only state owns the
    // confirmed-tick lifetime.
    uint64_t fadeTick = 0;
    uint64_t dieTick = std::numeric_limits<uint64_t>::max();
};

class ScriptMapPresentationState final {
public:
    // RefCode uses a 64-entry circular RadarEvent array. Keep that bounded
    // history at the session boundary; no presentation consumer may grow it
    // indefinitely when a script emits repeated events.
    static constexpr size_t kMaximumRadarEvents = 64;

    void reset(uint64_t presentationEpoch = 0) noexcept;
    void rebindPresentationEpoch(uint64_t presentationEpoch) noexcept;
    [[nodiscard]] bool setRadarHidden(bool hidden,
                                      ScriptPresentationControlStamp stamp) noexcept;
    [[nodiscard]] bool setRadarForced(bool forced,
                                      ScriptPresentationControlStamp stamp) noexcept;
    [[nodiscard]] bool setBorderShroudEnabled(bool enabled,
                                              ScriptPresentationControlStamp stamp) noexcept;
    [[nodiscard]] bool setBoundary(int32_t boundaryIndex,
                                   ScriptPresentationControlStamp stamp) noexcept;
    void appendRadarEvent(ScriptRadarEventPresentation event);
    // MAP reveal/shroud and REFRESH_RADAR mutate the separate visibility
    // authority rather than one of this class's durable policy fields. They
    // still need an ordered presentation invalidation for radar consumers.
    void noteMapMutation(ScriptPresentationControlStamp stamp) noexcept;
    // Returns true only when one or more events expired. `dieTick` remains
    // visible on its exact frame, matching Radar::update's `frame > dieFrame`
    // condition in RefCode.
    [[nodiscard]] bool advanceRadarEvents(uint64_t confirmedTick) noexcept;

    [[nodiscard]] ScriptRadarMode radarMode() const noexcept {
        return m_radarForced ? ScriptRadarMode::Forced
                             : (m_radarHidden ? ScriptRadarMode::Disabled : ScriptRadarMode::Normal);
    }
    [[nodiscard]] bool radarVisible() const noexcept {
        return m_radarForced || !m_radarHidden;
    }
    [[nodiscard]] bool radarHidden() const noexcept { return m_radarHidden; }
    [[nodiscard]] bool radarForced() const noexcept { return m_radarForced; }
    [[nodiscard]] bool borderShroudEnabled() const noexcept { return m_borderShroudEnabled; }
    [[nodiscard]] const ScriptPresentationControlStamp& borderShroudStamp() const noexcept {
        return m_borderShroudStamp;
    }
    [[nodiscard]] int32_t boundaryIndex() const noexcept { return m_boundaryIndex; }
    [[nodiscard]] container::Span<const ScriptRadarEventPresentation> radarEvents() const noexcept {
        return m_radarEvents;
    }
    [[nodiscard]] const ScriptPresentationControlStamp& lastMutation() const noexcept {
        return m_lastMutation;
    }

private:
    bool m_radarHidden = false;
    bool m_radarForced = false;
    // RefCode's display starts with shrouded unused terrain. This is world
    // presentation policy only; TerrainLogic's playable boundary remains the
    // authoritative gameplay/query extent regardless of this flag.
    bool m_borderShroudEnabled = true;
    ScriptPresentationControlStamp m_borderShroudStamp{};
    int32_t m_boundaryIndex = 0;
    container::Vector<ScriptRadarEventPresentation> m_radarEvents;
    ScriptPresentationControlStamp m_lastMutation{};
};

} // namespace engine::script
