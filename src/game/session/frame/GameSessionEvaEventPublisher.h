#pragma once

#include "presentation/audio/EvaEventContracts.h"
#include "game/data/base/SpecialPowerType.h"
#include "game/player/PlayerRelationshipMatrix.h"
#include "game/session/transaction/GameSessionGameplayPublicationPort.h"

#include <cstdint>
#include <optional>

namespace engine {

class GameSessionContentStartState;
class GameSessionScriptPresentationState;

// RefCode spells the superweapon EVA vocabulary out three times, once per
// announcement, each as a hand-written SpecialPowerType ladder:
// Player::onStructureConstructionComplete for DETECTED,
// InGameUI::draw for READY and SpecialPowerModule::aboutToDoSpecialPower for
// LAUNCHED. Folding the shared type->family and relationship->audience
// classification into one place keeps the three call sites from drifting.
enum class EvaSuperweaponAnnouncement : uint8_t {
    Detected,
    Launched,
    Ready,
};

// RefCode compares the observing player against the owning player and treats
// everything that is not explicitly ENEMIES - including NEUTRAL - as an ally.
enum class EvaSuperweaponAudience : uint8_t {
    Own,
    Ally,
    Enemy,
};

[[nodiscard]] EvaSuperweaponAudience evaSuperweaponAudience(
    bool ownedByObserver, PlayerRelationship observerToOwner) noexcept;

// Returns no value for any power RefCode does not announce, including the
// GPS scrambler and sneak attack under Detected/Ready: those two only ever
// got LAUNCHED lines authored in Eva.ini.
[[nodiscard]] std::optional<audio::EvaEventType> evaSuperweaponEvent(
    game::SpecialPowerType type, EvaSuperweaponAnnouncement announcement,
    EvaSuperweaponAudience audience) noexcept;

// Resolves one confirmed EVA event against frozen content and emits a
// detached audio request. It owns no cooldown runtime or audio backend.
class GameSessionEvaEventPublisher final {
public:
    GameSessionEvaEventPublisher(
        GameSessionContentStartState& content,
        GameSessionGameplayPublicationPort publication) noexcept
        : m_content(content), m_publication(publication) {}

    void publish(
        audio::EvaEventType type, uint64_t confirmedTick,
        uint64_t variationKey);

    // RefCode's Eva owns two shapes of trigger. Most events are edge-driven:
    // a gameplay decision calls setShouldPlay() once and update() consumes the
    // flag on the next client frame. LowPower is instead a predicate that
    // Eva::shouldPlayLowPower re-evaluates whenever no check for it is
    // outstanding. This method carries that polled family; it is observer-local
    // and writes only presentation state.
    void publishPolledObserverConditions(
        GameSessionScriptPresentationState& presentation,
        uint64_t confirmedTick);

    // Drains announcements decided inside authoritative transactions that own
    // no publication port of their own. Each entry already carries the
    // observer-relative announcement chosen at the decision site.
    void publishPendingAnnouncements(
        GameSessionScriptPresentationState& presentation);

private:
    // Frames after which an offer that never reached the speech channel may be
    // made again, taken from the authored ExpirationTimeMS.
    [[nodiscard]] uint32_t polledRetryFrames(
        audio::EvaEventType type) const noexcept;


    GameSessionContentStartState& m_content;
    GameSessionGameplayPublicationPort m_publication;
};

} // namespace engine
