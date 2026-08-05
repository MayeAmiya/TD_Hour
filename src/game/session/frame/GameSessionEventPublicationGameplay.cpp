#include "game/session/frame/GameSessionGameplayEventCollector.h"
#include "game/session/frame/GameSessionEvaEventPublisher.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/object/simulation/special/ObjectSpecialPower.h"

namespace engine {

void GameSessionGameplayEventCollector::publishSuperweaponLaunched(
    const ObjectSpecialPowerExecutionEvent& event) {
    const PlayerState* observer = m_content.m_players.localPlayer();
    if (!observer || !event.player) return;
    const SpecialPowerDefinition* definition =
        m_content.m_contentSnapshot.findSpecialPower(event.content);
    if (!definition) return;
    const std::optional<audio::EvaEventType> evaType = evaSuperweaponEvent(
        definition->specialPowerType,
        EvaSuperweaponAnnouncement::Launched,
        evaSuperweaponAudience(
            observer->id == event.player,
            m_content.m_players.relationship(observer->id, event.player)));
    if (!evaType) return;
    GameSessionEvaEventPublisher{m_content, m_publication}.publish(
        *evaType, event.confirmedTick,
        (static_cast<uint64_t>(event.source.value) << 32u) ^
            static_cast<uint64_t>(event.readyTick));
}

void GameSessionGameplayEventCollector::collectSpecialPowerProducerEvents() {
    // Containment gameplay consequences are consumed by the typed gameplay
    // transaction executor. This collector retains only player/script-facing
    // SpecialPower execution facts.
    container::Vector<ObjectSpecialPowerExecutionEvent> events =
        m_world.m_objectSimulation
            .takeSpecialPowerExecutionEvents();
    for (ObjectSpecialPowerExecutionEvent& event : events) {
        if (m_content.m_active &&
            event.commandSource == ObjectOrderSource::Player &&
            event.player && event.source && event.sourceSequence != 0) {
            const bool accepted =
                event.status == ObjectSpecialPowerExecutionStatus::Activated ||
                event.status ==
                    ObjectSpecialPowerExecutionStatus::Approaching;
            m_presentation.m_commandBackendOutcomes.push_back({
                .player = event.player,
                .source = event.source,
                .sourceSequence = event.sourceSequence,
                .kind = CommandBackendKind::SpecialPower,
                .accepted = accepted,
                .confirmedTick =
                    static_cast<GameTick>(event.confirmedTick),
            });
        }
        if (event.status == ObjectSpecialPowerExecutionStatus::Activated) {
            // SpecialPowerModule::aboutToDoSpecialPower announces the launch
            // to everybody, choosing between the own / ally / enemy line by
            // the observing player's relationship to the firing player. The
            // authored TimeBetweenChecksMS in Eva.ini is what keeps a rapid
            // pair of launches from stacking, so no extra gate belongs here.
            publishSuperweaponLaunched(event);
        }
        if (event.status == ObjectSpecialPowerExecutionStatus::Activated &&
            event.scriptTriggered) {
            const SpecialPowerDefinition* definition =
                m_content.m_contentSnapshot
                    .findSpecialPower(event.content);
            if (definition) {
                static_cast<void>(m_presentation
                    .m_scriptGameplayEvents.recordSpecialPower({
                        .phase = script::ScriptSpecialPowerEventPhase::Triggered,
                        .player = event.player,
                        .source = event.source,
                        .specialPower = definition->name,
                        .confirmedTick = event.confirmedTick,
                    }));
            }
        }
    }
}

} // namespace engine
