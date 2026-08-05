#pragma once

#include "core/container/container_types.h"

namespace engine {

class GameSessionContentStartState;
class GameSessionGameplayPublicationPort;
class GameSessionScriptPresentationState;
class GameSessionWorldState;
struct ObjectDeathEvent;
struct ObjectAirfieldEvent;
struct ObjectCountermeasureEvent;
struct ObjectMovementEvent;
struct ObjectWeaponBonusUpdateEvent;

// Drains confirmed object feedback once and projects it into bounded,
// presentation-owned journals. It owns neither ECS nor renderer state.
class GameSessionObjectFeedbackPublisher final {
public:
    GameSessionObjectFeedbackPublisher(
        GameSessionContentStartState& content,
        GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        GameSessionGameplayPublicationPort& publication) noexcept
        : m_content(content),
          m_world(world),
          m_presentation(presentation),
          m_publication(publication) {}

    void publish();

private:
    // Per-unit death and movement cues from ThingTemplate's authored audio
    // family. Both drains used to be discarded outright, which is why no unit
    // ever made a sound when it started moving or died.
    void publishDeathAudio(container::Vector<ObjectDeathEvent> events);
    void publishMovementAudio(container::Vector<ObjectMovementEvent> events);
    void publishAirfieldFeedback(
        container::Vector<ObjectAirfieldEvent> events);
    // WeaponBonus is already consumed by the confirmed ModelCondition/FRENZY
    // and ObjectUi snapshot paths. These events make that existing snapshot
    // boundary visible in the same confirmed frame without inventing FX.
    void publishWeaponBonusFeedback(
        container::Vector<ObjectWeaponBonusUpdateEvent> events);
    [[nodiscard]] uint64_t nextFeedbackIdentity() noexcept;

    GameSessionContentStartState& m_content;
    GameSessionWorldState& m_world;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionGameplayPublicationPort& m_publication;
};

} // namespace engine
