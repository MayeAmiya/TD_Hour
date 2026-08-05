#include "core/container/container_types.h"
#include "game/session/query/LocalSelectionCommandBarPresentationConsumer.h"

#include "game/object/definition/ObjectArchetype.h"
#include "game/object/definition/ThingFactory.h"
#include "game/object/component/ObjectComponentsContainmentEnergy.h"
#include "game/object/component/ObjectComponentsCoreLifecycleOrder.h"
#include "game/object/component/ObjectComponentsPresentationModel.h"
#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
namespace engine::selection {

std::optional<LocalCommandBarSelection>
LocalSelectionCommandBarPresentationConsumer::resolveSingleObject(
    const GameSession& session, const LocalSelectionState& selection) {
    // Multi-select uses a separate common-command calculation in RefCode.
    // Until that presentation producer exists, selecting an arbitrary member
    // would make a map-script override appear on the wrong ControlBar.
    const container::Span<const ObjectId> selected = selection.selected();
    const GameSessionStateRoot& state = session.domainState();
    const GameSessionContentStartState& content = state.contentState();
    const GameSessionWorldState& world = state.worldState();
    if (!content.m_active || selected.size() != 1 || !selected.front()) {
        return std::nullopt;
    }

    const std::optional<ecs::entity> entity =
        world.m_objects.entityFromId(selected.front());
    // entityFromId rejects PendingDestroy objects. Recheck the stable identity
    // too, so a malformed/stale ObjectId-to-entity association cannot cross
    // this local UI boundary.
    if (!entity) return std::nullopt;
    const ObjectIdentityComponent* identity =
        ecs::try_get<ObjectIdentityComponent>(world.m_registry, *entity);
    if (!identity || identity->id != selected.front()) return std::nullopt;

    const ThingTemplateComponent* templateComponent =
        ecs::try_get<ThingTemplateComponent>(world.m_registry, *entity);
    const RenderModelComponent* visual =
        ecs::try_get<RenderModelComponent>(world.m_registry, *entity);
    const ObjectHealthComponent* health =
        ecs::try_get<ObjectHealthComponent>(world.m_registry, *entity);
    if (!templateComponent || !templateComponent->archetype ||
        !visual || visual->modelAsset.empty() || visual->hidden ||
        (health && health->effectivelyDead)) {
        return std::nullopt;
    }

    // The selected type must be the exact immutable archetype frozen when
    // this session began. Do not let a local UI action query ThingFactory or
    // use a half-initialized component name after a content reload.
    const container::StringView componentName = templateComponent->name;
    const container::StringView archetypeName = templateComponent->archetype->templateData.name;
    if (componentName.empty() || archetypeName.empty() || componentName != archetypeName) {
        return std::nullopt;
    }
    const container::SharedPtr<const game::ObjectArchetype> frozenArchetype =
        content.m_contentSnapshot.findObjectArchetype(componentName);
    if (!frozenArchetype || frozenArchetype.get() != templateComponent->archetype.get()) {
        return std::nullopt;
    }

    return LocalCommandBarSelection{
        .object = selected.front(),
        .objectType = container::String{componentName},
    };
}

std::optional<container::String>
LocalSelectionCommandBarPresentationConsumer::resolveSingleObjectType(
    const GameSession& session, const LocalSelectionState& selection) {
    const std::optional<LocalCommandBarSelection> resolved =
        resolveSingleObject(session, selection);
    return resolved ? std::optional<container::String>{resolved->objectType} : std::nullopt;
}

} // namespace engine::selection
