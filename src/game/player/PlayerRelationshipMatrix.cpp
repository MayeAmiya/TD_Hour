#include "core/container/container_types.h"
#include "PlayerRelationshipMatrix.h"

#include <algorithm>

namespace engine {

PlayerRelationshipMatrix::PlayerRelationshipMatrix() {
    reset();
}

void PlayerRelationshipMatrix::reset() noexcept {
    m_values.fill(PlayerRelationship::Neutral);
    for (uint8_t index = 0; index < static_cast<uint8_t>(PLAYER_REGISTRY_CAPACITY); ++index) {
        const PlayerId player{index};
        m_values[offset(player, player)] = PlayerRelationship::Allies;
    }
}

void PlayerRelationshipMatrix::initializeDefaults(
    container::Span<const PlayerAllianceAssignment> players) noexcept {
    reset();

    container::Array<AllianceGroupId, PLAYER_REGISTRY_CAPACITY> alliances{};
    container::Array<bool, PLAYER_REGISTRY_CAPACITY> active{};
    for (const PlayerAllianceAssignment assignment : players) {
        if (!inRange(assignment.player)) continue;
        active[assignment.player.value] = true;
        alliances[assignment.player.value] = assignment.alliance;
    }

    // Neutral always exists in a resolved match even when a caller did not
    // include it in the setup array.  Its outgoing/incoming relations remain
    // Neutral, except for neutral->neutral seeded by reset().
    active[NEUTRAL_PLAYER_ID.value] = true;

    for (uint8_t fromValue = 0; fromValue < static_cast<uint8_t>(PLAYER_REGISTRY_CAPACITY);
         ++fromValue) {
        if (!active[fromValue]) continue;
        const PlayerId from{fromValue};
        for (uint8_t toValue = 0; toValue < static_cast<uint8_t>(PLAYER_REGISTRY_CAPACITY);
             ++toValue) {
            if (!active[toValue]) continue;
            const PlayerId to{toValue};
            if (from == to) continue;
            if (from.isNeutral() || to.isNeutral()) continue;

            const AllianceGroupId fromAlliance = alliances[fromValue];
            const AllianceGroupId toAlliance = alliances[toValue];
            const bool sameAlliance = fromAlliance && toAlliance && fromAlliance == toAlliance;
            m_values[offset(from, to)] = sameAlliance
                ? PlayerRelationship::Allies
                : PlayerRelationship::Enemies;
        }
    }
}

PlayerRelationship PlayerRelationshipMatrix::get(PlayerId from, PlayerId to) const noexcept {
    if (!inRange(from) || !inRange(to)) return PlayerRelationship::Neutral;
    return m_values[offset(from, to)];
}

bool PlayerRelationshipMatrix::set(PlayerId from, PlayerId to,
                                   PlayerRelationship relationship) noexcept {
    if (!inRange(from) || !inRange(to)) return false;
    if (relationship != PlayerRelationship::Allies &&
        relationship != PlayerRelationship::Enemies &&
        relationship != PlayerRelationship::Neutral) {
        return false;
    }
    if (from == to) {
        if (relationship != PlayerRelationship::Allies) return false;
        return true;
    }
    m_values[offset(from, to)] = relationship;
    return true;
}

} // namespace engine
