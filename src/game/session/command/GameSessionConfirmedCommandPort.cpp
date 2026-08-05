#include "game/session/command/GameSessionConfirmedCommandPort.h"

#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/query/GameSessionObjectQueryPort.h"
#include "game/session/transaction/GameSessionBuildPlacementEvaluator.h"
#include "game/session/transaction/GameSessionObjectProductionTransactions.h"
#include "game/session/transaction/GameSessionObjectStateTransactions.h"
#include "game/session/transaction/GameSessionTransactionPorts.h"

namespace engine
{

GameSessionStateRoot& GameSessionConfirmedCommandPort::domainState() noexcept
{
    return *m_state;
}

const GameSessionStateRoot& GameSessionConfirmedCommandPort::domainState() const noexcept
{
    return *m_state;
}

bool GameSessionConfirmedCommandPort::isActive() const noexcept
{
    return m_state->contentState().m_active;
}

uint64_t GameSessionConfirmedCommandPort::confirmedTick() const noexcept
{
    return m_state->presentationState().m_confirmedTick;
}

GameSessionObjectQueryPort GameSessionConfirmedCommandPort::objectQuery() const noexcept
{
    return GameSessionObjectQueryPort{m_state->worldState().m_objects};
}

GameSessionBuildPlacementLegalityEvaluation GameSessionConfirmedCommandPort::evaluateBuildPlacementFixed(
    ObjectId source,
    LogicFixedVec3 position,
    math::q32_32 yawRadians,
    PlayerId player,
    const game::ObjectArchetype& product,
    bool consumeBuildability)
{
    GameSessionStateRoot& state = domainState();
    return GameSessionBuildPlacementEvaluator{
        state.contentState(), state.aiState(), state.presentationState(), state.worldState()}
        .evaluateFixed(source, position, yawRadians, player, product, consumeBuildability);
}

GameSessionCaptionCommandResult GameSessionConfirmedCommandPort::setBeaconText(PlayerId player,
                                                                               container::Span<const ObjectId> actors,
                                                                               container::StringView text,
                                                                               uint64_t confirmedTick)
{
    GameSessionCaptionCommandResult result;
    GameSessionStateRoot& state = domainState();
    if (!state.contentState().m_active || !state.presentationState().m_hasConfirmedFrame ||
        confirmedTick != state.presentationState().m_confirmedTick)
    {
        result.message = "beacon-text command is outside the confirmed frame";
        return result;
    }
    const PlayerState* playerState = state.contentState().m_players.get(player);
    if (!playerState || !playerState->isCommandPlayer())
    {
        result.message = "beacon-text command has no live command player";
        return result;
    }
    if (actors.size() != 1u)
    {
        result.message = "beacon-text command requires exactly one actor";
        return result;
    }

    ObjectId previous = INVALID_OBJECT_ID;
    for (const ObjectId actor : actors)
    {
        if (!actor || (previous && !(previous < actor)))
        {
            result.message = "beacon-text actors are not canonical unique ObjectIds";
            return result;
        }
        if (state.worldState().m_ownership.ownerOf(actor) != std::optional<PlayerId>{player} ||
            state.worldState().m_objects.isPendingDestroy(actor))
        {
            result.message = "beacon-text actor is stale or not controlled by the player";
            return result;
        }
        const std::optional<ecs::entity> entity = state.worldState().m_objects.entityFromId(actor);
        const ThingTemplateComponent* type =
            entity ? ecs::try_get<ThingTemplateComponent>(state.worldState().m_registry, *entity) : nullptr;
        if (!type || !type->archetype || !type->archetype->techBuildingPlan ||
            type->archetype->techBuildingPlan->beacons.empty())
        {
            result.message = "beacon-text actor is not a Beacon object";
            return result;
        }
        previous = actor;
    }

    result.accepted = true;
    result.actorCount = actors.size();
    for (const ObjectId actor : actors)
    {
        if (setObjectDrawableCaption(actor, text, confirmedTick))
        {
            ++result.changedCount;
        }
    }
    return result;
}

bool GameSessionConfirmedCommandPort::setObjectDrawableCaption(ObjectId object,
                                                               container::StringView text,
                                                               uint64_t confirmedTick)
{
    GameSessionStateRoot& state = domainState();
    if (!state.contentState().m_active || !state.presentationState().m_hasConfirmedFrame ||
        confirmedTick != state.presentationState().m_confirmedTick || !object ||
        state.worldState().m_objects.isPendingDestroy(object))
    {
        return false;
    }
    return GameSessionObjectStateTransactions{
        state.worldState().m_registry, state.worldState().m_objects}
        .setDrawableCaption(object, text, confirmedTick);
}

GameSessionProductionCommandResult GameSessionConfirmedCommandPort::queueProduction(
    ObjectId producer, PlayerId player, container::StringView product, uint32_t sourceSequence, uint64_t confirmedTick)
{
    return productionTransactions().queueProduction(producer, player, product, sourceSequence, confirmedTick);
}

GameSessionProductionCommandResult GameSessionConfirmedCommandPort::cancelProduction(ObjectId producer,
                                                                                     PlayerId player,
                                                                                     uint32_t productionId,
                                                                                     uint64_t confirmedTick)
{
    return productionTransactions().cancelProduction(producer, player, productionId, confirmedTick);
}

GameSessionProductionCommandResult GameSessionConfirmedCommandPort::queuePlayerUpgrade(
    ObjectId producer, PlayerId player, container::StringView upgrade, uint32_t sourceSequence, uint64_t confirmedTick)
{
    return productionTransactions().queuePlayerUpgrade(producer, player, upgrade, sourceSequence, confirmedTick);
}

GameSessionProductionCommandResult GameSessionConfirmedCommandPort::cancelPlayerUpgrade(ObjectId producer,
                                                                                        PlayerId player,
                                                                                        container::StringView upgrade,
                                                                                        uint64_t confirmedTick)
{
    return productionTransactions().cancelPlayerUpgrade(producer, player, upgrade, confirmedTick);
}

GameSessionProductionCommandResult GameSessionConfirmedCommandPort::setFactoryRallyPoint(ObjectId producer,
                                                                                         PlayerId player,
                                                                                         LogicFixedVec3 position,
                                                                                         uint64_t confirmedTick)
{
    return productionTransactions().setFactoryRallyPoint(producer, player, position, confirmedTick);
}

GameSessionObjectProductionTransactions GameSessionConfirmedCommandPort::productionTransactions() noexcept
{
    return m_production;
}

} // namespace engine
