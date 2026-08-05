#pragma once

#include "core/container/container_types.h"

#include "MatchSetup.h"
#include "PlayerRelationshipMatrix.h"
#include "core/ecs/ObjectId.h"
#include "game/data/base/UpgradeCatalog.h"
#include "math/fixed/q32_32.h"
#include <cstdint>
#include <optional>
namespace engine::scenario {
class ScenarioDefinition;
}

namespace engine {

struct ScienceDefinition;
class RankInfoCatalog;

enum class ScienceAvailability : uint8_t {
    Available,
    Disabled,
    Hidden,
};

// RefCode ScriptEngine records a consumable notification whenever a player
// newly acquires a science. The condition path queries one matching event at a
// time; it is not a synonym for the durable `known` set.
struct PendingScienceAcquisition final {
    container::String science;
    uint32_t count = 0;
};

struct PlayerScienceState final {
    // Frozen faction baseline used by the ZH rank-reset transaction. Rank
    // downgrades discard purchased/granted sciences, then restore these
    // intrinsic values before replaying Rank 1..N grants.
    container::Vector<container::String> intrinsicKnown;
    container::Vector<container::String> known;
    container::Vector<container::String> disabled;
    container::Vector<container::String> hidden;
    // Sorted by exact (case-sensitive) Science name. A count is sufficient:
    // legacy PLAYER_ACQUIRED_SCIENCE only asks for one named acquisition and
    // consumes one matching notification, never observes global event order.
    container::Vector<PendingScienceAcquisition> pendingAcquisitions;
    int32_t purchasePoints = 0;
    int32_t intrinsicPurchasePoints = 0;
};

// Campaign/skirmish progression belongs to the authoritative player record,
// not to a UI control bar.  It deliberately keeps only data whose detailed
// rank/science catalogs may be loaded later; scripts can already mutate the
// values without inventing an AI or production module.
struct PlayerProgressState final {
    int64_t skillPoints = 0;
    int32_t rankLevel = 1;
    // Script authoring uses one-based skillset indices; this stored value is
    // zero-based and -1 explicitly means no selected skillset.
    int32_t selectedSkillset = -1;
    math::q32_32 skillPointMultiplier{int32_t{1}};
    bool listedInScoreScreen = true;
};

struct PlayerTeamRelationshipOverride final {
    ObjectTeamId target = INVALID_OBJECT_TEAM_ID;
    PlayerRelationship relationship = PlayerRelationship::Neutral;
};

struct PlayerUpgradeState final {
    // RefCode Player::m_upgradesCompleted / m_upgradesInProgress: bit indices
    // are UpgradeContentId.value - 1 from the sealed UpgradeCatalog.
    UpgradeMask completed;
    UpgradeMask inProgress;
};

struct PlayerProductionModifierState final {
    container::Vector<ProductionPercentModifier> cost;
    container::Vector<ProductionPercentModifier> time;
    container::Vector<ProductionVeterancyModifier> veterancy;
    // Legacy Handicap Dict projection. Scenario/session launch owns writing
    // these values; production consumes only this frozen Player state. The
    // neutral defaults preserve maps which do not author handicap fields.
    math::q32_32 genericBuildCostHandicap{int32_t{1}};
    math::q32_32 structureBuildCostHandicap{int32_t{1}};
    math::q32_32 genericBuildTimeHandicap{int32_t{1}};
    math::q32_32 structureBuildTimeHandicap{int32_t{1}};
};

// Script-authored production admission policy.
struct PlayerConstructionPolicyState final {
    int32_t teamDelaySeconds = 0;
    bool baseConstructionEnabled = true;
    bool unitConstructionEnabled = true;
};

// Authoritative counterpart to RefCode Energy. All values are integer
// kilowatt-like units: a player owns one aggregate, while the ECS energy
// system derives it from live ObjectEnergyComponents. The sabotage deadline
// is an absolute confirmed tick, never a wall-clock duration.
struct PlayerEnergyState final {
    int32_t production = 0;
    int32_t consumption = 0;
    uint64_t sabotagedUntilTick = 0;

    [[nodiscard]] bool isSabotaged(uint64_t confirmedTick) const noexcept {
        return confirmedTick < sabotagedUntilTick;
    }
    [[nodiscard]] int32_t effectiveProduction(uint64_t confirmedTick) const noexcept {
        return isSabotaged(confirmedTick) ? 0 : production;
    }
    [[nodiscard]] bool hasSufficientPower(uint64_t confirmedTick) const noexcept {
        return !isSabotaged(confirmedTick) && production >= consumption;
    }
};

struct PlayerRadarState final {
    uint32_t providerCount = 0;
    uint32_t disableProofProviderCount = 0;

    [[nodiscard]] bool hasRadar(
        const PlayerEnergyState& energy,
        uint64_t confirmedTick) const noexcept {
        if (providerCount == 0) return false;
        return energy.hasSufficientPower(confirmedTick) ||
               disableProofProviderCount != 0;
    }
};

struct PlayerStateRevisions final {
    uint64_t economy = 0;
    uint64_t technology = 0;
    uint64_t production = 0;
    uint64_t energy = 0;
    uint64_t radar = 0;
    uint64_t diplomacy = 0;
    uint64_t progression = 0;
    uint64_t scoring = 0;
    uint64_t tactics = 0;
};

enum class PlayerScoredObjectKind : uint8_t {
    Unit,
    Building,
};

struct PlayerScoreObjectCount final {
    // Destroyed counts are partitioned by victim player like RefCode's
    // m_objectsDestroyed[playerIdx]. Lost counts use the owning player here.
    PlayerId relatedPlayer = INVALID_PLAYER_ID;
    PlayerScoredObjectKind kind = PlayerScoredObjectKind::Unit;
    container::String templateName;
    uint64_t destroyed = 0;
    uint64_t lost = 0;
    uint64_t built = 0;
    uint64_t captured = 0;
};

struct PlayerScoreState final {
    uint64_t moneyEarned = 0;
    uint64_t moneySpent = 0;
    uint64_t unitsBuilt = 0;
    uint64_t buildingsBuilt = 0;
    uint64_t factionBuildingsCaptured = 0;
    uint64_t techBuildingsCaptured = 0;
    container::Array<uint64_t, PLAYER_REGISTRY_CAPACITY> unitsDestroyed{};
    container::Array<uint64_t, PLAYER_REGISTRY_CAPACITY> buildingsDestroyed{};
    uint64_t unitsLost = 0;
    uint64_t buildingsLost = 0;
    // Canonical `(relatedPlayer, kind, ASCII template)` order; no unordered
    // map iteration enters the deterministic digest.
    container::Vector<PlayerScoreObjectCount> objectCounts;
};

struct PlayerAcademyState final {
    bool spentCashBeforeBuildingSupplyCenter = false;
    bool researchedRadar = false;
    bool hadStrategyCenter = false;
    bool choseStrategyCenterBattlePlan = false;
    bool hadTunnelNetwork = false;
    bool builtBarracksWithinFiveMinutes = false;
    bool builtWarFactoryWithinTenMinutes = false;
    bool builtAdvancedTechWithinFifteenMinutes = false;
    bool hadPowerLastSample = false;
    bool hasPowerSample = false;
    uint64_t supplyCentersBuilt = 0;
    uint64_t peonsBuilt = 0;
    uint64_t structuresCaptured = 0;
    uint64_t generalsPointsSpent = 0;
    uint64_t specialPowersUsed = 0;
    uint64_t structuresGarrisoned = 0;
    uint64_t idleBuildingUnitsMaxTicks = 0;
    uint64_t lastUnitBuiltTick = 0;
    uint64_t upgradesPurchased = 0;
    uint64_t powerOutMaxTicks = 0;
    uint64_t powerOutStartedTick = 0;
    uint64_t gatherersBuilt = 0;
    uint64_t heroesBuilt = 0;
    uint64_t unitsEnteredTunnelNetwork = 0;
    uint64_t secondaryIncomeUnitsBuilt = 0;
    uint64_t clearedGarrisonedBuildings = 0;
    uint64_t salvageCollected = 0;
    uint64_t guardAbilityUsed = 0;
    uint64_t lastIncomeTick = 0;
    uint64_t maxTicksBetweenIncome = 0;
    uint64_t minesCreated = 0;
    uint64_t minesCleared = 0;
    uint64_t vehiclesRecovered = 0;
    uint64_t vehiclesSniped = 0;
    uint64_t disguisableVehiclesBuilt = 0;
    uint64_t vehiclesDisguised = 0;
    uint64_t firestormsCreated = 0;
};

// Immutable facts captured from a completed object's KindOf/Contain recipe.
// PlayerRegistry deliberately consumes this value instead of inspecting an
// ECS entity or a mutable ThingTemplate across the player-system boundary.
struct PlayerAcademyProductionFacts final {
    bool supplyCenter = false;
    bool dozer = false;
    bool infantry = false;
    bool vehicle = false;
    bool harvester = false;
    bool hero = false;
    bool strategyCenter = false;
    bool tunnelNetwork = false;
    bool secondaryIncome = false;
    bool barracks = false;
    bool warFactory = false;
    bool advancedTech = false;
    bool disguiser = false;
};

enum class PlayerAcademyEvent : uint8_t {
    BuildingCaptured,
    GeneralsPointsSpent,
    BuildingGarrisoned,
    BattlePlanSelected,
    UnitEnteredTunnelNetwork,
    ClearedGarrisonedBuilding,
    VehicleDisguised,
    FirestormCreated,
    GuardAbilityUsed,
    SalvageCollected,
    MineCleared,
    VehicleRecovered,
    VehicleSniped,
    MineCreated,
};

struct ScenarioRosterApplyReport final {
    size_t materializedPlayerCount = 0;
    size_t reusedCommandPlayerCount = 0;
    size_t relationshipOverrideCount = 0;
    container::Vector<container::String> warnings;
    container::Vector<container::String> errors;

    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

// The original has two distinct diplomacy seeds before applying the directed
// SidesList playerEnemies/playerAllies fields. Campaign/Challenge maps begin
// neutral and opt into every authored relation; skirmish/network matches use
// the resolved lobby alliance groups as their baseline.
enum class ScenarioDiplomacyBaseline : uint8_t {
    AuthoredNeutral,
    PreserveMatchAlliances,
};

// Simulation-authoritative player state.  There is intentionally no `local`
// flag: which player is controlled by this process lives in LocalControlContext
// and is excluded from digest/save/network state.
struct PlayerState final {
    PlayerId id = INVALID_PLAYER_ID;
    MatchPlayerSlotId slot = INVALID_MATCH_PLAYER_SLOT_ID;
    PlayerControllerKind controller = PlayerControllerKind::Human;
    AiDifficulty aiDifficulty = AiDifficulty::None;
    PlayerParticipationKind participation = PlayerParticipationKind::Participant;
    // Frozen PlayerTemplate::PlayableSide.  This is deliberately distinct
    // from simulation participation: campaign Civilian/map owners can be
    // active authoritative players while still being ineligible for score and
    // kill-experience credit in RefCode.
    bool playableSide = false;
    PlayerLifeState life = PlayerLifeState::Setup;
    FactionTemplateId faction = INVALID_FACTION_TEMPLATE_ID;
    MultiplayerColorId color = INVALID_MULTIPLAYER_COLOR_ID;
    AllianceGroupId alliance = INVALID_ALLIANCE_GROUP_ID;
    container::String displayName;
    container::String side;
    container::String baseSide;
    int32_t startPosition = -1;
    int64_t cash = 0;
    // Highest unlocked CashBountyPower percentage. The original value is
    // player-sticky and only increases as sciences/providers become
    // available; Q32.32 keeps kill rewards deterministic.
    math::q32_32 cashBountyPercent{};
    PlayerScienceState sciences;
    PlayerProgressState progress;
    PlayerUpgradeState upgrades;
    PlayerProductionModifierState productionModifiers;
    PlayerConstructionPolicyState constructionPolicy;
    PlayerEnergyState energy;
    PlayerRadarState radar;
    PlayerScoreState score;
    PlayerAcademyState academy;
    // Persistent per-opponent combat history used by skirmish scripts.
    // PLAYER_REGISTRY_CAPACITY is 16, so one stable bitset is sufficient.
    uint32_t attackedByMask = 0;
    // RefCode Player::m_unitsShouldHunt is sticky. PLAYER_HUNT sets it and
    // later Team/Named Hunt orders from this player inherit the policy.
    bool unitsShouldHunt = false;
    // Confirmed counterpart of the local retaliation option. The modern
    // session starts enabled (matching the shipped client default); a future
    // player command may change it only through PlayerRegistry.
    bool logicalRetaliationEnabled = true;
    // Directional Player->Team override. It precedes the ordinary
    // Player->Player matrix but follows the source Team's own overrides.
    container::Vector<PlayerTeamRelationshipOverride>
        teamRelationshipOverrides;
    PlayerStateRevisions revisions;

    [[nodiscard]] bool isNeutral() const noexcept {
        return id.isNeutral() || controller == PlayerControllerKind::Neutral;
    }
    [[nodiscard]] bool isSimulationParticipant() const noexcept {
        return !isNeutral() && participation == PlayerParticipationKind::Participant;
    }
    [[nodiscard]] bool isPlayableSide() const noexcept {
        return playableSide && isSimulationParticipant();
    }
    [[nodiscard]] bool isCommandPlayer() const noexcept {
        return slot && isSimulationParticipant() &&
            (controller == PlayerControllerKind::Human || controller == PlayerControllerKind::Ai);
    }
};

// Command/lobby players carry a MultiplayerColorId. Campaign and other
// map-only players are instantiated directly from a PlayerTemplate instead,
// so their concrete presentation colour comes from PreferredColor, matching
// RefCode Player::init(). Neutral and malformed owners remain white.
[[nodiscard]] PlayerRgbColor resolvePlayerPresentationColor(
    const PlayerState& player,
    const MultiplayerRuleset& ruleset,
    bool night = false) noexcept;

// Modern replacement for RefCode's global PlayerList.  It is session-owned,
// fixed-slot, O(1) by PlayerId and exposes canonical PlayerId order.  It owns
// no ECS entity collection; ownership indexes begin only with the later ECS
// lifecycle phase.
class PlayerRegistry final {
public:
    bool initialize(const ResolvedMatchSetup& setup, const MultiplayerRuleset& ruleset,
                    uint64_t expectedSimulationContentFingerprint,
                    LocalControlContext localControl, container::String* error = nullptr);
    // Applies immutable map SidesList data after command/lobby resolution but
    // before any map object is created. It materializes scenario-only AI and
    // civilian players into the remaining 16-player namespace, preserves an
    // already resolved local command seat where aliases/factions match, and
    // re-seeds directed diplomacy according to the explicit launch policy
    // before applying authored overrides.
    [[nodiscard]] bool applyScenarioDefinition(const scenario::ScenarioDefinition& scenario,
                                               const MultiplayerRuleset& ruleset,
                                               ScenarioDiplomacyBaseline diplomacyBaseline,
                                               ScenarioRosterApplyReport* report = nullptr);
    void reset() noexcept;

    [[nodiscard]] const PlayerState* get(PlayerId id) const noexcept;
    [[nodiscard]] const PlayerState* neutralPlayer() const noexcept { return get(NEUTRAL_PLAYER_ID); }
    [[nodiscard]] const PlayerState* localPlayer() const noexcept;
    [[nodiscard]] PlayerId localPlayerId() const noexcept;
    [[nodiscard]] container::Span<const PlayerId> activePlayerIds() const noexcept { return m_activePlayerIds; }
    [[nodiscard]] size_t playerCount() const noexcept { return m_activePlayerIds.size(); }

    [[nodiscard]] const PlayerRelationshipMatrix& relationships() const noexcept {
        return m_relationships;
    }
    [[nodiscard]] PlayerRelationship relationship(PlayerId from, PlayerId to) const noexcept {
        return m_relationships.get(from, to);
    }
    [[nodiscard]] bool setRelationship(PlayerId from, PlayerId to,
                                       PlayerRelationship relationship) noexcept;
    [[nodiscard]] bool setTeamRelationshipOverride(
        PlayerId from, ObjectTeamId to,
        PlayerRelationship relationship);
    [[nodiscard]] bool removeTeamRelationshipOverride(
        PlayerId from, ObjectTeamId to);
    [[nodiscard]] std::optional<PlayerRelationship>
    teamRelationshipOverride(PlayerId from, ObjectTeamId to) const noexcept;

    // Script actions set/adjust money permissively, matching RefCode's
    // Money::withdraw/deposit path: negative balances clamp to zero instead
    // of turning a valid authored script into a rejected transaction.
    [[nodiscard]] bool setCash(PlayerId player, int64_t amount) noexcept;
    [[nodiscard]] bool adjustCash(PlayerId player, int64_t delta) noexcept;
    [[nodiscard]] bool deposit(PlayerId player, int64_t amount) noexcept;
    [[nodiscard]] bool trySpend(PlayerId player, int64_t amount) noexcept;
    [[nodiscard]] bool raiseCashBountyPercent(
        PlayerId player, math::q32_32 percentage) noexcept;
    // ObjectEnergySystem is the sole normal writer of production/consumption.
    // Keeping the mutation here ensures energy participates in the canonical
    // PlayerRegistry digest and future script/Power systems never own a
    // parallel mutable player aggregate.
    [[nodiscard]] bool setEnergyTotals(PlayerId player, int32_t production,
                                       int32_t consumption) noexcept;
    [[nodiscard]] bool setRadarProviderTotals(
        PlayerId player, uint32_t providers,
        uint32_t disableProofProviders) noexcept;
    [[nodiscard]] bool setPowerSabotagedUntil(PlayerId player,
                                              uint64_t confirmedTick) noexcept;
    [[nodiscard]] bool clearExpiredPowerSabotage(uint64_t confirmedTick) noexcept;
    [[nodiscard]] bool setScienceAvailability(PlayerId player, container::String science,
                                              ScienceAvailability availability);
    // These small mutations are the common foundation for script-driven
    // rank/science actions. Science purchase accepts an immutable definition
    // supplied by the frozen content snapshot; PlayerRegistry owns the
    // atomic player-state transaction but never owns a mutable string table.
    void initializeRankProgression(
        const RankInfoCatalog& ranks, int32_t rankLevelLimit);
    [[nodiscard]] bool addSkillPoints(
        PlayerId player, int32_t delta, const RankInfoCatalog& ranks,
        int32_t rankLevelLimit);
    [[nodiscard]] bool setRankLevel(PlayerId player, int32_t level,
                                    int32_t rankLevelLimit,
                                    const RankInfoCatalog& ranks);
    [[nodiscard]] bool setSelectedSkillset(PlayerId player, int32_t zeroBasedSkillset) noexcept;
    [[nodiscard]] bool setSkillPointMultiplier(
        PlayerId player, math::q32_32 multiplier) noexcept;
    [[nodiscard]] bool setListedInScoreScreen(PlayerId player, bool listed) noexcept;
    void setScoreAccumulationEnabled(bool enabled) noexcept {
        m_scoreAccumulationEnabled = enabled;
    }
    [[nodiscard]] bool scoreAccumulationEnabled() const noexcept {
        return m_scoreAccumulationEnabled;
    }
    [[nodiscard]] bool recordObjectLost(
        PlayerId player, container::String templateName,
        PlayerScoredObjectKind kind);
    [[nodiscard]] bool recordObjectDestroyed(
        PlayerId player, PlayerId victimPlayer,
        container::String templateName, PlayerScoredObjectKind kind);
    [[nodiscard]] bool recordMoneyEarned(
        PlayerId player, uint64_t amount,
        uint64_t confirmedTick) noexcept;
    [[nodiscard]] bool recordMoneySpent(
        PlayerId player, uint64_t amount) noexcept;
    [[nodiscard]] bool recordObjectBuilt(
        PlayerId player, container::String templateName,
        PlayerScoredObjectKind kind);
    [[nodiscard]] bool removeObjectBuilt(
        PlayerId player, container::StringView templateName,
        PlayerScoredObjectKind kind) noexcept;
    [[nodiscard]] bool recordObjectCaptured(
        PlayerId player, container::String templateName,
        bool factionBuilding);
    [[nodiscard]] bool recordAcademyProduction(
        PlayerId player, const PlayerAcademyProductionFacts& facts,
        uint64_t confirmedTick, uint32_t logicFramesPerSecond) noexcept;
    [[nodiscard]] bool recordAcademyUpgrade(
        PlayerId player, bool radarClassified, bool granted) noexcept;
    [[nodiscard]] bool recordAcademySpecialPower(
        PlayerId player, bool superpowerClassified) noexcept;
    [[nodiscard]] bool recordAcademyIncome(
        PlayerId player, uint64_t confirmedTick) noexcept;
    [[nodiscard]] bool recordAcademyEvent(
        PlayerId player, PlayerAcademyEvent event,
        uint64_t amount = 1) noexcept;
    [[nodiscard]] bool updateAcademyPeriodicState(
        PlayerId player, uint64_t confirmedTick,
        uint64_t supplyCenterCost) noexcept;
    [[nodiscard]] bool recordClearedGarrisonedBuilding(PlayerId player) noexcept;
    [[nodiscard]] bool setLifeState(PlayerId player, PlayerLifeState life) noexcept;
    [[nodiscard]] bool setBaseConstructionEnabled(PlayerId player, bool enabled) noexcept;
    [[nodiscard]] bool setUnitConstructionEnabled(PlayerId player, bool enabled) noexcept;
    [[nodiscard]] bool setTeamDelaySeconds(PlayerId player, int32_t seconds) noexcept;
    [[nodiscard]] bool grantScience(PlayerId player, container::String science);
    [[nodiscard]] bool canPurchaseScience(PlayerId player,
                                          const ScienceDefinition& science) const;
    [[nodiscard]] bool tryPurchaseScience(PlayerId player,
                                          const ScienceDefinition& science);
    // Compatibility helper for catalog-neutral callers/tests. Production
    // script effects must use the immutable ScienceDefinition overload above
    // so prerequisite and catalog availability rules cannot be bypassed.
    [[nodiscard]] bool tryPurchaseScience(PlayerId player, container::String science,
                                          int32_t purchaseCost);
    // Consumes one exact-science acquisition notification, matching
    // ScriptEngine::isScienceAcquired(..., removeFromList = TRUE). It does
    // not answer whether the player durably knows the science.
    [[nodiscard]] bool consumeScienceAcquired(PlayerId player,
                                               container::StringView science) noexcept;
    // A production command validates the frozen CommandButton science
    // requirements at its authoritative ingress.  Keep that lookup in the
    // player authority rather than exposing the sorted science vector to a
    // factory system.
    [[nodiscard]] bool hasScience(PlayerId player, container::StringView science) const noexcept;
    // Hot path: ContentId / mask bit. Name overloads resolve through catalog
    // once then forward (script bridges, INI tokens).
    [[nodiscard]] bool hasUpgradeComplete(PlayerId player,
                                          UpgradeContentId upgrade) const noexcept;
    [[nodiscard]] bool hasUpgradeInProgress(PlayerId player,
                                            UpgradeContentId upgrade) const noexcept;
    [[nodiscard]] bool hasUpgradeComplete(
        PlayerId player, container::StringView upgrade,
        const UpgradeCatalog& catalog) const noexcept;
    [[nodiscard]] bool hasUpgradeInProgress(
        PlayerId player, container::StringView upgrade,
        const UpgradeCatalog& catalog) const noexcept;
    [[nodiscard]] bool markAttackedBy(PlayerId victim, PlayerId attacker) noexcept;
    [[nodiscard]] bool wasAttackedBy(PlayerId victim, PlayerId attacker) const noexcept;
    [[nodiscard]] bool setUnitsShouldHunt(PlayerId player, bool enabled) noexcept;
    [[nodiscard]] bool setLogicalRetaliationEnabled(
        PlayerId player, bool enabled) noexcept;
    // Script/direct grants remain permissive: a confirmed queue uses the
    // stricter begin/cancel/commit trio below so an arbitrary complete action
    // cannot masquerade as a valid production acknowledgement.
    [[nodiscard]] bool markUpgradeComplete(PlayerId player, UpgradeContentId upgrade);
    [[nodiscard]] bool markUpgradeInProgress(PlayerId player, UpgradeContentId upgrade);
    [[nodiscard]] bool markUpgradeComplete(PlayerId player, container::StringView upgrade,
                                           const UpgradeCatalog& catalog);
    [[nodiscard]] bool markUpgradeInProgress(PlayerId player, container::StringView upgrade,
                                             const UpgradeCatalog& catalog);
    // Queue admission rejects both completed and already reserved technology.
    [[nodiscard]] bool beginQueuedPlayerUpgrade(PlayerId player, UpgradeContentId upgrade);
    [[nodiscard]] bool cancelQueuedPlayerUpgrade(PlayerId player,
                                                  UpgradeContentId upgrade) noexcept;
    [[nodiscard]] bool commitQueuedPlayerUpgrade(PlayerId player, UpgradeContentId upgrade);
    [[nodiscard]] bool beginQueuedPlayerUpgrade(PlayerId player, container::StringView upgrade,
                                                const UpgradeCatalog& catalog);
    [[nodiscard]] bool cancelQueuedPlayerUpgrade(PlayerId player, container::StringView upgrade,
                                                 const UpgradeCatalog& catalog) noexcept;
    [[nodiscard]] bool commitQueuedPlayerUpgrade(PlayerId player, container::StringView upgrade,
                                                 const UpgradeCatalog& catalog);

    // Canonical simulation state used by lockstep checksums. Local control,
    // display names, player color and other presentation projections are
    // intentionally excluded. contentDigest is available for replay/lobby
    // diagnostics that do need the full visible roster.
    [[nodiscard]] uint64_t simulationDigest() const noexcept;
    [[nodiscard]] uint64_t contentDigest() const noexcept;
    [[nodiscard]] uint64_t digest() const noexcept { return simulationDigest(); }
    [[nodiscard]] uint64_t resolvedSetupSimulationDigest() const noexcept {
        return m_resolvedSetupDigest;
    }
    [[nodiscard]] uint64_t resolvedSetupContentDigest() const noexcept {
        return m_resolvedSetupContentDigest;
    }
    // Compatibility alias for callers that previously meant the simulation
    // descriptor, never a presentation roster checksum.
    [[nodiscard]] uint64_t resolvedSetupDigest() const noexcept {
        return resolvedSetupSimulationDigest();
    }
    [[nodiscard]] uint64_t playerRulesetFingerprint() const noexcept {
        return m_playerRulesetFingerprint;
    }
    [[nodiscard]] uint64_t simulationContentFingerprint() const noexcept {
        return m_simulationContentFingerprint;
    }

private:
    [[nodiscard]] static bool canonicalInsert(container::Vector<container::String>& values, container::String value);
    [[nodiscard]] static bool canonicalErase(container::Vector<container::String>& values, container::StringView value);
    [[nodiscard]] PlayerState* getMutable(PlayerId id) noexcept;
    [[nodiscard]] uint64_t digestImpl(bool includePresentation) const noexcept;
    void bumpDiplomacyRevision(PlayerId player) noexcept;
    void rebuildActivePlayerIds() noexcept;

    container::Array<std::optional<PlayerState>, PLAYER_REGISTRY_CAPACITY> m_players;
    container::Vector<PlayerId> m_activePlayerIds;
    PlayerRelationshipMatrix m_relationships;
    LocalControlContext m_localControl;
    uint64_t m_resolvedSetupDigest = 0;
    uint64_t m_resolvedSetupContentDigest = 0;
    uint64_t m_playerRulesetFingerprint = 0;
    uint64_t m_simulationContentFingerprint = 0;
    bool m_scoreAccumulationEnabled = true;
};

} // namespace engine
