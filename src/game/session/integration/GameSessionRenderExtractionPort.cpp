#include "game/session/state/GameSessionDomainState.h"
#include "game/session/integration/GameSessionRenderExtractionPort.h"

#include "GameRenderExtraction.h"
#include "GameGroundDecalExtraction.h"
#include "game/object/component/ObjectDirty.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace engine {
namespace {

// 提取器只读 confirmed ECS。dirty 确认是组合根在完整 snapshot 已生成后的
// 独立提交步骤，不能混进提取过程并令失败/早退留下半确认状态。
void acknowledgeRenderExtraction(ecs::registry& registry) {
    container::Vector<ecs::entity> dirtyEntities;
    const auto view = ecs::view<const ObjectDirtyComponent>(registry);
    for (const ecs::entity entity : view) {
        if ((view.template get<const ObjectDirtyComponent>(entity).domains &
             objectDirtyBit(ObjectDirtyDomain::RenderExtraction)) != 0) {
            dirtyEntities.push_back(entity);
        }
    }
    for (const ecs::entity entity : dirtyEntities) {
        clearObjectDirty(
            registry, entity, ObjectDirtyDomain::RenderExtraction);
    }
}

} // namespace

render::RenderCameraSnapshot GameSessionRenderExtractionPort::camera(
    const GameCameraState& camera) const noexcept {
    return GameRenderExtraction::extractCamera(camera);
}

render::WorldRenderSnapshot GameSessionRenderExtractionPort::world(
    render::RenderCameraSnapshot camera,
    uint64_t simulationFrame,
    container::Span<const ObjectId> localSelection,
    ObjectId localHover,
    bool showPlayerWaypoints,
    bool includeVisualAssetDependencies) {
    render::WorldRenderSnapshot snapshot = GameRenderExtraction::extract(
        *m_content, *m_world, *m_presentation, *m_objectEvents, *m_cache,
        camera, simulationFrame, localSelection, localHover,
        showPlayerWaypoints,
        includeVisualAssetDependencies);
    acknowledgeRenderExtraction(m_world->m_registry);
    return snapshot;
}

render::GroundDecalPresentationBatch
GameSessionRenderExtractionPort::takeGroundDecals() {
    GameSessionWorldState& world = *m_world;
    GameSessionObjectEventState& events = *m_objectEvents;
    const GameSessionContentStartState& content = *m_content;
    GameSessionScriptPresentationState& presentation = *m_presentation;

    container::Vector<ObjectRadiusDecalEvent> radiusEvents =
        world.m_objectSimulation.takeRadiusDecalEvents();
    radiusEvents.insert(
        radiusEvents.end(),
        std::make_move_iterator(events.m_projectileRadiusDecalEvents.begin()),
        std::make_move_iterator(events.m_projectileRadiusDecalEvents.end()));
    events.m_projectileRadiusDecalEvents.clear();

    return render_extraction_detail::extractGroundDecalPresentation({
        .registry = world.m_registry,
        .players = content.m_players,
        .ruleset = GameSessionRulesetQueryPort{content.m_ruleset.get()},
        .presentationEpoch = presentation.m_scriptPresentationEpoch,
        .confirmedFrame = presentation.m_confirmedTick,
        .logicFramesPerSecond = static_cast<uint32_t>(std::max(
            1, content.m_startInfo.gameSpeedFPS)),
        .drawIconUiEnabled =
            presentation.m_scriptClientOptions.drawIconUiEnabled(),
        .radiusEvents = std::move(radiusEvents),
        .dynamicShroudEvents =
            world.m_objectSimulation.takeDynamicShroudDecalEvents(),
    });
}

} // namespace engine
