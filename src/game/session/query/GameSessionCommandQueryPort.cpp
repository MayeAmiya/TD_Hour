#include "game/session/query/GameSessionCommandQueryPort.h"

#include "game/session/state/GameSessionDomainState.h"
#include "game/session/command/GameSessionPlayerCommandPolicy.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "game/command/CommandButtonStore.h"
#include "game/data/base/ScienceCatalog.h"

#include <algorithm>
#include <optional>

namespace engine {

bool GameSessionCommandQueryPort::objectForbidsPlayerCommands(
    ObjectId object) const noexcept {
    return session_command_policy::objectForbidsPlayerCommands(
        m_world->m_registry, m_world->m_objects, object);
}

bool GameSessionCommandQueryPort::isCommandPlayer(
    PlayerId player) const noexcept {
    const PlayerState* state = m_content->m_players.get(player);
    return state && state->isCommandPlayer();
}

bool GameSessionCommandQueryPort::isControllableBeacon(
    PlayerId player, ObjectId object) const noexcept {
    const auto& world = *m_world;
    const std::optional<ecs::entity> entity =
        world.m_objects.entityFromId(object);
    if (!entity || world.m_ownership.ownerOf(object) !=
            std::optional<PlayerId>{player}) {
        return false;
    }
    const ThingTemplateComponent* objectTemplate =
        ecs::try_get<ThingTemplateComponent>(world.m_registry, *entity);
    return objectTemplate && objectTemplate->archetype &&
        objectTemplate->archetype->techBuildingPlan &&
        !objectTemplate->archetype->techBuildingPlan->beacons.empty();
}

bool GameSessionCommandQueryPort::hasOrderCapability(
    ObjectId object, ai::ObjectAIOrderCapability capability) const noexcept {
    return m_ai && m_ai->m_objectAI.hasOrderCapability(object, capability);
}

std::optional<GameCommand>
GameSessionCommandQueryPort::composeSciencePurchase(
    SciencePurchaseRequest request) const {
    if (request.buttonStableId == 0 || request.commandButtonName.empty() ||
        request.science.empty()) {
        return std::nullopt;
    }
    const auto& content = *m_content;
    const PlayerState* localPlayer = content.m_players.localPlayer();
    const game::CommandButtonTemplate* button =
        content.m_contentSnapshot.findCommandButton(
            request.commandButtonName);
    const ScienceCatalog* sciences = content.m_contentSnapshot.scienceCatalog();
    if (!localPlayer || !localPlayer->isCommandPlayer() || !button ||
        button->descriptor.stableId != request.buttonStableId ||
        button->descriptor.kind != game::CommandButtonKind::PurchaseScience ||
        !button->descriptor.userActivatable() || !sciences) {
        return std::nullopt;
    }
    const bool authoredScience =
        (!button->sciences.empty() &&
         std::find(button->sciences.begin(), button->sciences.end(),
                   request.science) != button->sciences.end()) ||
        (button->sciences.empty() && button->science == request.science);
    if (!authoredScience) return std::nullopt;
    const ScienceDefinition* selected = sciences->find(request.science);
    if (!selected || !selected->grantable ||
        !content.m_players.canPurchaseScience(localPlayer->id, *selected)) {
        return std::nullopt;
    }

    GameCommand command;
    command.tick = request.tick;
    command.player = localPlayer->id;
    command.source = CommandSource::Local;
    command.type = GameCommandType::PurchaseScience;
    command.commandName = selected->name;
    command.activation = {
        .requestSequence = request.requestSequence,
        .buttonStableId = request.buttonStableId,
        .commandKind = game::CommandButtonKind::PurchaseScience,
    };
    return command;
}

} // namespace engine
