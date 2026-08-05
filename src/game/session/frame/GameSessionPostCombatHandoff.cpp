#include "game/session/core/GameSessionDomainComposition.h"
#include "game/object/definition/ObjectArchetype.h"

#include "game/object/simulation/movement/ObjectPositionAuthority.h"

namespace engine::detail {

void GameSessionDomainComposition::finalizePostCommandSimulationHandoff(
    const GameSessionPostCombatFrameState& frame) {
    const auto& clientTerrainMovementSources =
        frame.clientTerrainMovementSources;
    for (const GameSessionPostCombatFrameState::ClientTerrainMovementSource& source :
         clientTerrainMovementSources) {
        if (!m_world.m_registry.valid(source.entity) ||
            m_world.m_objects.isPendingDestroy(source.id)) {
            continue;
        }
        const ObjectFixedTransformComponent* transform =
            ecs::try_get<ObjectFixedTransformComponent>(
                m_world.m_registry, source.entity);
        const ObjectGeometryComponent* geometry =
            ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, source.entity);
        const ThingTemplateComponent* type =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, source.entity);
        if (!transform || !transform->authoritative || !geometry || !type ||
            !type->archetype) continue;
        const float x = transform->position.x.to_float();
        const float y = transform->position.y.to_float();
        const float z = transform->position.z.to_float();
        const float dx = x - source.previousPosition.x();
        const float dy = y - source.previousPosition.y();
        const float dz = z - source.previousPosition.z();
        if (dx * dx + dy * dy + dz * dz <=
            std::numeric_limits<float>::epsilon()) {
            continue;
        }
        const math::q32_32 radiusFixed =
            geometry->shape == ObjectGeometryShape::Box
            ? std::min(geometry->majorRadiusFixed,
                       geometry->minorRadiusFixed)
            : geometry->majorRadiusFixed;
        const float radius = radiusFixed.to_float();
        static_cast<void>(m_world.m_clientTerrainObjects.unitMoved({
            .source = source.id.value,
            .position = {x, y, z},
            .forward = {
                std::cos(transform->yawRadians.to_float()),
                std::sin(transform->yawRadians.to_float()), 0.0f},
            .collisionRadius = std::max(0.0f, radius),
            .crusherLevel =
                type->archetype->templateData.crusherLevel,
        }, m_presentation.m_confirmedTick));
    }
}

} // namespace engine::detail
