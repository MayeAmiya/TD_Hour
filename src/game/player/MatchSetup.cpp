#include "core/container/container_types.h"
#include "core/container/string_utils.h"
#include "MatchSetup.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace engine {
namespace {

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

constexpr auto equalsInsensitive = container::asciiEqualIgnoreCase;

[[nodiscard]] uint64_t mix(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

class DigestWriter final {
public:
    void byte(uint8_t value) noexcept {
        m_value ^= value;
        m_value *= kFnvPrime;
    }

    void boolean(bool value) noexcept { byte(value ? 1u : 0u); }
    void u32(uint32_t value) noexcept {
        for (uint32_t shift = 0; shift < 32; shift += 8) {
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
        }
    }
    void i32(int32_t value) noexcept { u32(static_cast<uint32_t>(value)); }
    void u64(uint64_t value) noexcept {
        for (uint32_t shift = 0; shift < 64; shift += 8) {
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
        }
    }
    void string(container::StringView value) noexcept {
        u32(static_cast<uint32_t>(value.size()));
        for (const unsigned char character : value) byte(character);
    }
    [[nodiscard]] uint64_t finish() const noexcept { return m_value; }

private:
    uint64_t m_value = kFnvOffsetBasis;
};

void addIssue(MatchSetupResolution& result, MatchSetupIssueCode code,
              MatchPlayerSlotId slot, container::String message) {
    result.issues.push_back({.code = code, .slot = slot, .message = std::move(message)});
}

struct ResolvedController final {
    PlayerControllerKind kind = PlayerControllerKind::Human;
    AiDifficulty aiDifficulty = AiDifficulty::None;
};

[[nodiscard]] std::optional<ResolvedController> controllerFor(SlotState state) noexcept {
    switch (state) {
    case SLOT_HUMAN:
        return ResolvedController{.kind = PlayerControllerKind::Human};
    case SLOT_AI:
        // The legacy generic AI state is retained for CLI/import input and
        // resolves to the original medium/default behavior.
        return ResolvedController{.kind = PlayerControllerKind::Ai,
                                  .aiDifficulty = AiDifficulty::Normal};
    case SLOT_EASY_AI:
        return ResolvedController{.kind = PlayerControllerKind::Ai,
                                  .aiDifficulty = AiDifficulty::Easy};
    case SLOT_NORMAL_AI:
        return ResolvedController{.kind = PlayerControllerKind::Ai,
                                  .aiDifficulty = AiDifficulty::Normal};
    case SLOT_HARD_AI:
        return ResolvedController{.kind = PlayerControllerKind::Ai,
                                  .aiDifficulty = AiDifficulty::Hard};
    case SLOT_OPEN:
    case SLOT_CLOSED:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] container::Vector<FactionTemplateId> eligibleFactions(const MultiplayerRuleset& ruleset,
                                                                bool oldFactionsOnly) {
    container::Vector<FactionTemplateId> result;
    for (const FactionTemplateId id : ruleset.playableTemplateIds()) {
        const FactionTemplate* templateValue = ruleset.findFaction(id);
        if (!templateValue) continue;
        if (oldFactionsOnly && !templateValue->oldFaction) continue;
        result.push_back(id);
    }
    std::sort(result.begin(), result.end());
    return result;
}

template <typename Id>
[[nodiscard]] std::optional<Id> chooseUnused(container::Span<const Id> values,
                                              const container::Vector<Id>& used,
                                              uint64_t seed) {
    if (values.empty()) return std::nullopt;
    const size_t start = static_cast<size_t>(seed % values.size());
    for (size_t offset = 0; offset < values.size(); ++offset) {
        const Id candidate = values[(start + offset) % values.size()];
        if (std::find(used.begin(), used.end(), candidate) == used.end()) return candidate;
    }
    return std::nullopt;
}

[[nodiscard]] bool shareAlliance(const ResolvedPlayerSetup& lhs,
                                 const ResolvedPlayerSetup& rhs) noexcept {
    return lhs.alliance && rhs.alliance && lhs.alliance == rhs.alliance;
}

[[nodiscard]] bool isKnownAiDifficulty(AiDifficulty value) noexcept {
    return static_cast<uint8_t>(value) <= static_cast<uint8_t>(AiDifficulty::Hard);
}

[[nodiscard]] const MatchStartPosition* findStartPosition(
    container::Span<const MatchStartPosition> layout, int32_t index) noexcept {
    const auto found = std::lower_bound(layout.begin(), layout.end(), index,
        [](const MatchStartPosition& candidate, int32_t value) {
            return candidate.index < value;
        });
    return found != layout.end() && found->index == index ? &*found : nullptr;
}

[[nodiscard]] double squaredDistance(const MatchStartPosition& lhs,
                                     const MatchStartPosition& rhs) noexcept {
    const double dx = static_cast<double>(lhs.worldX) - static_cast<double>(rhs.worldX);
    const double dy = static_cast<double>(lhs.worldY) - static_cast<double>(rhs.worldY);
    return dx * dx + dy * dy;
}

void addStartIssue(MatchSetupResolution& result, MatchSetupIssueCode code,
                   MatchPlayerSlotId slot, container::String message) {
    addIssue(result, code, slot, std::move(message));
}

// The original loop used global RNG and mutable GameSlot state.  This modern
// value-only allocator first honours explicit reservations, then makes every
// remaining choice from a canonical map layout.  New teams maximize separation
// from already placed teams; members of an existing alliance minimize their
// distance to that team while retaining as much enemy separation as possible.
// Hash tie-breaking avoids container-order and UI timing influence.
[[nodiscard]] bool assignStartPositions(const MatchDraft& draft,
                                        ResolvedMatchSetup& setup,
                                        MatchSetupResolution& result) {
    container::Vector<MatchStartPosition> layout = draft.availableStartPositions;
    std::sort(layout.begin(), layout.end(), [](const MatchStartPosition& lhs,
                                               const MatchStartPosition& rhs) {
        return lhs.index < rhs.index;
    });
    for (size_t index = 0; index < layout.size(); ++index) {
        const MatchStartPosition& position = layout[index];
        if (position.index < 0 || !std::isfinite(position.worldX) ||
            !std::isfinite(position.worldY)) {
            addStartIssue(result, MatchSetupIssueCode::InvalidStartPosition,
                          INVALID_MATCH_PLAYER_SLOT_ID,
                          "map start layout contains an invalid position");
        }
        if (index != 0 && layout[index - 1].index == position.index) {
            addStartIssue(result, MatchSetupIssueCode::InvalidStartPosition,
                          INVALID_MATCH_PLAYER_SLOT_ID,
                          "map start layout contains duplicate start indices");
        }
    }
    if (!result.issues.empty()) return false;

    container::Vector<int32_t> used;
    used.reserve(setup.players.size());
    const auto isUsed = [&used](int32_t index) {
        return std::find(used.begin(), used.end(), index) != used.end();
    };

    // Validate the complete set of explicit selections before assigning any
    // random slot. This prevents a later source-order dependent fallback.
    for (const ResolvedPlayerSetup& player : setup.players) {
        if (player.participation != PlayerParticipationKind::Participant) continue;
        if (player.startPosition < -1) {
            addStartIssue(result, MatchSetupIssueCode::InvalidStartPosition, player.slot,
                          "player start position uses an unsupported negative sentinel");
            continue;
        }
        if (player.startPosition < 0) continue;
        if (!layout.empty() && !findStartPosition(layout, player.startPosition)) {
            addStartIssue(result, MatchSetupIssueCode::InvalidStartPosition, player.slot,
                          "requested start position is absent from the map layout");
            continue;
        }
        if (isUsed(player.startPosition)) {
            addStartIssue(result, MatchSetupIssueCode::DuplicateStartPosition, player.slot,
                          "two resolved players request the same start position");
            continue;
        }
        used.push_back(player.startPosition);
    }
    if (!result.issues.empty() || layout.empty()) return result.issues.empty();

    const auto candidateTieBreak = [&draft](const ResolvedPlayerSetup& player,
                                             const MatchStartPosition& candidate) {
        return mix(static_cast<uint64_t>(draft.seed) ^
                   (static_cast<uint64_t>(player.slot.value) << 32u) ^
                   static_cast<uint32_t>(candidate.index) ^ 0x53544152545f4c59ull);
    };
    for (ResolvedPlayerSetup& player : setup.players) {
        if (player.participation != PlayerParticipationKind::Participant ||
            player.startPosition >= 0) {
            continue;
        }
        if (used.size() >= layout.size()) {
            addStartIssue(result, MatchSetupIssueCode::NoAvailableStartPosition, player.slot,
                          "no unused map start position remains for this participant");
            continue;
        }

        const MatchStartPosition* best = nullptr;
        double bestPrimary = 0.0;
        double bestSecondary = 0.0;
        uint64_t bestTieBreak = 0;
        bool bestHasTeamMate = false;
        for (const MatchStartPosition& candidate : layout) {
            if (isUsed(candidate.index)) continue;

            bool hasTeamMate = false;
            bool hasOpponent = false;
            double nearestTeamMate = std::numeric_limits<double>::infinity();
            double nearestOpponent = std::numeric_limits<double>::infinity();
            for (const ResolvedPlayerSetup& assigned : setup.players) {
                if (assigned.participation != PlayerParticipationKind::Participant ||
                    assigned.startPosition < 0) {
                    continue;
                }
                const MatchStartPosition* assignedPosition =
                    findStartPosition(layout, assigned.startPosition);
                if (!assignedPosition) continue;
                const double distance = squaredDistance(candidate, *assignedPosition);
                if (shareAlliance(player, assigned)) {
                    hasTeamMate = true;
                    nearestTeamMate = std::min(nearestTeamMate, distance);
                } else {
                    hasOpponent = true;
                    nearestOpponent = std::min(nearestOpponent, distance);
                }
            }

            // With no teammate assigned this slot begins a new team, so it
            // maximizes the nearest opponent distance.  Once a team anchor
            // exists it chooses the closest free teammate position, then
            // maximizes enemy separation as a stable secondary criterion.
            const double primary = hasTeamMate ? nearestTeamMate
                : (hasOpponent ? nearestOpponent : 0.0);
            const double secondary = hasTeamMate
                ? (hasOpponent ? nearestOpponent : std::numeric_limits<double>::infinity())
                : 0.0;
            const uint64_t tieBreak = candidateTieBreak(player, candidate);
            const bool better = !best ||
                (hasTeamMate != bestHasTeamMate && hasTeamMate) ||
                (hasTeamMate == bestHasTeamMate &&
                 ((hasTeamMate && (primary < bestPrimary ||
                    (primary == bestPrimary && (secondary > bestSecondary ||
                     (secondary == bestSecondary && tieBreak < bestTieBreak))))) ||
                  (!hasTeamMate && (primary > bestPrimary ||
                    (primary == bestPrimary && tieBreak < bestTieBreak)))));
            if (better) {
                best = &candidate;
                bestPrimary = primary;
                bestSecondary = secondary;
                bestTieBreak = tieBreak;
                bestHasTeamMate = hasTeamMate;
            }
        }
        if (!best) {
            addStartIssue(result, MatchSetupIssueCode::NoAvailableStartPosition, player.slot,
                          "no unused map start position remains for this participant");
            continue;
        }
        player.startPosition = best->index;
        used.push_back(best->index);
    }
    return result.issues.empty();
}

} // namespace

namespace {

[[nodiscard]] bool canonicalPlayerOrder(const ResolvedPlayerSetup* lhs,
                                        const ResolvedPlayerSetup* rhs) noexcept {
    if (lhs->player != rhs->player) return lhs->player < rhs->player;
    if (lhs->slot != rhs->slot) return lhs->slot < rhs->slot;
    if (lhs->controller != rhs->controller) {
        return static_cast<uint8_t>(lhs->controller) < static_cast<uint8_t>(rhs->controller);
    }
    if (lhs->aiDifficulty != rhs->aiDifficulty) {
        return static_cast<uint8_t>(lhs->aiDifficulty) < static_cast<uint8_t>(rhs->aiDifficulty);
    }
    if (lhs->participation != rhs->participation) {
        return static_cast<uint8_t>(lhs->participation) < static_cast<uint8_t>(rhs->participation);
    }
    if (lhs->faction != rhs->faction) return lhs->faction < rhs->faction;
    if (lhs->color != rhs->color) return lhs->color < rhs->color;
    if (lhs->alliance != rhs->alliance) return lhs->alliance < rhs->alliance;
    if (lhs->startPosition != rhs->startPosition) return lhs->startPosition < rhs->startPosition;
    if (lhs->startingMoney != rhs->startingMoney) return lhs->startingMoney < rhs->startingMoney;
    return lhs->displayName < rhs->displayName;
}

void writeSetupHeader(DigestWriter& writer, const ResolvedMatchSetup& setup) noexcept {
    writer.u32(setup.schemaVersion);
    writer.byte(static_cast<uint8_t>(setup.mode));
    writer.string(setup.mapName);
    writer.u32(setup.mapCrc);
    writer.u32(setup.mapSize);
    writer.i32(setup.difficulty);
    writer.i32(setup.rankPoints);
    writer.i32(setup.gameSpeedFps);
    writer.u32(setup.seed);
    writer.boolean(setup.superweaponRestricted);
    writer.boolean(setup.oldFactionsOnly);
    writer.u64(setup.playerRulesetFingerprint);
    writer.u64(setup.simulationContentFingerprint);
}

[[nodiscard]] container::Vector<const ResolvedPlayerSetup*> canonicalPlayers(
    const ResolvedMatchSetup& setup) {
    container::Vector<const ResolvedPlayerSetup*> ordered;
    ordered.reserve(setup.players.size());
    for (const ResolvedPlayerSetup& player : setup.players) ordered.push_back(&player);
    std::sort(ordered.begin(), ordered.end(), canonicalPlayerOrder);
    return ordered;
}

void writeSimulationPlayer(DigestWriter& writer, const ResolvedPlayerSetup& player) noexcept {
    writer.byte(player.player.value);
    writer.byte(player.slot.value);
    writer.byte(static_cast<uint8_t>(player.controller));
    writer.byte(static_cast<uint8_t>(player.aiDifficulty));
    writer.byte(static_cast<uint8_t>(player.participation));
    writer.u32(player.faction.value);
    writer.byte(player.alliance.value);
    writer.i32(player.startPosition);
    writer.i32(player.startingMoney);
}

void writeContentPlayer(DigestWriter& writer, const ResolvedPlayerSetup& player) noexcept {
    writeSimulationPlayer(writer, player);
    writer.u32(player.color.value);
    writer.string(player.displayName);
}

} // namespace

uint64_t ResolvedMatchSetup::simulationDigest() const {
    DigestWriter writer;
    writeSetupHeader(writer, *this);
    const container::Vector<const ResolvedPlayerSetup*> ordered = canonicalPlayers(*this);
    writer.u32(static_cast<uint32_t>(ordered.size()));
    for (const ResolvedPlayerSetup* player : ordered) writeSimulationPlayer(writer, *player);
    return writer.finish();
}

uint64_t ResolvedMatchSetup::contentDigest() const {
    DigestWriter writer;
    writeSetupHeader(writer, *this);
    const container::Vector<const ResolvedPlayerSetup*> ordered = canonicalPlayers(*this);
    writer.u32(static_cast<uint32_t>(ordered.size()));
    for (const ResolvedPlayerSetup* player : ordered) writeContentPlayer(writer, *player);
    return writer.finish();
}

ResolvedMatchSetupValidation validateResolvedMatchSetup(const ResolvedMatchSetup& setup,
                                                         const MultiplayerRuleset* ruleset,
                                                         uint64_t expectedSimulationContentFingerprint) {
    ResolvedMatchSetupValidation result;
    const auto add = [&result](ResolvedMatchSetupIssueCode code, MatchPlayerSlotId slot,
                               container::String message) {
        result.issues.push_back({.code = code, .slot = slot, .message = std::move(message)});
    };

    if (setup.schemaVersion != ResolvedMatchSetup::kSchemaVersion) {
        add(ResolvedMatchSetupIssueCode::UnsupportedSchema, INVALID_MATCH_PLAYER_SLOT_ID,
            "resolved match setup has an unsupported schema version");
    }
    if (static_cast<uint8_t>(setup.mode) >= static_cast<uint8_t>(GameMode::Invalid)) {
        add(ResolvedMatchSetupIssueCode::InvalidMode, INVALID_MATCH_PLAYER_SLOT_ID,
            "resolved match setup has an invalid game mode");
    }
    if (setup.gameSpeedFps <= 0) {
        add(ResolvedMatchSetupIssueCode::InvalidGameSpeed, INVALID_MATCH_PLAYER_SLOT_ID,
            "resolved match setup has a non-positive game speed");
    }
    if (setup.playerRulesetFingerprint == 0) {
        add(ResolvedMatchSetupIssueCode::MissingPlayerRulesetFingerprint, INVALID_MATCH_PLAYER_SLOT_ID,
            "resolved match setup has no player-rules fingerprint");
    }
    if (setup.simulationContentFingerprint == 0) {
        add(ResolvedMatchSetupIssueCode::MissingSimulationContentFingerprint,
            INVALID_MATCH_PLAYER_SLOT_ID,
            "resolved match setup has no aggregate simulation content fingerprint");
    }
    if (setup.players.size() > PLAYER_SLOT_COUNT) {
        add(ResolvedMatchSetupIssueCode::TooManyPlayers, INVALID_MATCH_PLAYER_SLOT_ID,
            "resolved match setup exceeds command-slot capacity");
    }
    if (ruleset) {
        if (!ruleset->isLoaded() ||
            setup.playerRulesetFingerprint != ruleset->simulationFingerprint()) {
            add(ResolvedMatchSetupIssueCode::PlayerRulesetMismatch, INVALID_MATCH_PLAYER_SLOT_ID,
                "resolved match setup does not match the loaded player ruleset");
        }
    }
    if (expectedSimulationContentFingerprint != 0 &&
        setup.simulationContentFingerprint != expectedSimulationContentFingerprint) {
        add(ResolvedMatchSetupIssueCode::SimulationContentMismatch,
            INVALID_MATCH_PLAYER_SLOT_ID,
            "resolved match setup does not match the loaded simulation content");
    }

    container::Array<bool, PLAYER_SLOT_COUNT> seenSlots{};
    container::Array<bool, MAP_PLAYER_COUNT> seenPlayers{};
    container::Vector<MultiplayerColorId> usedColors;
    container::Vector<int32_t> usedStarts;
    for (const ResolvedPlayerSetup& player : setup.players) {
        const bool validIdentity = player.player.isMapPlayer() && player.slot &&
            player.player.value == player.slot.value;
        if (!validIdentity) {
            add(ResolvedMatchSetupIssueCode::InvalidPlayerIdentity, player.slot,
                "resolved player does not have a valid slot-backed map player identity");
            continue;
        }
        if (seenPlayers[player.player.value]) {
            add(ResolvedMatchSetupIssueCode::DuplicatePlayer, player.slot,
                "resolved match setup contains duplicate player identities");
        }
        seenPlayers[player.player.value] = true;
        if (seenSlots[player.slot.value]) {
            add(ResolvedMatchSetupIssueCode::DuplicateSlot, player.slot,
                "resolved match setup contains duplicate command slots");
        }
        seenSlots[player.slot.value] = true;

        if (player.participation != PlayerParticipationKind::Participant &&
            player.participation != PlayerParticipationKind::Observer) {
            add(ResolvedMatchSetupIssueCode::InvalidParticipation, player.slot,
                "resolved player has an unknown participation kind");
            continue;
        }
        if (player.participation == PlayerParticipationKind::Observer) {
            if (player.controller != PlayerControllerKind::Observer ||
                player.aiDifficulty != AiDifficulty::None || player.faction ||
                player.color || player.alliance || player.startPosition != -1 ||
                player.startingMoney != 0) {
                add(ResolvedMatchSetupIssueCode::InvalidObserverState, player.slot,
                    "observer setup carries participant simulation state");
            }
            continue;
        }

        if (player.controller != PlayerControllerKind::Human &&
            player.controller != PlayerControllerKind::Ai) {
            add(ResolvedMatchSetupIssueCode::InvalidController, player.slot,
                "participant setup has an invalid controller");
        }
        if (!isKnownAiDifficulty(player.aiDifficulty) ||
            (player.controller == PlayerControllerKind::Human &&
             player.aiDifficulty != AiDifficulty::None) ||
            (player.controller == PlayerControllerKind::Ai &&
             player.aiDifficulty == AiDifficulty::None)) {
            add(ResolvedMatchSetupIssueCode::InvalidAiDifficulty, player.slot,
                "participant setup has an invalid AI difficulty for its controller");
        }
        if (!player.faction || !player.color || player.startPosition < -1 ||
            player.startingMoney < 0) {
            add(ResolvedMatchSetupIssueCode::InvalidParticipantState, player.slot,
                "participant setup is missing faction/color or has invalid spawn/cash state");
        }
        if (player.color) {
            if (std::find(usedColors.begin(), usedColors.end(), player.color) != usedColors.end()) {
                add(ResolvedMatchSetupIssueCode::DuplicateColor, player.slot,
                    "resolved match setup contains duplicate multiplayer colors");
            }
            usedColors.push_back(player.color);
        }
        if (player.startPosition >= 0) {
            if (std::find(usedStarts.begin(), usedStarts.end(), player.startPosition) != usedStarts.end()) {
                add(ResolvedMatchSetupIssueCode::DuplicateStartPosition, player.slot,
                    "resolved match setup contains duplicate start positions");
            }
            usedStarts.push_back(player.startPosition);
        }
        if (!ruleset) continue;
        const FactionTemplate* faction = ruleset->findFaction(player.faction);
        if (!faction || !faction->playable || faction->observer) {
            add(ResolvedMatchSetupIssueCode::MissingFaction, player.slot,
                "participant setup refers to a missing or unplayable faction");
        } else if (setup.oldFactionsOnly && !faction->oldFaction) {
            add(ResolvedMatchSetupIssueCode::OldFactionRestricted, player.slot,
                "participant setup refers to a faction excluded by oldFactionsOnly");
        }
        if (!ruleset->findColor(player.color)) {
            add(ResolvedMatchSetupIssueCode::MissingColor, player.slot,
                "participant setup refers to a missing multiplayer color");
        }
    }
    return result;
}

MatchSetupResolution MatchSetupResolver::resolve(const MatchDraft& draft,
                                                 const MultiplayerRuleset& ruleset,
                                                 uint64_t simulationContentFingerprint) {
    MatchSetupResolution result;
    if (!ruleset.isLoaded()) {
        addIssue(result, MatchSetupIssueCode::MissingFaction, INVALID_MATCH_PLAYER_SLOT_ID,
                 "cannot resolve a match without a loaded immutable ruleset");
        return result;
    }
    if (simulationContentFingerprint == 0) {
        addIssue(result, MatchSetupIssueCode::InvalidResolvedSetup, INVALID_MATCH_PLAYER_SLOT_ID,
                 "cannot resolve a match without a frozen simulation content fingerprint");
        return result;
    }

    const container::Vector<FactionTemplateId> eligible = eligibleFactions(ruleset, draft.oldFactionsOnly);

    ResolvedMatchSetup setup;
    setup.mode = draft.mode;
    setup.mapName = draft.mapName;
    setup.mapCrc = draft.mapCrc;
    setup.mapSize = draft.mapSize;
    setup.difficulty = draft.difficulty;
    setup.rankPoints = draft.rankPoints;
    setup.gameSpeedFps = draft.gameSpeedFps;
    setup.seed = draft.seed;
    setup.superweaponRestricted = draft.superweaponRestricted;
    setup.oldFactionsOnly = draft.oldFactionsOnly;
    setup.playerRulesetFingerprint = ruleset.simulationFingerprint();
    setup.simulationContentFingerprint = simulationContentFingerprint;

    container::Array<bool, PLAYER_SLOT_COUNT> seenSlots{};
    container::Vector<MultiplayerColorId> usedColors;
    const container::Vector<MultiplayerColorId>& availableColors = ruleset.colorIdsByAuthoredOrder();

    for (size_t sourceIndex = 0; sourceIndex < draft.slots.size(); ++sourceIndex) {
        const MatchPlayerDraft& source = draft.slots[sourceIndex];
        MatchPlayerSlotId slot = source.slot;
        if (slot == INVALID_MATCH_PLAYER_SLOT_ID) {
            slot = MatchPlayerSlotId{static_cast<uint8_t>(sourceIndex)};
        }
        if (!slot) {
            addIssue(result, MatchSetupIssueCode::InvalidSlot, slot,
                     "match draft contains a slot outside the supported range");
            continue;
        }
        if (seenSlots[slot.value]) {
            addIssue(result, MatchSetupIssueCode::DuplicateSlot, slot,
                     "match draft contains the same player slot more than once");
            continue;
        }
        seenSlots[slot.value] = true;

        const std::optional<ResolvedController> controller = controllerFor(source.requestedState);
        if (!controller) continue;

        if (source.participation != PlayerParticipationKind::Participant &&
            source.participation != PlayerParticipationKind::Observer) {
            addIssue(result, MatchSetupIssueCode::InvalidObserverSelection, slot,
                     "match draft contains an unknown player participation kind");
            continue;
        }
        if (source.legacyPlayableTemplateIndex < LEGACY_PLAYER_TEMPLATE_RANDOM) {
            addIssue(result, MatchSetupIssueCode::InvalidTemplateIndex, slot,
                     "legacy player template uses an unsupported negative sentinel");
            continue;
        }
        if (source.legacyColorIndex < -1) {
            addIssue(result, MatchSetupIssueCode::InvalidColorIndex, slot,
                     "legacy player color uses an unsupported negative sentinel");
            continue;
        }

        const FactionTemplate* namedFaction = nullptr;
        if (!source.requestedTemplateName.empty()) {
            namedFaction = ruleset.findFaction(source.requestedTemplateName);
            if (!namedFaction) {
                addIssue(result, MatchSetupIssueCode::MissingFaction, slot,
                         "requested faction template was not found: " + source.requestedTemplateName);
                continue;
            }
        }

        const bool observer = source.participation == PlayerParticipationKind::Observer ||
            (namedFaction && namedFaction->observer);
        const container::String displayName = source.displayName.empty()
            ? (observer ? "Observer_" : "Player_") + std::to_string(slot.value)
            : source.displayName;
        if (observer) {
            // An observer is a roster participant, not a magic/fake faction.
            // It must not consume a player color, starting cash, alliance or
            // spawn position, and it remains unable to author lockstep input.
            if ((source.participation == PlayerParticipationKind::Observer && namedFaction &&
                 !namedFaction->observer) ||
                source.legacyPlayableTemplateIndex != LEGACY_PLAYER_TEMPLATE_RANDOM ||
                !source.requestedSide.empty() || !source.requestedBaseSide.empty() ||
                source.legacyColorIndex != -1 || source.startPosition != -1 ||
                source.allianceNumber != -1) {
                addIssue(result, MatchSetupIssueCode::InvalidObserverSelection, slot,
                         "observer requests must not carry a playable faction, color, alliance or spawn");
                continue;
            }
            setup.players.push_back({
                .player = PlayerId::fromSlot(slot),
                .slot = slot,
                .controller = PlayerControllerKind::Observer,
                .participation = PlayerParticipationKind::Observer,
                .displayName = displayName,
            });
            continue;
        }

        std::optional<FactionTemplateId> selectedFaction;
        if (namedFaction) {
            selectedFaction = namedFaction->id;
        }
        if (source.legacyPlayableTemplateIndex >= 0) {
            const auto indexed = ruleset.playableTemplateIdAt(
                static_cast<size_t>(source.legacyPlayableTemplateIndex));
            if (!indexed) {
                addIssue(result, MatchSetupIssueCode::InvalidTemplateIndex, slot,
                         "legacy playable faction index is outside the frozen ruleset");
            } else if (selectedFaction && *selectedFaction != *indexed) {
                addIssue(result, MatchSetupIssueCode::ConflictingLegacyFaction, slot,
                         "named faction and legacy playable faction index resolve to different templates");
            } else {
                selectedFaction = *indexed;
            }
        }
        if (!selectedFaction) {
            if (eligible.empty()) {
                addIssue(result, MatchSetupIssueCode::NoEligibleFaction, slot,
                         "ruleset contains no playable faction eligible for this match");
                continue;
            }
            container::Vector<FactionTemplateId> constrained;
            if (!source.requestedSide.empty() || !source.requestedBaseSide.empty()) {
                for (const FactionTemplateId candidate : eligible) {
                    const FactionTemplate* candidateTemplate = ruleset.findFaction(candidate);
                    if (!candidateTemplate) continue;
                    const bool sideMatches = source.requestedSide.empty() ||
                        equalsInsensitive(source.requestedSide, candidateTemplate->side);
                    const bool baseSideMatches = source.requestedBaseSide.empty() ||
                        equalsInsensitive(source.requestedBaseSide, candidateTemplate->baseSide);
                    if (sideMatches && baseSideMatches) constrained.push_back(candidate);
                }
                if (constrained.empty()) {
                    addIssue(result, MatchSetupIssueCode::MissingFaction, slot,
                             "requested side/base side has no eligible faction in the frozen ruleset");
                    continue;
                }
            }
            const container::Vector<FactionTemplateId>& candidates = constrained.empty() ? eligible : constrained;
            const uint64_t randomKey = mix(static_cast<uint64_t>(draft.seed) ^
                                           (static_cast<uint64_t>(slot.value) << 32u) ^
                                           0x4652414354494f4eull);
            selectedFaction = candidates[static_cast<size_t>(randomKey % candidates.size())];
        }

        const FactionTemplate* faction = ruleset.findFaction(*selectedFaction);
        if (!faction) {
            addIssue(result, MatchSetupIssueCode::MissingFaction, slot,
                     "resolved faction handle is absent from the frozen ruleset");
            continue;
        }
        if (!faction->playable || faction->observer) {
            addIssue(result, MatchSetupIssueCode::FactionNotPlayable, slot,
                     "requested faction is not a playable participant: " + faction->name);
            continue;
        }
        if (draft.oldFactionsOnly && !faction->oldFaction) {
            addIssue(result, MatchSetupIssueCode::OldFactionRestricted, slot,
                     "requested faction is excluded by oldFactionsOnly: " + faction->name);
            continue;
        }
        if (!source.requestedSide.empty() && !equalsInsensitive(source.requestedSide, faction->side)) {
            addIssue(result, MatchSetupIssueCode::ConflictingLegacySide, slot,
                     "requested side conflicts with resolved faction: " + source.requestedSide);
            continue;
        }
        if (!source.requestedBaseSide.empty() && !equalsInsensitive(source.requestedBaseSide, faction->baseSide)) {
            addIssue(result, MatchSetupIssueCode::ConflictingLegacySide, slot,
                     "requested base side conflicts with resolved faction: " + source.requestedBaseSide);
            continue;
        }

        MultiplayerColorId color = INVALID_MULTIPLAYER_COLOR_ID;
        if (source.legacyColorIndex >= 0) {
            const int64_t colorIndex = source.legacyColorIndex;
            if (colorIndex >= static_cast<int64_t>(availableColors.size())) {
                addIssue(result, MatchSetupIssueCode::InvalidColorIndex, slot,
                         "legacy color index is outside the frozen ruleset");
                continue;
            }
            color = availableColors[static_cast<size_t>(colorIndex)];
            if (std::find(usedColors.begin(), usedColors.end(), color) != usedColors.end()) {
                addIssue(result, MatchSetupIssueCode::DuplicateColor, slot,
                         "two resolved players request the same multiplayer color");
                continue;
            }
        } else if (draft.mode == GameMode::SinglePlayer &&
                   !availableColors.empty()) {
            // Campaign/mission players do not use the skirmish random-color
            // allocator.  RefCode initializes Player::m_color from the
            // selected PlayerTemplate's PreferredColor unless the map or
            // lobby explicitly overrides it.  Preserve that contract by
            // selecting the matching frozen MultiplayerColor entry (China
            // and CN01 therefore resolve to ColorRed).
            for (const MultiplayerColorId candidate : availableColors) {
                const MultiplayerColorDefinition* definition =
                    ruleset.findColor(candidate);
                if (!definition ||
                    definition->day != faction->simulation.preferredColor ||
                    std::find(usedColors.begin(), usedColors.end(), candidate) !=
                        usedColors.end()) {
                    continue;
                }
                color = candidate;
                break;
            }
            if (!color) {
                const auto picked = chooseUnused<MultiplayerColorId>(
                    availableColors, usedColors,
                    mix(static_cast<uint64_t>(draft.seed) ^
                        (static_cast<uint64_t>(slot.value) << 32u) ^
                        0x434f4c4f525f4944ull));
                if (picked) color = *picked;
            }
            if (!color) {
                addIssue(result, MatchSetupIssueCode::NoAvailableColor, slot,
                         "no unused multiplayer color remains for this participant");
                continue;
            }
        } else if (!availableColors.empty()) {
            const auto picked = chooseUnused<MultiplayerColorId>(
                availableColors, usedColors,
                mix(static_cast<uint64_t>(draft.seed) ^ (static_cast<uint64_t>(slot.value) << 32u) ^
                    0x434f4c4f525f4944ull));
            if (!picked) {
                addIssue(result, MatchSetupIssueCode::NoAvailableColor, slot,
                         "no unused multiplayer color remains for this participant");
                continue;
            }
            color = *picked;
        } else {
            addIssue(result, MatchSetupIssueCode::NoAvailableColor, slot,
                     "ruleset contains no multiplayer colors for this participant");
            continue;
        }
        if (color) usedColors.push_back(color);

        AllianceGroupId alliance = INVALID_ALLIANCE_GROUP_ID;
        if (source.allianceNumber < -1) {
            addIssue(result, MatchSetupIssueCode::InvalidAlliance, slot,
                     "alliance number uses an unsupported negative sentinel");
            continue;
        }
        if (source.allianceNumber >= 0) {
            if (source.allianceNumber >= static_cast<int32_t>(std::numeric_limits<uint8_t>::max())) {
                addIssue(result, MatchSetupIssueCode::InvalidAlliance, slot,
                         "alliance number exceeds the portable match setup range");
                continue;
            }
            alliance = AllianceGroupId{static_cast<uint8_t>(source.allianceNumber)};
        }

        const int32_t defaultCash = draft.requestedStartingMoney > 0
            ? draft.requestedStartingMoney
            : ruleset.multiplayer().defaultStartingMoney;
        const int32_t startingMoney = faction->simulation.startingMoney != 0
            ? faction->simulation.startingMoney
            : defaultCash;
        setup.players.push_back({
            .player = PlayerId::fromSlot(slot),
            .slot = slot,
            .controller = controller->kind,
            .aiDifficulty = controller->aiDifficulty,
            .participation = PlayerParticipationKind::Participant,
            .faction = faction->id,
            .color = color,
            .alliance = alliance,
            .displayName = displayName,
            // The layout-aware resolver assigns random starts only after the
            // full roster has been canonicalized.  Keep an explicit request
            // here as data rather than mutating a GameSlot in-place.
            .startPosition = source.startPosition,
            .startingMoney = startingMoney,
        });
    }

    if (!result.issues.empty()) return result;
    std::sort(setup.players.begin(), setup.players.end(), [](const ResolvedPlayerSetup& lhs,
                                                              const ResolvedPlayerSetup& rhs) {
        return lhs.player < rhs.player;
    });
    if (!assignStartPositions(draft, setup, result)) return result;
    const ResolvedMatchSetupValidation validation = validateResolvedMatchSetup(setup, &ruleset);
    if (!validation.ok()) {
        for (const ResolvedMatchSetupIssue& issue : validation.issues) {
            addIssue(result, MatchSetupIssueCode::InvalidResolvedSetup, issue.slot, issue.message);
        }
        return result;
    }
    result.setup = std::move(setup);
    return result;
}

MatchDraft LegacyMatchSetupAdapter::draftFromGameStartInfo(const GameStartInfo& source,
                                                           LocalControlContext& localContext) {
    MatchDraft draft;
    draft.mode = source.mode;
    draft.mapName = source.mapName;
    draft.mapCrc = source.mapCRC;
    draft.mapSize = source.mapSize;
    draft.declaredRulesCrc = source.rulesCRC;
    draft.difficulty = source.difficulty;
    draft.rankPoints = source.rankPoints;
    draft.gameSpeedFps = source.gameSpeedFPS;
    draft.seed = static_cast<uint32_t>(source.seed);
    draft.superweaponRestricted = source.superweaponRestricted;
    draft.oldFactionsOnly = source.oldFactionsOnly;
    draft.requestedStartingMoney = source.startingMoney;

    localContext.controlledSlot = (source.localPlayerSlot >= 0 && source.localPlayerSlot < MAX_SLOTS)
        ? MatchPlayerSlotId{static_cast<uint8_t>(source.localPlayerSlot)}
        : INVALID_MATCH_PLAYER_SLOT_ID;
    for (size_t index = 0; index < draft.slots.size(); ++index) {
        const GameSlot& sourceSlot = source.slots[index];
        MatchPlayerDraft& destination = draft.slots[index];
        destination.slot = MatchPlayerSlotId{static_cast<uint8_t>(index)};
        destination.requestedState = sourceSlot.state;
        destination.displayName = sourceSlot.name;
        if (sourceSlot.playerTemplate == LEGACY_PLAYER_TEMPLATE_OBSERVER) {
            // RefCode's -2 is an observer role, not a faction/template index.
            // Normalize it immediately so no later resolver path can mistake
            // it for a random player choice.
            destination.participation = PlayerParticipationKind::Observer;
            destination.legacyPlayableTemplateIndex = LEGACY_PLAYER_TEMPLATE_RANDOM;
            destination.legacyColorIndex = -1;
            destination.startPosition = -1;
            destination.allianceNumber = -1;
        } else {
            destination.legacyPlayableTemplateIndex = sourceSlot.playerTemplate;
            destination.legacyColorIndex = sourceSlot.color;
            destination.startPosition = sourceSlot.startPos;
            destination.allianceNumber = sourceSlot.teamNumber;
        }
    }

    if (localContext.controlledSlot) {
        MatchPlayerDraft& local = draft.slots[localContext.controlledSlot.value];
        local.requestedTemplateName = source.localPlayerTemplateName;
        local.requestedSide = source.localPlayerSide;
        local.requestedBaseSide = source.localPlayerBaseSide;
    }
    return draft;
}

} // namespace engine
