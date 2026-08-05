#include "game/session/integration/GameSessionScriptAuthorityPort.h"

#include "game/session/state/GameSessionDomainState.h"

#include <algorithm>

namespace engine::script {

bool GameSessionScriptAuthorityPort::applyWaterAuthority(
    const ScriptWaterEffect& effect, const ScriptEffectHeader& header) {
    const game::terrain::PolygonTriggerRecord* trigger =
        m_content.m_terrain.triggerByName(effect.waterName);
    if (!trigger || !trigger->water) {
        emitDiagnostic(header,
            "Script water effect references a missing/non-water trigger: " +
                effect.waterName);
        return true;
    }
    if (effect.command != ScriptWaterCommand::SetHeight) {
        emitDiagnostic(header,
            "Script water enabled-state effect awaits the water visibility module");
        return true;
    }
    // The legacy compiler only screens an authored real parameter for
    // finiteness, so a huge-but-finite WATER_CHANGE_HEIGHT value arrives here
    // already saturated to INT64_MAX raw. TerrainLogic rejects it, but its bool
    // return also means "no change"; report the malformed map explicitly rather
    // than letting the effect vanish.
    if (!game::terrain::TerrainLogic::admissibleWaterValueFixed(effect.value) ||
        !game::terrain::TerrainLogic::admissibleWaterValueFixed(
            effect.damagePerSecond)) {
        emitDiagnostic(header,
            "Script water effect has an out-of-range height/damage value: " +
                effect.waterName);
        return true;
    }
    if (effect.transitionTicks == 0) {
        // Exact Q32.32 value of the legacy float literal 999999.9f
        // (999999.875). Keep float conversion outside authoritative runtime.
        constexpr int64_t LegacyImmediateWaterDamageRaw = 4294966759129088ll;
        if (m_content.m_terrain.setWaterHeightFixed(
                trigger->id, effect.value,
                math::q32_32::from_raw(LegacyImmediateWaterDamageRaw))) {
            m_damageTransactions.resolveTerrainWaterDamage();
        }
        return true;
    }
    if (trigger->points.empty()) {
        emitDiagnostic(header, "Script water trigger has no polygon points");
        return true;
    }
    const std::optional<int64_t> currentRaw =
        m_content.m_terrain.waterHeightRawAt(
            math::q32_32{trigger->points.front().x}.raw(),
            math::q32_32{trigger->points.front().y}.raw());
    if (!currentRaw) {
        emitDiagnostic(header,
            "Script water transition could not resolve a current water surface");
        return true;
    }
    const math::q32_32 distance = math::q32_32::abs(
        effect.value - math::q32_32::from_raw(*currentRaw));
    const math::q32_32 framesPerSecond{static_cast<int32_t>(
        std::max(1, m_content.m_startInfo.gameSpeedFPS))};
    const math::q32_32 rate = distance * framesPerSecond /
        math::q32_32{static_cast<int32_t>(effect.transitionTicks)};
    if (!m_content.m_terrain.beginFloodFixed(
            trigger->id, effect.value, rate, effect.damagePerSecond)) {
        emitDiagnostic(header, "Script water-transition effect was rejected");
    }
    return true;
}

} // namespace engine::script
