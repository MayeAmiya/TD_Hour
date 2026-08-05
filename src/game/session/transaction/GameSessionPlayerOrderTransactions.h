#pragma once

#include "game/command/GameCommand.h"
#include "game/session/command/OrderContracts.h"
#include "math/fixed/q32_32.h"

namespace engine
{

namespace ai {
class ObjectAIRuntime;
struct ObjectAIOrderCapabilitySnapshot;
}

enum class ObjectIntentionalContactKind : uint8_t;
class GameSessionContentStartState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;

// Canonical deterministic PlayerOrder admission shared by confirmed player
// commands and strategic AI. Producers submit values; only this transaction
// filters authored command policy and mutates object order queues.
class GameSessionPlayerOrderTransactions final
{
public:
    GameSessionPlayerOrderTransactions(GameSessionContentStartState& content,
                                       GameSessionScriptPresentationState& presentation,
                                       GameSessionWorldState& world,
                                       ai::ObjectAIRuntime& objectAI,
                                       ai::ObjectAIOrderCapabilitySnapshot&
                                           orderCapabilityScratch) noexcept
        : m_content(content)
        , m_presentation(presentation)
        , m_world(world)
        , m_objectAI(objectAI)
        , m_orderCapabilityScratch(orderCapabilityScratch)
    {
    }

    [[nodiscard]] OrderExecutionResult execute(const PlayerOrder& order);
    [[nodiscard]] OrderExecutionResult toggleFormation(
        const GameCommand& command);
    [[nodiscard]] OrderExecutionResult executeIntentionalContact(
        const GameCommand& command,
        container::Span<const ObjectId> admittedActors,
        ObjectIntentionalContactKind contactKind);
    [[nodiscard]] bool executeRailedTransport(
        ObjectId transport, uint64_t confirmedTick);
    [[nodiscard]] bool stagePendingEvacuation(
        ObjectId container, PlayerId player,
        uint64_t externalOrderRevision, uint64_t issuedTick,
        uint64_t deadlineTick, uint32_t sourceSequence,
        math::q32_32 landingZ, bool previousUsePreciseZPosition);

private:
    GameSessionContentStartState& m_content;
    GameSessionScriptPresentationState& m_presentation;
    GameSessionWorldState& m_world;
    ai::ObjectAIRuntime& m_objectAI;
    ai::ObjectAIOrderCapabilitySnapshot& m_orderCapabilityScratch;
};

} // namespace engine
