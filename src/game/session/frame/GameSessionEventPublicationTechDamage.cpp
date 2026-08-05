#include "game/session/frame/GameSessionFxAnchorSnapshot.h"
#include "game/session/frame/GameSessionObjectEventPublisher.h"
#include "game/session/presentation/GameSessionPresentationDetail.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/weapon/GameSessionWeaponEventDrainDetail.h"
#include "game/object/simulation/structure/ObjectTechBuilding.h"
#include "presentation/render/SupportDrawPresentation.h"
#include "debug/debug.h"

namespace engine {

void GameSessionObjectEventPublisher::publishTechAndBeacon() {
    container::Vector<ObjectTechBuildingEvent> techEvents =
        m_world.m_objectSimulation.takeTechBuildingEvents();
    for (const ObjectTechBuildingEvent& event : techEvents) {
        if (event.kind == ObjectTechBuildingEventKind::PulseFx &&
            !event.fxList.empty()) {
            const std::optional<game::FxInvocationAnchor> anchor =
                session_fx::snapshotAnchor(
                    m_world.m_registry, m_world.m_objects, event.object);
            if (anchor) {
                static_cast<void>(m_publication.emitFxInvocationEvent({
                    .fxListName = event.fxList,
                    .anchorKind =
                        game::FxInvocationAnchorKind::ObjectAttachment,
                    .primary = *anchor,
                }));
            }
        }
    }
    container::Vector<ObjectBeaconClientEvent> beaconEvents =
        m_world.m_objectSimulation.takeBeaconClientEvents();
    constexpr char hexDigits[] = "0123456789ABCDEF";
    for (ObjectBeaconClientEvent& event : beaconEvents) {
        const math::vec3 color =
            game_session_presentation_detail::resolveScriptIndicatorColor(
                m_presentation.m_scriptObjectPresentation,
                m_world.m_registry,
                m_content.m_players,
                m_content.m_ruleset.get(),
                m_world.m_objects.entityFromId(event.object),
                event.object);
        const auto byte = [](float value) {
            return static_cast<uint32_t>(std::clamp(
                std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f),
                0l, 255l));
        };
        event.indicatorColorRgb =
            (byte(color.x()) << 16u) |
            (byte(color.y()) << 8u) |
            byte(color.z());
        if (event.kind == ObjectBeaconClientEventKind::RadarPulse) {
            auto& presentation = m_presentation;
            if (presentation.m_renderBeaconRadarEpoch !=
                presentation.m_scriptPresentationEpoch) {
                presentation.m_renderBeaconRadarHistory.clear();
                presentation.m_renderBeaconRadarEpoch =
                    presentation.m_scriptPresentationEpoch;
            }
            presentation.m_renderBeaconRadarHistory.push_back(event);
        }
        if (event.kind != ObjectBeaconClientEventKind::ShowSmoke &&
            event.kind != ObjectBeaconClientEventKind::Hide) {
            continue;
        }
        const uint64_t attachmentGroup =
            render::beaconAttachmentGroup(
                event.object.value, event.authoredOrder);
        game::FxInvocationEvent invocation{
            .control = event.kind == ObjectBeaconClientEventKind::Hide
                ? game::FxInvocationControlKind::StopAttachedParticleGroup
                : game::FxInvocationControlKind::Execute,
            .anchorKind = game::FxInvocationAnchorKind::ObjectAttachment,
            .primary = game::FxInvocationAnchor{
                .object = event.object,
                .position = math::vec3{
                    event.position.x.to_float(),
                    event.position.y.to_float(),
                    event.position.z.to_float()},
            },
            .attachmentGroup = attachmentGroup,
        };
        if (event.kind == ObjectBeaconClientEventKind::ShowSmoke) {
            game::FxInvocationEvent replaceStop = invocation;
            replaceStop.control =
                game::FxInvocationControlKind::StopAttachedParticleGroup;
            static_cast<void>(m_publication.emitFxInvocationEvent(
                std::move(replaceStop)));
            container::String particleName = "BeaconSmoke000000";
            for (size_t nibble = 0; nibble < 6u; ++nibble) {
                const uint32_t shift = static_cast<uint32_t>(
                    (5u - nibble) * 4u);
                particleName[11u + nibble] =
                    hexDigits[(event.indicatorColorRgb >> shift) & 0x0fu];
            }
            invocation.directParticle = game::FxDirectParticleRequest{
                .particleSystemName = std::move(particleName),
                .fallbackParticleSystemName = "BeaconSmokeFFFFFF",
                .fallbackColorKeyTint = math::vec3{
                    static_cast<float>((event.indicatorColorRgb >> 16u) & 0xffu) /
                        255.0f,
                    static_cast<float>((event.indicatorColorRgb >> 8u) & 0xffu) /
                        255.0f,
                    static_cast<float>(event.indicatorColorRgb & 0xffu) / 255.0f,
                },
                .emitterCount = 1,
                .attachToObject = true,
            };
        }
        static_cast<void>(m_publication.emitFxInvocationEvent(
            std::move(invocation)));
    }
}

} // namespace engine
