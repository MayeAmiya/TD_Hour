#include "LocalPlacementPresentationState.h"

#include <cmath>

namespace engine::selection {

LocalPlacementCommandIntent resolveLocalPlacementCommand(
    const game::CommandButtonTemplate& button) {
    if (!button.descriptor.userActivatable()) return {};
    if (button.descriptor.kind ==
        game::CommandButtonKind::DozerConstructCancel) {
        return {.kind = LocalPlacementCommandKind::Cancel};
    }
    if (button.descriptor.kind !=
            game::CommandButtonKind::DozerConstruct ||
        button.object.empty()) {
        return {};
    }
    return {
        .kind = LocalPlacementCommandKind::Begin,
        .productType = button.object,
    };
}

void LocalPlacementPresentationState::reset(
    uint64_t presentationEpoch) noexcept {
    m_presentationEpoch = presentationEpoch;
    m_animationStartTick = 0;
    m_sourceObject = INVALID_OBJECT_ID;
    m_objectType.clear();
    m_geometryMajorRadius = 0.0f;
    m_geometryMinorRadius = 0.0f;
    m_geometryIsBox = false;
    m_factoryExitWidth = 0.0f;
    m_factoryExtraBibWidth = 0.0f;
    m_active = false;
    m_backend = LocalPlacementBackendKind::Build;
    m_tiles.clear();
    m_tileIdentities.clear();
    m_lineEndPosition = {};
    m_hasLineEndPosition = false;
    m_activation = {};
}

render::RenderEntityId
LocalPlacementPresentationState::allocatePreviewIdentity() noexcept {
    constexpr uint64_t kOrdinalMask = 0x0000ffffffffffffull;
    m_nextPreviewOrdinal = (m_nextPreviewOrdinal + 1u) & kOrdinalMask;
    if (m_nextPreviewOrdinal == 0) ++m_nextPreviewOrdinal;
    // 11 reserves a renderer-local placement domain distinct from both live
    // ObjectIds and the 10 client-terrain domain. The low 48-bit ordinal is
    // shared by the place icon and its terrain-bib feedback.
    return 0xc000000000000000ull | m_nextPreviewOrdinal;
}

bool LocalPlacementPresentationState::begin(
    uint64_t presentationEpoch, uint64_t animationStartTick,
    ObjectId sourceObject,
    const game::ThingTemplate& product,
    LocalPlacementBackendKind backend,
    CommandActivationContext activation) {
    if (presentationEpoch == 0 || !sourceObject || product.name.empty() ||
        product.geometry.majorRadiusFixed < math::q32_32{} ||
        product.geometry.minorRadiusFixed < math::q32_32{}) {
        return false;
    }
    if (m_presentationEpoch != presentationEpoch) reset(presentationEpoch);
    m_animationStartTick = animationStartTick;
    m_sourceObject = sourceObject;
    m_objectType = product.name;
    m_geometryMajorRadius = product.geometry.majorRadiusFixed.to_float();
    m_geometryMinorRadius = product.geometry.minorRadiusFixed.to_float();
    m_geometryIsBox = product.geometry.type == game::ObjectGeometryType::Box;
    m_factoryExitWidth = product.factoryExitWidthFixed.to_float();
    m_factoryExtraBibWidth = product.factoryExtraBibWidthFixed.to_float();
    m_active = true;
    m_backend = backend;
    m_tiles.clear();
    m_tileIdentities.clear();
    m_tileIdentities.push_back(allocatePreviewIdentity());
    m_tiles.push_back({
        .previewIdentity = m_tileIdentities.front(),
        .yawRadians = product.placementViewAngleRadiansFixed,
    });
    m_lineEndPosition = {};
    m_hasLineEndPosition = false;
    m_activation = activation;
    return true;
}

bool LocalPlacementPresentationState::updatePose(
    render::RenderVector position, math::q32_32 yawRadians) noexcept {
    if (!m_active || m_tiles.empty() || !std::isfinite(position.x()) ||
        !std::isfinite(position.y()) || !std::isfinite(position.z())) {
        return false;
    }
    const CommandPosition fixedPosition{
        .x = math::q32_32{position.x()},
        .y = math::q32_32{position.y()},
        .z = math::q32_32{position.z()},
        .valid = true,
    };
    const math::q32_32 fixedYaw = yawRadians;
    TileState& tile = m_tiles.front();
    if (m_tiles.size() == 1u && !m_hasLineEndPosition && tile.hasPose &&
        tile.position.x == fixedPosition.x &&
        tile.position.y == fixedPosition.y && tile.position.z == fixedPosition.z &&
        tile.yawRadians == fixedYaw) {
        return true;
    }
    // This is the compatibility single-point API. If an anchored line was
    // active, returning to cursor pose must not retain its tail or end anchor.
    m_tiles.resize(1u);
    m_lineEndPosition = {};
    m_hasLineEndPosition = false;
    tile.position = fixedPosition;
    tile.yawRadians = fixedYaw;
    tile.hasPose = true;
    // A moved preview must not retain a legality result calculated for the
    // old world pose. The controller republishes it on its own cadence.
    tile.legality = LocalPlacementLegality::Unchecked;
    tile.obstructions.clear();
    return true;
}

bool LocalPlacementPresentationState::publishLegality(
    LocalPlacementLegality legality,
    container::Span<const render::TerrainBibFootprintInput> obstructions) {
    if (!m_active || m_tiles.empty() || !m_tiles.front().hasPose) return false;
    container::Vector<render::TerrainBibFootprintInput> next;
    if (legality == LocalPlacementLegality::Illegal) {
        next.reserve(obstructions.size());
        for (render::TerrainBibFootprintInput obstruction : obstructions) {
            obstruction.kind = render::TerrainBibKind::Building;
            obstruction.highlighted = true;
            if (!render::buildTerrainBibFootprint(obstruction)) return false;
            next.push_back(std::move(obstruction));
        }
    }
    m_tiles.front().legality = legality;
    m_tiles.front().obstructions = std::move(next);
    return true;
}

bool LocalPlacementPresentationState::replaceTiles(
    container::Span<const LocalPlacementPreviewTileInput> tiles) {
    return replaceTilesImpl(tiles, false, {});
}

bool LocalPlacementPresentationState::replaceLineTiles(
    container::Span<const LocalPlacementPreviewTileInput> tiles,
    CommandPosition lineEndPosition) {
    return replaceTilesImpl(tiles, true, lineEndPosition);
}

bool LocalPlacementPresentationState::replaceTilesImpl(
    container::Span<const LocalPlacementPreviewTileInput> tiles,
    bool hasLineEndPosition, CommandPosition lineEndPosition) {
    if (!m_active || tiles.empty()) return false;
    if (hasLineEndPosition && !lineEndPosition.valid) {
        return false;
    }

    container::Vector<TileState> next;
    next.reserve(tiles.size());
    for (const LocalPlacementPreviewTileInput& input : tiles) {
        if (!input.position.valid) return false;
        TileState tile;
        tile.position = input.position;
        tile.yawRadians = input.yawRadians;
        tile.hasPose = true;
        tile.legality = input.legality;
        if (input.legality == LocalPlacementLegality::Illegal) {
            tile.obstructions.reserve(input.obstructions.size());
            for (render::TerrainBibFootprintInput obstruction :
                 input.obstructions) {
                obstruction.kind = render::TerrainBibKind::Building;
                obstruction.highlighted = true;
                if (!render::buildTerrainBibFootprint(obstruction)) {
                    return false;
                }
                tile.obstructions.push_back(std::move(obstruction));
            }
        }
        next.push_back(std::move(tile));
    }

    while (m_tileIdentities.size() < next.size()) {
        m_tileIdentities.push_back(allocatePreviewIdentity());
    }
    for (size_t index = 0; index < next.size(); ++index) {
        next[index].previewIdentity = m_tileIdentities[index];
    }
    m_tiles = std::move(next);
    m_lineEndPosition = hasLineEndPosition
        ? lineEndPosition : CommandPosition{};
    m_hasLineEndPosition = hasLineEndPosition;
    return true;
}

bool LocalPlacementPresentationState::cancel(
    render::RenderEntityId expectedPreviewIdentity) noexcept {
    if (!m_active) return false;
    if (expectedPreviewIdentity != 0 &&
        (m_tiles.empty() ||
         m_tiles.front().previewIdentity != expectedPreviewIdentity)) {
        return false;
    }
    const uint64_t epoch = m_presentationEpoch;
    reset(epoch);
    return true;
}

void LocalPlacementPresentationState::appendTerrainBibs(
    container::Vector<render::TerrainBibRenderData>& output) const {
    if (!m_active) return;
    for (const TileState& tile : m_tiles) {
        if (!tile.hasPose) continue;
        const render::TerrainBibTint tint = [&] {
            switch (tile.legality) {
            case LocalPlacementLegality::Unchecked:
                return render::TerrainBibTint::Blue;
            case LocalPlacementLegality::Legal:
                return render::TerrainBibTint::Green;
            case LocalPlacementLegality::Illegal:
                return render::TerrainBibTint::Red;
            }
            return render::TerrainBibTint::Blue;
        }();
        const render::TerrainBibFootprintInput preview{
            .ownerIdentity = tile.previewIdentity,
            .kind = render::TerrainBibKind::PlacementPreview,
            .position = {
                tile.position.x.to_float(), tile.position.y.to_float(),
                tile.position.z.to_float()},
            .yawRadians = tile.yawRadians.to_float(),
            .geometryMajorRadius = m_geometryMajorRadius,
            .geometryMinorRadius = m_geometryMinorRadius,
            .geometryIsBox = m_geometryIsBox,
            .factoryExitWidth = m_factoryExitWidth,
            .factoryExtraBibWidth = m_factoryExtraBibWidth,
            .highlighted =
                tile.legality == LocalPlacementLegality::Illegal,
            .tint = tint,
            .receivesVisibility = true,
        };
        if (std::optional<render::TerrainBibRenderData> bib =
                render::buildTerrainBibFootprint(preview)) {
            output.push_back(std::move(*bib));
        }
        for (render::TerrainBibFootprintInput obstruction :
             tile.obstructions) {
            obstruction.tint = render::TerrainBibTint::Red;
            if (std::optional<render::TerrainBibRenderData> bib =
                    render::buildTerrainBibFootprint(obstruction)) {
                output.push_back(std::move(*bib));
            }
        }
    }
}

LocalPlacementPreviewSnapshot
LocalPlacementPresentationState::snapshot() const {
    return m_tiles.empty() ? LocalPlacementPreviewSnapshot{}
                           : snapshot(m_tiles.front());
}

LocalPlacementPreviewSnapshot LocalPlacementPresentationState::snapshot(
    const TileState& tile) const {
    return {
        .presentationEpoch = m_presentationEpoch,
        .animationStartTick = m_animationStartTick,
        .previewIdentity = tile.previewIdentity,
        .sourceObject = m_sourceObject,
        .objectType = m_objectType,
        .fixedPosition = tile.position,
        .fixedYawRadians = tile.yawRadians,
        .position = {
            tile.position.x.to_float(), tile.position.y.to_float(),
            tile.position.z.to_float()},
        .yawRadians = tile.yawRadians.to_float(),
        .fixedLineEndPosition = m_lineEndPosition,
        .lineEndPosition = {
            m_lineEndPosition.x.to_float(),
            m_lineEndPosition.y.to_float(),
            m_lineEndPosition.z.to_float()},
        .hasPose = tile.hasPose,
        .hasLineEndPosition = m_hasLineEndPosition,
        .legality = tile.legality,
        .backend = m_backend,
        .activation = m_activation,
    };
}

container::Vector<LocalPlacementPreviewSnapshot>
LocalPlacementPresentationState::snapshots() const {
    container::Vector<LocalPlacementPreviewSnapshot> result;
    result.reserve(m_tiles.size());
    for (const TileState& tile : m_tiles) result.push_back(snapshot(tile));
    return result;
}

} // namespace engine::selection
