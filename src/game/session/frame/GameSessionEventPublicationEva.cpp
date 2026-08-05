#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/frame/GameSessionEvaEventPublisher.h"
#include "game/session/frame/GameSessionObjectDeathFeedbackPublisher.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"
#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"

#include "game/audio/EvaEventCatalog.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine {
namespace {

[[nodiscard]] uint32_t millisecondsToFrames(
    uint32_t milliseconds, uint32_t framesPerSecond) noexcept {
    if (milliseconds == 0) return 0;
    const uint64_t frames =
        (static_cast<uint64_t>(milliseconds) *
             std::max<uint32_t>(1u, framesPerSecond) +
         999u) /
        1000u;
    return static_cast<uint32_t>(std::min<uint64_t>(
        frames, std::numeric_limits<uint32_t>::max()));
}

[[nodiscard]] uint64_t evaVariationSeed(
    uint64_t confirmedTick, uint64_t variationKey,
    audio::EvaEventType type) noexcept {
    uint64_t value = confirmedTick ^ variationKey ^
        static_cast<uint64_t>(type);
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

[[nodiscard]] uint64_t saturatingTickAdd(uint64_t value,
                                         uint64_t delta) noexcept {
    return value > std::numeric_limits<uint64_t>::max() - delta
        ? std::numeric_limits<uint64_t>::max() : value + delta;
}

[[nodiscard]] uint64_t floatingTextFadeFrames(
    uint8_t initialAlpha, float vanishPerSecond,
    uint32_t framesPerSecond) noexcept {
    if (!(vanishPerSecond > 0.0f) || !std::isfinite(vanishPerSecond))
        return std::numeric_limits<uint64_t>::max();
    uint32_t alpha = initialAlpha;
    const float perFrame = vanishPerSecond /
        static_cast<float>(std::max(1u, framesPerSecond));
    for (uint64_t frame = 1; frame <= 4096u; ++frame) {
        const uint32_t amount = static_cast<uint32_t>(std::max(
            0.0f, std::round(static_cast<float>(frame) * perFrame)));
        alpha = amount >= alpha ? 0u : alpha - amount;
        if (alpha == 0) return frame;
    }
    return 4096u;
}

// One superweapon family per authored EVA triple. RefCode folds the three
// ParticleCannon and three NeutronMissile SpecialPowerType spellings onto the
// same announcement, so keep that grouping explicit rather than repeating the
// ladder at each call site.
enum class EvaSuperweaponFamily : uint8_t {
    None,
    ParticleCannon,
    Nuke,
    ScudStorm,
    GpsScrambler,
    SneakAttack,
};

[[nodiscard]] EvaSuperweaponFamily evaSuperweaponFamily(
    game::SpecialPowerType type) noexcept {
    switch (type) {
    case game::SpecialPowerType::ParticleUplinkCannon:
    case game::SpecialPowerType::SupwParticleUplinkCannon:
    case game::SpecialPowerType::LazrParticleUplinkCannon:
        return EvaSuperweaponFamily::ParticleCannon;
    case game::SpecialPowerType::NeutronMissile:
    case game::SpecialPowerType::NukeNeutronMissile:
    case game::SpecialPowerType::SupwNeutronMissile:
        return EvaSuperweaponFamily::Nuke;
    case game::SpecialPowerType::ScudStorm:
        return EvaSuperweaponFamily::ScudStorm;
    case game::SpecialPowerType::GpsScrambler:
    case game::SpecialPowerType::SlthGpsScrambler:
        return EvaSuperweaponFamily::GpsScrambler;
    case game::SpecialPowerType::SneakAttack:
        return EvaSuperweaponFamily::SneakAttack;
    default:
        return EvaSuperweaponFamily::None;
    }
}

} // namespace

EvaSuperweaponAudience evaSuperweaponAudience(
    bool ownedByObserver, PlayerRelationship observerToOwner) noexcept {
    if (ownedByObserver) return EvaSuperweaponAudience::Own;
    return observerToOwner == PlayerRelationship::Enemies
        ? EvaSuperweaponAudience::Enemy
        : EvaSuperweaponAudience::Ally;
}

std::optional<audio::EvaEventType> evaSuperweaponEvent(
    game::SpecialPowerType type, EvaSuperweaponAnnouncement announcement,
    EvaSuperweaponAudience audience) noexcept {
    using Type = audio::EvaEventType;
    switch (evaSuperweaponFamily(type)) {
    case EvaSuperweaponFamily::ParticleCannon:
        switch (announcement) {
        case EvaSuperweaponAnnouncement::Detected:
            return audience == EvaSuperweaponAudience::Own
                ? Type::SuperweaponDetectedOwnParticleCannon
                : audience == EvaSuperweaponAudience::Ally
                    ? Type::SuperweaponDetectedAllyParticleCannon
                    : Type::SuperweaponDetectedEnemyParticleCannon;
        case EvaSuperweaponAnnouncement::Launched:
            return audience == EvaSuperweaponAudience::Own
                ? Type::SuperweaponLaunchedOwnParticleCannon
                : audience == EvaSuperweaponAudience::Ally
                    ? Type::SuperweaponLaunchedAllyParticleCannon
                    : Type::SuperweaponLaunchedEnemyParticleCannon;
        case EvaSuperweaponAnnouncement::Ready:
            return audience == EvaSuperweaponAudience::Own
                ? Type::SuperweaponReadyOwnParticleCannon
                : audience == EvaSuperweaponAudience::Ally
                    ? Type::SuperweaponReadyAllyParticleCannon
                    : Type::SuperweaponReadyEnemyParticleCannon;
        }
        return std::nullopt;
    case EvaSuperweaponFamily::Nuke:
        switch (announcement) {
        case EvaSuperweaponAnnouncement::Detected:
            return audience == EvaSuperweaponAudience::Own
                ? Type::SuperweaponDetectedOwnNuke
                : audience == EvaSuperweaponAudience::Ally
                    ? Type::SuperweaponDetectedAllyNuke
                    : Type::SuperweaponDetectedEnemyNuke;
        case EvaSuperweaponAnnouncement::Launched:
            return audience == EvaSuperweaponAudience::Own
                ? Type::SuperweaponLaunchedOwnNuke
                : audience == EvaSuperweaponAudience::Ally
                    ? Type::SuperweaponLaunchedAllyNuke
                    : Type::SuperweaponLaunchedEnemyNuke;
        case EvaSuperweaponAnnouncement::Ready:
            return audience == EvaSuperweaponAudience::Own
                ? Type::SuperweaponReadyOwnNuke
                : audience == EvaSuperweaponAudience::Ally
                    ? Type::SuperweaponReadyAllyNuke
                    : Type::SuperweaponReadyEnemyNuke;
        }
        return std::nullopt;
    case EvaSuperweaponFamily::ScudStorm:
        switch (announcement) {
        case EvaSuperweaponAnnouncement::Detected:
            return audience == EvaSuperweaponAudience::Own
                ? Type::SuperweaponDetectedOwnScudStorm
                : audience == EvaSuperweaponAudience::Ally
                    ? Type::SuperweaponDetectedAllyScudStorm
                    : Type::SuperweaponDetectedEnemyScudStorm;
        case EvaSuperweaponAnnouncement::Launched:
            return audience == EvaSuperweaponAudience::Own
                ? Type::SuperweaponLaunchedOwnScudStorm
                : audience == EvaSuperweaponAudience::Ally
                    ? Type::SuperweaponLaunchedAllyScudStorm
                    : Type::SuperweaponLaunchedEnemyScudStorm;
        case EvaSuperweaponAnnouncement::Ready:
            return audience == EvaSuperweaponAudience::Own
                ? Type::SuperweaponReadyOwnScudStorm
                : audience == EvaSuperweaponAudience::Ally
                    ? Type::SuperweaponReadyAllyScudStorm
                    : Type::SuperweaponReadyEnemyScudStorm;
        }
        return std::nullopt;
    case EvaSuperweaponFamily::GpsScrambler:
        if (announcement != EvaSuperweaponAnnouncement::Launched)
            return std::nullopt;
        return audience == EvaSuperweaponAudience::Own
            ? Type::SuperweaponLaunchedOwnGpsScrambler
            : audience == EvaSuperweaponAudience::Ally
                ? Type::SuperweaponLaunchedAllyGpsScrambler
                : Type::SuperweaponLaunchedEnemyGpsScrambler;
    case EvaSuperweaponFamily::SneakAttack:
        if (announcement != EvaSuperweaponAnnouncement::Launched)
            return std::nullopt;
        return audience == EvaSuperweaponAudience::Own
            ? Type::SuperweaponLaunchedOwnSneakAttack
            : audience == EvaSuperweaponAudience::Ally
                ? Type::SuperweaponLaunchedAllySneakAttack
                : Type::SuperweaponLaunchedEnemySneakAttack;
    case EvaSuperweaponFamily::None:
        break;
    }
    return std::nullopt;
}

void GameSessionEvaEventPublisher::publish(
    audio::EvaEventType type, uint64_t confirmedTick,
    uint64_t variationKey) {
    const PlayerState* localPlayer =
        m_content.m_players.localPlayer();
    if (!localPlayer) return;
    const game::EvaEventCatalog* catalog =
        m_content.m_contentSnapshot.evaEventCatalog();
    const game::EvaEventDefinition* definition = catalog
        ? catalog->find(type) : nullptr;
    if (!definition || definition->priority == 0) return;
    const uint32_t framesPerSecond = static_cast<uint32_t>(
        std::max(1, m_content.m_startInfo.gameSpeedFPS));
    static_cast<void>(m_publication.emitAudioEvent({
        .eventName = catalog->resolveSound(
            type, localPlayer->side,
            evaVariationSeed(confirmedTick, variationKey, type)),
        .sourcePlayer = localPlayer->id,
        .eva = true,
        .evaPolicy = audio::EvaPresentationPolicy{
            .type = type,
            .priority = definition->priority,
            .cooldownFrames = millisecondsToFrames(
                definition->cooldownMilliseconds, framesPerSecond),
            .expirationFrames = millisecondsToFrames(
                definition->expirationMilliseconds, framesPerSecond),
        },
    }));
}

void GameSessionEvaEventPublisher::publishPendingAnnouncements(
    GameSessionScriptPresentationState& presentation) {
    if (presentation.m_pendingEvaAnnouncements.empty()) return;
    container::Vector<PendingEvaAnnouncement> pending =
        std::move(presentation.m_pendingEvaAnnouncements);
    presentation.m_pendingEvaAnnouncements.clear();
    for (const PendingEvaAnnouncement& announcement : pending) {
        if (announcement.type == audio::EvaEventType::Count) continue;
        publish(
            announcement.type, announcement.confirmedTick,
            announcement.variationKey);
    }
}

uint32_t GameSessionEvaEventPublisher::polledRetryFrames(
    audio::EvaEventType type) const noexcept {
    const game::EvaEventCatalog* catalog =
        m_content.m_contentSnapshot.evaEventCatalog();
    const game::EvaEventDefinition* definition = catalog
        ? catalog->find(type) : nullptr;
    const uint32_t framesPerSecond = static_cast<uint32_t>(
        std::max(1, m_content.m_startInfo.gameSpeedFPS));
    // An authored ExpirationTimeMS of zero would otherwise re-offer every
    // frame; RefCode's own default is five seconds.
    return std::max<uint32_t>(1u, millisecondsToFrames(
        definition && definition->expirationMilliseconds != 0
            ? definition->expirationMilliseconds : 5'000u,
        framesPerSecond));
}

void GameSessionEvaEventPublisher::publishPolledObserverConditions(
    GameSessionScriptPresentationState& presentation,
    uint64_t confirmedTick) {
    // RefCode Eva::update returns before frame 2 so an aggregate that has not
    // yet been derived from live power-bearing objects cannot announce a false
    // outage on the opening frames of a match.
    if (confirmedTick < 2) return;
    const PlayerState* localPlayer = m_content.m_players.localPlayer();
    if (!localPlayer) return;

    // RefCode Player::setRankLevel announces the promotion from inside the
    // authoritative rank transaction, and consequently also announces the
    // rank-1 initialisation and any script-driven downgrade. Watching the
    // observed rank from here keeps the announcement entirely out of
    // simulation and limits it to a genuine increase.
    int32_t& observedRank = presentation.m_evaObservedRankLevel;
    const int32_t currentRank = localPlayer->progress.rankLevel;
    if (observedRank == 0) {
        observedRank = currentRank;
    } else if (currentRank != observedRank) {
        const bool promoted = currentRank > observedRank;
        observedRank = currentRank;
        if (promoted) {
            publish(
                audio::EvaEventType::GeneralLevelUp, confirmedTick,
                (static_cast<uint64_t>(localPlayer->id.value) << 32u) ^
                    static_cast<uint64_t>(
                        static_cast<uint32_t>(currentRank)));
        }
    }

    constexpr size_t kLowPowerIndex =
        static_cast<size_t>(audio::EvaEventType::LowPower);
    uint64_t& lowPowerRetryTick =
        presentation.m_evaPolledRetryTicks[kLowPowerIndex];
    // PlayerEnergyState::hasSufficientPower is the exact counterpart of
    // RefCode Energy::hasSufficientPower, including the sabotage window.
    if (localPlayer->energy.hasSufficientPower(confirmedTick)) {
        // Power restored: the next outage announces immediately, matching
        // RefCode where the erased check leaves the predicate free to re-fire.
        lowPowerRetryTick = 0;
        return;
    }
    if (confirmedTick < lowPowerRetryTick) return;
    lowPowerRetryTick = saturatingTickAdd(
        confirmedTick,
        polledRetryFrames(audio::EvaEventType::LowPower));
    // The variation key is deliberately player-scoped rather than
    // object-scoped: a repeated outage should not reuse one sound forever.
    publish(
        audio::EvaEventType::LowPower, confirmedTick,
        (static_cast<uint64_t>(localPlayer->id.value) << 8u) ^
            0x4c4f57504f574552ull); // "LOWPOWER"
}

void detail::GameSessionWeaponEventDrain::publishObjectCashFloatingText(
    ObjectId object, LogicFixedVec3 position, int64_t signedAmount,
    uint32_t color, uint64_t confirmedTick) {
    if (signedAmount == 0) return;
    auto& presentation = m_presentation;
    const RenderObjectFeedbackGameData& settings =
        presentation.m_renderGameDataSettings.visual.objectFeedback;
    const uint32_t framesPerSecond = static_cast<uint32_t>(
        std::max(1, m_content.m_startInfo.gameSpeedFPS));
    uint64_t timeoutFrames = settings.floatingTextTimeoutMilliseconds == 0u
        ? std::max<uint64_t>(1u, framesPerSecond / 3u)
        : (static_cast<uint64_t>(settings.floatingTextTimeoutMilliseconds) *
               framesPerSecond +
           999u) /
              1000u;
    timeoutFrames = std::max<uint64_t>(1u, timeoutFrames);
    const uint64_t timeoutTick = saturatingTickAdd(
        confirmedTick, timeoutFrames);
    constexpr uint8_t kInitialAlpha = 255u;
    const uint64_t fadeFrames = floatingTextFadeFrames(
        kInitialAlpha, settings.floatingTextVanishPerSecond,
        framesPerSecond);
    const uint64_t expireTick = fadeFrames ==
            std::numeric_limits<uint64_t>::max()
        ? std::numeric_limits<uint64_t>::max()
        : saturatingTickAdd(timeoutTick, fadeFrames);

    if (presentation.m_objectFeedbackOrdinal !=
        std::numeric_limits<uint64_t>::max()) {
        ++presentation.m_objectFeedbackOrdinal;
    }
    if (presentation.m_objectFeedbackOrdinal == 0)
        presentation.m_objectFeedbackOrdinal = 1;
    constexpr size_t kMaximumObjectFeedbackEntries = 256u;
    if (presentation.m_objectFloatingTexts.size() >=
        kMaximumObjectFeedbackEntries) {
        presentation.m_objectFloatingTexts.erase(
            presentation.m_objectFloatingTexts.begin());
    }
    presentation.m_objectFloatingTexts.push_back({
        .identity = presentation.m_objectFeedbackOrdinal,
        .object = object,
        .worldAnchor = {
            position.x.to_float(), position.y.to_float(),
            position.z.to_float()},
        .amount = signedAmount,
        .color = color,
        .startTick = confirmedTick,
        .timeoutTick = timeoutTick,
        .expireTick = expireTick,
        .logicFramesPerSecond = framesPerSecond,
        .moveUpPerSecond = settings.floatingTextMoveUpPerSecond,
        .vanishPerSecond = settings.floatingTextVanishPerSecond,
    });
    if (presentation.m_scriptPresentationSequence !=
        std::numeric_limits<uint64_t>::max()) {
        ++presentation.m_scriptPresentationSequence;
    }
}

GameSessionObjectDeathFeedbackPublisher::
GameSessionObjectDeathFeedbackPublisher(
    GameSessionContentStartState& content,
    GameSessionScriptPresentationState& presentation,
    GameSessionGameplayPublicationPort publication) noexcept
    : m_content(content),
      m_presentation(presentation),
      m_publication(publication) {}

void GameSessionObjectDeathFeedbackPublisher::publish(
    const ObjectHealthEvent& event) {
    if (event.kind != ObjectHealthEventKind::Died ||
        event.source == event.object) {
        return;
    }
    const PlayerState* localPlayer =
        m_content.m_players.localPlayer();
    if (!localPlayer || event.victimPlayer != localPlayer->id) return;

    std::optional<audio::EvaEventType> evaType;
    if (event.victimEvaBuilding) {
        evaType = audio::EvaEventType::BuildingLost;
    } else if (event.victimEvaUnit) {
        evaType = audio::EvaEventType::UnitLost;

        const uint64_t framesPerSecond = static_cast<uint64_t>(
            std::max(1, m_content.m_startInfo.gameSpeedFPS));
        const uint64_t duplicateWindow = framesPerSecond * 10u;
        auto& history =
            m_presentation.m_objectLossRadarEvents;
        std::erase_if(history, [&](const ObjectLossRadarPresentationEvent& prior) {
            return event.confirmedTick >= prior.confirmedTick &&
                event.confirmedTick - prior.confirmedTick >= duplicateWindow;
        });
        const math::q32_32 duplicateDistance{int32_t{250}};
        const math::q32_32 duplicateDistanceSquared =
            duplicateDistance * duplicateDistance;
        const bool duplicate = std::any_of(
            history.begin(), history.end(),
            [&](const ObjectLossRadarPresentationEvent& prior) {
                if (event.confirmedTick < prior.confirmedTick ||
                    event.confirmedTick - prior.confirmedTick >=
                        duplicateWindow) {
                    return false;
                }
                const math::q32_32 dx =
                    event.victimPositionFixed.x - prior.position.x;
                const math::q32_32 dy =
                    event.victimPositionFixed.y - prior.position.y;
                return dx * dx + dy * dy <= duplicateDistanceSquared;
            });
        if (!duplicate) {
            if (history.size() >=
                script::ScriptMapPresentationState::kMaximumRadarEvents) {
                history.erase(history.begin());
            }
            history.push_back({
                .object = event.object,
                .position = event.victimPositionFixed,
                .confirmedTick = event.confirmedTick,
            });
            ++m_presentation.m_scriptPresentationSequence;
            if (m_presentation.m_scriptPresentationSequence == 0)
                ++m_presentation.m_scriptPresentationSequence;
        }
    }
    if (!evaType) return;

    GameSessionEvaEventPublisher{
        m_content,
        m_publication}
        .publish(
        *evaType, event.confirmedTick,
        static_cast<uint64_t>(event.object.value) << 32u);
}

} // namespace engine
