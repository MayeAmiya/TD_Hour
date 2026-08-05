#include "core/container/container_types.h"
#include "PlayerRegistry.h"

#include "game/data/base/ScienceCatalog.h"
#include "game/data/base/RankInfoCatalog.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace engine {
namespace {

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

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
    void u64(uint64_t value) noexcept {
        for (uint32_t shift = 0; shift < 64; shift += 8) {
            byte(static_cast<uint8_t>((value >> shift) & 0xffu));
        }
    }
    void i32(int32_t value) noexcept { u32(static_cast<uint32_t>(value)); }
    void i64(int64_t value) noexcept { u64(static_cast<uint64_t>(value)); }
    void string(container::StringView value) noexcept {
        u32(static_cast<uint32_t>(value.size()));
        for (const unsigned char character : value) byte(character);
    }
    [[nodiscard]] uint64_t finish() const noexcept { return m_value; }

private:
    uint64_t m_value = kFnvOffsetBasis;
};

template <typename T>
void hashStringVector(DigestWriter& writer, const container::Vector<T>& values) {
    writer.u32(static_cast<uint32_t>(values.size()));
    for (const T& value : values) writer.string(value);
}

void hashPendingScienceAcquisitions(
    DigestWriter& writer, const container::Vector<PendingScienceAcquisition>& acquisitions) {
    writer.u32(static_cast<uint32_t>(acquisitions.size()));
    for (const PendingScienceAcquisition& acquisition : acquisitions) {
        writer.string(acquisition.science);
        writer.u32(acquisition.count);
    }
}

void hashPercentModifiers(DigestWriter& writer,
                          const container::Vector<ProductionPercentModifier>& modifiers) {
    writer.u32(static_cast<uint32_t>(modifiers.size()));
    for (const ProductionPercentModifier& modifier : modifiers) {
        writer.string(modifier.thingTemplateName);
        writer.i32(modifier.multiplierBasisPoints);
    }
}

void hashVeterancyModifiers(DigestWriter& writer,
                            const container::Vector<ProductionVeterancyModifier>& modifiers) {
    writer.u32(static_cast<uint32_t>(modifiers.size()));
    for (const ProductionVeterancyModifier& modifier : modifiers) {
        writer.string(modifier.thingTemplateName);
        writer.string(modifier.veterancyName);
    }
}

[[nodiscard]] int64_t saturatedAdd(int64_t value, int64_t delta) noexcept {
    if (delta > 0 && value > std::numeric_limits<int64_t>::max() - delta) {
        return std::numeric_limits<int64_t>::max();
    }
    if (delta < 0 && value < std::numeric_limits<int64_t>::min() - delta) {
        return std::numeric_limits<int64_t>::min();
    }
    return value + delta;
}

[[nodiscard]] int32_t clampRankLevel(int32_t level, int32_t limit) noexcept {
    const int32_t effectiveLimit = std::max(1, limit);
    return std::clamp(level, 1, effectiveLimit);
}

void copyAndNormalizeScienceNames(container::Vector<container::String>& destination,
                                  const container::Vector<container::String>& source) {
    destination = source;
    // ScienceType is a case-sensitive legacy NameKey. Do not fold authored
    // identifiers while materializing a faction's intrinsic science list.
    std::sort(destination.begin(), destination.end());
    destination.erase(std::unique(destination.begin(), destination.end()), destination.end());
}

[[nodiscard]] bool scienceContains(const container::Vector<container::String>& values,
                                   container::StringView value) {
    const auto found = std::lower_bound(values.begin(), values.end(), value,
        [](const container::String& left, container::StringView right) {
            return container::StringView{left} < right;
        });
    return found != values.end() && container::StringView{*found} == value;
}

[[nodiscard]] bool scienceInsert(container::Vector<container::String>& values, container::String value) {
    const auto found = std::lower_bound(values.begin(), values.end(), container::StringView{value},
        [](const container::String& left, container::StringView right) {
            return container::StringView{left} < right;
        });
    if (found != values.end() && *found == value) return false;
    values.insert(found, std::move(value));
    return true;
}

[[nodiscard]] bool scienceErase(container::Vector<container::String>& values, container::StringView value) {
    const auto found = std::lower_bound(values.begin(), values.end(), value,
        [](const container::String& left, container::StringView right) {
            return container::StringView{left} < right;
        });
    if (found == values.end() || container::StringView{*found} != value) return false;
    values.erase(found);
    return true;
}

[[nodiscard]] bool upgradeContains(const container::Vector<container::String>& values,
                                   container::StringView value) noexcept {
    const auto found = std::lower_bound(values.begin(), values.end(), value,
        [](const container::String& left, container::StringView right) {
            return container::StringView{left} < right;
        });
    return found != values.end() && container::StringView{*found} == value;
}

void recordScienceAcquisition(PlayerScienceState& sciences, container::StringView science) {
    const auto found = std::lower_bound(sciences.pendingAcquisitions.begin(),
                                        sciences.pendingAcquisitions.end(), science,
        [](const PendingScienceAcquisition& left, container::StringView right) {
            return container::StringView{left.science} < right;
        });
    if (found != sciences.pendingAcquisitions.end() &&
        container::StringView{found->science} == science) {
        if (found->count != std::numeric_limits<uint32_t>::max()) ++found->count;
        return;
    }
    sciences.pendingAcquisitions.insert(found, {
        .science = container::String{science},
        .count = 1,
    });
}


} // namespace

PlayerRgbColor resolvePlayerPresentationColor(
    const PlayerState& player,
    const MultiplayerRuleset& ruleset,
    bool night) noexcept {
    if (const MultiplayerColorDefinition* multiplayerColor =
            ruleset.findColor(player.color)) {
        return night ? multiplayerColor->night : multiplayerColor->day;
    }
    if (const FactionTemplate* faction = ruleset.findFaction(player.faction)) {
        // RefCode initializes both Player::m_color and m_nightColor from the
        // PlayerTemplate preferred colour before an optional lobby/map
        // override is applied.
        return faction->simulation.preferredColor;
    }
    return {255u, 255u, 255u};
}

bool PlayerRegistry::initialize(const ResolvedMatchSetup& setup, const MultiplayerRuleset& ruleset,
                                uint64_t expectedSimulationContentFingerprint,
                                LocalControlContext localControl, container::String* error) {
    if (error) error->clear();
    reset();
    const ResolvedMatchSetupValidation validation = validateResolvedMatchSetup(
        setup, &ruleset, expectedSimulationContentFingerprint);
    if (!validation.ok()) {
        if (error) *error = validation.issues.front().message;
        return false;
    }

    container::Vector<PlayerAllianceAssignment> alliances;
    alliances.reserve(setup.players.size());
    for (const ResolvedPlayerSetup& source : setup.players) {
        PlayerState player;
        player.id = source.player;
        player.slot = source.slot;
        player.controller = source.controller;
        player.aiDifficulty = source.aiDifficulty;
        player.participation = source.participation;
        player.life = source.participation == PlayerParticipationKind::Observer
            ? PlayerLifeState::Observer
            : PlayerLifeState::Active;
        player.displayName = source.displayName;
        if (source.participation == PlayerParticipationKind::Participant) {
            // The shared descriptor validation above establishes all roster
            // invariants.  This layer now only materializes its own runtime
            // state and keeps no second, drifting validation implementation.
            const FactionTemplate* faction = ruleset.findFaction(source.faction);
            if (!faction) {
                if (error) *error = "validated faction disappeared while creating the player registry";
                reset();
                return false;
            }
            player.faction = source.faction;
            player.playableSide = faction->playable && !faction->observer;
            player.color = source.color;
            player.alliance = source.alliance;
            player.side = faction->side;
            player.baseSide = faction->baseSide;
            player.startPosition = source.startPosition;
            player.cash = source.startingMoney;
            copyAndNormalizeScienceNames(
                player.sciences.intrinsicKnown,
                faction->simulation.intrinsicSciences);
            player.sciences.known = player.sciences.intrinsicKnown;
            player.sciences.intrinsicPurchasePoints =
                faction->simulation.intrinsicSciencePurchasePoints;
            player.sciences.purchasePoints =
                player.sciences.intrinsicPurchasePoints;
            player.productionModifiers.cost = faction->simulation.productionCostModifiers;
            player.productionModifiers.time = faction->simulation.productionTimeModifiers;
            player.productionModifiers.veterancy = faction->simulation.productionVeterancyModifiers;
            alliances.push_back({.player = source.player, .alliance = source.alliance});
        }

        m_players[source.player.value] = std::move(player);
        m_activePlayerIds.push_back(source.player);
    }

    // Neutral has a fixed non-command ID and exists even on a headless or
    // empty setup.  It is a normal registry entry rather than a special vector
    // position, eliminating the current sparse-slot collision class.
    PlayerState neutral;
    neutral.id = NEUTRAL_PLAYER_ID;
    neutral.controller = PlayerControllerKind::Neutral;
    neutral.life = PlayerLifeState::Active;
    neutral.displayName = "Neutral";
    neutral.side = "Neutral";
    neutral.baseSide = "Neutral";
    m_players[NEUTRAL_PLAYER_ID.value] = std::move(neutral);
    m_activePlayerIds.push_back(NEUTRAL_PLAYER_ID);
    std::sort(m_activePlayerIds.begin(), m_activePlayerIds.end());

    m_relationships.initializeDefaults(alliances);
    m_localControl = localControl;
    if (m_localControl.controlledSlot) {
        const PlayerId local = PlayerId::fromSlot(m_localControl.controlledSlot);
        if (!get(local)) {
            if (error) *error = "local control slot is not an active player in the resolved match";
            reset();
            return false;
        }
    }
    m_resolvedSetupDigest = setup.simulationDigest();
    m_resolvedSetupContentDigest = setup.contentDigest();
    m_playerRulesetFingerprint = setup.playerRulesetFingerprint;
    m_simulationContentFingerprint = setup.simulationContentFingerprint;
    return true;
}


void PlayerRegistry::reset() noexcept {
    for (auto& player : m_players) player.reset();
    m_activePlayerIds.clear();
    m_relationships.reset();
    m_localControl = {};
    m_resolvedSetupDigest = 0;
    m_resolvedSetupContentDigest = 0;
    m_playerRulesetFingerprint = 0;
    m_simulationContentFingerprint = 0;
    m_scoreAccumulationEnabled = true;
}

void PlayerRegistry::rebuildActivePlayerIds() noexcept {
    m_activePlayerIds.clear();
    for (uint8_t value = 0; value < static_cast<uint8_t>(PLAYER_REGISTRY_CAPACITY); ++value) {
        if (m_players[value]) m_activePlayerIds.push_back(PlayerId{value});
    }
}

const PlayerState* PlayerRegistry::get(PlayerId id) const noexcept {
    if (!id.isValid()) return nullptr;
    const std::optional<PlayerState>& result = m_players[id.value];
    return result ? &*result : nullptr;
}

PlayerState* PlayerRegistry::getMutable(PlayerId id) noexcept {
    if (!id.isValid()) return nullptr;
    std::optional<PlayerState>& result = m_players[id.value];
    return result ? &*result : nullptr;
}

const PlayerState* PlayerRegistry::localPlayer() const noexcept {
    return get(localPlayerId());
}

PlayerId PlayerRegistry::localPlayerId() const noexcept {
    return m_localControl.controlledSlot
        ? PlayerId::fromSlot(m_localControl.controlledSlot)
        : INVALID_PLAYER_ID;
}

bool PlayerRegistry::setRelationship(PlayerId from, PlayerId to,
                                     PlayerRelationship relationship) noexcept {
    if (!get(from) || !get(to)) return false;
    if (!m_relationships.set(from, to, relationship)) return false;
    bumpDiplomacyRevision(from);
    return true;
}

bool PlayerRegistry::setTeamRelationshipOverride(
    PlayerId from, ObjectTeamId to,
    PlayerRelationship relationship) {
    PlayerState* source = getMutable(from);
    if (!source || !to) return false;
    auto position = std::lower_bound(
        source->teamRelationshipOverrides.begin(),
        source->teamRelationshipOverrides.end(), to,
        [](const PlayerTeamRelationshipOverride& entry,
           ObjectTeamId value) { return entry.target < value; });
    if (position != source->teamRelationshipOverrides.end() &&
        position->target == to) {
        if (position->relationship == relationship) return false;
        position->relationship = relationship;
    } else {
        source->teamRelationshipOverrides.insert(position, {
            .target = to,
            .relationship = relationship,
        });
    }
    ++source->revisions.diplomacy;
    return true;
}

bool PlayerRegistry::removeTeamRelationshipOverride(
    PlayerId from, ObjectTeamId to) {
    PlayerState* source = getMutable(from);
    if (!source || !to) return false;
    const auto position = std::lower_bound(
        source->teamRelationshipOverrides.begin(),
        source->teamRelationshipOverrides.end(), to,
        [](const PlayerTeamRelationshipOverride& entry,
           ObjectTeamId value) { return entry.target < value; });
    if (position == source->teamRelationshipOverrides.end() ||
        position->target != to) return false;
    source->teamRelationshipOverrides.erase(position);
    ++source->revisions.diplomacy;
    return true;
}

std::optional<PlayerRelationship>
PlayerRegistry::teamRelationshipOverride(
    PlayerId from, ObjectTeamId to) const noexcept {
    const PlayerState* source = get(from);
    if (!source || !to) return std::nullopt;
    const auto position = std::lower_bound(
        source->teamRelationshipOverrides.begin(),
        source->teamRelationshipOverrides.end(), to,
        [](const PlayerTeamRelationshipOverride& entry,
           ObjectTeamId value) { return entry.target < value; });
    return position != source->teamRelationshipOverrides.end() &&
            position->target == to
        ? std::optional<PlayerRelationship>{position->relationship}
        : std::nullopt;
}

bool PlayerRegistry::setCash(PlayerId player, int64_t amount) noexcept {
    PlayerState* target = getMutable(player);
    if (!target) return false;

    const int64_t clamped = std::max<int64_t>(amount, 0);
    if (target->cash != clamped) {
        target->cash = clamped;
        ++target->revisions.economy;
    }
    return true;
}

bool PlayerRegistry::adjustCash(PlayerId player, int64_t delta) noexcept {
    PlayerState* target = getMutable(player);
    if (!target) return false;

    int64_t adjusted = target->cash;
    if (delta > 0) {
        adjusted = target->cash > std::numeric_limits<int64_t>::max() - delta
            ? std::numeric_limits<int64_t>::max()
            : target->cash + delta;
    } else if (delta < 0) {
        // Avoid negating INT64_MIN: every negative delta whose magnitude is
        // greater than the current balance has the same legacy result, zero.
        adjusted = delta < -target->cash ? 0 : target->cash + delta;
    }
    if (target->cash != adjusted) {
        target->cash = adjusted;
        ++target->revisions.economy;
    }
    return true;
}

bool PlayerRegistry::deposit(PlayerId player, int64_t amount) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || amount <= 0 || target->cash > std::numeric_limits<int64_t>::max() - amount) {
        return false;
    }
    target->cash += amount;
    ++target->revisions.economy;
    return true;
}

bool PlayerRegistry::trySpend(PlayerId player, int64_t amount) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || amount <= 0 || target->cash < amount) return false;
    target->cash -= amount;
    ++target->revisions.economy;
    return true;
}

bool PlayerRegistry::raiseCashBountyPercent(
    PlayerId player, math::q32_32 percentage) noexcept {
    PlayerState* target = getMutable(player);
    if (!target) return false;
    percentage = math::q32_32::max(math::q32_32{}, percentage);
    if (percentage <= target->cashBountyPercent) return false;
    target->cashBountyPercent = percentage;
    ++target->revisions.economy;
    return true;
}

bool PlayerRegistry::setEnergyTotals(PlayerId player, int32_t production,
                                     int32_t consumption) noexcept {
    PlayerState* target = getMutable(player);
    if (!target) return false;
    production = std::max(production, 0);
    consumption = std::max(consumption, 0);
    if (target->energy.production == production && target->energy.consumption == consumption) {
        return false;
    }
    target->energy.production = production;
    target->energy.consumption = consumption;
    ++target->revisions.energy;
    return true;
}

bool PlayerRegistry::setRadarProviderTotals(
    PlayerId player, uint32_t providers,
    uint32_t disableProofProviders) noexcept {
    PlayerState* target = getMutable(player);
    if (!target) return false;
    disableProofProviders = std::min(disableProofProviders, providers);
    if (target->radar.providerCount == providers &&
        target->radar.disableProofProviderCount ==
            disableProofProviders) {
        return false;
    }
    target->radar.providerCount = providers;
    target->radar.disableProofProviderCount = disableProofProviders;
    ++target->revisions.radar;
    if (target->revisions.radar == 0) ++target->revisions.radar;
    return true;
}

bool PlayerRegistry::setPowerSabotagedUntil(PlayerId player,
                                            uint64_t confirmedTick) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || target->energy.sabotagedUntilTick == confirmedTick) return false;
    target->energy.sabotagedUntilTick = confirmedTick;
    ++target->revisions.energy;
    return true;
}

bool PlayerRegistry::clearExpiredPowerSabotage(uint64_t confirmedTick) noexcept {
    bool changed = false;
    for (const PlayerId id : m_activePlayerIds) {
        PlayerState* target = getMutable(id);
        if (!target || target->energy.sabotagedUntilTick == 0 ||
            confirmedTick <= target->energy.sabotagedUntilTick) {
            continue;
        }
        target->energy.sabotagedUntilTick = 0;
        ++target->revisions.energy;
        changed = true;
    }
    return changed;
}

bool PlayerRegistry::setScienceAvailability(PlayerId player, container::String science,
                                            ScienceAvailability availability) {
    PlayerState* target = getMutable(player);
    if (!target || science.empty()) return false;
    bool changed = false;
    switch (availability) {
    case ScienceAvailability::Available:
        changed |= scienceErase(target->sciences.disabled, science);
        changed |= scienceErase(target->sciences.hidden, science);
        break;
    case ScienceAvailability::Disabled:
        changed |= scienceErase(target->sciences.hidden, science);
        changed |= scienceInsert(target->sciences.disabled, std::move(science));
        break;
    case ScienceAvailability::Hidden:
        changed |= scienceErase(target->sciences.disabled, science);
        changed |= scienceInsert(target->sciences.hidden, std::move(science));
        break;
    }
    if (changed) ++target->revisions.technology;
    return changed;
}

void PlayerRegistry::initializeRankProgression(
    const RankInfoCatalog& ranks, int32_t rankLevelLimit) {
    if (!ranks.isLoaded() || ranks.size() == 0u) return;
    for (const PlayerId player : m_activePlayerIds) {
        PlayerState* target = getMutable(player);
        if (!target || !target->isSimulationParticipant()) continue;
        target->progress.skillPoints = 0;
        target->progress.rankLevel = 0;
        target->sciences.known = target->sciences.intrinsicKnown;
        target->sciences.purchasePoints =
            target->sciences.intrinsicPurchasePoints;
        target->sciences.pendingAcquisitions.clear();
        for (const container::String& science :
             target->sciences.intrinsicKnown) {
            recordScienceAcquisition(target->sciences, science);
        }
        static_cast<void>(setRankLevel(
            player, 1, rankLevelLimit, ranks));
    }
}

bool PlayerRegistry::addSkillPoints(
    PlayerId player, int32_t delta, const RankInfoCatalog& ranks,
    int32_t rankLevelLimit) {
    PlayerState* target = getMutable(player);
    if (!target || delta == 0 || !ranks.isLoaded() || ranks.size() == 0u) {
        return false;
    }

    const math::q32_32 multiplier = math::q32_32::max(
        math::q32_32{}, target->progress.skillPointMultiplier);
    const int64_t scaledRaw =
        (math::q32_32{delta} * multiplier).raw();
    constexpr uint64_t kFractionMask = (uint64_t{1} << 32u) - 1u;
    int64_t adjusted = 0;
    if (scaledRaw >= 0) {
        adjusted = scaledRaw >> 32u;
        if ((static_cast<uint64_t>(scaledRaw) & kFractionMask) != 0u)
            ++adjusted;
    } else {
        const uint64_t magnitude = static_cast<uint64_t>(-(scaledRaw + 1)) + 1u;
        adjusted = -static_cast<int64_t>(magnitude >> 32u);
    }
    if (adjusted == 0) return false;

    const uint32_t maximumRank = static_cast<uint32_t>(std::min<size_t>(
        ranks.size(), static_cast<size_t>(std::max(1, rankLevelLimit))));
    const RankInfoDefinition* cap = ranks.find(maximumRank);
    if (!cap) return false;
    const int64_t pointCap = std::max<int64_t>(0, cap->skillPointsNeeded);
    // ZH caps points at the threshold of the highest permitted rank. Keep the
    // existing non-negative guard for malformed negative script deltas.
    const int64_t next = std::clamp<int64_t>(
        saturatedAdd(target->progress.skillPoints, adjusted), 0, pointCap);
    if (target->progress.skillPoints == next) return false;
    target->progress.skillPoints = next;
    ++target->revisions.progression;

    while (target->progress.rankLevel <
           static_cast<int32_t>(maximumRank)) {
        const uint32_t nextLevel = static_cast<uint32_t>(
            target->progress.rankLevel + 1);
        const RankInfoDefinition* nextRank = ranks.find(nextLevel);
        if (!nextRank || target->progress.skillPoints <
                nextRank->skillPointsNeeded) {
            break;
        }
        if (!setRankLevel(
                player, static_cast<int32_t>(nextLevel), rankLevelLimit,
                ranks)) {
            break;
        }
    }
    return true;
}

bool PlayerRegistry::setRankLevel(PlayerId player, int32_t level,
                                  int32_t rankLevelLimit,
                                  const RankInfoCatalog& ranks) {
    PlayerState* target = getMutable(player);
    if (!target || !ranks.isLoaded() || ranks.size() == 0u) return false;
    const int32_t catalogLimit = static_cast<int32_t>(std::min<size_t>(
        ranks.size(), static_cast<size_t>(std::numeric_limits<int32_t>::max())));
    const int32_t normalized = clampRankLevel(
        level, std::min(rankLevelLimit, catalogLimit));
    if (target->progress.rankLevel == normalized) return false;

    const int64_t previousSkillPoints = target->progress.skillPoints;
    const int32_t previousRankLevel = target->progress.rankLevel;
    const container::Vector<container::String> previousSciences =
        target->sciences.known;
    const int32_t previousPurchasePoints =
        target->sciences.purchasePoints;

    if (normalized < target->progress.rankLevel) {
        // Player::setRankLevel resets all purchased/granted sciences and
        // generals points before replaying the requested rank from level 1.
        target->progress.skillPoints = 0;
        target->progress.rankLevel = 0;
        target->sciences.known = target->sciences.intrinsicKnown;
        target->sciences.purchasePoints =
            target->sciences.intrinsicPurchasePoints;
        for (const container::String& science :
             target->sciences.intrinsicKnown) {
            recordScienceAcquisition(target->sciences, science);
        }
    }

    for (int32_t current = target->progress.rankLevel + 1;
         current <= normalized; ++current) {
        const RankInfoDefinition* rank = ranks.find(
            static_cast<uint32_t>(current));
        if (!rank) break;
        target->sciences.purchasePoints = static_cast<int32_t>(
            std::min<int64_t>(
                static_cast<int64_t>(std::numeric_limits<int32_t>::max()),
                static_cast<int64_t>(target->sciences.purchasePoints) +
                    rank->sciencePurchasePointsGranted));
        target->progress.skillPoints = std::max<int64_t>(
            target->progress.skillPoints, rank->skillPointsNeeded);
        for (const container::String& science : rank->sciencesGranted) {
            if (science.empty() ||
                !scienceInsert(target->sciences.known, science)) {
                continue;
            }
            recordScienceAcquisition(target->sciences, science);
        }
        target->progress.rankLevel = current;
    }

    if (target->progress.skillPoints != previousSkillPoints ||
        target->progress.rankLevel != previousRankLevel) {
        ++target->revisions.progression;
    }
    if (target->sciences.known != previousSciences ||
        target->sciences.purchasePoints != previousPurchasePoints) {
        ++target->revisions.technology;
    }
    return true;
}

bool PlayerRegistry::setSelectedSkillset(PlayerId player, int32_t zeroBasedSkillset) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || zeroBasedSkillset < -1) return false;
    if (target->progress.selectedSkillset == zeroBasedSkillset) return false;
    target->progress.selectedSkillset = zeroBasedSkillset;
    ++target->revisions.progression;
    return true;
}

bool PlayerRegistry::setSkillPointMultiplier(
    PlayerId player, math::q32_32 multiplier) noexcept {
    PlayerState* target = getMutable(player);
    if (!target) return false;
    // A negative experience multiplier is neither useful nor safe across
    // later rank-table logic. Preserve normal 0 = no gain and allow authored
    // boosts above one without an arbitrary cap.
    const math::q32_32 normalized = math::q32_32::max(
        math::q32_32{}, multiplier);
    if (target->progress.skillPointMultiplier == normalized) return false;
    target->progress.skillPointMultiplier = normalized;
    ++target->revisions.progression;
    return true;
}

bool PlayerRegistry::setListedInScoreScreen(PlayerId player, bool listed) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || target->progress.listedInScoreScreen == listed) return false;
    target->progress.listedInScoreScreen = listed;
    ++target->revisions.scoring;
    return true;
}

namespace {

[[nodiscard]] bool scoreCountLess(
    const PlayerScoreObjectCount& left,
    const PlayerScoreObjectCount& right) noexcept {
    if (left.relatedPlayer != right.relatedPlayer)
        return left.relatedPlayer < right.relatedPlayer;
    if (left.kind != right.kind)
        return static_cast<uint8_t>(left.kind) <
               static_cast<uint8_t>(right.kind);
    return left.templateName < right.templateName;
}

PlayerScoreObjectCount& scoreCount(
    PlayerScoreState& score, PlayerId relatedPlayer,
    PlayerScoredObjectKind kind, container::String templateName) {
    PlayerScoreObjectCount key{
        .relatedPlayer = relatedPlayer,
        .kind = kind,
        .templateName = std::move(templateName),
    };
    const auto position = std::lower_bound(
        score.objectCounts.begin(), score.objectCounts.end(), key,
        scoreCountLess);
    if (position != score.objectCounts.end() &&
        position->relatedPlayer == key.relatedPlayer &&
        position->kind == key.kind &&
        position->templateName == key.templateName) {
        return *position;
    }
    return *score.objectCounts.insert(position, std::move(key));
}

void bumpScoringRevision(PlayerState& player) noexcept {
    ++player.revisions.scoring;
    if (player.revisions.scoring == 0) ++player.revisions.scoring;
}

void saturatingScoreAdd(uint64_t& value, uint64_t amount) noexcept {
    value = amount > std::numeric_limits<uint64_t>::max() - value
        ? std::numeric_limits<uint64_t>::max()
        : value + amount;
}

} // namespace

bool PlayerRegistry::recordObjectLost(
    PlayerId player, container::String templateName,
    PlayerScoredObjectKind kind) {
    PlayerState* target = getMutable(player);
    if (!m_scoreAccumulationEnabled || !target || templateName.empty())
        return false;
    if (kind == PlayerScoredObjectKind::Building)
        ++target->score.buildingsLost;
    else
        ++target->score.unitsLost;
    ++scoreCount(target->score, player, kind,
                 std::move(templateName)).lost;
    bumpScoringRevision(*target);
    return true;
}

bool PlayerRegistry::recordObjectDestroyed(
    PlayerId player, PlayerId victimPlayer,
    container::String templateName, PlayerScoredObjectKind kind) {
    PlayerState* target = getMutable(player);
    if (!m_scoreAccumulationEnabled || !target ||
        !victimPlayer.isValid() || templateName.empty())
        return false;
    const size_t victimIndex = static_cast<size_t>(victimPlayer.value);
    if (kind == PlayerScoredObjectKind::Building)
        ++target->score.buildingsDestroyed[victimIndex];
    else
        ++target->score.unitsDestroyed[victimIndex];
    ++scoreCount(target->score, victimPlayer, kind,
                 std::move(templateName)).destroyed;
    bumpScoringRevision(*target);
    return true;
}

bool PlayerRegistry::recordMoneyEarned(
    PlayerId player, uint64_t amount,
    uint64_t confirmedTick) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || amount == 0) return false;
    PlayerAcademyState& academy = target->academy;
    const uint64_t delta = confirmedTick >= academy.lastIncomeTick
        ? confirmedTick - academy.lastIncomeTick : 0;
    academy.maxTicksBetweenIncome = std::max(
        academy.maxTicksBetweenIncome, delta);
    academy.lastIncomeTick = confirmedTick;
    if (m_scoreAccumulationEnabled)
        saturatingScoreAdd(target->score.moneyEarned, amount);
    bumpScoringRevision(*target);
    return true;
}

bool PlayerRegistry::recordMoneySpent(
    PlayerId player, uint64_t amount) noexcept {
    PlayerState* target = getMutable(player);
    if (!m_scoreAccumulationEnabled || !target || amount == 0)
        return false;
    saturatingScoreAdd(target->score.moneySpent, amount);
    bumpScoringRevision(*target);
    return true;
}

bool PlayerRegistry::recordObjectBuilt(
    PlayerId player, container::String templateName,
    PlayerScoredObjectKind kind) {
    PlayerState* target = getMutable(player);
    if (!m_scoreAccumulationEnabled || !target || templateName.empty())
        return false;
    if (kind == PlayerScoredObjectKind::Building)
        saturatingScoreAdd(target->score.buildingsBuilt, 1);
    else
        saturatingScoreAdd(target->score.unitsBuilt, 1);
    saturatingScoreAdd(
        scoreCount(target->score, player, kind,
                   std::move(templateName)).built,
        1);
    bumpScoringRevision(*target);
    return true;
}

bool PlayerRegistry::removeObjectBuilt(
    PlayerId player, container::StringView templateName,
    PlayerScoredObjectKind kind) noexcept {
    PlayerState* target = getMutable(player);
    if (!m_scoreAccumulationEnabled || !target || templateName.empty())
        return false;
    const auto position = std::find_if(
        target->score.objectCounts.begin(),
        target->score.objectCounts.end(),
        [player, kind, templateName](
            const PlayerScoreObjectCount& count) noexcept {
            return count.relatedPlayer == player &&
                count.kind == kind &&
                count.templateName == templateName;
        });
    if (position == target->score.objectCounts.end() ||
        position->relatedPlayer != player || position->kind != kind ||
        position->templateName != templateName || position->built == 0) {
        return false;
    }
    --position->built;
    uint64_t& total = kind == PlayerScoredObjectKind::Building
        ? target->score.buildingsBuilt : target->score.unitsBuilt;
    if (total != 0) --total;
    bumpScoringRevision(*target);
    return true;
}

bool PlayerRegistry::recordObjectCaptured(
    PlayerId player, container::String templateName,
    bool factionBuilding) {
    PlayerState* target = getMutable(player);
    if (!m_scoreAccumulationEnabled || !target || templateName.empty())
        return false;
    if (factionBuilding)
        saturatingScoreAdd(
            target->score.factionBuildingsCaptured, 1);
    else
        saturatingScoreAdd(target->score.techBuildingsCaptured, 1);
    saturatingScoreAdd(
        scoreCount(target->score, player,
                   PlayerScoredObjectKind::Building,
                   std::move(templateName)).captured,
        1);
    bumpScoringRevision(*target);
    return true;
}

bool PlayerRegistry::recordAcademyProduction(
    PlayerId player, const PlayerAcademyProductionFacts& facts,
    uint64_t confirmedTick, uint32_t logicFramesPerSecond) noexcept {
    PlayerState* target = getMutable(player);
    if (!target) return false;
    if (!(facts.supplyCenter || facts.dozer || facts.infantry ||
          facts.vehicle || facts.harvester || facts.hero ||
          facts.strategyCenter || facts.tunnelNetwork ||
          facts.secondaryIncome || facts.barracks ||
          facts.warFactory || facts.advancedTech || facts.disguiser)) {
        return false;
    }
    PlayerAcademyState& academy = target->academy;
    if (facts.supplyCenter)
        saturatingScoreAdd(academy.supplyCentersBuilt, 1);
    if (facts.dozer)
        saturatingScoreAdd(academy.peonsBuilt, 1);
    if ((facts.infantry || facts.vehicle) && !facts.dozer &&
        !facts.harvester) {
        const uint64_t idleTicks = confirmedTick >= academy.lastUnitBuiltTick
            ? confirmedTick - academy.lastUnitBuiltTick : 0;
        academy.idleBuildingUnitsMaxTicks = std::max(
            academy.idleBuildingUnitsMaxTicks, idleTicks);
        academy.lastUnitBuiltTick = confirmedTick;
    }
    if (facts.harvester)
        saturatingScoreAdd(academy.gatherersBuilt, 1);
    if (facts.hero)
        saturatingScoreAdd(academy.heroesBuilt, 1);
    academy.hadStrategyCenter = academy.hadStrategyCenter ||
        facts.strategyCenter;
    academy.hadTunnelNetwork = academy.hadTunnelNetwork ||
        facts.tunnelNetwork;
    if (facts.secondaryIncome)
        saturatingScoreAdd(academy.secondaryIncomeUnitsBuilt, 1);

    const uint64_t fps = std::max<uint32_t>(1u, logicFramesPerSecond);
    const auto deadline = [fps](uint64_t seconds) noexcept {
        return seconds > std::numeric_limits<uint64_t>::max() / fps
            ? std::numeric_limits<uint64_t>::max() : seconds * fps;
    };
    academy.builtBarracksWithinFiveMinutes =
        academy.builtBarracksWithinFiveMinutes ||
        (facts.barracks && confirmedTick <= deadline(300));
    academy.builtWarFactoryWithinTenMinutes =
        academy.builtWarFactoryWithinTenMinutes ||
        (facts.warFactory && confirmedTick <= deadline(600));
    academy.builtAdvancedTechWithinFifteenMinutes =
        academy.builtAdvancedTechWithinFifteenMinutes ||
        (facts.advancedTech && confirmedTick <= deadline(900));
    if (facts.disguiser)
        saturatingScoreAdd(academy.disguisableVehiclesBuilt, 1);
    bumpScoringRevision(*target);
    return true;
}

bool PlayerRegistry::recordAcademyUpgrade(
    PlayerId player, bool radarClassified, bool granted) noexcept {
    PlayerState* target = getMutable(player);
    if (!target) return false;
    PlayerAcademyState& academy = target->academy;
    academy.researchedRadar = academy.researchedRadar || radarClassified;
    if (!granted) saturatingScoreAdd(academy.upgradesPurchased, 1);
    bumpScoringRevision(*target);
    return true;
}

bool PlayerRegistry::recordAcademySpecialPower(
    PlayerId player, bool superpowerClassified) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || !superpowerClassified) return false;
    saturatingScoreAdd(target->academy.specialPowersUsed, 1);
    bumpScoringRevision(*target);
    return true;
}

bool PlayerRegistry::recordAcademyIncome(
    PlayerId player, uint64_t confirmedTick) noexcept {
    PlayerState* target = getMutable(player);
    if (!target) return false;
    PlayerAcademyState& academy = target->academy;
    const uint64_t delta = confirmedTick >= academy.lastIncomeTick
        ? confirmedTick - academy.lastIncomeTick : 0;
    academy.maxTicksBetweenIncome = std::max(
        academy.maxTicksBetweenIncome, delta);
    academy.lastIncomeTick = confirmedTick;
    bumpScoringRevision(*target);
    return true;
}

bool PlayerRegistry::recordAcademyEvent(
    PlayerId player, PlayerAcademyEvent event, uint64_t amount) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || amount == 0) return false;
    PlayerAcademyState& academy = target->academy;
    switch (event) {
    case PlayerAcademyEvent::BuildingCaptured:
        saturatingScoreAdd(academy.structuresCaptured, amount); break;
    case PlayerAcademyEvent::GeneralsPointsSpent:
        saturatingScoreAdd(academy.generalsPointsSpent, amount); break;
    case PlayerAcademyEvent::BuildingGarrisoned:
        saturatingScoreAdd(academy.structuresGarrisoned, amount); break;
    case PlayerAcademyEvent::BattlePlanSelected:
        academy.choseStrategyCenterBattlePlan = true; break;
    case PlayerAcademyEvent::UnitEnteredTunnelNetwork:
        saturatingScoreAdd(academy.unitsEnteredTunnelNetwork, amount); break;
    case PlayerAcademyEvent::ClearedGarrisonedBuilding:
        saturatingScoreAdd(academy.clearedGarrisonedBuildings, amount); break;
    case PlayerAcademyEvent::VehicleDisguised:
        saturatingScoreAdd(academy.vehiclesDisguised, amount); break;
    case PlayerAcademyEvent::FirestormCreated:
        saturatingScoreAdd(academy.firestormsCreated, amount); break;
    case PlayerAcademyEvent::GuardAbilityUsed:
        saturatingScoreAdd(academy.guardAbilityUsed, amount); break;
    case PlayerAcademyEvent::SalvageCollected:
        saturatingScoreAdd(academy.salvageCollected, amount); break;
    case PlayerAcademyEvent::MineCleared:
        saturatingScoreAdd(academy.minesCleared, amount); break;
    case PlayerAcademyEvent::VehicleRecovered:
        saturatingScoreAdd(academy.vehiclesRecovered, amount); break;
    case PlayerAcademyEvent::VehicleSniped:
        saturatingScoreAdd(academy.vehiclesSniped, amount); break;
    case PlayerAcademyEvent::MineCreated:
        saturatingScoreAdd(academy.minesCreated, amount); break;
    }
    bumpScoringRevision(*target);
    return true;
}

bool PlayerRegistry::updateAcademyPeriodicState(
    PlayerId player, uint64_t confirmedTick,
    uint64_t supplyCenterCost) noexcept {
    PlayerState* target = getMutable(player);
    if (!target) return false;
    PlayerAcademyState& academy = target->academy;
    bool changed = false;
    if (!academy.spentCashBeforeBuildingSupplyCenter &&
        academy.supplyCentersBuilt == 0 && target->cash >= 0 &&
        static_cast<uint64_t>(target->cash) < supplyCenterCost) {
        academy.spentCashBeforeBuildingSupplyCenter = true;
        changed = true;
    }
    const bool hasPower = target->energy.production >=
        target->energy.consumption;
    if (!academy.hasPowerSample) {
        academy.hasPowerSample = true;
        academy.hadPowerLastSample = hasPower;
        if (!hasPower) academy.powerOutStartedTick = confirmedTick;
        changed = true;
    } else if (hasPower != academy.hadPowerLastSample) {
        if (!hasPower) {
            academy.powerOutStartedTick = confirmedTick;
        } else {
            const uint64_t duration = confirmedTick >=
                    academy.powerOutStartedTick
                ? confirmedTick - academy.powerOutStartedTick : 0;
            academy.powerOutMaxTicks = std::max(
                academy.powerOutMaxTicks, duration);
        }
        academy.hadPowerLastSample = hasPower;
        changed = true;
    }
    if (changed) bumpScoringRevision(*target);
    return changed;
}

bool PlayerRegistry::recordClearedGarrisonedBuilding(
    PlayerId player) noexcept {
    return recordAcademyEvent(
        player, PlayerAcademyEvent::ClearedGarrisonedBuilding);
}

bool PlayerRegistry::setLifeState(PlayerId player, PlayerLifeState life) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || target->isNeutral() || target->life == life) return false;
    target->life = life;
    ++target->revisions.scoring;
    return true;
}

bool PlayerRegistry::setBaseConstructionEnabled(PlayerId player, bool enabled) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || target->constructionPolicy.baseConstructionEnabled == enabled) return false;
    target->constructionPolicy.baseConstructionEnabled = enabled;
    ++target->revisions.production;
    return true;
}

bool PlayerRegistry::setUnitConstructionEnabled(PlayerId player, bool enabled) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || target->constructionPolicy.unitConstructionEnabled == enabled) return false;
    target->constructionPolicy.unitConstructionEnabled = enabled;
    ++target->revisions.production;
    return true;
}

bool PlayerRegistry::setTeamDelaySeconds(PlayerId player, int32_t seconds) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || target->constructionPolicy.teamDelaySeconds == seconds) return false;
    target->constructionPolicy.teamDelaySeconds = seconds;
    ++target->revisions.production;
    return true;
}

bool PlayerRegistry::grantScience(PlayerId player, container::String science) {
    PlayerState* target = getMutable(player);
    if (!target || science.empty()) return false;
    // RefCode Player::grantScience delegates to addScience(), which only
    // inserts into m_sciences. Script-authored Disabled/Hidden state is not
    // implicitly cleared by a grant; preserving it also keeps this mutation
    // consistent with the successful purchase path.
    const bool changed = scienceInsert(target->sciences.known, science);
    if (changed) {
        recordScienceAcquisition(target->sciences, science);
        ++target->revisions.technology;
    }
    return changed;
}

bool PlayerRegistry::canPurchaseScience(PlayerId player,
                                        const ScienceDefinition& science) const {
    const PlayerState* target = get(player);
    if (!target || science.name.empty() || science.purchasePointCost <= 0 ||
        target->sciences.purchasePoints < science.purchasePointCost) {
        return false;
    }
    if (scienceContains(target->sciences.known, science.name) ||
        scienceContains(target->sciences.disabled, science.name) ||
        scienceContains(target->sciences.hidden, science.name)) {
        return false;
    }

    // ScienceStore::playerHasPrereqsForScience checks the direct authored
    // prerequisites.  They are all required; a deeper chain becomes
    // available naturally when each intermediate science is acquired.
    for (const container::String& prerequisite : science.prerequisiteSciences) {
        if (prerequisite.empty() || !scienceContains(target->sciences.known, prerequisite)) {
            return false;
        }
    }
    return true;
}

bool PlayerRegistry::tryPurchaseScience(PlayerId player,
                                        const ScienceDefinition& science) {
    if (!canPurchaseScience(player, science)) return false;
    PlayerState* target = getMutable(player);
    if (!target) return false;

    // Insert before charging.  The vector allocation is the only operation
    // here that can fail; once it has completed, subtraction is bounded by
    // canPurchaseScience and cannot leave a partially spent transaction.
    if (!scienceInsert(target->sciences.known, science.name)) return false;
    target->sciences.purchasePoints -= science.purchasePointCost;
    recordScienceAcquisition(target->sciences, science.name);
    ++target->revisions.technology;
    saturatingScoreAdd(
        target->academy.generalsPointsSpent,
        static_cast<uint64_t>(science.purchasePointCost));
    bumpScoringRevision(*target);
    return true;
}

bool PlayerRegistry::tryPurchaseScience(PlayerId player, container::String science,
                                        int32_t purchaseCost) {
    ScienceDefinition definition{
        .name = std::move(science),
        .purchasePointCost = purchaseCost,
    };
    return tryPurchaseScience(player, definition);
}

bool PlayerRegistry::consumeScienceAcquired(PlayerId player, container::StringView science) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || science.empty()) return false;
    auto& acquisitions = target->sciences.pendingAcquisitions;
    const auto found = std::lower_bound(acquisitions.begin(), acquisitions.end(), science,
        [](const PendingScienceAcquisition& left, container::StringView right) {
            return container::StringView{left.science} < right;
        });
    if (found == acquisitions.end() || container::StringView{found->science} != science ||
        found->count == 0) {
        return false;
    }
    --found->count;
    if (found->count == 0) acquisitions.erase(found);
    return true;
}

bool PlayerRegistry::hasScience(PlayerId player, container::StringView science) const noexcept {
    const PlayerState* target = get(player);
    return target && !science.empty() && scienceContains(target->sciences.known, science);
}

bool PlayerRegistry::markUpgradeComplete(PlayerId player, UpgradeContentId upgrade) {
    PlayerState* target = getMutable(player);
    if (!target || !upgradeIdInMaskRange(upgrade)) return false;
    const size_t bit = upgradeBitIndex(upgrade);
    bool changed = false;
    if (!target->upgrades.completed.test(bit)) {
        target->upgrades.completed.set(bit);
        changed = true;
    }
    if (target->upgrades.inProgress.test(bit)) {
        target->upgrades.inProgress.reset(bit);
        changed = true;
    }
    if (changed) ++target->revisions.technology;
    return changed;
}

bool PlayerRegistry::hasUpgradeComplete(PlayerId player,
                                        UpgradeContentId upgrade) const noexcept {
    const PlayerState* target = get(player);
    return target && upgradeMaskTest(target->upgrades.completed, upgrade);
}

bool PlayerRegistry::hasUpgradeInProgress(PlayerId player,
                                          UpgradeContentId upgrade) const noexcept {
    const PlayerState* target = get(player);
    return target && upgradeMaskTest(target->upgrades.inProgress, upgrade);
}

bool PlayerRegistry::hasUpgradeComplete(
    PlayerId player, container::StringView upgrade,
    const UpgradeCatalog& catalog) const noexcept {
    const UpgradeDefinition* definition = catalog.find(upgrade);
    return definition && hasUpgradeComplete(player, definition->id);
}

bool PlayerRegistry::hasUpgradeInProgress(
    PlayerId player, container::StringView upgrade,
    const UpgradeCatalog& catalog) const noexcept {
    const UpgradeDefinition* definition = catalog.find(upgrade);
    return definition && hasUpgradeInProgress(player, definition->id);
}

bool PlayerRegistry::markUpgradeComplete(PlayerId player, container::StringView upgrade,
                                         const UpgradeCatalog& catalog) {
    const UpgradeDefinition* definition = catalog.find(upgrade);
    return definition && markUpgradeComplete(player, definition->id);
}

bool PlayerRegistry::markUpgradeInProgress(PlayerId player, container::StringView upgrade,
                                           const UpgradeCatalog& catalog) {
    const UpgradeDefinition* definition = catalog.find(upgrade);
    return definition && markUpgradeInProgress(player, definition->id);
}

bool PlayerRegistry::markAttackedBy(PlayerId victim, PlayerId attacker) noexcept {
    PlayerState* target = getMutable(victim);
    if (!target || !get(attacker) || attacker.value >= 32) return false;
    const uint32_t bit = uint32_t{1} << attacker.value;
    if ((target->attackedByMask & bit) != 0) return false;
    target->attackedByMask |= bit;
    return true;
}

bool PlayerRegistry::wasAttackedBy(PlayerId victim, PlayerId attacker) const noexcept {
    const PlayerState* target = get(victim);
    if (!target || !get(attacker) || attacker.value >= 32) return false;
    return (target->attackedByMask & (uint32_t{1} << attacker.value)) != 0;
}

bool PlayerRegistry::setUnitsShouldHunt(PlayerId player, bool enabled) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || target->unitsShouldHunt == enabled) return false;
    target->unitsShouldHunt = enabled;
    ++target->revisions.tactics;
    return true;
}

bool PlayerRegistry::setLogicalRetaliationEnabled(
    PlayerId player, bool enabled) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || target->logicalRetaliationEnabled == enabled) return false;
    target->logicalRetaliationEnabled = enabled;
    ++target->revisions.tactics;
    return true;
}

bool PlayerRegistry::markUpgradeInProgress(PlayerId player, UpgradeContentId upgrade) {
    return beginQueuedPlayerUpgrade(player, upgrade);
}

bool PlayerRegistry::beginQueuedPlayerUpgrade(PlayerId player, UpgradeContentId upgrade) {
    PlayerState* target = getMutable(player);
    if (!target || !upgradeIdInMaskRange(upgrade)) return false;
    const size_t bit = upgradeBitIndex(upgrade);
    if (target->upgrades.completed.test(bit) ||
        target->upgrades.inProgress.test(bit)) {
        return false;
    }
    target->upgrades.inProgress.set(bit);
    ++target->revisions.technology;
    return true;
}

bool PlayerRegistry::cancelQueuedPlayerUpgrade(PlayerId player,
                                               UpgradeContentId upgrade) noexcept {
    PlayerState* target = getMutable(player);
    if (!target || !upgradeIdInMaskRange(upgrade)) return false;
    const size_t bit = upgradeBitIndex(upgrade);
    if (!target->upgrades.inProgress.test(bit)) return false;
    target->upgrades.inProgress.reset(bit);
    ++target->revisions.technology;
    return true;
}

bool PlayerRegistry::commitQueuedPlayerUpgrade(PlayerId player, UpgradeContentId upgrade) {
    PlayerState* target = getMutable(player);
    if (!target || !upgradeIdInMaskRange(upgrade)) return false;
    const size_t bit = upgradeBitIndex(upgrade);
    if (!target->upgrades.inProgress.test(bit) ||
        target->upgrades.completed.test(bit)) {
        return false;
    }
    target->upgrades.completed.set(bit);
    target->upgrades.inProgress.reset(bit);
    ++target->revisions.technology;
    return true;
}

bool PlayerRegistry::beginQueuedPlayerUpgrade(PlayerId player, container::StringView upgrade,
                                              const UpgradeCatalog& catalog) {
    const UpgradeDefinition* definition = catalog.find(upgrade);
    return definition && beginQueuedPlayerUpgrade(player, definition->id);
}

bool PlayerRegistry::cancelQueuedPlayerUpgrade(PlayerId player, container::StringView upgrade,
                                               const UpgradeCatalog& catalog) noexcept {
    const UpgradeDefinition* definition = catalog.find(upgrade);
    return definition && cancelQueuedPlayerUpgrade(player, definition->id);
}

bool PlayerRegistry::commitQueuedPlayerUpgrade(PlayerId player, container::StringView upgrade,
                                               const UpgradeCatalog& catalog) {
    const UpgradeDefinition* definition = catalog.find(upgrade);
    return definition && commitQueuedPlayerUpgrade(player, definition->id);
}

uint64_t PlayerRegistry::simulationDigest() const noexcept {
    return digestImpl(false);
}

uint64_t PlayerRegistry::contentDigest() const noexcept {
    return digestImpl(true);
}

uint64_t PlayerRegistry::digestImpl(bool includePresentation) const noexcept {
    DigestWriter writer;
    writer.boolean(m_scoreAccumulationEnabled);
    writer.u64(m_resolvedSetupDigest);
    if (includePresentation) writer.u64(m_resolvedSetupContentDigest);
    writer.u64(m_playerRulesetFingerprint);
    writer.u64(m_simulationContentFingerprint);
    writer.u32(static_cast<uint32_t>(m_activePlayerIds.size()));
    for (const PlayerId id : m_activePlayerIds) {
        const PlayerState* player = get(id);
        if (!player) continue;
        writer.byte(player->id.value);
        writer.byte(player->slot.value);
        writer.byte(static_cast<uint8_t>(player->controller));
        writer.byte(static_cast<uint8_t>(player->aiDifficulty));
        writer.byte(static_cast<uint8_t>(player->participation));
        writer.boolean(player->playableSide);
        writer.byte(static_cast<uint8_t>(player->life));
        writer.u32(player->faction.value);
        writer.byte(player->alliance.value);
        writer.i32(player->startPosition);
        writer.i64(player->cash);
        writer.i64(player->cashBountyPercent.raw());
        hashStringVector(writer, player->sciences.intrinsicKnown);
        hashStringVector(writer, player->sciences.known);
        hashStringVector(writer, player->sciences.disabled);
        hashStringVector(writer, player->sciences.hidden);
        hashPendingScienceAcquisitions(writer, player->sciences.pendingAcquisitions);
        writer.i32(player->sciences.purchasePoints);
        writer.i32(player->sciences.intrinsicPurchasePoints);
        writer.i64(player->progress.skillPoints);
        writer.i32(player->progress.rankLevel);
        writer.i32(player->progress.selectedSkillset);
        writer.u64(static_cast<uint64_t>(
            player->progress.skillPointMultiplier.raw()));
        writer.boolean(player->progress.listedInScoreScreen);
        for (size_t bit = 0; bit < kUpgradeMaskBits; ++bit) {
            if (player->upgrades.completed.test(bit))
                writer.u32(static_cast<uint32_t>(bit + 1u));
        }
        writer.u32(0);
        for (size_t bit = 0; bit < kUpgradeMaskBits; ++bit) {
            if (player->upgrades.inProgress.test(bit))
                writer.u32(static_cast<uint32_t>(bit + 1u));
        }
        writer.u32(0);
        hashPercentModifiers(writer, player->productionModifiers.cost);
        hashPercentModifiers(writer, player->productionModifiers.time);
        hashVeterancyModifiers(writer, player->productionModifiers.veterancy);
        writer.i64(player->productionModifiers.genericBuildCostHandicap.raw());
        writer.i64(player->productionModifiers.structureBuildCostHandicap.raw());
        writer.i64(player->productionModifiers.genericBuildTimeHandicap.raw());
        writer.i64(player->productionModifiers.structureBuildTimeHandicap.raw());
        writer.boolean(player->constructionPolicy.baseConstructionEnabled);
        writer.boolean(player->constructionPolicy.unitConstructionEnabled);
        writer.i32(player->constructionPolicy.teamDelaySeconds);
        writer.i32(player->energy.production);
        writer.i32(player->energy.consumption);
        writer.u64(player->energy.sabotagedUntilTick);
        writer.u32(player->radar.providerCount);
        writer.u32(player->radar.disableProofProviderCount);
        writer.u32(player->attackedByMask);
        writer.boolean(player->unitsShouldHunt);
        writer.boolean(player->logicalRetaliationEnabled);
        writer.u64(static_cast<uint64_t>(
            player->teamRelationshipOverrides.size()));
        for (const PlayerTeamRelationshipOverride& override :
             player->teamRelationshipOverrides) {
            writer.u32(override.target.value);
            writer.byte(static_cast<uint8_t>(override.relationship));
        }
        for (const uint64_t value : player->score.unitsDestroyed)
            writer.u64(value);
        for (const uint64_t value : player->score.buildingsDestroyed)
            writer.u64(value);
        writer.u64(player->score.unitsLost);
        writer.u64(player->score.buildingsLost);
        writer.u64(player->score.moneyEarned);
        writer.u64(player->score.moneySpent);
        writer.u64(player->score.unitsBuilt);
        writer.u64(player->score.buildingsBuilt);
        writer.u64(player->score.factionBuildingsCaptured);
        writer.u64(player->score.techBuildingsCaptured);
        writer.u64(player->score.objectCounts.size());
        for (const PlayerScoreObjectCount& count :
             player->score.objectCounts) {
            writer.byte(count.relatedPlayer.value);
            writer.byte(static_cast<uint8_t>(count.kind));
            writer.string(count.templateName);
            writer.u64(count.destroyed);
            writer.u64(count.lost);
            writer.u64(count.built);
            writer.u64(count.captured);
        }
        writer.boolean(player->academy.spentCashBeforeBuildingSupplyCenter);
        writer.boolean(player->academy.researchedRadar);
        writer.boolean(player->academy.hadStrategyCenter);
        writer.boolean(player->academy.choseStrategyCenterBattlePlan);
        writer.boolean(player->academy.hadTunnelNetwork);
        writer.boolean(player->academy.builtBarracksWithinFiveMinutes);
        writer.boolean(player->academy.builtWarFactoryWithinTenMinutes);
        writer.boolean(player->academy.builtAdvancedTechWithinFifteenMinutes);
        writer.boolean(player->academy.hadPowerLastSample);
        writer.boolean(player->academy.hasPowerSample);
        writer.u64(player->academy.supplyCentersBuilt);
        writer.u64(player->academy.peonsBuilt);
        writer.u64(player->academy.structuresCaptured);
        writer.u64(player->academy.generalsPointsSpent);
        writer.u64(player->academy.specialPowersUsed);
        writer.u64(player->academy.structuresGarrisoned);
        writer.u64(player->academy.idleBuildingUnitsMaxTicks);
        writer.u64(player->academy.lastUnitBuiltTick);
        writer.u64(player->academy.upgradesPurchased);
        writer.u64(player->academy.powerOutMaxTicks);
        writer.u64(player->academy.powerOutStartedTick);
        writer.u64(player->academy.gatherersBuilt);
        writer.u64(player->academy.heroesBuilt);
        writer.u64(player->academy.unitsEnteredTunnelNetwork);
        writer.u64(player->academy.secondaryIncomeUnitsBuilt);
        writer.u64(player->academy.clearedGarrisonedBuildings);
        writer.u64(player->academy.salvageCollected);
        writer.u64(player->academy.guardAbilityUsed);
        writer.u64(player->academy.lastIncomeTick);
        writer.u64(player->academy.maxTicksBetweenIncome);
        writer.u64(player->academy.minesCreated);
        writer.u64(player->academy.minesCleared);
        writer.u64(player->academy.vehiclesRecovered);
        writer.u64(player->academy.vehiclesSniped);
        writer.u64(player->academy.disguisableVehiclesBuilt);
        writer.u64(player->academy.vehiclesDisguised);
        writer.u64(player->academy.firestormsCreated);
        writer.u64(player->revisions.economy);
        writer.u64(player->revisions.technology);
        writer.u64(player->revisions.production);
        writer.u64(player->revisions.energy);
        writer.u64(player->revisions.radar);
        writer.u64(player->revisions.diplomacy);
        writer.u64(player->revisions.progression);
        writer.u64(player->revisions.scoring);
        writer.u64(player->revisions.tactics);
        if (includePresentation) {
            writer.u32(player->color.value);
            writer.string(player->displayName);
            writer.string(player->side);
            writer.string(player->baseSide);
        }
    }
    for (const PlayerRelationship relationship : m_relationships.canonicalValues()) {
        writer.byte(static_cast<uint8_t>(relationship));
    }
    return writer.finish();
}

bool PlayerRegistry::canonicalInsert(container::Vector<container::String>& values, container::String value) {
    const auto found = std::lower_bound(values.begin(), values.end(), container::StringView{value},
        [](const container::String& lhs, container::StringView rhs) { return container::StringView{lhs} < rhs; });
    if (found != values.end() && *found == value) return false;
    values.insert(found, std::move(value));
    return true;
}

bool PlayerRegistry::canonicalErase(container::Vector<container::String>& values, container::StringView value) {
    const auto found = std::lower_bound(values.begin(), values.end(), value,
        [](const container::String& lhs, container::StringView rhs) { return container::StringView{lhs} < rhs; });
    if (found == values.end() || container::StringView{*found} != value) return false;
    values.erase(found);
    return true;
}

void PlayerRegistry::bumpDiplomacyRevision(PlayerId player) noexcept {
    if (PlayerState* target = getMutable(player)) ++target->revisions.diplomacy;
}

} // namespace engine
