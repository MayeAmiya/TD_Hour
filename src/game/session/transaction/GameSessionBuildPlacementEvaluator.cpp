#include "game/session/transaction/GameSessionBuildPlacementEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "game/navigation/integration/NavigationDestinationAdjustment.h"
#include "game/navigation/integration/NavigationTerrainLayerMapping.h"
#include "game/object/ai/runtime/ObjectAIOrderAdmission.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/object/simulation/runtime/ObjectCollisionContact.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/terrain/TerrainConstructionQuery.h"
#include "math/fixed/q32_32_trig.h"

namespace engine
{
namespace
{

[[nodiscard]] const navigation::NavigationZoneField* placementZoneField(
    container::Span<const navigation::NavigationZoneField> zones,
    navigation::NavigationLayerId layer,
    navigation::NavigationProfileId profile,
    navigation::NavigationMovementMask movement,
    navigation::NavigationClearanceClass clearance) noexcept
{
    for (const navigation::NavigationZoneField& zone : zones)
    {
        if (zone.isBuilt() && zone.layer() == layer && zone.profile() == profile && zone.movementMask() == movement)
        {
            if (zone.clearanceClass() != clearance)
                continue;
            return &zone;
        }
    }
    return nullptr;
}

[[nodiscard]] bool placementCellsShareZone(const navigation::NavigationLayerSet& layers,
                                           container::Span<const navigation::NavigationZoneField> zones,
                                           navigation::NavigationProfileId profile,
                                           navigation::NavigationMovementMask movement,
                                           navigation::NavigationClearanceClass clearance,
                                           navigation::NavigationLayerCell left,
                                           navigation::NavigationLayerCell right) noexcept
{
    if (!left || !right || left.layer != right.layer)
        return false;
    const navigation::NavigationGrid* grid = layers.find(left.layer);
    const navigation::NavigationZoneField* zone = placementZoneField(zones, left.layer, profile, movement, clearance);
    return grid && zone && grid->traversable(left.cell, movement, left.layer, clearance) &&
           grid->traversable(right.cell, movement, right.layer, clearance) && zone->sameZone(left.cell, right.cell);
}

// RefCode CLEAR_PATH starts on the builder's current pathfind layer while a
// build footprint is always admitted on ground. This coarse query uses the
// already-published zone components and active directed portal graph; it does
// not enqueue an AI path or trust the ground cell underneath an elevated
// builder. Scratch storage is session-owned and reused by every preview.
[[nodiscard]] bool placementLayerRouteExists(const navigation::NavigationLayerSet& layers,
                                             container::Span<const navigation::NavigationZoneField> zones,
                                             const navigation::NavigationPortalGraph& graph,
                                             navigation::NavigationProfileId profile,
                                             navigation::NavigationMovementMask movement,
                                             navigation::NavigationClearanceClass clearance,
                                             navigation::NavigationLayerCell start,
                                             navigation::NavigationLayerCell goal,
                                             container::Vector<uint8_t>& reachable,
                                             container::Vector<uint32_t>& queue)
{
    if (placementCellsShareZone(layers, zones, profile, movement, clearance, start, goal))
    {
        return true;
    }
    const auto endpoints = graph.endpoints();
    reachable.assign(endpoints.size(), uint8_t{0});
    queue.clear();
    queue.reserve(endpoints.size());
    const auto admitFrom = [&](navigation::NavigationLayerCell origin)
    {
        for (uint32_t index = 0; index < endpoints.size(); ++index)
        {
            if (reachable[index] == 0 &&
                placementCellsShareZone(layers, zones, profile, movement, clearance, origin, endpoints[index]))
            {
                reachable[index] = 1;
                queue.push_back(index);
            }
        }
    };
    admitFrom(start);
    for (size_t cursor = 0; cursor < queue.size(); ++cursor)
    {
        const uint32_t current = queue[cursor];
        if (placementCellsShareZone(layers, zones, profile, movement, clearance, endpoints[current], goal))
        {
            return true;
        }
        admitFrom(endpoints[current]);
        for (const navigation::NavigationCoarsePortalEdge& edge : graph.edges())
        {
            if (edge.fromEndpoint != current || edge.profile != profile || edge.toEndpoint >= endpoints.size() ||
                reachable[edge.toEndpoint] != 0)
            {
                continue;
            }
            const navigation::NavigationLayerCell target = endpoints[edge.toEndpoint];
            const navigation::NavigationGrid* targetGrid = layers.find(target.layer);
            if (!targetGrid || !targetGrid->traversable(target.cell, movement, target.layer, clearance))
            {
                continue;
            }
            reachable[edge.toEndpoint] = 1;
            queue.push_back(edge.toEndpoint);
        }
    }
    return false;
}

[[nodiscard]] bool hasObjectKind(const ObjectKindOfComponent* kinds, game::ObjectKindOf sought) noexcept
{
    return kinds && game::objectHasKind(kinds->mask, sought);
}

} // namespace

bool GameSessionBuildPlacementEvaluator::objectAIOwnsMoveStop(
    ObjectId object) const noexcept
{
    if (!object) return false;
    const std::optional<ai::AIActorHandle> actor =
        m_ai.m_objectAI.find(object);
    const ai::ObjectAIOrderAdmissionStorage* admission = actor
        ? m_ai.m_objectAI.orderAdmission(*actor) : nullptr;
    ai::ObjectAIOrderAdmissionSlotView slot;
    return actor && admission &&
        admission->readSlot(actor->slot, slot) ==
            ai::ObjectAIOrderAdmissionStatus::Success &&
        ai::hasObjectAIOrderCapability(
            slot.capabilities, ai::ObjectAIOrderCapability::MoveStop);
}

GameSessionBuildPlacementLegalityEvaluation GameSessionBuildPlacementEvaluator::evaluateFixed(
    ObjectId sourceObject,
    const LogicFixedVec3& placementPosition,
    math::q32_32 placementYaw,
    PlayerId player,
    const game::ObjectArchetype& product,
    bool finalConfirmation,
    bool requireBuilderReachability)
{
    const auto finish = [](selection::LocalPlacementLegality legality,
                           container::Span<const render::TerrainBibFootprintInput> obstructions = {},
                           GameSessionBuildPlacementRejection rejection =
                               GameSessionBuildPlacementRejection::None)
    {
        GameSessionBuildPlacementLegalityEvaluation result;
        result.legality = legality;
        result.rejection = rejection;
        result.obstructions.assign(obstructions.begin(), obstructions.end());
        result.evaluated = true;
        return result;
    };
    if (!m_content.m_active || !m_content.m_terrain.isLoaded() || !player.isMapPlayer() || !sourceObject)
    {
        return {};
    }

    using Fixed = math::q32_32;
    const auto fixedFloorQuotient = [](Fixed numerator, Fixed denominator) noexcept
    {
        if (denominator <= Fixed{})
            return int64_t{};
        int64_t quotient = numerator.raw() / denominator.raw();
        if (numerator.raw() < 0 && numerator.raw() % denominator.raw() != 0)
        {
            --quotient;
        }
        return quotient;
    };

    // Shroud must be the first observable rejection. Reporting a hidden
    // collision or terrain detail would let the placement cursor probe
    // unrevealed cells, exactly the information leak BuildAssistant avoids.
    const PlayerState* issuingPlayer = m_content.m_players.get(player);
    if (const auto visibility = m_world.m_mapVisibility.snapshot();
        issuingPlayer && issuingPlayer->controller == PlayerControllerKind::Human && visibility &&
        visibility->renderingActive)
    {
        const Fixed visibilityCellSize = Fixed::from_raw(visibility->cellWorldSizeRaw);
        if (visibilityCellSize <= Fixed{})
            return finish(selection::LocalPlacementLegality::Illegal, {},
                          GameSessionBuildPlacementRejection::VisibilityUnavailable);
        const int64_t rawCellX =
            fixedFloorQuotient(placementPosition.x - Fixed::from_raw(visibility->originXRaw), visibilityCellSize);
        const int64_t rawCellY =
            fixedFloorQuotient(placementPosition.y - Fixed::from_raw(visibility->originYRaw), visibilityCellSize);
        const int32_t cellX = static_cast<int32_t>(
            std::clamp<int64_t>(rawCellX, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
        const int32_t cellY = static_cast<int32_t>(
            std::clamp<int64_t>(rawCellY, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
        if (visibility->cellState(player, cellX, cellY) != game::terrain::MapVisibilityCellState::Clear)
        {
            return finish(selection::LocalPlacementLegality::Illegal, {},
                          GameSessionBuildPlacementRejection::Shroud);
        }
    }

    const game::ObjectGeometryTemplate& productGeometry = product.templateData.geometry;
    const Fixed halfX = Fixed::max(Fixed{}, productGeometry.majorRadiusFixed);
    const Fixed halfY = productGeometry.type == game::ObjectGeometryType::Box
                            ? Fixed::max(Fixed{}, productGeometry.minorRadiusFixed)
                            : halfX;
    const math::q32_32_sincos placementHeading = math::fixed_sincos(placementYaw);
    Fixed minimumHeight{};
    Fixed maximumHeight{};
    bool sampledHeight = false;
    bool terrainRejected = false;
    bool notFlatEnough = false;
    const game::terrain::TerrainExtentRaw playableExtent = m_content.m_terrain.map().playableExtentRaw();
    const Fixed minimumEdgeDistance =
        Fixed::max(Fixed{}, m_content.m_objectSimulationRules.buildPlacement.minimumDistanceFromMapEdge);
    const Fixed sampleResolution{int32_t{10}};
    const auto positiveCeilToInt32 = [](Fixed value) noexcept
    {
        if (value <= Fixed{})
            return int32_t{};
        int64_t result = value.raw() >> 32u;
        if ((value.raw() & 0xffffffffll) != 0)
            ++result;
        return static_cast<int32_t>(std::min<int64_t>(result, std::numeric_limits<int32_t>::max()));
    };
    const int32_t samplesX = std::max(int32_t{1}, positiveCeilToInt32(halfX * Fixed{int32_t{2}} / sampleResolution));
    const int32_t samplesY = std::max(int32_t{1}, positiveCeilToInt32(halfY * Fixed{int32_t{2}} / sampleResolution));
    // CELL_IMPASSABLE is a map/topology fact in the original build check.
    // Query the modern static navigation grid here rather than the composed
    // runtime grid: dynamic building occupancy is handled by the object pass
    // below (including its stealth/shroud rules), and consulting it here
    // would turn a hidden structure into a placement-cursor information leak.
    // A started terrain session always owns a navigation grid; treating a
    // missing/out-of-range cell as rejected keeps authority fail-closed.
    const navigation::NavigationGrid* staticNavigation =
        m_content.m_navigation.isInitialized() ? &m_content.m_navigation.staticGrid() : nullptr;
    // BuildAssistant chooses the destination pathfind layer once from the
    // placement origin. Sampling highestPathfindLayerAtXY across every edge
    // of the footprint incorrectly rejects a ground building merely because
    // one corner overlaps a bridge's XY projection.
    if (m_content.m_terrain.highestPathfindLayerAtXYRaw(placementPosition.x.raw(), placementPosition.y.raw()) !=
        game::terrain::kGroundPathfindLayer)
    {
        terrainRejected = true;
    }
    const auto samplePoint = [&](Fixed localX, Fixed localY)
    {
        if (productGeometry.type != game::ObjectGeometryType::Box &&
            localX * localX + localY * localY >
                halfX * halfX + Fixed::from_fraction(1, 1000))
        {
            return;
        }
        const Fixed fixedWorldX =
            placementPosition.x + placementHeading.cosine * localX - placementHeading.sine * localY;
        const Fixed fixedWorldY =
            placementPosition.y + placementHeading.sine * localX + placementHeading.cosine * localY;
        const navigation::NavigationCellId navigationCell = staticNavigation ? staticNavigation->cellAt({
                                                                                   .xRaw = fixedWorldX.raw(),
                                                                                   .yRaw = fixedWorldY.raw(),
                                                                                   .zRaw = 0,
                                                                               })
                                                                             : navigation::InvalidNavigationCell;
        const navigation::NavigationCellValue navigationValue =
            staticNavigation ? staticNavigation->cell(navigationCell) : navigation::NavigationCellValue{};
        const std::optional<game::terrain::TerrainCell> terrainCell =
            m_content.m_terrain.map().cellAtRaw(fixedWorldX.raw(), fixedWorldY.raw());
        const game::terrain::TerrainHeightfieldData& heightfield = m_content.m_terrain.map().heightfield();
        const bool restrictsConstruction =
            terrainCell && heightfield.blendTiles && heightfield.blendTiles->isValidFor(heightfield.heights.size()) &&
            game::terrain::terrainRestrictsConstructionAtBaseTile(
                m_content.m_objectSimulationRules.buildPlacement.terrainTypes,
                *heightfield.blendTiles,
                static_cast<size_t>(terrainCell->y) * static_cast<size_t>(heightfield.width) +
                    static_cast<size_t>(terrainCell->x));
        if (!m_content.m_terrain.map().isInsidePlayableRaw(fixedWorldX.raw(), fixedWorldY.raw()) ||
            fixedWorldX < Fixed::from_raw(playableExtent.minimumX) + minimumEdgeDistance ||
            fixedWorldX > Fixed::from_raw(playableExtent.maximumX) - minimumEdgeDistance ||
            fixedWorldY < Fixed::from_raw(playableExtent.minimumY) + minimumEdgeDistance ||
            fixedWorldY > Fixed::from_raw(playableExtent.maximumY) - minimumEdgeDistance || !staticNavigation ||
            !staticNavigation->contains(navigationCell) ||
            navigationValue.passability != navigation::NavigationPassability::Traversable ||
            (navigationValue.movementMask & navigation::NavigationMovement::Ground) == 0 || restrictsConstruction ||
            m_content.m_terrain.isUnderwaterLegacyRaw(fixedWorldX.raw(), fixedWorldY.raw()) ||
            m_content.m_terrain.isCliffCellRaw(fixedWorldX.raw(), fixedWorldY.raw()))
        {
            terrainRejected = true;
            return;
        }
        const Fixed height = Fixed::from_raw(m_content.m_terrain.groundHeightRaw(fixedWorldX.raw(), fixedWorldY.raw()));
        if (!sampledHeight)
        {
            minimumHeight = height;
            maximumHeight = height;
            sampledHeight = true;
        }
        else
        {
            minimumHeight = Fixed::min(minimumHeight, height);
            maximumHeight = Fixed::max(maximumHeight, height);
        }
    };
    // Preserve BuildAssistant::iterateFootprint exactly: advance by the
    // fixed MAP_XY_FACTOR and snap only the final overshooting point back to
    // the positive edge. Uniformly subdividing the extent changes which
    // cliff/restricted/nav cells are observed on non-multiple dimensions.
    for (int32_t yIndex = 0; yIndex <= samplesY && !terrainRejected; ++yIndex)
    {
        const Fixed localY = yIndex == samplesY ? halfY : -halfY + sampleResolution * Fixed{yIndex};
        for (int32_t xIndex = 0; xIndex <= samplesX && !terrainRejected; ++xIndex)
        {
            const Fixed localX = xIndex == samplesX ? halfX : -halfX + sampleResolution * Fixed{xIndex};
            samplePoint(localX, localY);
            if (xIndex == samplesX)
                break;
        }
        if (yIndex == samplesY)
            break;
    }
    // The legacy loop can accept zero points for a circular footprint smaller
    // than one map cell (all four corners lie outside the circle). Keep the
    // original grid everywhere else, but fail closed on that historical hole
    // by sampling the centre once.
    if (!terrainRejected && !sampledHeight)
    {
        samplePoint(Fixed{}, Fixed{});
    }
    const Fixed allowedHeightVariation =
        Fixed::max(Fixed{}, m_content.m_objectSimulationRules.buildPlacement.allowedHeightVariation);
    if (!terrainRejected && (!sampledHeight || maximumHeight - minimumHeight > allowedHeightVariation))
    {
        terrainRejected = true;
        notFlatEnough = sampledHeight;
    }

    struct Shape final
    {
        LogicFixedVec3 position{};
        Fixed yaw{};
        Fixed halfX{};
        Fixed halfY{};
        Fixed height{};
        ObjectGeometryShape kind = ObjectGeometryShape::Sphere;
    };
    const auto geometryFromShape = [](const Shape& shape)
    {
        ObjectGeometryComponent geometry;
        geometry.shape = shape.kind;
        geometry.majorRadiusFixed = shape.halfX;
        geometry.minorRadiusFixed = shape.halfY;
        geometry.heightFixed = shape.height;
        geometry.boundingCircleRadiusFixed = shape.halfX;
        geometry.boundingSphereRadiusFixed = shape.halfX;
        return geometry;
    };
    const auto overlaps = [&geometryFromShape](const Shape& left, const Shape& right)
    {
        const ObjectGeometryComponent leftGeometry = geometryFromShape(left);
        const ObjectGeometryComponent rightGeometry = geometryFromShape(right);
        ObjectCollisionContact contact;
        return computeObjectCollisionContact(
            left.position, left.yaw, leftGeometry, right.position, right.yaw, rightGeometry, contact);
    };
    const auto baseShape =
        [](LogicFixedVec3 position, Fixed yaw, game::ObjectGeometryType type, Fixed major, Fixed minor, Fixed height)
    {
        Shape shape;
        shape.position = position;
        shape.yaw = yaw;
        shape.halfX = Fixed::max(Fixed{}, major);
        shape.halfY = type == game::ObjectGeometryType::Box ? Fixed::max(Fixed{}, minor) : shape.halfX;
        shape.height = type == game::ObjectGeometryType::Sphere ? shape.halfX : Fixed::max(Fixed{}, height);
        switch (type)
        {
        case game::ObjectGeometryType::Sphere:
            shape.kind = ObjectGeometryShape::Sphere;
            break;
        case game::ObjectGeometryType::Cylinder:
            shape.kind = ObjectGeometryShape::Cylinder;
            break;
        case game::ObjectGeometryType::Box:
            shape.kind = ObjectGeometryShape::Box;
            break;
        }
        return shape;
    };
    const auto componentShape = [](LogicFixedVec3 position, Fixed yaw, const ObjectGeometryComponent& geometry)
    {
        Shape shape;
        shape.position = position;
        shape.yaw = yaw;
        shape.kind = geometry.shape;
        shape.halfX = Fixed::max(Fixed{}, geometry.majorRadiusFixed);
        shape.halfY =
            geometry.shape == ObjectGeometryShape::Box ? Fixed::max(Fixed{}, geometry.minorRadiusFixed) : shape.halfX;
        shape.height =
            geometry.shape == ObjectGeometryShape::Sphere ? shape.halfX : Fixed::max(Fixed{}, geometry.heightFixed);
        return shape;
    };
    const auto expandedBase = [](Shape source, Fixed extra)
    {
        const Fixed expansion = Fixed::max(Fixed{}, extra);
        // No authored factory bib expansion means the base geometry itself.
        // Converting a zero-expansion Sphere/Cylinder into the legacy 40-unit
        // rectangular clearance volume creates false 3D placement collisions.
        if (expansion == Fixed{})
            return source;
        const bool wasBox = source.kind == ObjectGeometryShape::Box;
        if (source.kind == ObjectGeometryShape::Sphere)
            source.position.z -= source.halfX;
        source.kind = ObjectGeometryShape::Box;
        source.halfX += expansion;
        source.halfY += expansion;
        if (!wasBox)
            source.height = Fixed{int32_t{40}};
        return source;
    };
    const auto exitShape = [](const Shape& base, Fixed width)
    {
        Shape exit;
        exit.kind = base.kind;
        exit.yaw = base.yaw;
        exit.position = base.position;
        exit.height = base.height;
        exit.halfX = Fixed::max(Fixed{}, width) / Fixed{int32_t{2}};
        exit.halfY = base.kind == ObjectGeometryShape::Box ? base.halfY : exit.halfX;
        const Fixed offset = base.halfX + exit.halfX;
        const math::q32_32_sincos heading = math::fixed_sincos(base.yaw);
        exit.position.x += heading.cosine * offset;
        exit.position.y += heading.sine * offset;
        return exit;
    };

    const Shape productBase = baseShape(placementPosition,
                                        placementYaw,
                                        productGeometry.type,
                                        productGeometry.majorRadiusFixed,
                                        productGeometry.minorRadiusFixed,
                                        productGeometry.heightFixed);
    Fixed productExtraWidth = product.templateData.factoryExtraBibWidthFixed;
    Fixed productExitWidth = product.templateData.factoryExitWidthFixed;
    const PlayerState* placementPlayer = m_content.m_players.get(player);
    const Fixed skirmishFactoryClearance{int32_t{30}};
    if (placementPlayer && placementPlayer->controller == PlayerControllerKind::Ai &&
        productExtraWidth < skirmishFactoryClearance)
    {
        productExtraWidth = skirmishFactoryClearance;
        productExitWidth = Fixed::max(Fixed{}, productExitWidth - productExtraWidth);
    }
    const Shape productExpanded = expandedBase(productBase, productExtraWidth);
    const Shape productExit = exitShape(productBase, productExitWidth);
    const bool hasProductExit = productExit.halfX > Fixed{};
    const auto visibility = m_world.m_mapVisibility.snapshot();
    for (const ObjectSpatialRecord& record : m_world.m_spatialIndex.records())
    {
        if (!record.object || record.object == sourceObject)
            continue;
        const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(record.object);
        if (!entity)
            continue;
        const TransformComponent* transform = ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
        const ObjectGeometryComponent* geometry = ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, *entity);
        const ObjectKindOfComponent* kinds = ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *entity);
        const ObjectHealthComponent* health = ecs::try_get<ObjectHealthComponent>(m_world.m_registry, *entity);
        if (!transform || !geometry || hasObjectKind(kinds, game::ObjectKindOf::Mine) ||
            hasObjectKind(kinds, game::ObjectKindOf::Inert) || hasObjectKind(kinds, game::ObjectKindOf::NoCollide) ||
            hasObjectKind(kinds, game::ObjectKindOf::Shrubbery) ||
            hasObjectKind(kinds, game::ObjectKindOf::ClearedByBuild) || (health && health->effectivelyDead))
        {
            continue;
        }
        const ObjectStatusComponent* status = ecs::try_get<ObjectStatusComponent>(m_world.m_registry, *entity);
        if (status && status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::NoCollisions)))
        {
            continue;
        }
        const OwnerComponent* owner = ecs::try_get<OwnerComponent>(m_world.m_registry, *entity);
        const PlayerRelationship relationship =
            owner ? relationshipBetweenPlayerAndObject(m_world.m_registry, m_content.m_players, player, *entity)
                  : PlayerRelationship::Neutral;
        const bool allied = relationship == PlayerRelationship::Allies;
        const bool stealthed = status && status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::Stealthed));
        const bool detected = status && status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::Detected));
        const bool disguised = status && status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::Disguised));
        const Shape targetBase =
            componentShape(readAuthoritativeObjectPosition(m_world.m_registry, *entity, *transform),
                           readAuthoritativeObjectYaw(m_world.m_registry, *entity, *transform),
                           *geometry);
        if (!allied && stealthed && !detected && !disguised)
        {
            // Cursor movement deliberately pretends the hidden object is not
            // there. A click repeats the base-footprint test but fails without
            // a blocker Bib, preserving both construction rules and stealth.
            if (finalConfirmation && overlaps(productBase, targetBase))
            {
                return finish(selection::LocalPlacementLegality::Illegal, {},
                              GameSessionBuildPlacementRejection::HiddenObject);
            }
            continue;
        }

        bool collides = overlaps(productBase, targetBase);
        const bool structure = hasObjectKind(kinds, game::ObjectKindOf::Structure);
        const bool kindImmobile = hasObjectKind(kinds, game::ObjectKindOf::Immobile);
        bool factoryClearanceForcesBlock = false;
        const ThingTemplateComponent* targetTemplate =
            ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
        Fixed targetExitWidth{};
        Fixed targetExtraWidth{};
        if (targetTemplate && targetTemplate->archetype)
        {
            targetExitWidth = targetTemplate->archetype->templateData.factoryExitWidthFixed;
            targetExtraWidth = targetTemplate->archetype->templateData.factoryExtraBibWidthFixed;
        }
        if (structure)
        {
            const Shape targetExpanded = expandedBase(targetBase, targetExtraWidth);
            const Shape targetExit = exitShape(targetBase, targetExitWidth);
            factoryClearanceForcesBlock = overlaps(productExpanded, targetExpanded);
            const bool exitCollision =
                (hasProductExit && overlaps(productExit, targetExpanded)) ||
                (targetExit.halfX > Fixed{} && overlaps(productExpanded, targetExit)) ||
                (hasProductExit && targetExit.halfX > Fixed{} && overlaps(productExit, targetExit));
            // BuildAssistant treats expanded base clearance as an
            // unconditional structure obstruction. Exit-only overlap blocks
            // only an authored IMMOBILE structure; a movable object can be
            // asked to clear the factory lane.
            collides = collides || factoryClearanceForcesBlock || (kindImmobile && exitCollision);
        }
        if (!collides)
            continue;

        // RefCode returns LBC_SHROUD as soon as an otherwise relevant
        // footprint collision is found. Do this before the movable/enemy/busy
        // classification so a fogged neutral unit cannot be inferred from a
        // cursor result, while the explicit hidden-stealth branch above keeps
        // its preview/final-confirm split.
        const LogicFixedVec3 targetPosition = readAuthoritativeObjectPosition(m_world.m_registry, *entity, *transform);
        if (visibility && visibility->renderingActive &&
            !visibility->footprintHasClearCellRaw(
                player,
                targetPosition.x.raw(),
                targetPosition.y.raw(),
                math::q32_32::max(math::q32_32{}, geometry->boundingCircleRadiusFixed).raw()))
        {
            return finish(selection::LocalPlacementLegality::Illegal, {},
                          GameSessionBuildPlacementRejection::Shroud);
        }

        const bool immobile =
            kindImmobile || (status && status->hasAny(game::objectStatusBit(game::ObjectStatusFlag::Immobile)));
        const bool enemy = relationship == PlayerRelationship::Enemies;
        const bool disabled = isObjectDisabled(m_world.m_registry, *entity, m_presentation.m_confirmedTick);
        const bool constructionEvacuationCapable = !immobile &&
            objectAIOwnsMoveStop(record.object);
        // Only actors whose ObjectAI contract owns Move/Stop may be admitted
        // as construction occupants. The lifecycle transaction gives those
        // actors a new-goal evacuation order after the foundation exists,
        // including sleeping/busy/temporarily disabled allies. A nominally
        // mobile object without that consumer is a hard blocker; otherwise it
        // would be accepted here and remain embedded in the completed site.
        if (allied && !structure && constructionEvacuationCapable)
            continue;
        if (!factoryClearanceForcesBlock &&
            constructionEvacuationCapable && !enemy && !disabled)
            continue;
        const render::TerrainBibFootprintInput blocker{
            .ownerIdentity = record.object.value,
            .kind = render::TerrainBibKind::Building,
            .position = {transform->x, transform->y, transform->z},
            .yawRadians = transform->rotation,
            .geometryMajorRadius = geometry->majorRadiusFixed.to_float(),
            .geometryMinorRadius = geometry->minorRadiusFixed.to_float(),
            .geometryIsBox = geometry->shape == ObjectGeometryShape::Box,
            .factoryExitWidth = targetExitWidth.to_float(),
            .factoryExtraBibWidth = targetExtraWidth.to_float(),
            .highlighted = true,
            .receivesVisibility = true,
        };
        const container::Array<render::TerrainBibFootprintInput, 1> blockers{{blocker}};
        return finish(selection::LocalPlacementLegality::Illegal, blockers,
                      GameSessionBuildPlacementRejection::ObjectsInTheWay);
    }

    const Fixed supplyBuildBorder =
        Fixed::max(Fixed{}, m_content.m_objectSimulationRules.buildPlacement.supplyBuildBorder);
    if (supplyBuildBorder > Fixed{} &&
        game::objectHasKind(product.kindOfMask, game::ObjectKindOf::CannotBuildNearSupplies))
    {
        for (const ObjectSpatialRecord& record : m_world.m_spatialIndex.records())
        {
            if (!record.object)
                continue;
            const std::optional<ecs::entity> entity = m_world.m_objects.entityFromId(record.object);
            if (!entity)
                continue;
            const ObjectKindOfComponent* kinds = ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *entity);
            if (!hasObjectKind(kinds, game::ObjectKindOf::SupplySource))
                continue;
            const TransformComponent* transform = ecs::try_get<TransformComponent>(m_world.m_registry, *entity);
            const ObjectGeometryComponent* geometry =
                ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, *entity);
            if (!transform || !geometry)
                continue;

            Shape expandedSupply =
                componentShape(readAuthoritativeObjectPosition(m_world.m_registry, *entity, *transform),
                               readAuthoritativeObjectYaw(m_world.m_registry, *entity, *transform),
                               *geometry);
            expandedSupply.halfX += supplyBuildBorder;
            expandedSupply.halfY += supplyBuildBorder;
            if (!overlaps(productBase, expandedSupply))
                continue;

            if (visibility && visibility->renderingActive &&
                !visibility->footprintHasClearCellRaw(
                    player,
                    expandedSupply.position.x.raw(),
                    expandedSupply.position.y.raw(),
                    (math::q32_32::max(math::q32_32{}, geometry->boundingCircleRadiusFixed) + supplyBuildBorder).raw()))
            {
                return finish(selection::LocalPlacementLegality::Illegal, {},
                              GameSessionBuildPlacementRejection::Shroud);
            }
            const ThingTemplateComponent* targetTemplate =
                ecs::try_get<ThingTemplateComponent>(m_world.m_registry, *entity);
            const Fixed targetExitWidth = targetTemplate && targetTemplate->archetype
                                              ? targetTemplate->archetype->templateData.factoryExitWidthFixed
                                              : Fixed{};
            const Fixed targetExtraWidth = targetTemplate && targetTemplate->archetype
                                               ? targetTemplate->archetype->templateData.factoryExtraBibWidthFixed
                                               : Fixed{};
            const render::TerrainBibFootprintInput blocker{
                .ownerIdentity = record.object.value,
                .kind = render::TerrainBibKind::Building,
                .position = {transform->x, transform->y, transform->z},
                .yawRadians = transform->rotation,
                .geometryMajorRadius = geometry->majorRadiusFixed.to_float(),
                .geometryMinorRadius = geometry->minorRadiusFixed.to_float(),
                .geometryIsBox = geometry->shape == ObjectGeometryShape::Box,
                .factoryExitWidth = targetExitWidth.to_float(),
                .factoryExtraBibWidth = (targetExtraWidth + supplyBuildBorder).to_float(),
                .highlighted = true,
                .receivesVisibility = true,
            };
            const container::Array<render::TerrainBibFootprintInput, 1> blockers{{blocker}};
            return finish(selection::LocalPlacementLegality::Illegal, blockers,
                          GameSessionBuildPlacementRejection::TooCloseToSupplies);
        }
    }
    const std::optional<ecs::entity> builderEntity = m_world.m_objects.entityFromId(sourceObject);
    if (!builderEntity)
        return {};
    const ObjectKindOfComponent* builderKinds = ecs::try_get<ObjectKindOfComponent>(m_world.m_registry, *builderEntity);
    const ObjectStatusComponent* builderStatus =
        ecs::try_get<ObjectStatusComponent>(m_world.m_registry, *builderEntity);
    const bool builderImmobile =
        hasObjectKind(builderKinds, game::ObjectKindOf::Immobile) ||
        (builderStatus && builderStatus->hasAny(game::objectStatusBit(game::ObjectStatusFlag::Immobile)));
    if (!builderImmobile && requireBuilderReachability)
    {
        const ObjectFixedTransformComponent* builderTransform =
            ecs::try_get<ObjectFixedTransformComponent>(m_world.m_registry, *builderEntity);
        const ObjectLocomotionComponent* builderLocomotion =
            ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry, *builderEntity);
        const ObjectGeometryComponent* builderGeometry =
            ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, *builderEntity);
        if (!builderTransform || !builderTransform->authoritative || !builderLocomotion || !builderGeometry ||
            builderGeometry->boundingCircleRadiusFixed < math::q32_32{})
        {
            return finish(selection::LocalPlacementLegality::Illegal, {},
                          GameSessionBuildPlacementRejection::BuilderUnavailable);
        }
        // The original CLEAR_PATH gate requires a locomotor/AI update
        // interface, not membership in a particular scheduler storage. The
        // deterministic route query below consumes the builder's components
        // directly, so requiring ObjectAIRuntime::actorState() here adds an
        // unrelated transient rejection and can turn a valid dozer preview
        // red while its actor slot is being admitted or republished.
        if (!m_content.m_navigation.isInitialized() ||
            m_content.m_navigation.topologyPublicationState() ==
                navigation::NavigationTopologyPublicationState::Failed)
        {
            return finish(selection::LocalPlacementLegality::Illegal, {},
                          GameSessionBuildPlacementRejection::NavigationUnavailable);
        }
        // NavigationSystem keeps m_layers/m_layerZones/m_portalGraph as the
        // last complete, atomically published topology while a new revision
        // is prepared in separate pending buffers. Pathfinding itself keeps
        // consuming that published side during PublicationPending. Placement
        // must do the same: rejecting the query merely because any dynamic
        // event is queued turns every build icon red while units, structures,
        // water or terrain footprints are being republished. Object overlap
        // and terrain tests above already consume current authority state;
        // the coarse route check only answers whether the builder has a
        // published connected route and will be invalidated/repathed when the
        // pending topology is committed.
        game::terrain::TerrainPathfindLayerId startTerrainLayer = game::terrain::kGroundPathfindLayer;
        if (const ObjectTerrainLayerComponent* layer =
                ecs::try_get<ObjectTerrainLayerComponent>(m_world.m_registry, *builderEntity))
        {
            startTerrainLayer = layer->pathfindLayer;
        }
        else
        {
            // Fail-safe path only: every object created through the authoritative
            // boundary gets ObjectTerrainLayerComponent, so the branch above
            // normally wins.  Use the raw fixed-point query regardless — the
            // float overload does its surface comparison in float, which would
            // make a Legal/Illegal verdict (and therefore what the AI builds)
            // machine-dependent the moment any spawn path leaves a builder
            // without the component on a map with elevated pathfind surfaces.
            startTerrainLayer =
                m_content.m_terrain.pathfindLayerForDestinationRaw(builderTransform->position.x.raw(),
                                                                   builderTransform->position.y.raw(),
                                                                   builderTransform->position.z.raw());
        }
        navigation::NavigationLayerId startLayer;
        navigation::NavigationLayerId goalLayer;
        if (!navigation::tryNavigationLayerFromTerrainPathfindLayer(startTerrainLayer, startLayer) ||
            !navigation::tryNavigationLayerFromTerrainPathfindLayer(game::terrain::kGroundPathfindLayer, goalLayer))
        {
            return finish(selection::LocalPlacementLegality::Illegal, {},
                          GameSessionBuildPlacementRejection::InvalidNavigationLayer);
        }
        const navigation::NavigationLayerSet& layers = m_content.m_navigation.layers();
        const navigation::NavigationGrid* startGrid = layers.find(startLayer);
        const navigation::NavigationGrid* goalGrid = layers.find(goalLayer);
        if (!startGrid || !goalGrid)
        {
            return finish(selection::LocalPlacementLegality::Illegal, {},
                          GameSessionBuildPlacementRejection::NavigationUnavailable);
        }
        const navigation::NavigationCellId startCell = startGrid->cellAt({
            .xRaw = builderTransform->position.x.raw(),
            .yRaw = builderTransform->position.y.raw(),
            .zRaw = builderTransform->position.z.raw(),
        });
        const navigation::NavigationCellId goalCell = goalGrid->cellAt({
            .xRaw = placementPosition.x.raw(),
            .yRaw = placementPosition.y.raw(),
            .zRaw = placementPosition.z.raw(),
        });
        const navigation::NavigationZoneField& primaryZones = m_content.m_navigation.zones();
        constexpr game::LocomotorSurfaceMask groundSurface = game::locomotorSurfaceBit(game::LocomotorSurface::Ground);
        constexpr navigation::NavigationMovementMask groundMovement = navigation::NavigationMovement::Ground;
        const navigation::NavigationClearanceClass builderClearance = navigation::clearanceClassForRadiusRaw(
            math::q32_32::max(math::q32_32{}, builderGeometry->boundingCircleRadiusFixed).raw(),
            startGrid->transform().cellSizeRaw);
        if (!startGrid->contains(startCell) || !goalGrid->contains(goalCell))
        {
            return finish(selection::LocalPlacementLegality::Illegal, {},
                          GameSessionBuildPlacementRejection::InvalidNavigationCell);
        }
        if ((builderLocomotion->surfaces & groundSurface) == 0 || !primaryZones.profile() ||
            (primaryZones.movementMask() & groundMovement) == 0)
        {
            return finish(selection::LocalPlacementLegality::Illegal, {},
                          GameSessionBuildPlacementRejection::UnsupportedLocomotor);
        }
        navigation::NavigationLayerCell routeStart{startLayer, startCell};
        if (!startGrid->traversable(startCell, groundMovement, startLayer,
                                    builderClearance))
        {
            // RefCode's quick build-path query deliberately accepts a mobile
            // builder whose current cell is covered by a newly placed
            // structure: the real pathfinder enters its tunnelling mode until
            // the unit has escaped the overlapping obstacle.  Starting the
            // coarse zone query at the blocked cell instead makes the preview
            // fail until the player first moves the builder by hand.
            const navigation::NavigationDestinationAdjustmentResult adjustedStart =
                navigation::adjustNavigationDestination(
                    layers,
                    {
                        .desired = {
                            .xRaw = builderTransform->position.x.raw(),
                            .yRaw = builderTransform->position.y.raw(),
                            .zRaw = builderTransform->position.z.raw(),
                        },
                        .layer = startLayer,
                        .movementMask = groundMovement,
                        .clearance = builderClearance,
                        .allowAdjustment = true,
                    });
            if (!adjustedStart.accepted())
            {
                return finish(selection::LocalPlacementLegality::Illegal, {},
                              GameSessionBuildPlacementRejection::NoClearPath);
            }
            routeStart = adjustedStart.location;
        }
        if (!placementLayerRouteExists(layers,
                                       m_content.m_navigation.layerZones(),
                                       m_content.m_navigation.portalGraph(),
                                       primaryZones.profile(),
                                       groundMovement,
                                       builderClearance,
                                       routeStart,
                                       {goalLayer, goalCell},
                                       m_content.m_placementReachablePortalScratch,
                                       m_content.m_placementPortalQueueScratch))
        {
            return finish(selection::LocalPlacementLegality::Illegal, {},
                          GameSessionBuildPlacementRejection::NoClearPath);
        }
    }
    if (terrainRejected)
    {
        return finish(selection::LocalPlacementLegality::Illegal, {},
                      notFlatEnough
                          ? GameSessionBuildPlacementRejection::NotFlatEnough
                          : GameSessionBuildPlacementRejection::RestrictedTerrain);
    }
    return finish(selection::LocalPlacementLegality::Legal);
}
} // namespace engine
