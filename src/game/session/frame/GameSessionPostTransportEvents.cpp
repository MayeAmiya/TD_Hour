#include "game/session/core/GameSessionDomainComposition.h"
#include "game/session/frame/GameSessionFxAnchorSnapshot.h"
#include "core/container/string_utils.h"

#include "game/object/component/ObjectDirty.h"

#include <algorithm>
#include <type_traits>
#include <variant>

namespace engine::detail {

void GameSessionDomainComposition::publishPostCommandTransportEvents() {
    auto events = m_world.m_objectSimulation
                      .takeTransportPresentationEvents();
    for (ObjectTransportPresentationEvent& event : events) {
        std::visit([&](auto& payload) {
            using Event = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<
                              Event, ObjectTransportSeismicPresentation>) {
                // ZH compiles this optional terrain dome only with
                // DO_SEISMIC_SIMULATIONS. It has no authoritative effect.
                return;
            } else if constexpr (std::is_same_v<
                                     Event,
                                     ObjectTransportAudioPresentation>) {
                if (payload.eventName.empty()) return;
                static_cast<void>(m_publication.emitAudioEvent({
                    .eventName = payload.eventName,
                    .emitter = payload.object,
                    .owner = payload.object,
                    .position = math::vec3{
                        payload.x.to_float(), payload.y.to_float(),
                        payload.z.to_float()},
                }));
            } else if constexpr (std::is_same_v<
                                     Event,
                                     ObjectTransportFxPresentation>) {
                if (payload.fxList.empty()) return;
                const ObjectId anchorObject = payload.target
                    ? payload.target : payload.object;
                const std::optional<game::FxInvocationAnchor> anchor =
                    session_fx::snapshotAnchor(
                        m_world.m_registry,
                        m_world.m_objects, anchorObject);
                static_cast<void>(m_publication.emitFxInvocationEvent({
                    .fxListName = payload.fxList,
                    .anchorKind = anchor
                        ? game::FxInvocationAnchorKind::ObjectAttachment
                        : game::FxInvocationAnchorKind::WorldPosition,
                    .primary = anchor.value_or(session_fx::worldAnchor(
                        {payload.x.to_float(), payload.y.to_float(),
                         payload.z.to_float()},
                        anchorObject)),
                }));
            } else {
                static_assert(std::is_same_v<
                    Event, ObjectTransportDeliveryStartedPresentation>);
                static_cast<void>(m_world
                    .m_objectSimulation.killRadiusDecal(
                        m_world.m_registry,
                        m_world.m_objects,
                        payload.transport, payload.confirmedTick));
                if (!payload.decalTexture.empty() &&
                    payload.radius > math::q32_32{}) {
                    static_cast<void>(m_world
                        .m_objectSimulation.createRadiusDecal(
                            m_world.m_registry,
                            m_world.m_objects,
                            {.object = payload.transport,
                             .texture = payload.decalTexture,
                             .position = {
                                 payload.x, payload.y, payload.z},
                             .radius = payload.radius,
                             .shadowTypeMask =
                                 payload.decalShadowTypeMask,
                             .minimumOpacity =
                                 payload.decalMinimumOpacity,
                             .maximumOpacity =
                                 payload.decalMaximumOpacity,
                             .opacityThrobTicks =
                                 payload.decalOpacityThrobTicks,
                             .color = payload.decalColor,
                             .usesPlayerColor =
                                 payload.decalUsesPlayerColor,
                             .onlyVisibleToOwningPlayer =
                                 payload.decalOnlyVisibleToOwningPlayer,
                             .confirmedTick = payload.confirmedTick}));
                }
                const std::optional<ecs::entity> source =
                    m_world.m_objects
                        .entityFromIdIncludingPending(payload.transport);
                if (!source || payload.subObjectBaseName.empty() ||
                    payload.visibleSubObjectCount == 0) {
                    return;
                }
                ObjectSubObjectVisibilityOverrideComponent* overrides =
                    ecs::try_get<
                        ObjectSubObjectVisibilityOverrideComponent>(
                            m_world.m_registry,
                            *source);
                if (!overrides) {
                    overrides = &ecs::emplace<
                        ObjectSubObjectVisibilityOverrideComponent>(
                            m_world.m_registry,
                            *source);
                }
                for (uint32_t ordinal = 1;
                     ordinal <= payload.visibleSubObjectCount; ++ordinal) {
                    container::String name = payload.subObjectBaseName;
                    if (ordinal < 10u) name.push_back('0');
                    name += std::to_string(ordinal);
                    auto found = std::find_if(
                        overrides->entries.begin(),
                        overrides->entries.end(),
                        [&](const ObjectSubObjectVisibilityOverride& entry) {
                            return container::asciiEqualIgnoreCase(
                                entry.name, name);
                        });
                    if (found == overrides->entries.end()) {
                        overrides->entries.push_back({
                            .name = std::move(name),
                            .visible = true,
                            .active = true,
                        });
                    } else {
                        found->visible = true;
                        found->active = true;
                    }
                }
                ++overrides->revision;
                markObjectDirty(
                    m_world.m_registry, *source,
                    ObjectDirtyDomain::RenderExtraction);
            }
        }, event.payload);
    }
}

} // namespace engine::detail
