#pragma once

#include "core/container/container_types.h"

#include "FactionTemplate.h"
#include <cstdint>
#include <optional>
namespace engine {

// Legacy GameSlot.playerTemplate encodes these two sentinel values.  They are
// translated exactly once at the adapter boundary; new code uses the typed
// participation/selection fields below and never propagates negative magic.
inline constexpr int32_t LEGACY_PLAYER_TEMPLATE_RANDOM = -1;
inline constexpr int32_t LEGACY_PLAYER_TEMPLATE_OBSERVER = -2;

// UI, command line, replay-v2 and session-ticket input are intentionally
// translated into this draft form before touching simulation state.  It may
// still contain legacy positional indices and random requests; the resolver
// consumes those exactly once to produce ResolvedMatchSetup.
struct MatchPlayerDraft final {
    MatchPlayerSlotId slot = INVALID_MATCH_PLAYER_SLOT_ID;
    SlotState requestedState = SLOT_OPEN;
    container::String displayName;
    container::String requestedTemplateName;
    container::String requestedSide;
    container::String requestedBaseSide;
    // Current GeneralsTD lobby screens use an index into the frozen playable
    // template projection. Raw RefCode authored-template imports belong in a
    // separate adapter rather than silently sharing this index domain.
    int32_t legacyPlayableTemplateIndex = LEGACY_PLAYER_TEMPLATE_RANDOM;
    int32_t legacyColorIndex = -1;
    int32_t startPosition = -1;
    int32_t allianceNumber = -1;
    PlayerParticipationKind participation = PlayerParticipationKind::Participant;
};

// Detached map-start layout supplied by TerrainLogic before the roster is
// resolved.  Keeping coordinates with the index lets the resolver make a
// deterministic, team-aware placement without depending on a map object,
// renderer resource, or later ECS ownership system.
struct MatchStartPosition final {
    int32_t index = -1;
    float worldX = 0.0f;
    float worldY = 0.0f;
};

struct MatchDraft final {
    GameMode mode = GameMode::Invalid;
    container::String mapName;
    uint32_t mapCrc = 0;
    uint32_t mapSize = 0;
    uint32_t declaredRulesCrc = 0;
    int32_t difficulty = DIFFICULTY_NORMAL;
    int32_t rankPoints = 0;
    int32_t gameSpeedFps = DEFAULT_GAME_SPEED_FPS;
    uint32_t seed = 0;
    bool superweaponRestricted = false;
    bool oldFactionsOnly = false;
    // A non-positive value means use the multiplayer rules default unless a
    // template declares a non-zero StartMoney, matching original semantics.
    int32_t requestedStartingMoney = 0;
    container::Array<MatchPlayerDraft, PLAYER_SLOT_COUNT> slots{};
    // Terrain/map loading populates this before resolution.  Random starts
    // therefore become canonical match input rather than UI-local state.
    container::Vector<MatchStartPosition> availableStartPositions;
};

// Client-only state.  It never participates in a match digest, save, replay
// or network command stream because every connected client has a different
// local player.
struct LocalControlContext final {
    MatchPlayerSlotId controlledSlot = INVALID_MATCH_PLAYER_SLOT_ID;
};

struct ResolvedPlayerSetup final {
    PlayerId player = INVALID_PLAYER_ID;
    MatchPlayerSlotId slot = INVALID_MATCH_PLAYER_SLOT_ID;
    PlayerControllerKind controller = PlayerControllerKind::Human;
    AiDifficulty aiDifficulty = AiDifficulty::None;
    PlayerParticipationKind participation = PlayerParticipationKind::Participant;
    FactionTemplateId faction = INVALID_FACTION_TEMPLATE_ID;
    MultiplayerColorId color = INVALID_MULTIPLAYER_COLOR_ID;
    AllianceGroupId alliance = INVALID_ALLIANCE_GROUP_ID;
    container::String displayName;
    int32_t startPosition = -1;
    int32_t startingMoney = 0;
};

struct ResolvedMatchSetup final {
    static constexpr uint32_t kSchemaVersion = 4;

    uint32_t schemaVersion = kSchemaVersion;
    GameMode mode = GameMode::Invalid;
    container::String mapName;
    uint32_t mapCrc = 0;
    uint32_t mapSize = 0;
    int32_t difficulty = DIFFICULTY_NORMAL;
    int32_t rankPoints = 0;
    int32_t gameSpeedFps = DEFAULT_GAME_SPEED_FPS;
    uint32_t seed = 0;
    bool superweaponRestricted = false;
    bool oldFactionsOnly = false;
    // The player-rules fingerprint proves that persisted faction/color IDs
    // still resolve against this immutable catalog.
    uint64_t playerRulesetFingerprint = 0;
    // The aggregate fingerprint covers every currently loaded gameplay INI
    // source (weapons, objects, commands, locomotors and player rules), not
    // merely the player-template catalog.
    uint64_t simulationContentFingerprint = 0;
    container::Vector<ResolvedPlayerSetup> players;

    // Canonical across container insertion history and platform locale.  The
    // simulation digest deliberately excludes lobby-only text/color data;
    // contentDigest retains the full replay/roster presentation identity.
    // Neither is a cryptographic signature.
    [[nodiscard]] uint64_t simulationDigest() const;
    [[nodiscard]] uint64_t contentDigest() const;
    [[nodiscard]] uint64_t digest() const { return contentDigest(); }
};

enum class MatchSetupIssueCode : uint8_t {
    InvalidLocalSlot,
    LocalSlotNotActive,
    MissingFaction,
    FactionNotPlayable,
    ConflictingLegacyFaction,
    ConflictingLegacySide,
    InvalidTemplateIndex,
    InvalidColorIndex,
    DuplicateColor,
    NoAvailableColor,
    DuplicateStartPosition,
    InvalidStartPosition,
    NoAvailableStartPosition,
    NoEligibleFaction,
    OldFactionRestricted,
    InvalidAlliance,
    DuplicateSlot,
    InvalidSlot,
    InvalidObserverSelection,
    InvalidResolvedSetup,
};

struct MatchSetupIssue final {
    MatchSetupIssueCode code{};
    MatchPlayerSlotId slot = INVALID_MATCH_PLAYER_SLOT_ID;
    container::String message;
};

struct MatchSetupResolution final {
    std::optional<ResolvedMatchSetup> setup;
    container::Vector<MatchSetupIssue> issues;

    [[nodiscard]] bool ok() const noexcept { return setup.has_value(); }
};

// Resolved descriptors cross replay, network and session boundaries.  Keep
// their validation in one value-only gate rather than letting each consumer
// recreate a slightly different subset of the invariants.
enum class ResolvedMatchSetupIssueCode : uint8_t {
    UnsupportedSchema,
    InvalidMode,
    InvalidGameSpeed,
    MissingPlayerRulesetFingerprint,
    PlayerRulesetMismatch,
    MissingSimulationContentFingerprint,
    SimulationContentMismatch,
    TooManyPlayers,
    InvalidPlayerIdentity,
    DuplicatePlayer,
    DuplicateSlot,
    InvalidParticipation,
    InvalidController,
    InvalidAiDifficulty,
    InvalidObserverState,
    InvalidParticipantState,
    DuplicateColor,
    DuplicateStartPosition,
    MissingFaction,
    MissingColor,
    OldFactionRestricted,
};

struct ResolvedMatchSetupIssue final {
    ResolvedMatchSetupIssueCode code{};
    MatchPlayerSlotId slot = INVALID_MATCH_PLAYER_SLOT_ID;
    container::String message;
};

struct ResolvedMatchSetupValidation final {
    container::Vector<ResolvedMatchSetupIssue> issues;

    [[nodiscard]] bool ok() const noexcept { return issues.empty(); }
};

[[nodiscard]] ResolvedMatchSetupValidation validateResolvedMatchSetup(
    const ResolvedMatchSetup& setup, const MultiplayerRuleset* ruleset = nullptr,
    uint64_t expectedSimulationContentFingerprint = 0);

class MatchSetupResolver final {
public:
    [[nodiscard]] static MatchSetupResolution resolve(const MatchDraft& draft,
                                                       const MultiplayerRuleset& ruleset,
                                                       uint64_t simulationContentFingerprint);
};

// Legacy adapters live at the I/O boundary.  No new gameplay code should use
// GameStartInfo directly after it has produced a MatchDraft.
class LegacyMatchSetupAdapter final {
public:
    [[nodiscard]] static MatchDraft draftFromGameStartInfo(const GameStartInfo& source,
                                                            LocalControlContext& localContext);
};

} // namespace engine
