#pragma once

#include "game/script/contracts/ScriptPresentationValueTypes.h"

#include "ScriptCinematicPresentationControls.h"

#include <cstdint>

namespace engine::script {

// These two actions were controls for W3DView's bounded terrain window and
// rectangular drawable query.  A DX12 renderer owns neither of those legacy
// objects, but old maps still need to express the request without turning an
// otherwise valid ScriptList into an unsupported instruction.

// Session-owned retained compatibility state. It deliberately describes no
// renderer, terrain object, camera or ECS entity. The extraction boundary
// decides which portions have a faithful modern backend mapping:
//
// - DX12 terrain is already a complete map chunk set, so terrain oversize is
//   retained for diagnostics/compatibility but requires no legacy rebuild.
// - guardband is copied to the renderer, whose current radial culler applies
//   a conservative non-negative expansion.
class ScriptViewCompatibilityState final {
public:
    void reset(uint64_t presentationEpoch = 0) noexcept;
    void rebindPresentationEpoch(uint64_t presentationEpoch) noexcept;

    // Both original actions are direct assignments/requests. Even a duplicate
    // writes a new source stamp so a debugger can distinguish an authored
    // command from an untouched default state.
    [[nodiscard]] bool setTerrainOversizeTiles(
        int32_t tiles, ScriptPresentationControlStamp stamp) noexcept;
    [[nodiscard]] bool setGuardBandBias(float x, float y,
                                        ScriptPresentationControlStamp stamp) noexcept;

    [[nodiscard]] int32_t terrainOversizeTiles() const noexcept {
        return m_terrainOversizeTiles;
    }
    [[nodiscard]] float guardBandX() const noexcept { return m_guardBandX; }
    [[nodiscard]] float guardBandY() const noexcept { return m_guardBandY; }
    [[nodiscard]] const ScriptPresentationControlStamp& terrainOversizeStamp() const noexcept {
        return m_terrainOversizeStamp;
    }
    [[nodiscard]] const ScriptPresentationControlStamp& guardBandStamp() const noexcept {
        return m_guardBandStamp;
    }
    [[nodiscard]] const ScriptPresentationControlStamp& lastMutation() const noexcept {
        return m_lastMutation;
    }

private:
    int32_t m_terrainOversizeTiles = 0;
    float m_guardBandX = 0.0f;
    float m_guardBandY = 0.0f;
    ScriptPresentationControlStamp m_terrainOversizeStamp{};
    ScriptPresentationControlStamp m_guardBandStamp{};
    ScriptPresentationControlStamp m_lastMutation{};
};

} // namespace engine::script
