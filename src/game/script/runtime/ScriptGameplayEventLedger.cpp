#include "core/container/container_types.h"
#include "ScriptGameplayEventLedger.h"

#include <algorithm>
#include <utility>

namespace engine::script {

void ScriptGameplayEventLedger::reset() noexcept {
    m_nextSequence = 1;
    m_specialPowers.clear();
    m_upgrades.clear();
    m_bridgeTransitions.clear();
    m_containmentSamples.clear();
    m_buildingEntered.clear();
    m_trackedAreas.clear();
    m_areaStates.clear();
    m_nextAreaStates.clear();
    m_areaBaselineEstablished = false;
}

bool ScriptGameplayEventLedger::recordSpecialPower(
    ScriptSpecialPowerEvent event) {
    if (!event.player || !validName(event.specialPower)) return false;
    if (event.sequence == 0) {
        event.sequence = m_nextSequence++;
        if (m_nextSequence == 0) m_nextSequence = 1;
    }
    if (m_specialPowers.size() >= kMaximumPendingSpecialPowerEvents) {
        m_specialPowers.erase(m_specialPowers.begin());
    }
    m_specialPowers.push_back(std::move(event));
    return true;
}

std::optional<ScriptSpecialPowerEvent>
ScriptGameplayEventLedger::consumeSpecialPower(
    ScriptSpecialPowerEventPhase phase, PlayerId player,
    container::StringView specialPower, ObjectId source) noexcept {
    if (!player || !validName(specialPower)) return std::nullopt;
    const auto found = std::find_if(
        m_specialPowers.begin(), m_specialPowers.end(),
        [phase, player, specialPower, source](const ScriptSpecialPowerEvent& event) {
            return event.phase == phase && event.player == player &&
                event.specialPower == specialPower &&
                (!source || event.source == source);
        });
    if (found == m_specialPowers.end()) return std::nullopt;
    ScriptSpecialPowerEvent output = std::move(*found);
    m_specialPowers.erase(found);
    return output;
}

bool ScriptGameplayEventLedger::recordUpgrade(ScriptUpgradeEvent event) {
    if (!event.player || !validName(event.upgrade)) return false;
    if (event.sequence == 0) {
        event.sequence = m_nextSequence++;
        if (m_nextSequence == 0) m_nextSequence = 1;
    }
    if (m_upgrades.size() >= kMaximumPendingUpgradeEvents) {
        m_upgrades.erase(m_upgrades.begin());
    }
    m_upgrades.push_back(std::move(event));
    return true;
}

std::optional<ScriptUpgradeEvent> ScriptGameplayEventLedger::consumeUpgrade(
    PlayerId player, container::StringView upgrade, ObjectId source) noexcept {
    if (!player || !validName(upgrade)) return std::nullopt;
    const auto found = std::find_if(
        m_upgrades.begin(), m_upgrades.end(),
        [player, upgrade, source](const ScriptUpgradeEvent& event) {
            return event.player == player && event.upgrade == upgrade &&
                (!source || event.source == source);
        });
    if (found == m_upgrades.end()) return std::nullopt;
    ScriptUpgradeEvent output = std::move(*found);
    m_upgrades.erase(found);
    return output;
}

bool ScriptGameplayEventLedger::recordBridgeTransition(
    ScriptBridgeTransitionEvent event) {
    if (!event.bridge) return false;
    if (event.sequence == 0) {
        event.sequence = m_nextSequence++;
        if (m_nextSequence == 0) m_nextSequence = 1;
    }
    if (m_bridgeTransitions.size() >= kMaximumBridgeTransitionEvents) {
        m_bridgeTransitions.erase(m_bridgeTransitions.begin());
    }
    m_bridgeTransitions.push_back(event);
    return true;
}

bool ScriptGameplayEventLedger::bridgeTransitionObserved(
    ObjectId bridge, bool active, uint64_t scriptTick) const noexcept {
    if (!bridge) return false;
    return std::any_of(
        m_bridgeTransitions.begin(), m_bridgeTransitions.end(),
        [bridge, active, scriptTick](const ScriptBridgeTransitionEvent& event) {
            if (event.bridge != bridge || event.active != active) return false;
            return event.confirmedTick == scriptTick ||
                (event.confirmedTick != UINT64_MAX &&
                 event.confirmedTick + 1u == scriptTick);
        });
}

bool ScriptGameplayEventLedger::observeUnitEmptied(
    ObjectId object, size_t currentCount, uint64_t scriptTick) {
    if (!object) return false;
    const auto found = std::lower_bound(
        m_containmentSamples.begin(), m_containmentSamples.end(), object,
        [](const ScriptContainmentSample& sample, ObjectId needle) {
            return sample.object < needle;
        });
    if (found == m_containmentSamples.end() || found->object != object) {
        m_containmentSamples.insert(found, {
            .object = object,
            .frame = scriptTick,
            .count = currentCount,
        });
        return false;
    }
    if (scriptTick > 0 && found->frame == scriptTick - 1u &&
        found->count > 0 && currentCount == 0) {
        // RefCode deliberately does not update on true, making the edge a
        // broadcast to every same-frame condition evaluation.
        return true;
    }
    found->frame = scriptTick;
    found->count = currentCount;
    return false;
}

bool ScriptGameplayEventLedger::recordBuildingEntered(
    ScriptBuildingEnteredEvent event) {
    if (!event.building || !event.player) return false;
    if (event.sequence == 0) {
        event.sequence = m_nextSequence++;
        if (m_nextSequence == 0) m_nextSequence = 1;
    }
    // OpenContain stores one player mask; later same-tick entrants overwrite
    // the earlier player rather than accumulating a bit set.
    const auto existing = std::find_if(
        m_buildingEntered.rbegin(), m_buildingEntered.rend(),
        [&event](const ScriptBuildingEnteredEvent& value) {
            return value.building == event.building &&
                value.confirmedTick == event.confirmedTick;
        });
    if (existing != m_buildingEntered.rend()) {
        *existing = event;
        return true;
    }
    if (m_buildingEntered.size() >= kMaximumBuildingEnteredEvents) {
        m_buildingEntered.erase(m_buildingEntered.begin());
    }
    m_buildingEntered.push_back(event);
    return true;
}

bool ScriptGameplayEventLedger::buildingEnteredByPlayer(
    ObjectId building, PlayerId player, uint64_t scriptTick) const noexcept {
    if (!building || !player) return false;
    return std::any_of(
        m_buildingEntered.begin(), m_buildingEntered.end(),
        [building, player, scriptTick](const ScriptBuildingEnteredEvent& event) {
            if (event.building != building || event.player != player) return false;
            return event.confirmedTick == scriptTick ||
                (event.confirmedTick != UINT64_MAX &&
                 event.confirmedTick + 1u == scriptTick);
        });
}

void ScriptGameplayEventLedger::configureTrackedAreas(
    container::Vector<container::String> areaNames) {
    std::sort(areaNames.begin(), areaNames.end());
    areaNames.erase(std::unique(areaNames.begin(), areaNames.end()), areaNames.end());
    m_trackedAreas = std::move(areaNames);
    m_areaStates.clear();
    m_nextAreaStates.clear();
    m_areaBaselineEstablished = false;
}

void ScriptGameplayEventLedger::beginAreaRefresh() {
    m_nextAreaStates.clear();
    m_nextAreaStates.reserve(m_areaStates.size());
}

void ScriptGameplayEventLedger::recordAreaSample(
    ObjectId object, uint32_t areaIndex, bool inside, uint64_t scriptTick) {
    if (!object || areaIndex >= m_trackedAreas.size()) return;
    const auto keyLess = [](const ScriptAreaObjectState& state,
                            const std::pair<ObjectId, uint32_t>& key) {
        return state.object < key.first ||
            (state.object == key.first && state.areaIndex < key.second);
    };
    const auto previous = std::lower_bound(
        m_areaStates.begin(), m_areaStates.end(),
        std::pair<ObjectId, uint32_t>{object, areaIndex}, keyLess);
    ScriptAreaObjectState state{
        .object = object,
        .areaIndex = areaIndex,
        .inside = inside,
    };
    if (previous != m_areaStates.end() && previous->object == object &&
        previous->areaIndex == areaIndex) {
        state.enteredTick = previous->enteredTick;
        state.exitedTick = previous->exitedTick;
        if (previous->inside != inside) {
            if (inside) state.enteredTick = scriptTick;
            else state.exitedTick = scriptTick;
        }
    } else if (m_areaBaselineEstablished && inside) {
        state.enteredTick = scriptTick;
    }
    m_nextAreaStates.push_back(state);
}

void ScriptGameplayEventLedger::finishAreaRefresh() noexcept {
    m_areaStates.swap(m_nextAreaStates);
    m_nextAreaStates.clear();
    m_areaBaselineEstablished = true;
}

bool ScriptGameplayEventLedger::objectInsideArea(
    ObjectId object, container::StringView areaName) const noexcept {
    const auto area = std::lower_bound(
        m_trackedAreas.begin(), m_trackedAreas.end(), areaName,
        [](const container::String& value, container::StringView needle) {
            return container::StringView{value} < needle;
        });
    if (area == m_trackedAreas.end() || *area != areaName) return false;
    const uint32_t areaIndex = static_cast<uint32_t>(
        std::distance(m_trackedAreas.begin(), area));
    const auto state = std::lower_bound(
        m_areaStates.begin(), m_areaStates.end(),
        std::pair<ObjectId, uint32_t>{object, areaIndex},
        [](const ScriptAreaObjectState& value,
           const std::pair<ObjectId, uint32_t>& key) {
            return value.object < key.first ||
                (value.object == key.first && value.areaIndex < key.second);
        });
    return state != m_areaStates.end() && state->object == object &&
        state->areaIndex == areaIndex && state->inside;
}

bool ScriptGameplayEventLedger::objectAreaTransition(
    ObjectId object, container::StringView areaName,
    ScriptAreaTransitionKind kind, uint64_t scriptTick) const noexcept {
    const auto area = std::lower_bound(
        m_trackedAreas.begin(), m_trackedAreas.end(), areaName,
        [](const container::String& value, container::StringView needle) {
            return container::StringView{value} < needle;
        });
    if (area == m_trackedAreas.end() || *area != areaName) return false;
    const uint32_t areaIndex = static_cast<uint32_t>(
        std::distance(m_trackedAreas.begin(), area));
    const auto state = std::lower_bound(
        m_areaStates.begin(), m_areaStates.end(),
        std::pair<ObjectId, uint32_t>{object, areaIndex},
        [](const ScriptAreaObjectState& value,
           const std::pair<ObjectId, uint32_t>& key) {
            return value.object < key.first ||
                (value.object == key.first && value.areaIndex < key.second);
        });
    if (state == m_areaStates.end() || state->object != object ||
        state->areaIndex != areaIndex) return false;
    return (kind == ScriptAreaTransitionKind::Entered
            ? state->enteredTick : state->exitedTick) == scriptTick;
}

bool ScriptGameplayEventLedger::validName(container::StringView name) noexcept {
    return !name.empty() && name.size() <= 1024 &&
        name.find('\0') == container::StringView::npos;
}

} // namespace engine::script
