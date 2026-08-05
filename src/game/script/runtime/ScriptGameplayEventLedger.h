#pragma once

#include "core/container/container_types.h"

#include "game/player/PlayerTypes.h"
#include "core/ecs/ObjectId.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine::script {

enum class ScriptSpecialPowerEventPhase : uint8_t {
    Triggered,
    Midway,
    Completed,
};

// Value-only projection of ScriptEngine's triggered/finished power lists.
// `source` remains a stable historical ObjectId; a completion may be consumed
// after the creator has died, while a FROM_NAMED condition still resolves its
// currently live authored name before asking for an exact-source match.
struct ScriptSpecialPowerEvent final {
    ScriptSpecialPowerEventPhase phase = ScriptSpecialPowerEventPhase::Triggered;
    PlayerId player = INVALID_PLAYER_ID;
    ObjectId source = INVALID_OBJECT_ID;
    container::String specialPower;
    uint64_t confirmedTick = 0;
    uint64_t sequence = 0;
};

struct ScriptUpgradeEvent final {
    PlayerId player = INVALID_PLAYER_ID;
    ObjectId source = INVALID_OBJECT_ID;
    container::String upgrade;
    uint64_t confirmedTick = 0;
    uint64_t sequence = 0;
};

struct ScriptBridgeTransitionEvent final {
    ObjectId bridge = INVALID_OBJECT_ID;
    bool active = false;
    uint64_t confirmedTick = 0;
    uint64_t sequence = 0;
};

struct ScriptContainmentSample final {
    ObjectId object = INVALID_OBJECT_ID;
    uint64_t frame = 0;
    size_t count = 0;
};

struct ScriptBuildingEnteredEvent final {
    ObjectId building = INVALID_OBJECT_ID;
    PlayerId player = INVALID_PLAYER_ID;
    uint64_t confirmedTick = 0;
    uint64_t sequence = 0;
};

enum class ScriptAreaTransitionKind : uint8_t {
    Entered,
    Exited,
};

struct ScriptAreaObjectState final {
    ObjectId object = INVALID_OBJECT_ID;
    uint32_t areaIndex = 0;
    uint64_t enteredTick = 0;
    uint64_t exitedTick = 0;
    bool inside = false;
};

// Session-owned replacement for ScriptEngine's consumable gameplay lists.
// It deliberately has no ECS, Object pointer, renderer, audio or Runtime
// dependency. Conditions consume the oldest matching fact exactly once,
// preserving the original source-order competition between scripts.
class ScriptGameplayEventLedger final {
public:
    // Large enough for pathological campaign bursts while retaining a hard
    // bound against malformed maps that never consume their notifications.
    static constexpr size_t kMaximumPendingSpecialPowerEvents = 65'536;
    static constexpr size_t kMaximumPendingUpgradeEvents = 65'536;
    static constexpr size_t kMaximumBridgeTransitionEvents = 8'192;
    static constexpr size_t kMaximumBuildingEnteredEvents = 8'192;

    void reset() noexcept;

    [[nodiscard]] bool recordSpecialPower(ScriptSpecialPowerEvent event);
    [[nodiscard]] std::optional<ScriptSpecialPowerEvent> consumeSpecialPower(
        ScriptSpecialPowerEventPhase phase, PlayerId player,
        container::StringView specialPower,
        ObjectId source = INVALID_OBJECT_ID) noexcept;
    [[nodiscard]] bool recordUpgrade(ScriptUpgradeEvent event);
    [[nodiscard]] std::optional<ScriptUpgradeEvent> consumeUpgrade(
        PlayerId player, container::StringView upgrade,
        ObjectId source = INVALID_OBJECT_ID) noexcept;
    [[nodiscard]] bool recordBridgeTransition(ScriptBridgeTransitionEvent event);
    [[nodiscard]] bool bridgeTransitionObserved(
        ObjectId bridge, bool active, uint64_t scriptTick) const noexcept;
    [[nodiscard]] bool observeUnitEmptied(
        ObjectId object, size_t currentCount, uint64_t scriptTick);
    [[nodiscard]] bool recordBuildingEntered(ScriptBuildingEnteredEvent event);
    [[nodiscard]] bool buildingEnteredByPlayer(
        ObjectId building, PlayerId player, uint64_t scriptTick) const noexcept;
    void configureTrackedAreas(container::Vector<container::String> areaNames);
    [[nodiscard]] container::Span<const container::String> trackedAreas() const noexcept {
        return m_trackedAreas;
    }
    void beginAreaRefresh();
    void recordAreaSample(ObjectId object, uint32_t areaIndex,
                          bool inside, uint64_t scriptTick);
    void finishAreaRefresh() noexcept;
    [[nodiscard]] bool objectInsideArea(ObjectId object,
                                        container::StringView areaName) const noexcept;
    [[nodiscard]] bool objectAreaTransition(
        ObjectId object, container::StringView areaName,
        ScriptAreaTransitionKind kind, uint64_t scriptTick) const noexcept;

    [[nodiscard]] size_t pendingSpecialPowerCount() const noexcept {
        return m_specialPowers.size();
    }
    [[nodiscard]] size_t pendingUpgradeCount() const noexcept {
        return m_upgrades.size();
    }
    [[nodiscard]] size_t bridgeTransitionCount() const noexcept {
        return m_bridgeTransitions.size();
    }

private:
    [[nodiscard]] static bool validName(container::StringView name) noexcept;

    uint64_t m_nextSequence = 1;
    container::Vector<ScriptSpecialPowerEvent> m_specialPowers;
    container::Vector<ScriptUpgradeEvent> m_upgrades;
    container::Vector<ScriptBridgeTransitionEvent> m_bridgeTransitions;
    container::Vector<ScriptContainmentSample> m_containmentSamples;
    container::Vector<ScriptBuildingEnteredEvent> m_buildingEntered;
    container::Vector<container::String> m_trackedAreas;
    container::Vector<ScriptAreaObjectState> m_areaStates;
    container::Vector<ScriptAreaObjectState> m_nextAreaStates;
    bool m_areaBaselineEstablished = false;
};

} // namespace engine::script
