#pragma once

#include "core/container/container_types.h"

#include "presentation/camera/GameCameraState.h"
#include "core/ecs/ObjectId.h"
#include "presentation/render/RenderSceneSnapshot.h"

#include <cstdint>
namespace game::terrain {
struct TerrainHeightfieldData;
class TerrainLogic;
}

namespace engine::script {
struct ScriptTerrainRoadPresentationSettings;
struct ScriptWaterPresentationSettings;
}

namespace engine {
class ClientTerrainObjectStore;
class GameSessionContentStartState;
class GameSessionObjectEventState;
class GameSessionScriptPresentationState;
class GameSessionWorldState;
struct GameRenderExtractionCache;
struct GameRenderEntityExtractionSource;
struct GameRenderTerrainExtractionSource;

struct TerrainBridgeAuthoritativeRenderState final {
    uint64_t sourceRecordIndex = 0;
    uint64_t objectId = 0;
    render::TerrainBridgeDamageState damageState =
        render::TerrainBridgeDamageState::Pristine;
};

// The sole game-domain adapter in the world-render path. It reads stable ECS
// component values after a logic tick and produces a self-contained snapshot;
// it never exposes entities, templates, or game-owned pointers to rendering.
class GameRenderExtraction final {
public:
    // Explicit value-only bridge from the logic camera to a sealed render
    // frame. Keeping it in the game/render adapter ensures RendererSubsystem
    // never becomes camera authority merely by owning a GPU backend.
    [[nodiscard]] static render::RenderCameraSnapshot extractCamera(
        const GameCameraState& camera) noexcept;
    [[nodiscard]] static render::WorldRenderSnapshot extract(
        const GameSessionContentStartState& content,
        const GameSessionWorldState& world,
        GameSessionScriptPresentationState& presentation,
        const GameSessionObjectEventState& objectEvents,
        GameRenderExtractionCache& cache,
        render::RenderCameraSnapshot camera,
        uint64_t simulationFrame,
        // Selection is local presentation state. Callers supply a sorted
        // ObjectId view from LocalSelectionState; extraction overlays it onto
        // the detached snapshot instead of writing a per-client bit into ECS.
        container::Span<const ObjectId> localSelection = {},
        ObjectId localHover = INVALID_OBJECT_ID,
        bool showPlayerWaypoints = false,
        bool includeVisualAssetDependencies = false);

    // The terrain logic owns map parsing, collision and deformation. This is
    // its explicit conversion point to immutable renderer-facing data.
    [[nodiscard]] static container::SharedPtr<const render::TerrainRenderSnapshot> extractTerrain(
        const game::terrain::TerrainHeightfieldData& terrain,
        uint64_t terrainRevision);
    [[nodiscard]] static container::SharedPtr<const render::TerrainRenderSnapshot> extractTerrain(
        const game::terrain::TerrainLogic& terrain);

    // Applies presentation-only INI data to an already detached terrain
    // snapshot. Normal sessions and --debug-world-map must share this bridge;
    // otherwise the diagnostic path sees road style names but never receives
    // their authored textures, widths or water material settings.
    static void applyTerrainPresentation(
        render::TerrainRenderSnapshot& terrain,
        const script::ScriptWaterPresentationSettings& water,
        const script::ScriptTerrainRoadPresentationSettings& roads,
        int32_t waterType = 0);

    // Applies confirmed GenericBridge/Body state to detached terrain bridge
    // values. Map properties remain the fallback for bridges without a live
    // authoritative object. The input must be sorted by sourceRecordIndex,
    // then objectId; duplicate map bindings deterministically keep the first.
    static void applyTerrainBridgeAuthoritativeStates(
        render::TerrainRenderSnapshot& terrain,
        container::Span<const TerrainBridgeAuthoritativeRenderState> states);

    // Focused, value-only A10 bridge.  The normal extract() path invokes it
    // after authoritative ECS objects; exposing the pure append boundary lets
    // probes verify client terrain ownership without constructing a session
    // or renderer device.
    static void appendClientTerrainObjects(
        const ClientTerrainObjectStore& source,
        render::WorldRenderSnapshot& destination);

private:
    static void extractViewAndVisibility(
        GameSessionScriptPresentationState& presentation,
        const GameSessionContentStartState& content,
        const GameSessionWorldState& world,
        const void* sessionIdentity,
        render::WorldRenderSnapshot& snapshot,
        render::RenderCameraSnapshot camera,
        uint64_t simulationFrame);
    static void extractTerrainAdmission(
        const GameRenderTerrainExtractionSource& source,
        render::WorldRenderSnapshot& snapshot);
    static void extractEntitiesAndUi(
        const GameRenderEntityExtractionSource& source,
        render::WorldRenderSnapshot& snapshot,
        uint64_t simulationFrame,
        container::Span<const ObjectId> localSelection,
        ObjectId localHover,
        bool includeVisualAssetDependencies,
        container::Span<const ObjectId> objectFilter,
        bool filterObjects,
        container::Vector<render::TacticalRadarEventRenderSnapshot>*
            rawGameplayRadarCandidates);
    static void finalizeAssembly(
        const GameRenderTerrainExtractionSource& source,
        render::WorldRenderSnapshot& snapshot);
};

} // namespace engine
