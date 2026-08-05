#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include "debug/debug.h"
#include "game/data/base/LineBuildPlacementPlanner.h"
#include "game/object/component/ObjectComponentsMovementPhysics.h"
#include "game/object/definition/ObjectArchetype.h"
#include "game/object/simulation/economy/ObjectBuilder.h"
#include "game/object/simulation/movement/ObjectPositionAuthority.h"
#include "game/selection/LocalPlacementAnchorInput.h"
#include "game/session/command/OrderExecutor.h"
#include "game/session/core/GameSession.h"
#include "game/session/state/GameSessionDomainState.h"
#include "game/session/presentation/LocalPlacementPresentationPort.h"
#include "game/session/transaction/GameSessionBuildPlacementEvaluator.h"
#include "math/fixed/q32_32_trig.h"

namespace engine
{
namespace
{

[[nodiscard]] const char* placementRejectionName(
    GameSessionBuildPlacementRejection rejection) noexcept
{
    switch (rejection)
    {
    case GameSessionBuildPlacementRejection::None: return "none";
    case GameSessionBuildPlacementRejection::VisibilityUnavailable: return "visibility-unavailable";
    case GameSessionBuildPlacementRejection::Shroud: return "shroud";
    case GameSessionBuildPlacementRejection::RestrictedTerrain: return "restricted-terrain";
    case GameSessionBuildPlacementRejection::NotFlatEnough: return "not-flat-enough";
    case GameSessionBuildPlacementRejection::HiddenObject: return "hidden-object";
    case GameSessionBuildPlacementRejection::ObjectsInTheWay: return "objects-in-the-way";
    case GameSessionBuildPlacementRejection::TooCloseToSupplies: return "too-close-to-supplies";
    case GameSessionBuildPlacementRejection::BuilderUnavailable: return "builder-unavailable";
    case GameSessionBuildPlacementRejection::NavigationUnavailable: return "navigation-unavailable";
    case GameSessionBuildPlacementRejection::InvalidNavigationLayer: return "invalid-navigation-layer";
    case GameSessionBuildPlacementRejection::InvalidNavigationCell: return "invalid-navigation-cell";
    case GameSessionBuildPlacementRejection::UnsupportedLocomotor: return "unsupported-locomotor";
    case GameSessionBuildPlacementRejection::NoClearPath: return "no-clear-path";
    }
    return "unknown";
}

} // namespace

LocalPlacementPresentationPort::LocalPlacementPresentationPort(
    GameSessionContentStartState& content,
    GameSessionWorldState& world,
    GameSessionAIState& ai,
    GameSessionScriptPresentationState& presentation) noexcept
    : m_content(content), m_world(world), m_ai(ai),
      m_presentation(presentation),
      m_evaluator(content, ai, presentation, world) {}

bool LocalPlacementPresentationPort::begin(ObjectId sourceObject,
                                                                 container::StringView productType,
                                                                 selection::LocalPlacementBackendKind backend,
                                                                 CommandActivationContext activation)
{
    m_lastLoggedRejection = GameSessionBuildPlacementRejection::None;
    if (!m_content.m_active || !sourceObject || productType.empty() || !m_world.m_objects.entityFromId(sourceObject))
    {
        return false;
    }
    const PlayerState* localPlayer = m_content.m_players.localPlayer();
    if (!localPlayer || !localPlayer->isCommandPlayer() ||
        m_world.m_ownership.ownerOf(sourceObject) != localPlayer->id)
    {
        return false;
    }
    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(productType);
    return product && m_presentation.m_localPlacementPresentation.begin(
                          m_presentation.m_scriptPresentationEpoch,
                          m_presentation.m_confirmedTick,
                          sourceObject,
                          product->templateData,
                          backend,
                          activation);
}

bool LocalPlacementPresentationPort::updatePose(render::RenderVector position,
                                                                      math::q32_32 yawRadians) noexcept
{
    return m_content.m_active &&
           m_presentation.m_localPlacementPresentation.updatePose(position, yawRadians);
}

bool LocalPlacementPresentationPort::updateFromAnchors(
    render::RenderVector anchorStart,
    render::RenderVector anchorEnd,
    bool forceAttackSnap)
{
    const selection::LocalPlacementPreviewSnapshot activePlacement =
        m_presentation.m_localPlacementPresentation.snapshot();
    if (!m_content.m_active || !activePlacement.sourceObject ||
        activePlacement.objectType.empty() ||
        !std::isfinite(anchorStart.x()) ||
        !std::isfinite(anchorStart.y()) ||
        !std::isfinite(anchorStart.z()) ||
        !std::isfinite(anchorEnd.x()) ||
        !std::isfinite(anchorEnd.y()) ||
        !std::isfinite(anchorEnd.z()))
    {
        return false;
    }

    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(
            activePlacement.objectType);
    if (!product)
    {
        return false;
    }
    if (game::objectHasKind(product->kindOfMask,
                            game::ObjectKindOf::LineBuild))
    {
        return planLine(anchorStart, anchorEnd, forceAttackSnap);
    }

    const LogicFixedVec3 fixedStart = LogicFixedVec3::fromFloats(
        anchorStart.x(), anchorStart.y(), anchorStart.z());
    const LogicFixedVec3 fixedEnd = LogicFixedVec3::fromFloats(
        anchorEnd.x(), anchorEnd.y(), anchorEnd.z());
    const math::q32_32 deltaX = fixedEnd.x - fixedStart.x;
    const math::q32_32 deltaY = fixedEnd.y - fixedStart.y;
    const bool hasDirection =
        deltaX != math::q32_32{} || deltaY != math::q32_32{};
    const math::q32_32 rawYawRadians = hasDirection
        ? math::fixed_atan2(deltaY, deltaX)
        : activePlacement.fixedYawRadians;
    const math::q32_32 yawRadians =
        selection::snapLocalPlacementYaw45Fixed(
            rawYawRadians, forceAttackSnap && hasDirection);

    // Dragging chooses orientation only. RefCode keeps the place icon and the
    // eventual Build target at the mouse-down anchor, not at the drag end.
    return m_presentation.m_localPlacementPresentation.updatePose(
        anchorStart, yawRadians);
}

bool LocalPlacementPresentationPort::planLine(render::RenderVector anchorStart,
                                                                    render::RenderVector anchorEnd,
                                                                    bool forceAttackSnap)
{
    const selection::LocalPlacementPreviewSnapshot activePlacement =
        m_presentation.m_localPlacementPresentation.snapshot();
    if (!m_content.m_active || !m_content.m_terrain.isLoaded() ||
        !activePlacement.sourceObject || activePlacement.objectType.empty() || !std::isfinite(anchorStart.x()) ||
        !std::isfinite(anchorStart.y()) || !std::isfinite(anchorStart.z()) || !std::isfinite(anchorEnd.x()) ||
        !std::isfinite(anchorEnd.y()) || !std::isfinite(anchorEnd.z()))
    {
        return false;
    }

    const PlayerState* localPlayer = m_content.m_players.localPlayer();
    if (!localPlayer || !localPlayer->isCommandPlayer() ||
        m_world.m_ownership.ownerOf(activePlacement.sourceObject) != localPlayer->id ||
        !m_world.m_objects.entityFromId(activePlacement.sourceObject))
    {
        static_cast<void>(m_presentation.m_localPlacementPresentation.cancel());
        return false;
    }
    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(activePlacement.objectType);
    if (!product || !game::objectHasKind(product->kindOfMask, game::ObjectKindOf::LineBuild))
    {
        return false;
    }

    const LogicFixedVec3 fixedStart = LogicFixedVec3::fromFloats(anchorStart.x(), anchorStart.y(), anchorStart.z());
    const LogicFixedVec3 fixedEnd = LogicFixedVec3::fromFloats(anchorEnd.x(), anchorEnd.y(), anchorEnd.z());
    const math::q32_32 deltaX = fixedEnd.x - fixedStart.x;
    const math::q32_32 deltaY = fixedEnd.y - fixedStart.y;
    const bool hasDirection = deltaX != math::q32_32{} || deltaY != math::q32_32{};
    const math::q32_32 rawYawRadians =
        hasDirection ? math::fixed_atan2(deltaY, deltaX) : product->templateData.placementViewAngleRadiansFixed;
    const math::q32_32 yawRadians =
        selection::snapLocalPlacementYaw45Fixed(rawYawRadians, forceAttackSnap && hasDirection);

    struct PlanningContext final
    {
        LocalPlacementPresentationPort* session = nullptr;
        const game::ObjectArchetype* product = nullptr;
        PlayerId player = INVALID_PLAYER_ID;
        selection::LocalPlacementPreviewSnapshot placement;
        GameSessionBuildPlacementLegalityEvaluation firstRejection;
        LineBuildPosition firstRejectedPosition;
        uint32_t firstRejectedIndex = std::numeric_limits<uint32_t>::max();
    } context{
        .session = this,
        .product = product.get(),
        .player = localPlayer->id,
        .placement = activePlacement,
    };
    context.placement.fixedYawRadians = yawRadians;
    context.placement.yawRadians = yawRadians.to_float();
    context.placement.hasPose = true;

    LineBuildPlacementRequest request;
    request.start = {fixedStart.x, fixedStart.y, fixedStart.z};
    request.end = {fixedEnd.x, fixedEnd.y, fixedEnd.z};
    request.geometryMajorRadius = product->templateData.geometry.majorRadiusFixed;
    request.maxTiles = m_content.m_objectSimulationRules.buildPlacement.maxLineBuildObjects;
    request.callbackContext = &context;
    request.terrainHeight = [](void* opaque, math::q32_32 x, math::q32_32 y, math::q32_32& height) noexcept
    {
        PlanningContext& planning = *static_cast<PlanningContext*>(opaque);
        height = math::q32_32::from_raw(
            planning.session->m_content.m_terrain.groundHeightRaw(x.raw(), y.raw()));
        return true;
    };
    request.isLegal = [](void* opaque, const LineBuildPosition& position, uint32_t tileIndex) noexcept
    {
        PlanningContext& planning = *static_cast<PlanningContext*>(opaque);
        planning.placement.fixedPosition = {.x = position.x, .y = position.y, .z = position.z, .valid = true};
        planning.placement.position = {position.x.to_float(), position.y.to_float(), position.z.to_float()};
        GameSessionBuildPlacementLegalityEvaluation evaluation =
            planning.session->evaluate(planning.placement, planning.player, *planning.product, false);
        if (evaluation.evaluated && evaluation.legality == selection::LocalPlacementLegality::Legal)
        {
            return true;
        }
        planning.firstRejection = std::move(evaluation);
        planning.firstRejectedPosition = position;
        planning.firstRejectedIndex = tileIndex;
        return false;
    };

    const LineBuildPlacementPlan plan = planLineBuildPlacement(request);
    if (plan.boundedTileCount == 0)
        return false;

    container::Vector<selection::LocalPlacementPreviewTileInput> tiles;
    tiles.reserve(std::max<size_t>(1u, plan.legalPrefix.size()));
    for (const LineBuildPosition& position : plan.legalPrefix)
    {
        tiles.push_back({
            .position = {.x = position.x, .y = position.y, .z = position.z, .valid = true},
            .yawRadians = yawRadians,
            .legality = selection::LocalPlacementLegality::Legal,
        });
    }
    // A later rejection only terminates the visible legal prefix. If the
    // anchor itself is illegal, keep one red place icon and only the explicit
    // blockers returned by the same cursor evaluator.
    if (tiles.empty())
    {
        if (context.firstRejectedIndex != 0u || !context.firstRejection.evaluated)
        {
            return false;
        }
        tiles.push_back({
            .position =
                {
                    .x = context.firstRejectedPosition.x,
                    .y = context.firstRejectedPosition.y,
                    .z = context.firstRejectedPosition.z,
                    .valid = true,
                },
            .yawRadians = yawRadians,
            .legality = selection::LocalPlacementLegality::Illegal,
            .obstructions = context.firstRejection.obstructions,
        });
    }
    const math::q32_32 authoritativeEndZ = math::q32_32::from_raw(
        m_content.m_terrain.groundHeightRaw(fixedEnd.x.raw(), fixedEnd.y.raw()));
    return m_presentation.m_localPlacementPresentation.replaceLineTiles(
        tiles, {.x = fixedEnd.x, .y = fixedEnd.y, .z = authoritativeEndZ, .valid = true});
}

bool LocalPlacementPresentationPort::publishLegality(
    selection::LocalPlacementLegality legality, container::Span<const render::TerrainBibFootprintInput> obstructions)
{
    return m_content.m_active &&
           m_presentation.m_localPlacementPresentation.publishLegality(legality, obstructions);
}

bool LocalPlacementPresentationPort::refreshLegality(bool finalConfirmation)
{
    const selection::LocalPlacementPreviewSnapshot placement =
        m_presentation.m_localPlacementPresentation.snapshot();
    if (!m_content.m_active || !placement.sourceObject || !placement.hasPose ||
        placement.objectType.empty())
    {
        return false;
    }
    const PlayerState* localPlayer = m_content.m_players.localPlayer();
    if (!localPlayer || !localPlayer->isCommandPlayer() ||
        m_world.m_ownership.ownerOf(placement.sourceObject) != localPlayer->id ||
        !m_world.m_objects.entityFromId(placement.sourceObject))
    {
        static_cast<void>(m_presentation.m_localPlacementPresentation.cancel());
        return false;
    }
    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(placement.objectType);
    if (!product)
    {
        static_cast<void>(m_presentation.m_localPlacementPresentation.cancel());
        return false;
    }
    GameSessionBuildPlacementLegalityEvaluation evaluation =
        evaluate(placement, localPlayer->id, *product, finalConfirmation);
    if (evaluation.evaluated &&
        evaluation.rejection != m_lastLoggedRejection)
    {
        TD_LOG_DEBUG(
            "[GameSession] Local placement source={} product='{}' position=({:.2f},{:.2f},{:.2f}) result={} reason={}",
            placement.sourceObject.value, placement.objectType,
            placement.position.x(), placement.position.y(), placement.position.z(),
            evaluation.legality == selection::LocalPlacementLegality::Legal
                ? "legal" : "illegal",
            placementRejectionName(evaluation.rejection));
        m_lastLoggedRejection = evaluation.rejection;
    }
    return evaluation.evaluated && m_presentation.m_localPlacementPresentation.publishLegality(
                                       evaluation.legality, evaluation.obstructions);
}

bool LocalPlacementPresentationPort::builderHasPendingConstruction(
    ObjectId builder) const noexcept
{
    return builder && m_world.m_objectSimulation.isObjectBuilderTaskPending(
        m_world.m_registry, m_world.m_objects, builder,
        ObjectBuilderTaskKind::Build);
}

std::optional<selection::LocalPlacementPreviewSnapshot>
LocalPlacementPresentationPort::makeLocalConstructionRouteNodeFromAnchors(
    const selection::LocalPlacementPreviewSnapshot& placement,
    CommandPosition anchorStart, CommandPosition anchorEnd,
    bool hasDirection, bool forceAttackSnap) {
    if (!anchorStart.valid || !anchorEnd.valid ||
        placement.backend != selection::LocalPlacementBackendKind::Build) {
        return std::nullopt;
    }
    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(
            placement.objectType);
    if (!product) return std::nullopt;

    selection::LocalPlacementPreviewSnapshot candidate = placement;
    candidate.fixedPosition = anchorStart;
    candidate.position = {
        anchorStart.x.to_float(), anchorStart.y.to_float(),
        anchorStart.z.to_float()};
    candidate.selectionRadius = std::max(
        4.0f,
        product->templateData.geometry.boundingCircleRadiusFixed.to_float());
    candidate.hasPose = true;

    const math::q32_32 deltaX = anchorEnd.x - anchorStart.x;
    const math::q32_32 deltaY = anchorEnd.y - anchorStart.y;
    const bool direction = hasDirection &&
        (deltaX != math::q32_32{} || deltaY != math::q32_32{});
    const bool lineBuild = game::objectHasKind(
        product->kindOfMask, game::ObjectKindOf::LineBuild);
    const math::q32_32 fallbackYaw = lineBuild
        ? product->templateData.placementViewAngleRadiansFixed
        : placement.fixedYawRadians;
    candidate.fixedYawRadians =
        selection::snapLocalPlacementYaw45Fixed(
            direction ? math::fixed_atan2(deltaY, deltaX) : fallbackYaw,
            forceAttackSnap && direction);
    candidate.yawRadians = candidate.fixedYawRadians.to_float();
    candidate.hasLineEndPosition = lineBuild;
    if (lineBuild) {
        candidate.fixedLineEndPosition = anchorEnd;
        candidate.lineEndPosition = {
            anchorEnd.x.to_float(), anchorEnd.y.to_float(),
            anchorEnd.z.to_float()};
    } else {
        candidate.fixedLineEndPosition = {};
        candidate.lineEndPosition = {};
    }
    const PlayerState* localPlayer = m_content.m_players.localPlayer();
    if (!m_content.m_active || !candidate.sourceObject || !localPlayer ||
        !localPlayer->isCommandPlayer() ||
        m_world.m_ownership.ownerOf(candidate.sourceObject) !=
            localPlayer->id ||
        !m_world.m_objects.entityFromId(candidate.sourceObject)) {
        return std::nullopt;
    }
    const GameSessionBuildPlacementLegalityEvaluation evaluation =
        evaluate(candidate, localPlayer->id, *product, false);
    if (!evaluation.evaluated || evaluation.legality !=
            selection::LocalPlacementLegality::Legal) {
        return std::nullopt;
    }
    candidate.legality = selection::LocalPlacementLegality::Legal;
    candidate.feedback = selection::LocalPlacementPreviewFeedback::Queued;
    candidate.sourceSequence = 0;
    candidate.routeAnchorOnly = false;
    candidate.routeLocalOnly = true;
    // The placement activation admitted the cursor only. A later route Build
    // is validated against current state and must not replay this old UI token.
    candidate.activation = {};
    return candidate;
}

std::optional<GameCommand>
LocalPlacementPresentationPort::composeLocalConstructionRouteBuildCommand(
    const selection::LocalPlacementPreviewSnapshot& placement)
{
    if (placement.backend != selection::LocalPlacementBackendKind::Build ||
        !placement.routeLocalOnly) {
        return std::nullopt;
    }
    std::optional<GameCommand> command =
        composeCommand(placement, false, true);
    if (command) {
        command->queued = false;
        command->activation = {};
    }
    return command;
}

void LocalPlacementPresentationPort::setLocalConstructionRoutePreviews(
    container::Span<const selection::LocalPlacementPreviewSnapshot> previews)
{
    auto& published = m_presentation.m_queuedConstructionPlacements;
    std::erase_if(
        published,
        [](const selection::LocalPlacementPreviewSnapshot& value) noexcept {
            return value.routeLocalOnly;
        });
    container::Vector<ObjectId> anchoredBuilders;
    for (const selection::LocalPlacementPreviewSnapshot& preview : previews) {
        if (!preview.sourceObject || !preview.previewIdentity ||
            !preview.hasPose || preview.objectType.empty()) {
            continue;
        }
        const bool alreadyAnchored = std::any_of(
            published.begin(), published.end(),
            [&](const selection::LocalPlacementPreviewSnapshot& value) {
                return value.sourceObject == preview.sourceObject &&
                    value.routeAnchorOnly;
            });
        if (!alreadyAnchored && std::find(
                anchoredBuilders.begin(), anchoredBuilders.end(),
                preview.sourceObject) == anchoredBuilders.end()) {
            const std::optional<ecs::entity> entity =
                m_world.m_objects.entityFromId(preview.sourceObject);
            const ObjectFixedTransformComponent* transform = entity
                ? ecs::try_get<ObjectFixedTransformComponent>(
                      m_world.m_registry, *entity)
                : nullptr;
            if (transform && transform->authoritative) {
                selection::LocalPlacementPreviewSnapshot anchor;
                anchor.presentationEpoch = preview.presentationEpoch;
                anchor.animationStartTick = preview.animationStartTick;
                anchor.previewIdentity =
                    selection::localConstructionRouteAnchorIdentity(
                        preview.sourceObject);
                anchor.sourceObject = preview.sourceObject;
                anchor.fixedPosition = {
                    .x = transform->position.x,
                    .y = transform->position.y,
                    .z = transform->position.z,
                    .valid = true,
                };
                anchor.position = {
                    transform->position.x.to_float(),
                    transform->position.y.to_float(),
                    transform->position.z.to_float(),
                };
                anchor.hasPose = true;
                anchor.legality = selection::LocalPlacementLegality::Legal;
                anchor.feedback = selection::LocalPlacementPreviewFeedback::Queued;
                anchor.routeAnchorOnly = true;
                anchor.routeLocalOnly = true;
                published.push_back(std::move(anchor));
            }
            anchoredBuilders.push_back(preview.sourceObject);
        }
        selection::LocalPlacementPreviewSnapshot copy = preview;
        copy.routeLocalOnly = true;
        copy.sourceSequence = 0;
        published.push_back(std::move(copy));
    }
    std::stable_sort(
        published.begin(), published.end(),
        [](const selection::LocalPlacementPreviewSnapshot& left,
           const selection::LocalPlacementPreviewSnapshot& right) noexcept {
            return left.sourceObject < right.sourceObject;
        });
}

std::optional<CommandPosition> LocalPlacementPresentationPort::
    localConstructionRouteApproachTarget(
        ObjectId builder,
        const selection::LocalPlacementPreviewSnapshot& placement) const
    noexcept
{
    if (!m_content.m_active || !builder ||
        builder != placement.sourceObject ||
        !placement.fixedPosition.valid) {
        return std::nullopt;
    }
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(builder);
    const ObjectFixedTransformComponent* transform = entity
        ? ecs::try_get<ObjectFixedTransformComponent>(m_world.m_registry,
                                                       *entity)
        : nullptr;
    const ObjectLocomotionComponent* locomotion = entity
        ? ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry,
                                                   *entity)
        : nullptr;
    const ObjectGeometryComponent* builderGeometry = entity
        ? ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, *entity)
        : nullptr;
    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(
            placement.objectType);
    if (!transform || !transform->authoritative || !locomotion ||
        !product) {
        return std::nullopt;
    }
    const math::q32_32 builderRadius = builderGeometry
        ? math::q32_32::max(
              math::q32_32{},
              builderGeometry->boundingCircleRadiusFixed)
        : math::q32_32{int32_t{5}};
    const ObjectBuilderApproachResult approach = objectBuilderApproach(
        transform->position,
        {
            placement.fixedPosition.x,
            placement.fixedPosition.y,
            placement.fixedPosition.z,
        },
        builderRadius,
        product->templateData.geometry.boundingCircleRadiusFixed,
        locomotion->closeEnough);
    return CommandPosition{
        .x = approach.target.x,
        .y = approach.target.y,
        .z = approach.target.z,
        .valid = true,
    };
}

bool LocalPlacementPresentationPort::
    builderReachedLocalConstructionRouteNode(
        ObjectId builder,
        const selection::LocalPlacementPreviewSnapshot& placement) const
    noexcept
{
    if (!m_content.m_active || !builder ||
        builder != placement.sourceObject || !placement.fixedPosition.valid ||
        builderHasPendingConstruction(builder)) {
        return false;
    }
    const std::optional<ecs::entity> entity =
        m_world.m_objects.entityFromId(builder);
    const ObjectFixedTransformComponent* transform = entity
        ? ecs::try_get<ObjectFixedTransformComponent>(m_world.m_registry,
                                                       *entity)
        : nullptr;
    const ObjectLocomotionComponent* locomotion = entity
        ? ecs::try_get<ObjectLocomotionComponent>(m_world.m_registry,
                                                   *entity)
        : nullptr;
    const ObjectGeometryComponent* builderGeometry = entity
        ? ecs::try_get<ObjectGeometryComponent>(m_world.m_registry, *entity)
        : nullptr;
    if (!transform || !transform->authoritative || !locomotion) return false;

    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(
            placement.objectType);
    if (!product) return false;
    const math::q32_32 builderRadius = builderGeometry
        ? math::q32_32::max(
              math::q32_32{},
              builderGeometry->boundingCircleRadiusFixed)
        : math::q32_32{int32_t{5}};
    const math::q32_32 productRadius = math::q32_32::max(
        math::q32_32{},
        product->templateData.geometry.boundingCircleRadiusFixed);
    // Match ObjectBuilderSystem's authoritative approach threshold. The local
    // route only decides when to submit Build; placement, funds and ownership
    // are still revalidated by the confirmed transaction at that point.
    return objectBuilderApproach(
        transform->position,
        {
            placement.fixedPosition.x,
            placement.fixedPosition.y,
            placement.fixedPosition.z,
        },
        builderRadius, productRadius,
        locomotion->closeEnough).arrived;
}

std::optional<GameCommand> LocalPlacementPresentationPort::composeCommand(
    const selection::LocalPlacementPreviewSnapshot& placement,
    bool requirePublishedLegality, bool finalRevalidation,
    bool queuedPlanning)
{
    const PlayerState* localPlayer = m_content.m_players.localPlayer();
    const std::optional<ecs::entity> sourceEntity =
        m_world.m_objects.entityFromId(placement.sourceObject);
    const container::SharedPtr<const game::ObjectArchetype> product =
        m_content.m_contentSnapshot.findObjectArchetype(placement.objectType);
    if (!m_content.m_active || !placement.sourceObject || !placement.hasPose ||
        (requirePublishedLegality && placement.legality !=
            selection::LocalPlacementLegality::Legal) ||
        placement.objectType.empty() ||
        !localPlayer || !localPlayer->isCommandPlayer() ||
        m_world.m_ownership.ownerOf(placement.sourceObject) != localPlayer->id || !sourceEntity ||
        !product)
    {
        return std::nullopt;
    }
    if (finalRevalidation) {
        GameSessionBuildPlacementLegalityEvaluation evaluation =
            evaluate(placement, localPlayer->id, *product, true,
                     !placement.routeLocalOnly);
        if (!evaluation.evaluated || evaluation.legality !=
                selection::LocalPlacementLegality::Legal) {
            return std::nullopt;
        }
    }

    const bool specialPowerConstruct = placement.backend == selection::LocalPlacementBackendKind::SpecialPowerConstruct;
    const game::CommandButtonTemplate* constructButton = nullptr;
    if (specialPowerConstruct)
    {
        constructButton = m_content.m_contentSnapshot.findCommandButtonByStableId(
            placement.activation.buttonStableId);
        const game::CommandButtonKind kind = placement.activation.commandKind;
        if (!placement.activation.present() || !constructButton ||
            constructButton->descriptor.stableId != placement.activation.buttonStableId ||
            constructButton->descriptor.kind != kind ||
            (kind != game::CommandButtonKind::SpecialPowerConstruct &&
             kind != game::CommandButtonKind::SpecialPowerConstructFromShortcut) ||
            constructButton->object != placement.objectType || constructButton->specialPower.empty() ||
            placement.hasLineEndPosition)
        {
            return std::nullopt;
        }
    }
    else
    {
        bool ignoreBuildPrerequisites = false;
        if (!queuedPlanning &&
            (builderHasPendingConstruction(placement.sourceObject) ||
             !localPlayer->constructionPolicy.baseConstructionEnabled ||
             !makeProductionPolicyPort(
                  m_content,
                  m_presentation)
                  .admitsObjectBuildability(
                      localPlayer->id, *product,
                      ignoreBuildPrerequisites) ||
             !canObjectBuildTemplate(
                  m_world.m_registry, *sourceEntity,
                  m_content.m_contentSnapshot,
                  m_presentation.m_scriptCommandBarOverrides,
                  m_content.m_players, localPlayer->id, *product,
                  ignoreBuildPrerequisites) ||
             calculateObjectBuildCost(
                 *product, *localPlayer, m_world.m_registry,
                 m_world.m_objects) > localPlayer->cash))
        {
            // Keep placement active so a transient cash/prerequisite/producer
            // failure can be resolved without reopening the command button.
            return std::nullopt;
        }
    }
    GameCommand command;
    command.player = localPlayer->id;
    command.source = CommandSource::Local;
    command.type = specialPowerConstruct ? GameCommandType::SpecialPower : GameCommandType::Build;
    command.actors.push_back(placement.sourceObject);
    command.targetPosition = placement.fixedPosition;
    command.placementYawRadians = placement.fixedYawRadians;
    if (placement.hasLineEndPosition)
    {
        command.placementEndPosition = placement.fixedLineEndPosition;
    }
    command.commandName = specialPowerConstruct ? constructButton->name : placement.objectType;
    // The activation admits entry into placement mode.  Ordinary Build is a
    // later value command and is revalidated against current ownership,
    // buildability, cash and placement state; replaying the old button token
    // makes a direct placement look stale/consumed.  Special-power construct
    // still needs its ability identity at the confirmed boundary.
    command.activation = specialPowerConstruct
        ? placement.activation : CommandActivationContext{};
    container::String error;
    if (!OrderExecutor::fromGameCommand(command, &error))
    {
        TD_LOG_WARN("[GameSession] Local placement command was malformed: {}", error);
        return std::nullopt;
    }
    return command;
}

std::optional<GameCommand> LocalPlacementPresentationPort::takeCommand()
{
    std::optional<GameCommand> command = composeCommand(
        m_presentation.m_localPlacementPresentation.snapshot(), true, false);
    if (command) {
        static_cast<void>(
            m_presentation.m_localPlacementPresentation.cancel());
    }
    return command;
}

bool LocalPlacementPresentationPort::cancel(
    render::RenderEntityId expectedPreviewIdentity) noexcept
{
    m_lastLoggedRejection = GameSessionBuildPlacementRejection::None;
    return m_presentation.m_localPlacementPresentation.cancel(expectedPreviewIdentity);
}

GameSessionBuildPlacementLegalityEvaluation
LocalPlacementPresentationPort::evaluate(
    const selection::LocalPlacementPreviewSnapshot& placement,
    PlayerId player,
    const game::ObjectArchetype& product,
    bool finalConfirmation,
    bool requireBuilderReachability)
{
    if (!placement.hasPose || !placement.fixedPosition.valid)
    {
        return {};
    }
    return m_evaluator.evaluateFixed(
        placement.sourceObject,
        {placement.fixedPosition.x, placement.fixedPosition.y, placement.fixedPosition.z},
        placement.fixedYawRadians,
        player,
        product,
        finalConfirmation,
        requireBuilderReachability);
}
} // namespace engine
