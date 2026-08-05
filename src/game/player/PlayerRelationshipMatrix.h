#pragma once

#include "core/container/container_types.h"

#include "PlayerTypes.h"
namespace engine {

struct PlayerAllianceAssignment final {
    PlayerId player = INVALID_PLAYER_ID;
    AllianceGroupId alliance = INVALID_ALLIANCE_GROUP_ID;
};

// Fixed-size, directed diplomacy matrix.  It intentionally has no hash-table
// iteration and serializes in PlayerId order.  Alliance groups only seed the
// default values; later scenario/script systems may apply directed overrides.
class PlayerRelationshipMatrix final {
public:
    PlayerRelationshipMatrix();

    void reset() noexcept;

    // Initializes a new match's diplomacy.  Real players in the same valid
    // alliance group begin allied; real players in different/no groups begin
    // enemies; neutral is neutral to everybody except itself.
    void initializeDefaults(container::Span<const PlayerAllianceAssignment> players) noexcept;

    [[nodiscard]] PlayerRelationship get(PlayerId from, PlayerId to) const noexcept;
    // Self-relations are invariantly Allies.  Returns false for invalid IDs
    // or an attempt to violate that invariant.
    [[nodiscard]] bool set(PlayerId from, PlayerId to, PlayerRelationship relationship) noexcept;

    [[nodiscard]] container::Span<const PlayerRelationship> canonicalValues() const noexcept {
        return m_values;
    }

private:
    [[nodiscard]] static constexpr bool inRange(PlayerId player) noexcept {
        return player.isValid();
    }
    [[nodiscard]] static constexpr size_t offset(PlayerId from, PlayerId to) noexcept {
        return static_cast<size_t>(from.value) * PLAYER_REGISTRY_CAPACITY +
               static_cast<size_t>(to.value);
    }

    container::Array<PlayerRelationship, PLAYER_REGISTRY_CAPACITY * PLAYER_REGISTRY_CAPACITY> m_values{};
};

} // namespace engine
